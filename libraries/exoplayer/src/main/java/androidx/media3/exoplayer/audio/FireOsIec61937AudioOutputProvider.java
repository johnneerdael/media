/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package androidx.media3.exoplayer.audio;

import static com.google.common.base.Preconditions.checkNotNull;
import static com.google.common.base.Preconditions.checkState;
import static java.lang.Math.max;

import android.annotation.SuppressLint;
import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioTrack;
import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.ParserException;
import androidx.media3.common.util.Log;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.common.util.Util;
import androidx.media3.extractor.Ac3Util;
import androidx.media3.extractor.DtsUtil;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.EnumMap;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Fire OS experimental IEC61937 passthrough provider that packs encoded access units before
 * handing them to {@link AudioTrack}.
 */
@UnstableApi
public final class FireOsIec61937AudioOutputProvider extends ForwardingAudioOutputProvider {

  private static final String TAG = "FireOsIecProvider";

  private static final int IEC61937_PACKET_HEADER_BYTES = 8;
  private static final int IEC61937_PACKET_SYNC_WORD_1 = 0xF872;
  private static final int IEC61937_PACKET_SYNC_WORD_2 = 0x4E1F;
  private static final int IEC61937_DTS1 = 0x0B;
  private static final int IEC61937_DTS2 = 0x0C;
  private static final int IEC61937_DTS3 = 0x0D;
  private static final int IEC61937_DTSHD = 0x11;
  private static final int IEC61937_TRUEHD = 0x16;
  private static final int IEC61937_DTS_HD_CARRIER_RATE_HZ = 192_000;
  private static final int IEC61937_TRUEHD_CARRIER_RATE_HZ = 192_000;
  private static final int IEC61937_TRUEHD_PACKET_BYTES = 61_440;
  private static final int IEC61937_TRUEHD_LENGTH_CODE = 61_424;
  private static final int IEC61937_DTS_CORE_MAX_PACKET_BYTES = 8_192;
  private static final int IEC61937_DTS_HD_MAX_PACKET_BYTES = 65_536;
  private static final int TRUEHD_FORMAT_MAJOR_SYNC = 0xF8726FBA;
  private static final int TRUEHD_MAT_BUFFER_LIMIT = IEC61937_TRUEHD_PACKET_BYTES - 24;
  private static final int TRUEHD_MAT_MIDDLE_POSITION = 30_708 + IEC61937_PACKET_HEADER_BYTES;
  private static final byte[] DTS_HD_START_CODE =
      new byte[] {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, (byte) 0xFE, (byte) 0xFE};
  private static final byte[] TRUEHD_MAT_START_CODE =
      new byte[] {
        0x07, (byte) 0x9E, 0x00, 0x03, (byte) 0x84, 0x01, 0x01, 0x01, (byte) 0x80, 0x00, 0x56,
        (byte) 0xA5, 0x3B, (byte) 0xF4, (byte) 0x81, (byte) 0x83, 0x49, (byte) 0x80, 0x77,
        (byte) 0xE0
      };
  private static final byte[] TRUEHD_MAT_MIDDLE_CODE =
      new byte[] {
        (byte) 0xC3, (byte) 0xC1, 0x42, 0x49, 0x3B, (byte) 0xFA, (byte) 0x82, (byte) 0x83, 0x49,
        (byte) 0x80, 0x77, (byte) 0xE0
      };
  private static final byte[] TRUEHD_MAT_END_CODE =
      new byte[] {
        (byte) 0xC3, (byte) 0xC2, (byte) 0xC0, (byte) 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, (byte) 0x97, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00
      };

  private final AudioOutputProvider passthroughProvider;
  private final AudioOutputProvider iecCarrierProvider;
  private final Map<OutputConfig, CarrierPlan> carrierPlans;
  private final EnumMap<PackerKind, FallbackMode> fallbackModes;

  public FireOsIec61937AudioOutputProvider(Context context) {
    this(
        new AudioTrackAudioOutputProvider.Builder(context).build(),
        new AudioTrackAudioOutputProvider.Builder(context)
            .setAudioTrackBuilderModifier(FireOsIec61937AudioOutputProvider::overrideToIec61937)
            .build());
  }

  private FireOsIec61937AudioOutputProvider(
      AudioOutputProvider passthroughProvider, AudioOutputProvider iecCarrierProvider) {
    super(passthroughProvider);
    this.passthroughProvider = passthroughProvider;
    this.iecCarrierProvider = iecCarrierProvider;
    this.carrierPlans = new HashMap<>();
    this.fallbackModes = new EnumMap<>(PackerKind.class);
  }

  @Override
  public OutputConfig getOutputConfig(FormatConfig formatConfig) throws ConfigurationException {
    @Nullable PackerKind kind = getPackerKind(formatConfig.format);
    if (kind != null) {
      @Nullable FallbackMode fallbackMode = fallbackModes.get(kind);
      if (fallbackMode == FallbackMode.RAW_PASSTHROUGH) {
        Log.w(TAG, "IEC packer disabled after recoverable failure for " + kind);
        return passthroughProvider.getOutputConfig(formatConfig);
      }
      if (fallbackMode == FallbackMode.DTS_CORE_RAW) {
        Log.w(TAG, "Falling back to raw DTS core after IEC failure");
        return passthroughProvider.getOutputConfig(copyFormatConfig(formatConfig, downgradeToDtsCore(formatConfig.format)));
      }
    }

    OutputConfig passthroughConfig = passthroughProvider.getOutputConfig(formatConfig);
    @Nullable CarrierPlan plan = maybeCreateCarrierPlan(formatConfig.format, passthroughConfig);
    if (plan == null) {
      return passthroughConfig;
    }

    int carrierChannelMask = plan.packer.getCarrierChannelMask(passthroughConfig.channelMask);
    int minBufferSize =
        AudioTrack.getMinBufferSize(
            plan.packer.getCarrierSampleRateHz(), carrierChannelMask, AudioFormat.ENCODING_PCM_16BIT);
    int bufferSize =
        max(
            passthroughConfig.bufferSize,
            max(
                plan.packer.getMaximumPacketSizeBytes(),
                minBufferSize > 0
                    ? minBufferSize * 4
                    : plan.packer.getMaximumPacketSizeBytes() * 4));

    OutputConfig carrierConfig =
        passthroughConfig.buildUpon()
            .setEncoding(C.ENCODING_PCM_16BIT)
            .setSampleRate(plan.packer.getCarrierSampleRateHz())
            .setChannelMask(carrierChannelMask)
            .setEncapsulationMode(AudioTrack.ENCAPSULATION_MODE_NONE)
            .setBufferSize(bufferSize)
            .build();
    carrierPlans.put(carrierConfig, plan);
    Log.i(
        TAG,
        "Using Fire OS IEC carrier"
            + " kind="
            + plan.kind
            + " mime="
            + formatConfig.format.sampleMimeType
            + " carrierRate="
            + carrierConfig.sampleRate
            + " channelMask="
            + carrierConfig.channelMask
            + " bufferSize="
            + carrierConfig.bufferSize);
    return carrierConfig;
  }

  @Override
  public AudioOutput getAudioOutput(OutputConfig config) throws InitializationException {
    @Nullable CarrierPlan plan = carrierPlans.get(config);
    if (plan == null) {
      return passthroughProvider.getAudioOutput(config);
    }
    try {
      AudioOutput carrierOutput = iecCarrierProvider.getAudioOutput(config);
      return new FireOsIec61937AudioOutput(carrierOutput, plan.kind, plan.packer, this);
    } catch (InitializationException e) {
      requestFallback(plan.kind);
      throw e;
    }
  }

  private synchronized void requestFallback(PackerKind kind) {
    FallbackMode newMode =
        kind == PackerKind.DTS_HD ? FallbackMode.DTS_CORE_RAW : FallbackMode.RAW_PASSTHROUGH;
    @Nullable FallbackMode existingMode = fallbackModes.get(kind);
    if (existingMode == newMode) {
      return;
    }
    fallbackModes.put(kind, newMode);
    Log.w(TAG, "Recoverable IEC failure, enabling fallback " + newMode + " for " + kind);
  }

  @Nullable
  private CarrierPlan maybeCreateCarrierPlan(Format format, OutputConfig passthroughConfig) {
    @Nullable String sampleMimeType = format.sampleMimeType;
    if (sampleMimeType == null || passthroughConfig.isOffload || passthroughConfig.isTunneling) {
      return null;
    }
    if (MimeTypes.AUDIO_DTS.equals(sampleMimeType)) {
      return new CarrierPlan(PackerKind.DTS_CORE, new DtsCoreIecPacker(), passthroughConfig);
    }
    if (MimeTypes.AUDIO_DTS_HD.equals(sampleMimeType)
        || MimeTypes.AUDIO_DTS_X.equals(sampleMimeType)
        || MimeTypes.AUDIO_DTS_EXPRESS.equals(sampleMimeType)) {
      return new CarrierPlan(
          PackerKind.DTS_HD,
          new DtsHdIecPacker(
              format.sampleRate != Format.NO_VALUE ? format.sampleRate : 48_000,
              format.channelCount > 2),
          passthroughConfig);
    }
    if (MimeTypes.AUDIO_TRUEHD.equals(sampleMimeType)) {
      return new CarrierPlan(
          PackerKind.TRUEHD, new TrueHdIecPacker(format.channelCount > 2), passthroughConfig);
    }
    return null;
  }

  private static void overrideToIec61937(AudioTrack.Builder builder, OutputConfig config) {
    AudioFormat iecFormat =
        new AudioFormat.Builder()
            .setSampleRate(config.sampleRate)
            .setChannelMask(config.channelMask)
            .setEncoding(AudioFormat.ENCODING_IEC61937)
            .build();
    builder.setAudioFormat(iecFormat);
  }

  private static FormatConfig copyFormatConfig(FormatConfig formatConfig, Format format) {
    return new FormatConfig.Builder(format)
        .setAudioAttributes(formatConfig.audioAttributes)
        .setPreferredDevice(formatConfig.preferredDevice)
        .setEnableHighResolutionPcmOutput(formatConfig.enableHighResolutionPcmOutput)
        .setEnablePlaybackParameters(formatConfig.enablePlaybackParameters)
        .setEnableOffload(formatConfig.enableOffload)
        .setAudioSessionId(formatConfig.audioSessionId)
        .setVirtualDeviceId(formatConfig.virtualDeviceId)
        .setEnableTunneling(formatConfig.enableTunneling)
        .setPreferredBufferSize(formatConfig.preferredBufferSize)
        .build();
  }

  private static Format downgradeToDtsCore(Format format) {
    return format.buildUpon().setSampleMimeType(MimeTypes.AUDIO_DTS).setCodecs(null).build();
  }

  @Nullable
  private static PackerKind getPackerKind(Format format) {
    @Nullable String sampleMimeType = format.sampleMimeType;
    if (sampleMimeType == null) {
      return null;
    }
    if (MimeTypes.AUDIO_DTS.equals(sampleMimeType)) {
      return PackerKind.DTS_CORE;
    }
    if (MimeTypes.AUDIO_DTS_HD.equals(sampleMimeType)
        || MimeTypes.AUDIO_DTS_X.equals(sampleMimeType)
        || MimeTypes.AUDIO_DTS_EXPRESS.equals(sampleMimeType)) {
      return PackerKind.DTS_HD;
    }
    if (MimeTypes.AUDIO_TRUEHD.equals(sampleMimeType)) {
      return PackerKind.TRUEHD;
    }
    return null;
  }

  private enum PackerKind {
    DTS_CORE,
    DTS_HD,
    TRUEHD
  }

  private enum FallbackMode {
    RAW_PASSTHROUGH,
    DTS_CORE_RAW
  }

  private static final class CarrierPlan {
    public final PackerKind kind;
    public final IecPacker packer;
    public final OutputConfig passthroughConfig;

    public CarrierPlan(PackerKind kind, IecPacker packer, OutputConfig passthroughConfig) {
      this.kind = kind;
      this.packer = packer;
      this.passthroughConfig = passthroughConfig;
    }
  }

  private interface IecPacker {
    int getCarrierSampleRateHz();

    int getCarrierChannelMask(int passthroughChannelMask);

    int getMaximumPacketSizeBytes();

    ByteBuffer pack(ByteBuffer inputBuffer, int encodedAccessUnitCount);
  }

  private static final class DtsCoreIecPacker implements IecPacker {

    @Override
    public int getCarrierSampleRateHz() {
      return 48_000;
    }

    @Override
    public int getCarrierChannelMask(int passthroughChannelMask) {
      return AudioFormat.CHANNEL_OUT_STEREO;
    }

    @Override
    public int getMaximumPacketSizeBytes() {
      return IEC61937_DTS_CORE_MAX_PACKET_BYTES;
    }

    @Override
    public ByteBuffer pack(ByteBuffer inputBuffer, int encodedAccessUnitCount) {
      byte[] input = toArray(inputBuffer);
      List<FrameSlice> accessUnits = splitDtsAccessUnits(input, encodedAccessUnitCount);
      List<ByteBuffer> packets = new ArrayList<>(accessUnits.size());
      int totalSize = 0;
      for (FrameSlice accessUnit : accessUnits) {
        ByteBuffer packet = packSingleAccessUnit(input, accessUnit.offset, accessUnit.length);
        totalSize += packet.remaining();
        packets.add(packet);
      }
      return joinPackets(packets, totalSize);
    }

    private static ByteBuffer packSingleAccessUnit(byte[] input, int offset, int length) {
      byte[] accessUnit = copyRange(input, offset, length);
      int coreSize = DtsUtil.getDtsFrameSize(accessUnit);
      int sampleCount = DtsUtil.parseDtsAudioSampleCount(accessUnit);
      int dataType = getDtsCoreDataType(sampleCount);
      int packetSize = sampleCount * 4;
      int payloadSize = coreSize > 0 && coreSize < accessUnit.length ? coreSize : accessUnit.length;
      boolean omitPreamble = payloadSize == packetSize;
      ByteBuffer output = allocatePacket(packetSize);
      if (!omitPreamble) {
        putPreamble(output, dataType, payloadSize << 3);
      }
      output.put(accessUnit, 0, payloadSize);
      output.position(packetSize);
      output.flip();
      return output;
    }
  }

  private static final class DtsHdIecPacker implements IecPacker {

    private final int inputSampleRateHz;
    private final boolean keepMultichannelCarrier;

    public DtsHdIecPacker(int inputSampleRateHz, boolean keepMultichannelCarrier) {
      this.inputSampleRateHz = inputSampleRateHz;
      this.keepMultichannelCarrier = keepMultichannelCarrier;
    }

    @Override
    public int getCarrierSampleRateHz() {
      return IEC61937_DTS_HD_CARRIER_RATE_HZ;
    }

    @Override
    public int getCarrierChannelMask(int passthroughChannelMask) {
      return keepMultichannelCarrier ? passthroughChannelMask : AudioFormat.CHANNEL_OUT_STEREO;
    }

    @Override
    public int getMaximumPacketSizeBytes() {
      return IEC61937_DTS_HD_MAX_PACKET_BYTES;
    }

    @Override
    public ByteBuffer pack(ByteBuffer inputBuffer, int encodedAccessUnitCount) {
      byte[] input = toArray(inputBuffer);
      List<FrameSlice> accessUnits = splitDtsAccessUnits(input, encodedAccessUnitCount);
      List<ByteBuffer> packets = new ArrayList<>(accessUnits.size());
      int totalSize = 0;
      for (FrameSlice accessUnit : accessUnits) {
        ByteBuffer packet = packSingleAccessUnit(input, accessUnit.offset, accessUnit.length);
        totalSize += packet.remaining();
        packets.add(packet);
      }
      return joinPackets(packets, totalSize);
    }

    private ByteBuffer packSingleAccessUnit(byte[] input, int offset, int length) {
      byte[] accessUnit = copyRange(input, offset, length);
      int coreSize = DtsUtil.getDtsFrameSize(accessUnit);
      int sampleCount = DtsUtil.parseDtsAudioSampleCount(ByteBuffer.wrap(accessUnit));
      int repetitionPeriod = computeDtsHdRepetitionPeriod(sampleCount, inputSampleRateHz);
      int subtype = getDtsHdSubtype(repetitionPeriod);
      int packetSize = repetitionPeriod * 4;
      int payloadSize = accessUnit.length;
      if (DTS_HD_START_CODE.length + 2 + payloadSize > packetSize - IEC61937_PACKET_HEADER_BYTES
          && coreSize > 0) {
        Log.w(TAG, "DTS-HD burst overflow, sending DTS core for access unit");
        return DtsCoreIecPacker.packSingleAccessUnit(accessUnit, 0, accessUnit.length);
      }
      int outBytes = DTS_HD_START_CODE.length + 2 + payloadSize;
      int lengthCode = alignTo16(outBytes + IEC61937_PACKET_HEADER_BYTES) - IEC61937_PACKET_HEADER_BYTES;
      ByteBuffer output = allocatePacket(packetSize);
      putPreamble(output, IEC61937_DTSHD | (subtype << 8), lengthCode);
      output.put(DTS_HD_START_CODE);
      output.putShort(swapToBigEndianShort((short) payloadSize));
      output.put(accessUnit);
      output.position(packetSize);
      output.flip();
      return output;
    }
  }

  private static final class TrueHdIecPacker implements IecPacker {

    private final boolean keepMultichannelCarrier;
    private final TrueHdMatState state;
    private final ArrayDeque<byte[]> outputQueue;
    private byte[] matBuffer;
    private int matBufferCount;

    public TrueHdIecPacker(boolean keepMultichannelCarrier) {
      this.keepMultichannelCarrier = keepMultichannelCarrier;
      this.state = new TrueHdMatState();
      this.outputQueue = new ArrayDeque<>();
      this.matBuffer = new byte[0];
      this.matBufferCount = 0;
    }

    @Override
    public int getCarrierSampleRateHz() {
      return IEC61937_TRUEHD_CARRIER_RATE_HZ;
    }

    @Override
    public int getCarrierChannelMask(int passthroughChannelMask) {
      return keepMultichannelCarrier ? passthroughChannelMask : AudioFormat.CHANNEL_OUT_STEREO;
    }

    @Override
    public int getMaximumPacketSizeBytes() {
      return IEC61937_TRUEHD_PACKET_BYTES;
    }

    @Override
    public ByteBuffer pack(ByteBuffer inputBuffer, int encodedAccessUnitCount) {
      byte[] accessUnit = toArray(inputBuffer);
      if (encodedAccessUnitCount != Ac3Util.TRUEHD_RECHUNK_SAMPLE_COUNT) {
        Log.w(
            TAG,
            "Unexpected TrueHD access unit count="
                + encodedAccessUnitCount
                + ", expected="
                + Ac3Util.TRUEHD_RECHUNK_SAMPLE_COUNT);
      }
      packTrueHd(accessUnit);
      return drainOutputFrames();
    }

    private void packTrueHd(byte[] accessUnit) {
      if (accessUnit.length < 10) {
        return;
      }

      if (readBigEndianInt(accessUnit, 4) == TRUEHD_FORMAT_MAJOR_SYNC) {
        state.rateBits = (accessUnit[8] >>> 4) & 0x0F;
      } else if (!state.previousFrameTimeValid) {
        return;
      }

      int frameTime = readUnsignedShort(accessUnit, 2);
      int spaceSize = 0;
      if (state.previousFrameTimeValid) {
        int frameTimeDelta = (frameTime - state.previousFrameTime) & 0xFFFF;
        spaceSize = frameTimeDelta * (64 >> (state.rateBits & 7));
      }

      if (state.previousFrameTimeValid && spaceSize < state.previousMatFrameSize) {
        int alignment = 64 >> (state.rateBits & 7);
        spaceSize = align(state.previousMatFrameSize, alignment);
      }

      state.padding += spaceSize - state.previousMatFrameSize;
      if (state.padding > IEC61937_TRUEHD_PACKET_BYTES * 5) {
        Log.i(TAG, "TrueHD MAT packer seek detected, resetting state");
        state.resetAfterSeek();
        outputQueue.clear();
        matBuffer = new byte[0];
        matBufferCount = 0;
        return;
      }

      state.previousFrameTime = frameTime;
      state.previousFrameTimeValid = true;

      if (matBufferCount == 0) {
        writeMatHeader();
        if (!state.initialized) {
          state.initialized = true;
          state.currentMatFrameSize = 0;
        }
      }

      while (state.padding > 0) {
        writePadding();
        if (state.padding == 0 && matBufferCount != IEC61937_TRUEHD_PACKET_BYTES) {
          break;
        }
        if (matBufferCount == IEC61937_TRUEHD_PACKET_BYTES) {
          flushMatPacket();
          writeMatHeader();
        }
      }

      int remaining = fillMatBuffer(accessUnit, 0, accessUnit.length, TrueHdFillType.DATA);
      if (remaining != 0 || matBufferCount == IEC61937_TRUEHD_PACKET_BYTES) {
        flushMatPacket();
        if (remaining > 0) {
          writeMatHeader();
          int written = accessUnit.length - remaining;
          remaining = fillMatBuffer(accessUnit, written, remaining, TrueHdFillType.DATA);
          checkState(remaining == 0);
        }
      }

      state.previousMatFrameSize = state.currentMatFrameSize;
      state.currentMatFrameSize = 0;
    }

    private ByteBuffer drainOutputFrames() {
      if (outputQueue.isEmpty()) {
        return EMPTY_BUFFER;
      }
      int totalBytes = outputQueue.size() * IEC61937_TRUEHD_PACKET_BYTES;
      ByteBuffer output = allocatePacket(totalBytes);
      while (!outputQueue.isEmpty()) {
        output.put(checkNotNull(outputQueue.removeFirst()));
      }
      output.flip();
      return output;
    }

    private void writeMatHeader() {
      matBuffer = new byte[IEC61937_TRUEHD_PACKET_BYTES];
      int size = IEC61937_PACKET_HEADER_BYTES + TRUEHD_MAT_START_CODE.length;
      System.arraycopy(
          TRUEHD_MAT_START_CODE,
          0,
          matBuffer,
          IEC61937_PACKET_HEADER_BYTES,
          TRUEHD_MAT_START_CODE.length);
      matBufferCount = size;
      state.currentMatFrameSize += size;
      if (state.padding > 0) {
        if (state.padding > size) {
          state.padding -= size;
          state.currentMatFrameSize = 0;
        } else {
          state.currentMatFrameSize = size - state.padding;
          state.padding = 0;
        }
      }
    }

    private void writePadding() {
      if (state.padding <= 0) {
        return;
      }
      int remaining = fillMatBuffer(null, 0, state.padding, TrueHdFillType.PADDING);
      if (remaining >= 0) {
        state.padding = remaining;
        state.currentMatFrameSize = 0;
      } else {
        state.padding = 0;
        state.currentMatFrameSize = -remaining;
      }
    }

    private int fillMatBuffer(
        @Nullable byte[] data, int dataOffset, int size, TrueHdFillType fillType) {
      if (matBufferCount >= TRUEHD_MAT_BUFFER_LIMIT) {
        return size;
      }

      int remaining = size;
      if (matBufferCount <= TRUEHD_MAT_MIDDLE_POSITION
          && matBufferCount + size > TRUEHD_MAT_MIDDLE_POSITION) {
        int bytesBefore = TRUEHD_MAT_MIDDLE_POSITION - matBufferCount;
        appendMatData(data, dataOffset, bytesBefore, fillType);
        remaining -= bytesBefore;
        appendMatData(
            TRUEHD_MAT_MIDDLE_CODE, 0, TRUEHD_MAT_MIDDLE_CODE.length, TrueHdFillType.DATA);
        if (fillType == TrueHdFillType.PADDING) {
          remaining -= TRUEHD_MAT_MIDDLE_CODE.length;
        }
        if (remaining > 0) {
          int nextOffset = fillType == TrueHdFillType.DATA ? dataOffset + bytesBefore : dataOffset;
          remaining = fillMatBuffer(data, nextOffset, remaining, fillType);
        }
        return remaining;
      }

      if (matBufferCount + size >= TRUEHD_MAT_BUFFER_LIMIT) {
        int bytesBefore = TRUEHD_MAT_BUFFER_LIMIT - matBufferCount;
        appendMatData(data, dataOffset, bytesBefore, fillType);
        remaining -= bytesBefore;
        appendMatData(TRUEHD_MAT_END_CODE, 0, TRUEHD_MAT_END_CODE.length, TrueHdFillType.DATA);
        checkState(matBufferCount == IEC61937_TRUEHD_PACKET_BYTES);
        if (fillType == TrueHdFillType.PADDING) {
          remaining -= TRUEHD_MAT_END_CODE.length;
        }
        return remaining;
      }

      appendMatData(data, dataOffset, size, fillType);
      return 0;
    }

    private void appendMatData(
        @Nullable byte[] data, int dataOffset, int size, TrueHdFillType fillType) {
      if (size <= 0) {
        return;
      }
      if (fillType == TrueHdFillType.DATA) {
        checkNotNull(data);
        System.arraycopy(data, dataOffset, matBuffer, matBufferCount, size);
      }
      state.currentMatFrameSize += size;
      matBufferCount += size;
    }

    private void flushMatPacket() {
      if (matBufferCount == 0) {
        return;
      }
      checkState(matBufferCount == IEC61937_TRUEHD_PACKET_BYTES);
      writePreambleToByteArray(matBuffer, IEC61937_TRUEHD, IEC61937_TRUEHD_LENGTH_CODE);
      outputQueue.addLast(matBuffer);
      matBuffer = new byte[0];
      matBufferCount = 0;
    }
  }

  private enum TrueHdFillType {
    DATA,
    PADDING
  }

  private static final class TrueHdMatState {
    public boolean initialized;
    public int rateBits;
    public int previousFrameTime;
    public boolean previousFrameTimeValid;
    public int previousMatFrameSize;
    public int currentMatFrameSize;
    public int padding;

    public void resetAfterSeek() {
      initialized = true;
      rateBits = 0;
      previousFrameTime = 0;
      previousFrameTimeValid = false;
      previousMatFrameSize = 0;
      currentMatFrameSize = 0;
      padding = 0;
    }
  }

  private static final class FireOsIec61937AudioOutput extends ForwardingAudioOutput {

    private final PackerKind kind;
    private final IecPacker packer;
    private final FireOsIec61937AudioOutputProvider provider;
    @Nullable private ByteBuffer pendingInputBuffer;
    @Nullable private ByteBuffer pendingPackedBuffer;
    private int pendingAccessUnitCount;

    public FireOsIec61937AudioOutput(
        AudioOutput audioOutput,
        PackerKind kind,
        IecPacker packer,
        FireOsIec61937AudioOutputProvider provider) {
      super(audioOutput);
      this.kind = kind;
      this.packer = packer;
      this.provider = provider;
    }

    @Override
    public boolean write(ByteBuffer buffer, int encodedAccessUnitCount, long presentationTimeUs)
        throws WriteException {
      if (pendingInputBuffer == null) {
        pendingInputBuffer = buffer;
        pendingAccessUnitCount = encodedAccessUnitCount;
        try {
          pendingPackedBuffer = packer.pack(buffer.duplicate(), encodedAccessUnitCount);
        } catch (RuntimeException e) {
          provider.requestFallback(kind);
          throw new WriteException(AudioTrack.ERROR_BAD_VALUE, /* isRecoverable= */ true);
        }
      } else {
        checkState(pendingInputBuffer == buffer);
        checkState(pendingAccessUnitCount == encodedAccessUnitCount);
      }
      if (!checkNotNull(pendingPackedBuffer).hasRemaining()) {
        buffer.position(buffer.limit());
        clearPendingState();
        return true;
      }
      try {
        boolean fullyHandled =
            super.write(
                checkNotNull(pendingPackedBuffer),
                /* encodedAccessUnitCount= */ 1,
                presentationTimeUs);
        if (fullyHandled) {
          buffer.position(buffer.limit());
          clearPendingState();
        }
        return fullyHandled;
      } catch (WriteException e) {
        if (e.isRecoverable) {
          provider.requestFallback(kind);
        }
        throw e;
      }
    }

    @Override
    public void flush() {
      clearPendingState();
      super.flush();
    }

    private void clearPendingState() {
      pendingInputBuffer = null;
      pendingPackedBuffer = null;
      pendingAccessUnitCount = 0;
    }
  }

  private static int getDtsCoreDataType(int sampleCount) {
    switch (sampleCount) {
      case 512:
        return IEC61937_DTS1;
      case 1024:
        return IEC61937_DTS2;
      case 2048:
        return IEC61937_DTS3;
      default:
        throw new IllegalArgumentException("Unsupported DTS core sample count: " + sampleCount);
    }
  }

  private static int computeDtsHdRepetitionPeriod(int sampleCount, int inputSampleRateHz) {
    int period = IEC61937_DTS_HD_CARRIER_RATE_HZ * sampleCount / inputSampleRateHz;
    switch (period) {
      case 512:
      case 1024:
      case 2048:
      case 4096:
      case 8192:
      case 16384:
        return period;
      default:
        throw new IllegalArgumentException(
            "Unsupported DTS-HD repetition period: " + period + " sampleCount=" + sampleCount);
    }
  }

  private static int getDtsHdSubtype(int repetitionPeriod) {
    switch (repetitionPeriod) {
      case 512:
        return 0x0;
      case 1024:
        return 0x1;
      case 2048:
        return 0x2;
      case 4096:
        return 0x3;
      case 8192:
        return 0x4;
      case 16384:
        return 0x5;
      default:
        throw new IllegalArgumentException("Unsupported DTS-HD repetition period: " + repetitionPeriod);
    }
  }

  private static List<FrameSlice> splitDtsAccessUnits(byte[] input, int encodedAccessUnitCount) {
    ArrayList<FrameSlice> accessUnits = new ArrayList<>();
    int offset = 0;
    for (int i = 0; i < encodedAccessUnitCount && offset < input.length; i++) {
      int size = parseDtsAccessUnitSize(input, offset, input.length - offset);
      if (size <= 0 || offset + size > input.length) {
        break;
      }
      accessUnits.add(new FrameSlice(offset, size));
      offset += size;
    }
    if (accessUnits.isEmpty() || offset != input.length) {
      accessUnits.clear();
      accessUnits.add(new FrameSlice(0, input.length));
      Log.w(
          TAG,
          "Falling back to whole-buffer DTS packing"
              + " accessUnits="
              + encodedAccessUnitCount
              + " parsedBytes="
              + offset
              + "/"
              + input.length);
    }
    return accessUnits;
  }

  private static int parseDtsAccessUnitSize(byte[] input, int offset, int bytesRemaining) {
    if (bytesRemaining < 4) {
      return C.LENGTH_UNSET;
    }
    int word = Util.getBigEndianInt(ByteBuffer.wrap(input, offset, 4), 0);
    @DtsUtil.FrameType int frameType = DtsUtil.getFrameType(word);
    switch (frameType) {
      case DtsUtil.FRAME_TYPE_CORE:
        int coreSize = DtsUtil.getDtsFrameSize(copyRange(input, offset, min(bytesRemaining, 16)));
        if (coreSize <= 0 || coreSize > bytesRemaining) {
          return C.LENGTH_UNSET;
        }
        int totalSize = coreSize;
        int nextOffset = offset + coreSize;
        int remainingAfterCore = bytesRemaining - coreSize;
        if (remainingAfterCore >= 4) {
          int nextWord = Util.getBigEndianInt(ByteBuffer.wrap(input, nextOffset, 4), 0);
          @DtsUtil.FrameType int nextType = DtsUtil.getFrameType(nextWord);
          if (nextType == DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM) {
            int extensionSize = parseDtsHdFrameSize(input, nextOffset, remainingAfterCore);
            if (extensionSize > 0) {
              totalSize += extensionSize;
            }
          } else if (nextType == DtsUtil.FRAME_TYPE_UHD_SYNC
              || nextType == DtsUtil.FRAME_TYPE_UHD_NON_SYNC) {
            int uhdSize = parseDtsUhdFrameSize(input, nextOffset, remainingAfterCore);
            if (uhdSize > 0) {
              totalSize += uhdSize;
            }
          }
        }
        return totalSize;
      case DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM:
        return parseDtsHdFrameSize(input, offset, bytesRemaining);
      case DtsUtil.FRAME_TYPE_UHD_SYNC:
      case DtsUtil.FRAME_TYPE_UHD_NON_SYNC:
        return parseDtsUhdFrameSize(input, offset, bytesRemaining);
      default:
        return C.LENGTH_UNSET;
    }
  }

  private static int parseDtsHdFrameSize(byte[] input, int offset, int bytesRemaining) {
    try {
      byte[] headerPrefix = copyRange(input, offset, min(bytesRemaining, 32));
      int headerSize = DtsUtil.parseDtsHdHeaderSize(headerPrefix);
      if (headerSize <= 0 || headerSize > bytesRemaining) {
        return C.LENGTH_UNSET;
      }
      return DtsUtil.parseDtsHdHeader(copyRange(input, offset, headerSize)).frameSize;
    } catch (ParserException | IllegalArgumentException e) {
      Log.w(TAG, "Unable to parse DTS-HD frame size", e);
      return C.LENGTH_UNSET;
    }
  }

  private static int parseDtsUhdFrameSize(byte[] input, int offset, int bytesRemaining) {
    try {
      byte[] headerPrefix = copyRange(input, offset, min(bytesRemaining, 32));
      int headerSize = DtsUtil.parseDtsUhdHeaderSize(headerPrefix);
      if (headerSize <= 0 || headerSize > bytesRemaining) {
        return C.LENGTH_UNSET;
      }
      return DtsUtil.parseDtsUhdHeader(copyRange(input, offset, headerSize), new AtomicInteger())
          .frameSize;
    } catch (ParserException | IllegalArgumentException e) {
      Log.w(TAG, "Unable to parse DTS-UHD frame size", e);
      return C.LENGTH_UNSET;
    }
  }

  private static ByteBuffer joinPackets(List<ByteBuffer> packets, int totalSize) {
    ByteBuffer output = allocatePacket(totalSize);
    for (ByteBuffer packet : packets) {
      output.put(packet);
    }
    output.flip();
    return output;
  }

  private static ByteBuffer allocatePacket(int packetSize) {
    return ByteBuffer.allocateDirect(packetSize).order(ByteOrder.LITTLE_ENDIAN);
  }

  private static void putPreamble(ByteBuffer output, int dataType, int lengthCode) {
    output.putShort((short) IEC61937_PACKET_SYNC_WORD_1);
    output.putShort((short) IEC61937_PACKET_SYNC_WORD_2);
    output.putShort((short) dataType);
    output.putShort((short) lengthCode);
  }

  private static void writePreambleToByteArray(byte[] output, int dataType, int lengthCode) {
    writeLittleEndianShort(output, 0, IEC61937_PACKET_SYNC_WORD_1);
    writeLittleEndianShort(output, 2, IEC61937_PACKET_SYNC_WORD_2);
    writeLittleEndianShort(output, 4, dataType);
    writeLittleEndianShort(output, 6, lengthCode);
  }

  private static int alignTo16(int value) {
    return (value + 15) & ~15;
  }

  private static int align(int value, int alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
  }

  private static byte[] copyRange(byte[] source, int offset, int length) {
    byte[] copy = new byte[length];
    System.arraycopy(source, offset, copy, 0, length);
    return copy;
  }

  private static byte[] toArray(ByteBuffer buffer) {
    ByteBuffer duplicate = buffer.duplicate();
    byte[] data = new byte[duplicate.remaining()];
    duplicate.get(data);
    return data;
  }

  @SuppressLint("WrongConstant")
  private static short swapToBigEndianShort(short value) {
    return (short) (((value & 0xFF) << 8) | ((value >>> 8) & 0xFF));
  }

  private static void writeLittleEndianShort(byte[] target, int offset, int value) {
    target[offset] = (byte) (value & 0xFF);
    target[offset + 1] = (byte) ((value >>> 8) & 0xFF);
  }

  private static int readUnsignedShort(byte[] source, int offset) {
    return ((source[offset] & 0xFF) << 8) | (source[offset + 1] & 0xFF);
  }

  private static int readBigEndianInt(byte[] source, int offset) {
    return ((source[offset] & 0xFF) << 24)
        | ((source[offset + 1] & 0xFF) << 16)
        | ((source[offset + 2] & 0xFF) << 8)
        | (source[offset + 3] & 0xFF);
  }

  private static int min(int left, int right) {
    return left < right ? left : right;
  }

  private static final ByteBuffer EMPTY_BUFFER = ByteBuffer.allocateDirect(0).order(ByteOrder.LITTLE_ENDIAN);

  private static final class FrameSlice {
    public final int offset;
    public final int length;

    public FrameSlice(int offset, int length) {
      this.offset = offset;
      this.length = length;
    }
  }
}
