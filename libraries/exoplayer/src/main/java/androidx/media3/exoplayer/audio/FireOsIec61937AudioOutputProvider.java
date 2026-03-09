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
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.SystemClock;
import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.ParserException;
import androidx.media3.common.util.AmazonQuirks;
import androidx.media3.common.util.Log;
import androidx.media3.common.util.ParsableBitArray;
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
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Fire OS experimental IEC61937 passthrough provider that packs encoded access units before
 * handing them to {@link AudioTrack}.
 */
@UnstableApi
public final class FireOsIec61937AudioOutputProvider extends ForwardingAudioOutputProvider {

  private static final String TAG = "FireOsIecProvider";

  private static final int IEC61937_PACKET_HEADER_BYTES = 8;
  private static final int FIRE_OS_OUTPUT_RETRY_DELAY_MS = 200;
  private static final int FIRE_OS_SMALLER_BUFFER_RETRY_SIZE = 1_000_000;
  private static final int SUPERVISE_AUDIO_DELAY_MIN_STUCK_MS = 400;
  private static final int SUPERVISE_AUDIO_DELAY_MAX_BUFFER_MULTIPLIER = 2;
  private static final long SUPERVISE_AUDIO_DELAY_POSITION_TOLERANCE_US = 2_000L;
  private static final int SUPERVISE_AUDIO_DELAY_REOPEN_ERROR = AudioTrack.ERROR_INVALID_OPERATION;
  private static final int IEC61937_PACKET_SYNC_WORD_1 = 0xF872;
  private static final int IEC61937_PACKET_SYNC_WORD_2 = 0x4E1F;
  private static final int IEC61937_AC3 = 0x01;
  private static final int IEC61937_DTS1 = 0x0B;
  private static final int IEC61937_DTS2 = 0x0C;
  private static final int IEC61937_DTS3 = 0x0D;
  private static final int IEC61937_DTSHD = 0x11;
  private static final int IEC61937_E_AC3 = 0x15;
  private static final int IEC61937_TRUEHD = 0x16;
  private static final int IEC61937_AC3_PACKET_BYTES = 6_144;
  private static final int IEC61937_E_AC3_PACKET_BYTES = 24_576;
  private static final int IEC61937_DTS_HD_CARRIER_RATE_HZ = 192_000;
  private static final int IEC61937_TRUEHD_PACKET_BYTES = 61_440;
  private static final int IEC61937_TRUEHD_LENGTH_CODE = 61_424;
  private static final int IEC61937_DTS_CORE_MAX_PACKET_BYTES = 8_192;
  private static final int IEC61937_DTS_HD_MAX_PACKET_BYTES = 65_536;
  private static final int IEC61937_MULTICHANNEL_RAW_CARRIER_MASK =
      AudioFormat.CHANNEL_OUT_7POINT1_SURROUND;
  private static final int SYNTHETIC_PASSTHROUGH_BUFFER_SIZE_BYTES = 256 * 1024;
  private static final int TRUEHD_FORMAT_MAJOR_SYNC = 0xF8726FBA;
  private static final int MAX_AC3_PENDING_BYTES = 8 * 1024;
  private static final int MAX_DTS_PENDING_BYTES = 128 * 1024;
  private static final int MAX_TRUEHD_PENDING_BYTES = 128 * 1024;
  private static final int MAX_AC3_HEADER_SEARCH_BYTES = 64;
  private static final int MAX_DTS_HEADER_SEARCH_BYTES = 64;
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
  private static final byte DTS_SYNC_FIRST_BYTE_BE = 0x7F;
  private static final byte DTS_SYNC_FIRST_BYTE_14B_BE = 0x1F;
  private static final byte DTS_SYNC_FIRST_BYTE_LE = (byte) 0xFE;
  private static final byte DTS_SYNC_FIRST_BYTE_14B_LE = (byte) 0xFF;
  private static final int[] TRUEHD_CRC_TABLE = buildTrueHdCrcTable();
  private static final int[] TRUEHD_CHANNEL_MAP_WEIGHTS =
      new int[] {2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};

  private final AudioOutputProvider passthroughProvider;
  private final AudioOutputProvider iecCarrierProvider;
  private final Map<OutputConfig, CarrierPlan> carrierPlans;
  private final Map<IecProbeConfig, Boolean> iecSupportCache;
  private final Map<EncodedProbeConfig, Boolean> encodedSupportCache;
  private final Map<Integer, CapabilityMatrix> capabilityMatrices;
  private final EnumMap<PackerKind, FallbackMode> fallbackModes;
  private final Map<ByteBuffer, PreparedEncodedPacket> preparedPackets;
  private final Ac3SyncParserState ac3SyncParserState;
  private final DtsSyncParserState dtsSyncParserState;
  private final TrueHdSyncParserState trueHdSyncParserState;
  @Nullable private FireOsStreamInfo activeStreamInfo;

  public FireOsIec61937AudioOutputProvider(Context context) {
    this(
        new AudioTrackAudioOutputProvider.Builder(context).build(),
        new AudioTrackAudioOutputProvider.Builder(context)
            .setAudioTrackProvider(new Iec61937AudioTrackProvider())
            .build());
  }

  @VisibleForTesting
  /* package */ FireOsIec61937AudioOutputProvider(
      AudioOutputProvider passthroughProvider, AudioOutputProvider iecCarrierProvider) {
    super(passthroughProvider);
    this.passthroughProvider = passthroughProvider;
    this.iecCarrierProvider = iecCarrierProvider;
    this.carrierPlans = new HashMap<>();
    this.iecSupportCache = new HashMap<>();
    this.encodedSupportCache = new HashMap<>();
    this.capabilityMatrices = new HashMap<>();
    this.fallbackModes = new EnumMap<>(PackerKind.class);
    this.preparedPackets = new IdentityHashMap<>();
    this.ac3SyncParserState = new Ac3SyncParserState();
    this.dtsSyncParserState = new DtsSyncParserState();
    this.trueHdSyncParserState = new TrueHdSyncParserState();
    this.activeStreamInfo = null;
  }

  @Override
  public FormatSupport getFormatSupport(FormatConfig formatConfig) {
    boolean useKodiFireOsIecCapabilityModel = shouldUseKodiFireOsIecCapabilityModel();
    @Nullable FireOsStreamInfo decisionStreamInfo =
        getStreamInfoForPassthroughDecision(formatConfig.format);
    @Nullable PackerKind kind = getDecisionPackerKind(decisionStreamInfo);
    @Nullable FormatSupport passthroughSupport = null;
    if (kind == null) {
      if (useKodiFireOsIecCapabilityModel) {
        logIecDebug(
            "getFormatSupport rejected (classification unavailable)"
                + " kodiModel=true format={"
                + describeFormat(formatConfig.format)
                + "}");
        return FormatSupport.UNSUPPORTED;
      }
      passthroughSupport = passthroughProvider.getFormatSupport(formatConfig);
      return passthroughSupport;
    }
    @Nullable FallbackMode fallbackMode = fallbackModes.get(kind);
    if (fallbackMode == FallbackMode.STRICT_FAILURE) {
      return FormatSupport.UNSUPPORTED;
    }
    OutputConfig passthroughConfig =
        buildLogicalPassthroughOutputConfig(
            formatConfig,
            kind,
            /* preferWrappedConfig= */ false,
            decisionStreamInfo);
    @Nullable CarrierPlan carrierPlan =
        maybeCreateCarrierPlan(formatConfig.format, passthroughConfig, decisionStreamInfo);
    logIecDebug(
        "getFormatSupport kind="
            + kind
            + " kodiModel="
            + useKodiFireOsIecCapabilityModel
            + " format={"
            + describeFormat(formatConfig.format)
            + "}"
            + " streamInfo="
            + (decisionStreamInfo != null ? decisionStreamInfo.summary() : "null")
            + " carrier="
            + (carrierPlan != null ? describeOutputConfig(carrierPlan.carrierConfig) : "null")
            + " passthroughLevel="
            + (passthroughSupport != null ? passthroughSupport.supportLevel : "n/a"));
    if (carrierPlan == null) {
      if (useKodiFireOsIecCapabilityModel) {
        return FormatSupport.UNSUPPORTED;
      }
      if (passthroughSupport == null) {
        passthroughSupport = passthroughProvider.getFormatSupport(formatConfig);
      }
      return passthroughSupport;
    }
    return new FormatSupport.Builder()
        .setFormatSupportLevel(FORMAT_SUPPORTED_DIRECTLY)
        .setIsFormatSupportedForOffload(false)
        .setIsGaplessSupportedForOffload(false)
        .setIsSpeedChangeSupportedForOffload(false)
        .build();
  }

  @Override
  public OutputConfig getOutputConfig(FormatConfig formatConfig) throws ConfigurationException {
    @Nullable FireOsStreamInfo decisionStreamInfo =
        getStreamInfoForPassthroughDecision(formatConfig.format);
    @Nullable PackerKind kind = getDecisionPackerKind(decisionStreamInfo);
    if (kind == null && shouldUseKodiFireOsIecCapabilityModel()) {
      throw new ConfigurationException(
          "Strict Fire OS IEC mode requires parser-derived stream classification before config");
    }
    if (kind != null) {
      @Nullable FallbackMode fallbackMode = fallbackModes.get(kind);
      if (shouldUseKodiFireOsIecCapabilityModel()
          && fallbackMode != null
          && fallbackMode != FallbackMode.STRICT_FAILURE) {
        fallbackModes.remove(kind);
        fallbackMode = null;
      }
      if (fallbackMode == FallbackMode.STRICT_FAILURE) {
        throw new ConfigurationException(
            "Strict Fire OS IEC mode rejected recoverable fallback for " + kind);
      }
      if (fallbackMode == FallbackMode.RAW_PASSTHROUGH) {
        Log.w(TAG, "IEC packer disabled after recoverable failure for " + kind);
        return getPassthroughOutputConfigWithFireOsQuirks(formatConfig);
      }
      if (fallbackMode == FallbackMode.DTS_CORE_RAW) {
        Log.w(TAG, "Falling back to raw DTS core after IEC failure");
        return getPassthroughOutputConfigWithFireOsQuirks(
            copyFormatConfig(formatConfig, downgradeToDtsCore(formatConfig.format)));
      }
    }

    boolean requireKodiIecCarrierPlan =
        kind != null && shouldUseKodiFireOsIecCapabilityModel();
    OutputConfig passthroughConfig;
    boolean usedSyntheticLogicalConfig = false;
    if (requireKodiIecCarrierPlan) {
      passthroughConfig =
          buildLogicalPassthroughOutputConfig(
              formatConfig,
              kind,
              /* preferWrappedConfig= */ false,
              decisionStreamInfo);
      usedSyntheticLogicalConfig = true;
    } else {
      try {
        passthroughConfig = getPassthroughOutputConfigWithFireOsQuirks(formatConfig);
      } catch (ConfigurationException e) {
        if (kind == null) {
          throw e;
        }
        Log.w(
            TAG,
            "Falling back to synthetic logical passthrough config for "
                + formatConfig.format.sampleMimeType,
            e);
        passthroughConfig =
            buildLogicalPassthroughOutputConfig(
                formatConfig,
                kind,
                /* preferWrappedConfig= */ false,
                decisionStreamInfo);
        usedSyntheticLogicalConfig = true;
      }
    }
    @Nullable CarrierPlan plan =
        maybeCreateCarrierPlan(formatConfig.format, passthroughConfig, decisionStreamInfo);
    logIecDebug(
        "getOutputConfig kind="
            + kind
            + " kodiModel="
            + requireKodiIecCarrierPlan
            + " format={"
            + describeFormat(formatConfig.format)
            + "}"
            + " streamInfo="
            + (decisionStreamInfo != null ? decisionStreamInfo.summary() : "null")
            + " passthrough="
            + describeOutputConfig(passthroughConfig)
            + " carrier="
            + (plan != null ? describeOutputConfig(plan.carrierConfig) : "null")
            + " synthetic="
            + usedSyntheticLogicalConfig);
    if (plan == null) {
      if (requireKodiIecCarrierPlan || usedSyntheticLogicalConfig) {
        throw new ConfigurationException(
            "Unable to configure Fire OS IEC carrier for: " + formatConfig.format);
      }
      return passthroughConfig;
    }
    carrierPlans.put(passthroughConfig, plan);
    Log.i(
        TAG,
        "Using Fire OS IEC carrier"
            + " kind="
            + plan.kind
            + " mime="
            + formatConfig.format.sampleMimeType
            + " carrierRate="
            + plan.carrierConfig.sampleRate
            + " channelMask="
            + plan.carrierConfig.channelMask
            + " bufferSize="
            + plan.carrierConfig.bufferSize);
    return passthroughConfig;
  }

  @Override
  public AudioOutput getAudioOutput(OutputConfig config) throws InitializationException {
    @Nullable CarrierPlan plan = carrierPlans.get(config);
    if (plan == null) {
      return getAudioOutputWithRetry(passthroughProvider, config);
    }
    try {
      AudioOutput carrierOutput = getAudioOutputWithRetry(iecCarrierProvider, plan.carrierConfig);
      return new FireOsIec61937AudioOutput(
          carrierOutput, plan.kind, plan.normalizer, plan.packer, this);
    } catch (InitializationException e) {
      @Nullable CarrierPlan fallbackPlan =
          shouldAllowFireOsCompatibilityFallback() && !shouldUseKodiFireOsIecCapabilityModel()
              ? maybeCreateStereoCarrierFallback(plan)
              : null;
      if (fallbackPlan != null) {
        carrierPlans.put(config, fallbackPlan);
        try {
          AudioOutput carrierOutput =
              getAudioOutputWithRetry(iecCarrierProvider, fallbackPlan.carrierConfig);
          requestFallback(
              plan.kind,
              /* multichannelCarrierFailed= */ true,
              plan.allowStereoIecFallback);
          return new FireOsIec61937AudioOutput(
              carrierOutput,
              fallbackPlan.kind,
              fallbackPlan.normalizer,
              fallbackPlan.packer,
              this);
        } catch (InitializationException ignored) {
          // Fall through to normal fallback handling below.
        }
      }
      requestFallback(plan.kind, plan.packer.usesMultichannelCarrier(), plan.allowStereoIecFallback);
      throw e;
    }
  }

  private synchronized void requestFallback(
      PackerKind kind, boolean multichannelCarrierFailed, boolean allowStereoIecFallback) {
    if (shouldUseKodiFireOsIecCapabilityModel()) {
      logIecDebug(
          "requestFallback ignored in Kodi IEC mode kind="
              + kind
              + " multichannelFailed="
              + multichannelCarrierFailed
              + " allowStereo="
              + allowStereoIecFallback);
      return;
    }
    FallbackMode newMode;
    if (multichannelCarrierFailed && allowStereoIecFallback) {
      newMode = FallbackMode.STEREO_IEC;
    } else if (kind == PackerKind.DTS_HD
        && AmazonQuirks.shouldForceLimitedFireTvDtsCoreFallback()) {
      newMode = FallbackMode.DTS_CORE_RAW;
    } else if ((kind == PackerKind.AC3 || kind == PackerKind.E_AC3)
        && !supportsRawPassthrough(kind)) {
      newMode = FallbackMode.STEREO_IEC;
    } else {
      newMode = FallbackMode.RAW_PASSTHROUGH;
    }
    @Nullable FallbackMode existingMode = fallbackModes.get(kind);
    if (existingMode == newMode
        || existingMode == FallbackMode.STRICT_FAILURE
        || existingMode == FallbackMode.DTS_CORE_RAW
        || existingMode == FallbackMode.RAW_PASSTHROUGH) {
      return;
    }
    if (existingMode == FallbackMode.STEREO_IEC && newMode == FallbackMode.STEREO_IEC) {
      return;
    }
    fallbackModes.put(kind, newMode);
    logIecDebug(
        "requestFallback kind="
            + kind
            + " newMode="
            + newMode
            + " multichannelFailed="
            + multichannelCarrierFailed
            + " allowStereo="
            + allowStereoIecFallback);
    Log.w(TAG, "Recoverable IEC failure, enabling fallback " + newMode + " for " + kind);
  }

  private synchronized boolean allowsStereoIecFallback(PackerKind kind) {
    if (!shouldUseKodiFireOsIecCapabilityModel()) {
      return true;
    }
    if (kind == PackerKind.TRUEHD) {
      return false;
    }
    if (kind == PackerKind.DTS_HD && activeStreamInfo != null) {
      return !activeStreamInfo.requireMultichannelIecCarrier;
    }
    return true;
  }

  @Nullable
  private CarrierPlan maybeCreateCarrierPlan(Format format, OutputConfig passthroughConfig) {
    return maybeCreateCarrierPlan(format, passthroughConfig, getActiveStreamInfo());
  }

  @Nullable
  private CarrierPlan maybeCreateCarrierPlan(
      Format format,
      OutputConfig passthroughConfig,
      @Nullable FireOsStreamInfo streamInfo) {
    if (passthroughConfig.isOffload || passthroughConfig.isTunneling) {
      return null;
    }
    @Nullable PackerKind kind = getDecisionPackerKind(streamInfo);
    if (kind == null) {
      return null;
    }
    @Nullable FallbackMode fallbackMode = kind != null ? fallbackModes.get(kind) : null;
    CapabilityMatrix capabilityMatrix = getCapabilityMatrix(passthroughConfig.channelMask);
    int inputSampleRateHz = resolveInputSampleRateHz(format, streamInfo);
    if (kind == PackerKind.AC3) {
      if (!capabilityMatrix.supportsStereoCarrier(inputSampleRateHz, this)) {
        return null;
      }
      return buildCarrierPlanIfSupported(
          PackerKind.AC3,
          new Ac3BufferNormalizer(
              inputSampleRateHz, /* expectEac3= */ false, ac3SyncParserState),
          new Ac3IecPacker(inputSampleRateHz),
          passthroughConfig,
          streamInfo,
          /* allowStereoIecFallback= */ true);
    }
    if (kind == PackerKind.E_AC3) {
      if (!capabilityMatrix.supportsStereoCarrier(inputSampleRateHz * 4, this)) {
        return null;
      }
      return buildCarrierPlanIfSupported(
          PackerKind.E_AC3,
          new Ac3BufferNormalizer(
              inputSampleRateHz, /* expectEac3= */ true, ac3SyncParserState),
          new EAc3IecPacker(inputSampleRateHz),
          passthroughConfig,
          streamInfo,
          /* allowStereoIecFallback= */ true);
    }
    if (kind == PackerKind.DTS_CORE) {
      if (!capabilityMatrix.supportsStereoCarrier(inputSampleRateHz, this)) {
        return null;
      }
      return buildCarrierPlanIfSupported(
          PackerKind.DTS_CORE,
          new DtsBufferNormalizer(inputSampleRateHz, /* hd= */ false, dtsSyncParserState),
          new DtsCoreIecPacker(inputSampleRateHz),
          passthroughConfig,
          streamInfo,
          /* allowStereoIecFallback= */ true);
    }
    if (kind == PackerKind.DTS_HD) {
      if (shouldUseKodiFireOsIecCapabilityModel() && streamInfo == null) {
        if (!capabilityMatrix.supportsStereoCarrier(inputSampleRateHz, this)) {
          return null;
        }
        return buildCarrierPlanIfSupported(
            PackerKind.DTS_CORE,
            new DtsBufferNormalizer(inputSampleRateHz, /* hd= */ true, dtsSyncParserState),
            new DtsCoreIecPacker(inputSampleRateHz),
            passthroughConfig,
            streamInfo,
            /* allowStereoIecFallback= */ true);
      }
      if (streamInfo != null
          && (streamInfo.dtsStreamType == FireOsStreamInfo.DtsStreamType.DTS_512
              || streamInfo.dtsStreamType == FireOsStreamInfo.DtsStreamType.DTS_1024
              || streamInfo.dtsStreamType == FireOsStreamInfo.DtsStreamType.DTS_2048
              || streamInfo.dtsStreamType == FireOsStreamInfo.DtsStreamType.DTSHD_CORE
              || streamInfo.dtsStreamType == FireOsStreamInfo.DtsStreamType.UNKNOWN
              || streamInfo.dtsStreamType == FireOsStreamInfo.DtsStreamType.NONE)) {
        if (!capabilityMatrix.supportsStereoCarrier(streamInfo.outputRateHz, this)) {
          return null;
        }
        return buildCarrierPlanIfSupported(
            PackerKind.DTS_CORE,
            new DtsBufferNormalizer(inputSampleRateHz, /* hd= */ true, dtsSyncParserState),
            new DtsCoreIecPacker(inputSampleRateHz),
            passthroughConfig,
            streamInfo,
            /* allowStereoIecFallback= */ true);
      }
      boolean requireMultichannelCarrier =
          shouldUseKodiFireOsIecCapabilityModel()
              && streamInfo != null
              && streamInfo.requireMultichannelIecCarrier;
      int dtsCarrierRateHz =
          streamInfo != null && streamInfo.outputRateHz != Format.NO_VALUE
              ? streamInfo.outputRateHz
              : IEC61937_DTS_HD_CARRIER_RATE_HZ;
      boolean keepMultichannelCarrier =
          shouldUseMultichannelDtsHdCarrier(dtsCarrierRateHz, passthroughConfig.channelMask, streamInfo);
      if (fallbackMode == FallbackMode.STEREO_IEC) {
        keepMultichannelCarrier = false;
      }
      if (requireMultichannelCarrier && !keepMultichannelCarrier) {
        return null;
      }
      if (!keepMultichannelCarrier && !capabilityMatrix.supportsStereoCarrier(dtsCarrierRateHz, this)) {
        return null;
      }
      return buildCarrierPlanIfSupported(
          PackerKind.DTS_HD,
          new DtsBufferNormalizer(inputSampleRateHz, /* hd= */ true, dtsSyncParserState),
          new DtsHdIecPacker(
              inputSampleRateHz,
              keepMultichannelCarrier,
              streamInfo != null ? streamInfo.dtsPeriodFrames : C.LENGTH_UNSET),
          passthroughConfig,
          streamInfo,
          /* allowStereoIecFallback= */ !requireMultichannelCarrier);
    }
    if (kind == PackerKind.TRUEHD) {
      int trueHdInputRateHz =
          shouldUseKodiFireOsIecCapabilityModel() ? 48_000 : inputSampleRateHz;
      int carrierRateHz = getTrueHdCarrierRateHz(trueHdInputRateHz);
      boolean requireMultichannelCarrier = shouldUseKodiFireOsIecCapabilityModel();
      boolean keepMultichannelCarrier =
          shouldUseMultichannelTrueHdCarrier(
              carrierRateHz, passthroughConfig.channelMask, streamInfo, requireMultichannelCarrier);
      if (fallbackMode == FallbackMode.STEREO_IEC && !requireMultichannelCarrier) {
        keepMultichannelCarrier = false;
      }
      if (requireMultichannelCarrier && !keepMultichannelCarrier) {
        return null;
      }
      if (!keepMultichannelCarrier && !capabilityMatrix.supportsStereoCarrier(carrierRateHz, this)) {
        return null;
      }
      return buildCarrierPlanIfSupported(
          PackerKind.TRUEHD,
          new TrueHdBufferNormalizer(inputSampleRateHz),
          new TrueHdIecPacker(trueHdInputRateHz, keepMultichannelCarrier),
          passthroughConfig,
          streamInfo,
          /* allowStereoIecFallback= */ !requireMultichannelCarrier);
    }
    return null;
  }

  private boolean shouldUseMultichannelDtsHdCarrier(
      int carrierRateHz,
      int passthroughChannelMask,
      @Nullable FireOsStreamInfo streamInfo) {
    if (streamInfo == null
        || !streamInfo.requireMultichannelIecCarrier
        || passthroughChannelMask == AudioFormat.CHANNEL_OUT_STEREO) {
      return false;
    }
    if (!supportsCarrierPlan(
        carrierRateHz, IEC61937_MULTICHANNEL_RAW_CARRIER_MASK)) {
      Log.w(
          TAG,
          "Falling back to stereo IEC carrier for DTS-HD; multichannel IEC config unsupported"
              + " carrierRate="
              + carrierRateHz
              + " channelMask="
              + IEC61937_MULTICHANNEL_RAW_CARRIER_MASK);
      return false;
    }
    return true;
  }

  private boolean shouldUseMultichannelTrueHdCarrier(
      int carrierRateHz,
      int passthroughChannelMask,
      @Nullable FireOsStreamInfo streamInfo,
      boolean requireMultichannelCarrier) {
    if (streamInfo != null && !streamInfo.requireMultichannelIecCarrier) {
      return false;
    }
    if (passthroughChannelMask == AudioFormat.CHANNEL_OUT_STEREO) {
      return false;
    }
    if (!supportsCarrierPlan(carrierRateHz, IEC61937_MULTICHANNEL_RAW_CARRIER_MASK)) {
      Log.w(
          TAG,
          (requireMultichannelCarrier
                  ? "Rejecting TrueHD IEC carrier; multichannel IEC config unsupported"
                  : "Falling back to stereo IEC carrier for TrueHD; multichannel IEC config unsupported")
              + " sampleRate="
              + carrierRateHz
              + " channelMask="
              + IEC61937_MULTICHANNEL_RAW_CARRIER_MASK);
      return false;
    }
    return true;
  }

  private boolean supportsIec61937Config(int sampleRateHz, int channelMask) {
    IecProbeConfig probeConfig = new IecProbeConfig(sampleRateHz, channelMask);
    @Nullable Boolean cached = iecSupportCache.get(probeConfig);
    if (cached != null) {
      return cached;
    }
    boolean supported = verifyIec61937Track(sampleRateHz, channelMask);
    iecSupportCache.put(probeConfig, supported);
    return supported;
  }

  private boolean supportsEncodedConfig(int sampleRateHz, int channelMask, int encoding) {
    EncodedProbeConfig probeConfig = new EncodedProbeConfig(sampleRateHz, channelMask, encoding);
    @Nullable Boolean cached = encodedSupportCache.get(probeConfig);
    if (cached != null) {
      return cached;
    }
    boolean supported = verifyEncodedTrack(sampleRateHz, channelMask, encoding);
    encodedSupportCache.put(probeConfig, supported);
    return supported;
  }

  private CapabilityMatrix getCapabilityMatrix(int passthroughChannelMask) {
    Integer key = passthroughChannelMask;
    @Nullable CapabilityMatrix cached = capabilityMatrices.get(key);
    if (cached != null) {
      return cached;
    }
    CapabilityMatrix created =
        new CapabilityMatrix(
            supportsIec61937Config(48_000, AudioFormat.CHANNEL_OUT_STEREO),
            supportsIec61937Config(192_000, AudioFormat.CHANNEL_OUT_STEREO),
            supportsEncodedConfig(48_000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_AC3),
            supportsEncodedConfig(
                48_000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_E_AC3),
            supportsIec61937Config(192_000, IEC61937_MULTICHANNEL_RAW_CARRIER_MASK));
    capabilityMatrices.put(key, created);
    Log.i(
        TAG,
        "Fire OS IEC probe matrix"
            + " stereo48="
            + created.stereo48Supported
            + " stereo192="
            + created.stereo192Supported
            + " rawAc3="
            + created.rawAc3Supported
            + " rawEac3="
            + created.rawEac3Supported
            + " multi192="
            + created.multichannel192Supported
            + " channelMask="
            + passthroughChannelMask
            + " rawCarrierMask="
            + IEC61937_MULTICHANNEL_RAW_CARRIER_MASK);
    return created;
  }

  /* package */ @Nullable BufferMetadata inspectEncodedBuffer(
      Format format, ByteBuffer buffer, int encodedAccessUnitCount) {
    @Nullable PreparedEncodedPacket preparedPacket = peekPreparedPacket(buffer);
    if (preparedPacket != null && preparedPacket.encodedAccessUnitCount == encodedAccessUnitCount) {
      return preparedPacket.metadata;
    }
    @Nullable PreparedEncodedPacket freshlyPrepared =
        prepareEncodedPacket(format, buffer, encodedAccessUnitCount);
    return freshlyPrepared != null ? freshlyPrepared.metadata : null;
  }

  /* package */ @Nullable PreparedEncodedPacket prepareEncodedPacket(
      Format format, ByteBuffer buffer, int encodedAccessUnitCount) {
    @Nullable FireOsStreamInfo streamInfoHint = getStreamInfoForPassthroughDecision(format);
    byte[] inspectionBytes = toArray(buffer.duplicate());
    ArrayList<PackerKind> candidateKinds = new ArrayList<>(4);
    addCandidateKind(candidateKinds, detectPackerKindKodiStyle(inspectionBytes));
    if (streamInfoHint != null) {
      addCandidateKind(candidateKinds, getPackerKind(streamInfoHint));
    }
    addCandidateKind(candidateKinds, PackerKind.DTS_HD);
    addCandidateKind(candidateKinds, PackerKind.E_AC3);
    addCandidateKind(candidateKinds, PackerKind.TRUEHD);

    for (PackerKind candidateKind : candidateKinds) {
      @Nullable PreparedEncodedPacket preparedPacket;
      switch (candidateKind) {
        case TRUEHD:
          preparedPacket = prepareTrueHdPacket(format, buffer, encodedAccessUnitCount);
          break;
        case DTS_HD:
          preparedPacket = prepareDtsPacket(format, buffer, encodedAccessUnitCount);
          break;
        case E_AC3:
          preparedPacket = prepareAc3Packet(format, buffer, encodedAccessUnitCount);
          break;
        case AC3:
        case DTS_CORE:
        default:
          preparedPacket = null;
          break;
      }
      if (preparedPacket != null) {
        return preparedPacket;
      }
    }
    return null;
  }

  private @Nullable PackerKind detectPackerKindKodiStyle(byte[] input) {
    @Nullable PackerKind possibleKind = null;
    for (int offset = 0; offset + 8 < input.length; offset++) {
      int bytesRemaining = input.length - offset;

      if (looksLikeDtsSync(input, offset)) {
        int dtsSize = parseDtsAccessUnitSize(input, offset, bytesRemaining);
        if (dtsSize > 0) {
          return PackerKind.DTS_HD;
        }
        if (possibleKind == null) {
          possibleKind = PackerKind.DTS_HD;
        }
      }

      if (isAc3SyncWord(input, offset)) {
        int ac3Size = parseAc3AccessUnitSize(input, offset, bytesRemaining);
        if (ac3Size > 0) {
          return PackerKind.E_AC3;
        }
        if (possibleKind == null) {
          possibleKind = PackerKind.E_AC3;
        }
      }

      if (isPotentialTrueHdMajorSync(input, offset)) {
        TrueHdParseAttempt trueHdAttempt =
            tryParseTrueHdAccessUnit(input, offset, bytesRemaining, new TrueHdSyncParserState());
        if (trueHdAttempt.status == TrueHdParseStatus.FRAME) {
          return PackerKind.TRUEHD;
        }
        if (possibleKind == null) {
          possibleKind = PackerKind.TRUEHD;
        }
      }
    }
    return possibleKind;
  }

  private static boolean isPotentialTrueHdMajorSync(byte[] input, int offset) {
    if (offset + 7 >= input.length) {
      return false;
    }
    return (input[offset + 4] & 0xFF) == 0xF8
        && (input[offset + 5] & 0xFF) == 0x72
        && (input[offset + 6] & 0xFF) == 0x6F
        && (input[offset + 7] & 0xFF) == 0xBA;
  }

  private static void addCandidateKind(ArrayList<PackerKind> candidateKinds, @Nullable PackerKind kind) {
    if (kind == null || candidateKinds.contains(kind)) {
      return;
    }
    candidateKinds.add(kind);
  }

  private @Nullable PreparedEncodedPacket prepareDtsPacket(
      Format format, ByteBuffer buffer, int encodedAccessUnitCount) {
    int inputSampleRateHz = resolveInputSampleRateHz(format, getActiveStreamInfo());
    NormalizedPacketBatch batch =
        new DtsBufferNormalizer(inputSampleRateHz, /* hd= */ true, dtsSyncParserState)
            .normalize(buffer.duplicate(), encodedAccessUnitCount);
    if (batch.accessUnits.isEmpty()) {
      return null;
    }
    FireOsStreamInfo streamInfo = deriveDtsStreamInfo(batch);
    PackerKind effectiveKind = getEffectivePackerKind(PackerKind.DTS_HD, streamInfo);
    logIecDebug(
        "prepareDtsPacket accessUnits="
            + encodedAccessUnitCount
            + " normalized="
            + batch.accessUnits.size()
            + " bytes="
            + buffer.remaining()
            + " streamInfo="
            + streamInfo.summary());
    return new PreparedEncodedPacket(
        new BufferMetadata(
            effectiveKind,
            batch.accessUnits.size(),
            getTotalFrames(batch.accessUnits),
            streamInfo.inputSampleRateHz,
            streamInfo),
        batch,
        encodedAccessUnitCount);
  }

  private @Nullable PreparedEncodedPacket prepareAc3Packet(
      Format format, ByteBuffer buffer, int encodedAccessUnitCount) {
    int inputSampleRateHz = resolveInputSampleRateHz(format, getActiveStreamInfo());
    NormalizedPacketBatch batch =
        new Ac3BufferNormalizer(
                inputSampleRateHz, /* expectEac3= */ true, ac3SyncParserState)
            .normalize(buffer.duplicate(), encodedAccessUnitCount);
    if (batch.accessUnits.isEmpty()) {
      return null;
    }
    FireOsStreamInfo streamInfo = deriveAc3FamilyStreamInfo(batch);
    PackerKind effectiveKind =
        streamInfo.family == FireOsStreamInfo.StreamFamily.E_AC3 ? PackerKind.E_AC3 : PackerKind.AC3;
    logIecDebug(
        "prepareAc3Packet accessUnits="
            + encodedAccessUnitCount
            + " normalized="
            + batch.accessUnits.size()
            + " bytes="
            + buffer.remaining()
            + " streamInfo="
            + streamInfo.summary());
    return new PreparedEncodedPacket(
        new BufferMetadata(
            effectiveKind,
            batch.accessUnits.size(),
            getTotalFrames(batch.accessUnits),
            streamInfo.inputSampleRateHz,
            streamInfo),
        batch,
        encodedAccessUnitCount);
  }

  private PreparedEncodedPacket prepareTrueHdPacket(
      Format format, ByteBuffer buffer, int encodedAccessUnitCount) {
    TrueHdNormalizationResult normalizationResult =
        normalizeTrueHdBatch(buffer.duplicate(), encodedAccessUnitCount);
    if (normalizationResult.batch.accessUnits.isEmpty()) {
      Log.w(
          TAG,
          "Dropping TrueHD packet after parser sync failure"
              + " accessUnits="
              + encodedAccessUnitCount
              + " bytes="
              + buffer.remaining());
      return null;
    }
    @Nullable FireOsStreamInfo streamInfo = deriveTrueHdStreamInfo(normalizationResult.batch);
    if (streamInfo == null) {
      Log.w(
          TAG,
          "Dropping TrueHD packet until major sync metadata is available"
              + " accessUnits="
              + encodedAccessUnitCount
              + " bytes="
              + buffer.remaining());
      return null;
    }
    logIecDebug(
        "prepareTrueHdPacket accessUnits="
            + encodedAccessUnitCount
            + " normalized="
            + normalizationResult.batch.accessUnits.size()
            + " bytes="
            + buffer.remaining()
            + " streamInfo="
            + streamInfo.summary());
    return new PreparedEncodedPacket(
        new BufferMetadata(
            PackerKind.TRUEHD,
            normalizationResult.batch.accessUnits.size(),
            getTotalFrames(normalizationResult.batch.accessUnits),
            streamInfo.inputSampleRateHz,
            streamInfo),
        normalizationResult.batch,
        encodedAccessUnitCount);
  }

  private static PackerKind getEffectivePackerKind(
      PackerKind detectedKind, FireOsStreamInfo streamInfo) {
    if (streamInfo.family != FireOsStreamInfo.StreamFamily.DTS) {
      return detectedKind;
    }
    switch (streamInfo.dtsStreamType) {
      case DTS_512:
      case DTS_1024:
      case DTS_2048:
      case DTSHD_CORE:
        return PackerKind.DTS_CORE;
      case DTSHD:
      case DTSHD_MA:
        return PackerKind.DTS_HD;
      case NONE:
      case UNKNOWN:
      default:
        return detectedKind == PackerKind.DTS_HD ? PackerKind.DTS_CORE : detectedKind;
    }
  }

  /* package */ synchronized void updateStreamInfo(Format format, FireOsStreamInfo streamInfo) {
    activeStreamInfo = streamInfo;
  }

  /* package */ synchronized void resetSessionState() {
    activeStreamInfo = null;
    fallbackModes.clear();
    clearPreparedPackets();
    ac3SyncParserState.reset();
    dtsSyncParserState.reset();
    trueHdSyncParserState.reset();
  }

  /* package */ synchronized void registerPreparedPacket(
      ByteBuffer buffer, PreparedEncodedPacket preparedPacket) {
    preparedPackets.put(buffer, preparedPacket);
  }

  /* package */ synchronized void discardPreparedPacket(ByteBuffer buffer) {
    preparedPackets.remove(buffer);
  }

  /* package */ synchronized void clearPreparedPackets() {
    preparedPackets.clear();
  }

  private synchronized @Nullable PreparedEncodedPacket peekPreparedPacket(ByteBuffer buffer) {
    return preparedPackets.get(buffer);
  }

  private synchronized @Nullable PreparedEncodedPacket consumePreparedPacket(ByteBuffer buffer) {
    return preparedPackets.remove(buffer);
  }

  private synchronized @Nullable FireOsStreamInfo getActiveStreamInfo() {
    return activeStreamInfo;
  }

  private synchronized @Nullable FireOsStreamInfo getStreamInfoForPassthroughDecision(
      Format format) {
    @Nullable FireOsStreamInfo activeStreamInfo = getActiveStreamInfo();
    if (activeStreamInfo != null) {
      return activeStreamInfo;
    }
    return deriveKodiPrePlaybackStreamInfo(format);
  }

  private static @Nullable FireOsStreamInfo deriveKodiPrePlaybackStreamInfo(Format format) {
    @Nullable String codecMimeType = resolveCodecMimeTypeForPrePlayback(format);
    if (codecMimeType == null) {
      return null;
    }
    int inputSampleRateHz = format.sampleRate != Format.NO_VALUE ? format.sampleRate : 48_000;
    int inputChannelCount = format.channelCount != Format.NO_VALUE ? format.channelCount : 2;
    switch (codecMimeType) {
      case MimeTypes.AUDIO_AC3:
        return FireOsStreamInfo.createForAc3Family(
            /* isEac3= */ false, inputSampleRateHz, inputChannelCount);
      case MimeTypes.AUDIO_E_AC3:
      case MimeTypes.AUDIO_E_AC3_JOC:
        return FireOsStreamInfo.createForAc3Family(
            /* isEac3= */ true, inputSampleRateHz, inputChannelCount);
      case MimeTypes.AUDIO_TRUEHD:
        if (format.sampleRate == Format.NO_VALUE || format.channelCount == Format.NO_VALUE) {
          return FireOsStreamInfo.createForUnknownTrueHd();
        }
        return FireOsStreamInfo.createForTrueHd(inputSampleRateHz, inputChannelCount);
      case MimeTypes.AUDIO_DTS:
      case MimeTypes.AUDIO_DTS_EXPRESS:
      case MimeTypes.AUDIO_DTS_HD:
      case MimeTypes.AUDIO_DTS_X:
        return FireOsStreamInfo.createForDts(
            /* sampleMimeType= */ null,
            deriveKodiPrePlaybackDtsStreamType(codecMimeType, format.codecs),
            inputSampleRateHz,
            inputChannelCount,
            C.LENGTH_UNSET,
            /* diagnostics= */ "kodi-preplay");
      default:
        return null;
    }
  }

  private static @Nullable String resolveCodecMimeTypeForPrePlayback(Format format) {
    @Nullable String codecs = format.codecs;
    if (codecs != null) {
      String[] codecIds = codecs.split(",");
      for (String codecId : codecIds) {
        @Nullable String codecMimeType = MimeTypes.getMediaMimeType(codecId.trim());
        if (codecMimeType != null && MimeTypes.isAudio(codecMimeType)) {
          return codecMimeType;
        }
      }
    }
    return format.sampleMimeType;
  }

  private static FireOsStreamInfo.DtsStreamType deriveKodiPrePlaybackDtsStreamType(
      String codecMimeType, @Nullable String codecs) {
    if (MimeTypes.AUDIO_DTS_X.equals(codecMimeType)) {
      // Kodi maps DTS-HD MA X profiles to DTSHD_MA during pre-playback support selection.
      return FireOsStreamInfo.DtsStreamType.DTSHD_MA;
    }
    if (MimeTypes.AUDIO_DTS_HD.equals(codecMimeType)) {
      String normalizedCodecs = codecs == null ? "" : codecs.toLowerCase(Locale.US);
      if (normalizedCodecs.contains("dtsl")
          || normalizedCodecs.contains("dtsx")
          || normalizedCodecs.contains("ma")) {
        return FireOsStreamInfo.DtsStreamType.DTSHD_MA;
      }
      if (normalizedCodecs.contains("dtsh") || normalizedCodecs.contains("hra")) {
        return FireOsStreamInfo.DtsStreamType.DTSHD;
      }
      // Kodi defaults unknown DTS profile to core before frame parser refinement.
      return FireOsStreamInfo.DtsStreamType.DTSHD_CORE;
    }
    return FireOsStreamInfo.DtsStreamType.DTSHD_CORE;
  }

  private static int getTotalFrames(List<NormalizedAccessUnit> accessUnits) {
    int totalFrames = 0;
    for (NormalizedAccessUnit accessUnit : accessUnits) {
      totalFrames += max(accessUnit.sampleCount, 0);
    }
    return totalFrames;
  }

  /* package */ String getCapabilitySummary(int channelMask) {
    CapabilityMatrix matrix = getCapabilityMatrix(channelMask);
    return "stereo48="
        + matrix.stereo48Supported
        + ",stereo192="
        + matrix.stereo192Supported
        + ",rawAc3="
        + matrix.rawAc3Supported
        + ",rawEac3="
        + matrix.rawEac3Supported
        + ",multi192="
        + matrix.multichannel192Supported;
  }

  private OutputConfig getPassthroughOutputConfigWithFireOsQuirks(FormatConfig formatConfig)
      throws ConfigurationException {
    FormatConfig effectiveConfig = maybeApplyRawFireOsQuirk(formatConfig);
    return passthroughProvider.getOutputConfig(effectiveConfig);
  }

  private OutputConfig buildLogicalPassthroughOutputConfig(
      FormatConfig formatConfig,
      @Nullable PackerKind kind,
      boolean preferWrappedConfig,
      @Nullable FireOsStreamInfo streamInfo) {
    if (preferWrappedConfig) {
      try {
        return getPassthroughOutputConfigWithFireOsQuirks(formatConfig);
      } catch (ConfigurationException e) {
        throw new IllegalStateException("Wrapped passthrough config unexpectedly unavailable", e);
      }
    }
    checkNotNull(kind);
    return new OutputConfig.Builder()
        .setEncoding(resolveLogicalPassthroughEncoding(kind))
        .setSampleRate(
            resolveInputSampleRateHz(formatConfig.format, streamInfo))
        .setChannelMask(getLogicalPassthroughChannelMask(formatConfig.format, streamInfo, kind))
        .setBufferSize(
            formatConfig.preferredBufferSize != C.LENGTH_UNSET
                ? formatConfig.preferredBufferSize
                : SYNTHETIC_PASSTHROUGH_BUFFER_SIZE_BYTES)
        .setAudioAttributes(formatConfig.audioAttributes)
        .setAudioSessionId(formatConfig.audioSessionId)
        .setVirtualDeviceId(formatConfig.virtualDeviceId)
        .setIsTunneling(formatConfig.enableTunneling)
        .setEncapsulationMode(AudioTrack.ENCAPSULATION_MODE_NONE)
        .setIsOffload(false)
        .setUsePlaybackParameters(formatConfig.enablePlaybackParameters)
        .setUseOffloadGapless(false)
        .build();
  }

  private static int getLogicalPassthroughChannelMask(
      Format format, @Nullable FireOsStreamInfo streamInfo, @Nullable PackerKind kind) {
    if (streamInfo != null) {
      return streamInfo.logicalPassthroughChannelMask;
    }
    if (kind != null) {
      switch (kind) {
        case TRUEHD:
          return IEC61937_MULTICHANNEL_RAW_CARRIER_MASK;
        case AC3:
        case E_AC3:
        case DTS_CORE:
        case DTS_HD:
          return AudioFormat.CHANNEL_OUT_STEREO;
        default:
          break;
      }
    }
    int channelCount = format.channelCount;
    if (channelCount == Format.NO_VALUE) {
      return AudioFormat.CHANNEL_OUT_STEREO;
    }
    if (channelCount > 6) {
      return IEC61937_MULTICHANNEL_RAW_CARRIER_MASK;
    }
    if (channelCount > 2) {
      return AudioFormat.CHANNEL_OUT_5POINT1;
    }
    return AudioFormat.CHANNEL_OUT_STEREO;
  }

  private FireOsStreamInfo deriveDtsStreamInfo(NormalizedPacketBatch batch) {
    NormalizedAccessUnit accessUnit = batch.accessUnits.isEmpty() ? null : batch.accessUnits.get(0);
    if (accessUnit == null) {
      return FireOsStreamInfo.createForUnknown(/* sampleMimeType= */ null);
    }
    return FireOsDtsClassifier.classify(
        accessUnit.data,
        accessUnit.sampleCount,
        accessUnit.repetitionPeriodFrames,
        accessUnit.inputSampleRateHz,
        accessUnit.dtsUhdHeader != null
            ? accessUnit.dtsUhdHeader.channelCount
            : (accessUnit.dtsHdHeader != null ? accessUnit.dtsHdHeader.channelCount : 2),
        getPreviousDtsStreamType(),
        accessUnit.dtsHdHeader,
        accessUnit.dtsUhdHeader);
  }

  private FireOsStreamInfo.DtsStreamType getPreviousDtsStreamType() {
    @Nullable FireOsStreamInfo previousStreamInfo = getActiveStreamInfo();
    if (previousStreamInfo == null || previousStreamInfo.family != FireOsStreamInfo.StreamFamily.DTS) {
      return FireOsStreamInfo.DtsStreamType.NONE;
    }
    return previousStreamInfo.dtsStreamType;
  }

  private FireOsStreamInfo deriveAc3FamilyStreamInfo(NormalizedPacketBatch batch) {
    NormalizedAccessUnit accessUnit =
        batch.accessUnits.isEmpty() ? null : batch.accessUnits.get(0);
    if (accessUnit == null) {
      @Nullable FireOsStreamInfo previousStreamInfo = getActiveStreamInfo();
      if (previousStreamInfo != null
          && (previousStreamInfo.family == FireOsStreamInfo.StreamFamily.AC3
              || previousStreamInfo.family == FireOsStreamInfo.StreamFamily.E_AC3)) {
        return FireOsStreamInfo.createForAc3Family(
            previousStreamInfo.family == FireOsStreamInfo.StreamFamily.E_AC3,
            previousStreamInfo.inputSampleRateHz,
            previousStreamInfo.inputChannelCount);
      }
      return FireOsStreamInfo.createForUnknown(/* sampleMimeType= */ null);
    }
    Ac3Util.SyncFrameInfo syncFrameInfo =
        Ac3Util.parseAc3SyncframeInfo(new ParsableBitArray(accessUnit.data));
    boolean isEac3 = isEac3SyncFrame(syncFrameInfo);
    int sampleRateHz =
        syncFrameInfo.sampleRate != Format.NO_VALUE
            ? syncFrameInfo.sampleRate
            : (accessUnit.inputSampleRateHz != Format.NO_VALUE ? accessUnit.inputSampleRateHz : 48_000);
    int channelCount =
        syncFrameInfo.channelCount != Format.NO_VALUE
            ? syncFrameInfo.channelCount
            : 2;
    return FireOsStreamInfo.createForAc3Family(isEac3, sampleRateHz, channelCount);
  }

  private @Nullable FireOsStreamInfo deriveTrueHdStreamInfo(NormalizedPacketBatch batch) {
    @Nullable ParsedTrueHdStreamInfo parsedStreamInfo = null;
    for (NormalizedAccessUnit accessUnit : batch.accessUnits) {
      parsedStreamInfo = parseTrueHdStreamInfo(accessUnit.data);
      if (parsedStreamInfo != null) {
        break;
      }
    }
    if (parsedStreamInfo != null) {
      return FireOsStreamInfo.createForTrueHd(
          parsedStreamInfo.sampleRateHz, parsedStreamInfo.channelCount);
    }
    @Nullable FireOsStreamInfo previousStreamInfo = getActiveStreamInfo();
    if (previousStreamInfo != null && previousStreamInfo.family == FireOsStreamInfo.StreamFamily.TRUEHD) {
      return FireOsStreamInfo.createForTrueHd(
          previousStreamInfo.inputSampleRateHz, previousStreamInfo.inputChannelCount);
    }
    TrueHdSyncParserState parserState = trueHdSyncParserState.copy();
    if (parserState.hasSync && parserState.sampleRateHz > 0 && parserState.channelCount > 0) {
      return FireOsStreamInfo.createForTrueHd(parserState.sampleRateHz, parserState.channelCount);
    }
    return null;
  }

  private static @Nullable ParsedTrueHdStreamInfo parseTrueHdStreamInfo(byte[] accessUnit) {
    TrueHdSyncParserState parserState = new TrueHdSyncParserState();
    @Nullable ParsedTrueHdAccessUnitInfo parsedAccessUnit =
        parseTrueHdAccessUnitInfo(accessUnit, 0, accessUnit.length, parserState);
    return parsedAccessUnit != null ? parsedAccessUnit.streamInfo : null;
  }

  private static int getTrueHdChannelCount(int channelMap) {
    int channelCount = 0;
    for (int i = 0; i < TRUEHD_CHANNEL_MAP_WEIGHTS.length; i++) {
      channelCount += TRUEHD_CHANNEL_MAP_WEIGHTS[i] * ((channelMap >>> i) & 0x01);
    }
    return channelCount;
  }

  private static int resolveInputSampleRateHz(
      Format format, @Nullable FireOsStreamInfo streamInfo) {
    if (streamInfo != null && streamInfo.inputSampleRateHz > 0) {
      return streamInfo.inputSampleRateHz;
    }
    return format.sampleRate != Format.NO_VALUE ? format.sampleRate : 48_000;
  }

  private synchronized TrueHdNormalizationResult normalizeTrueHdBatch(
      ByteBuffer inputBuffer, int encodedAccessUnitCount) {
    byte[] input = toArray(inputBuffer);
    TrueHdSyncParserState parserState = trueHdSyncParserState.copy();
    TrueHdSplitResult splitResult = splitTrueHdAccessUnits(input, encodedAccessUnitCount, parserState);
    ArrayList<NormalizedAccessUnit> normalizedUnits = new ArrayList<>(splitResult.accessUnits.size());
    for (ParsedTrueHdFrameSlice accessUnit : splitResult.accessUnits) {
      byte[] bytes = copyRange(splitResult.source, accessUnit.offset, accessUnit.length);
      normalizedUnits.add(
          new NormalizedAccessUnit(
              bytes,
              parserState.sampleRateHz > 0 ? parserState.sampleRateHz : 48_000,
              accessUnit.sampleCount,
              bytes.length,
              C.LENGTH_UNSET,
              /* bitstreamMode= */ 0,
              /* littleEndian= */ false,
              /* dtsHdHeader= */ null,
              /* dtsUhdHeader= */ null));
    }
    trueHdSyncParserState.copyFrom(parserState);
    return new TrueHdNormalizationResult(
        new NormalizedPacketBatch(
            PackerKind.TRUEHD,
            encodedAccessUnitCount,
            parserState.sampleRateHz > 0 ? parserState.sampleRateHz : 48_000,
            normalizedUnits));
  }

  private FormatConfig maybeApplyRawFireOsQuirk(FormatConfig formatConfig) {
    @Nullable String sampleMimeType = formatConfig.format.sampleMimeType;
    if (!MimeTypes.AUDIO_AC3.equals(sampleMimeType)) {
      return formatConfig;
    }
    CapabilityMatrix capabilityMatrix =
        getCapabilityMatrix(Util.getAudioTrackChannelConfig(formatConfig.format.channelCount));
    if (capabilityMatrix.rawAc3Supported || !capabilityMatrix.rawEac3Supported) {
      return formatConfig;
    }
    Log.w(TAG, "Using Fire OS AC3->E-AC3 raw quirk fallback");
    return copyFormatConfig(
        formatConfig,
        formatConfig.format.buildUpon().setSampleMimeType(MimeTypes.AUDIO_E_AC3).setCodecs(null).build());
  }

  private boolean supportsRawPassthrough(PackerKind kind) {
    CapabilityMatrix capabilityMatrix = getCapabilityMatrix(AudioFormat.CHANNEL_OUT_STEREO);
    switch (kind) {
      case AC3:
        return capabilityMatrix.rawAc3Supported || capabilityMatrix.rawEac3Supported;
      case E_AC3:
        return capabilityMatrix.rawEac3Supported;
      default:
        return true;
    }
  }

  private AudioOutput getAudioOutputWithRetry(AudioOutputProvider provider, OutputConfig config)
      throws InitializationException {
    if (shouldUseKodiFireOsIecCapabilityModel()) {
      return provider.getAudioOutput(config);
    }
    try {
      return provider.getAudioOutput(config);
    } catch (InitializationException firstFailure) {
      sleepQuietly(FIRE_OS_OUTPUT_RETRY_DELAY_MS);
      try {
        return provider.getAudioOutput(config);
      } catch (InitializationException retryFailure) {
        firstFailure.addSuppressed(retryFailure);
      }
      if (config.bufferSize > FIRE_OS_SMALLER_BUFFER_RETRY_SIZE) {
        OutputConfig smallerConfig =
            config.buildUpon().setBufferSize(FIRE_OS_SMALLER_BUFFER_RETRY_SIZE).build();
        try {
          return provider.getAudioOutput(smallerConfig);
        } catch (InitializationException smallerFailure) {
          firstFailure.addSuppressed(smallerFailure);
        }
      }
      throw firstFailure;
    }
  }

  private static void sleepQuietly(int millis) {
    try {
      Thread.sleep(millis);
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
    }
  }

  @Nullable
  private CarrierPlan buildCarrierPlanIfSupported(
      PackerKind kind,
      EncodedBufferNormalizer normalizer,
      IecPacker packer,
      OutputConfig passthroughConfig,
      @Nullable FireOsStreamInfo streamInfo,
      boolean allowStereoIecFallback) {
    int carrierChannelMask = packer.getCarrierChannelMask(passthroughConfig.channelMask);
    if (!supportsCarrierPlan(packer.getCarrierSampleRateHz(), carrierChannelMask)) {
      Log.w(
          TAG,
          "IEC carrier config rejected"
              + " kind="
              + kind
              + " sampleRate="
              + packer.getCarrierSampleRateHz()
              + " channelMask="
              + carrierChannelMask);
      return null;
    }
    int minBufferSize =
        AudioTrack.getMinBufferSize(
            packer.getCarrierSampleRateHz(), carrierChannelMask, AudioFormat.ENCODING_IEC61937);
    int bufferSize =
        max(
            passthroughConfig.bufferSize,
            max(
                packer.getMaximumPacketSizeBytes(),
                minBufferSize > 0 ? minBufferSize * 4 : packer.getMaximumPacketSizeBytes() * 4));

    OutputConfig carrierConfig =
        passthroughConfig.buildUpon()
            .setEncoding(C.ENCODING_PCM_16BIT)
            .setSampleRate(packer.getCarrierSampleRateHz())
            .setChannelMask(carrierChannelMask)
            .setEncapsulationMode(AudioTrack.ENCAPSULATION_MODE_NONE)
            .setBufferSize(bufferSize)
            .build();
    return new CarrierPlan(
        kind,
        normalizer,
        packer,
        passthroughConfig,
        carrierConfig,
        streamInfo,
        allowStereoIecFallback);
  }

  @Nullable
  private CarrierPlan maybeCreateStereoCarrierFallback(CarrierPlan plan) {
    if (!plan.packer.usesMultichannelCarrier() || !plan.allowStereoIecFallback) {
      return null;
    }
    IecPacker stereoPacker = plan.packer.withMultichannelCarrier(false);
    if (stereoPacker == plan.packer) {
      return null;
    }
    return buildCarrierPlanIfSupported(
        plan.kind,
        plan.normalizer,
        stereoPacker,
        plan.passthroughConfig,
        plan.streamInfo,
        /* allowStereoIecFallback= */ true);
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

  @VisibleForTesting
  /* package */ void setIecSupportForTesting(int sampleRateHz, int channelMask, boolean supported) {
    iecSupportCache.put(new IecProbeConfig(sampleRateHz, channelMask), supported);
  }

  @VisibleForTesting
  /* package */ void setEncodedSupportForTesting(
      int sampleRateHz, int channelMask, int encoding, boolean supported) {
    encodedSupportCache.put(
        new EncodedProbeConfig(sampleRateHz, channelMask, encoding), supported);
  }

  @VisibleForTesting
  /* package */ static int[] getNormalizedPayloadSizesForTesting(
      PreparedEncodedPacket preparedPacket) {
    int[] payloadSizes = new int[preparedPacket.normalizedBatch.accessUnits.size()];
    for (int i = 0; i < preparedPacket.normalizedBatch.accessUnits.size(); i++) {
      payloadSizes[i] = preparedPacket.normalizedBatch.accessUnits.get(i).payloadSize;
    }
    return payloadSizes;
  }

  @VisibleForTesting
  /* package */ static int[] getNormalizedSampleCountsForTesting(
      PreparedEncodedPacket preparedPacket) {
    int[] sampleCounts = new int[preparedPacket.normalizedBatch.accessUnits.size()];
    for (int i = 0; i < preparedPacket.normalizedBatch.accessUnits.size(); i++) {
      sampleCounts[i] = preparedPacket.normalizedBatch.accessUnits.get(i).sampleCount;
    }
    return sampleCounts;
  }

  @VisibleForTesting
  /* package */ static void writeTrueHdMajorSyncCrcForTesting(byte[] accessUnit) {
    int majorSyncSize = getTrueHdMajorSyncSize(accessUnit, 0, accessUnit.length);
    checkState(majorSyncSize != C.LENGTH_UNSET && accessUnit.length >= 4 + majorSyncSize);
    int crc = computeTrueHdMajorSyncCrc(accessUnit, 4, majorSyncSize - 4);
    crc ^= ((accessUnit[majorSyncSize + 1] & 0xFF) << 8) | (accessUnit[majorSyncSize] & 0xFF);
    accessUnit[majorSyncSize + 2] = (byte) (crc & 0xFF);
    accessUnit[majorSyncSize + 3] = (byte) ((crc >>> 8) & 0xFF);
  }

  @VisibleForTesting
  /* package */ void requestFallbackForTesting(
      PackerKind kind, boolean multichannelCarrierFailed, boolean allowStereoIecFallback) {
    requestFallback(kind, multichannelCarrierFailed, allowStereoIecFallback);
  }

  @VisibleForTesting
  /* package */ static byte[] packPreparedPacketForTesting(
      PreparedEncodedPacket preparedPacket, boolean keepMultichannelCarrier) {
    return toArray(
        createPackerForTesting(
                preparedPacket.metadata.kind,
                preparedPacket.metadata.streamInfo.inputSampleRateHz,
                keepMultichannelCarrier,
                preparedPacket.metadata.streamInfo.dtsPeriodFrames)
            .pack(preparedPacket.normalizedBatch));
  }

  @VisibleForTesting
  /* package */ static byte[] packAccessUnitsForTesting(
      PackerKind kind,
      int inputSampleRateHz,
      int reportedAccessUnitCount,
      boolean keepMultichannelCarrier,
      int streamDtsPeriodFrames,
      TestAccessUnit... accessUnits) {
    ArrayList<NormalizedAccessUnit> normalizedAccessUnits = new ArrayList<>(accessUnits.length);
    for (TestAccessUnit accessUnit : accessUnits) {
      normalizedAccessUnits.add(
          new NormalizedAccessUnit(
              accessUnit.data,
              inputSampleRateHz,
              accessUnit.sampleCount,
              accessUnit.payloadSize,
              accessUnit.repetitionPeriodFrames,
              accessUnit.bitstreamMode,
              accessUnit.littleEndian,
              /* dtsHdHeader= */ null,
              /* dtsUhdHeader= */ null));
    }
    return toArray(
        createPackerForTesting(kind, inputSampleRateHz, keepMultichannelCarrier, streamDtsPeriodFrames)
            .pack(
                new NormalizedPacketBatch(
                    kind, reportedAccessUnitCount, inputSampleRateHz, normalizedAccessUnits)));
  }

  private static IecPacker createPackerForTesting(
      PackerKind kind,
      int inputSampleRateHz,
      boolean keepMultichannelCarrier,
      int streamDtsPeriodFrames) {
    switch (kind) {
      case AC3:
        return new Ac3IecPacker(inputSampleRateHz);
      case E_AC3:
        return new EAc3IecPacker(inputSampleRateHz);
      case DTS_CORE:
        return new DtsCoreIecPacker(inputSampleRateHz);
      case DTS_HD:
        return new DtsHdIecPacker(
            inputSampleRateHz, keepMultichannelCarrier, streamDtsPeriodFrames);
      case TRUEHD:
        return new TrueHdIecPacker(inputSampleRateHz, keepMultichannelCarrier);
      default:
        throw new IllegalArgumentException("Unsupported test packer kind: " + kind);
    }
  }

  private boolean shouldUseKodiFireOsIecCapabilityModel() {
    return AmazonQuirks.shouldAttemptExperimentalFireOsIecPassthrough();
  }

  private boolean shouldAllowFireOsCompatibilityFallback() {
    return AmazonQuirks.isFireOsCompatibilityFallbackEnabled();
  }

  private boolean shouldSuperviseFireOsIecAudioDelay() {
    return shouldUseKodiFireOsIecCapabilityModel()
        && AmazonQuirks.isFireOsIecSuperviseAudioDelayEnabled();
  }

  private static boolean shouldLogIecDetails() {
    return AmazonQuirks.isFireOsIecVerboseLoggingEnabled();
  }

  private static void logIecDebug(String message) {
    if (shouldLogIecDetails()) {
      Log.d(TAG, message);
    }
  }

  private static String describeFormat(Format format) {
    return "mime="
        + format.sampleMimeType
        + ",codecs="
        + (format.codecs != null ? format.codecs : "none")
        + ",sampleRate="
        + format.sampleRate
        + ",channels="
        + format.channelCount
        + ",pcm="
        + format.pcmEncoding
        + ",bitrate="
        + format.bitrate;
  }

  private static String describeOutputConfig(OutputConfig config) {
    return "encoding="
        + config.encoding
        + ",sampleRate="
        + config.sampleRate
        + ",channelMask="
        + config.channelMask
        + ",bufferSize="
        + config.bufferSize
        + ",tunneling="
        + config.isTunneling
        + ",offload="
        + config.isOffload
        + ",encapsulation="
        + config.encapsulationMode;
  }

  private boolean supportsCarrierPlan(int sampleRateHz, int channelMask) {
    if (!shouldUseKodiFireOsIecCapabilityModel()) {
      return supportsIec61937Config(sampleRateHz, channelMask);
    }
    CapabilityMatrix capabilityMatrix = getCapabilityMatrix(channelMask);
    if (channelMask == AudioFormat.CHANNEL_OUT_STEREO) {
      return capabilityMatrix.supportsStereoCarrier(sampleRateHz, this);
    }
    if (channelMask == IEC61937_MULTICHANNEL_RAW_CARRIER_MASK) {
      return capabilityMatrix.supportsMultichannelCarrier(sampleRateHz, this);
    }
    return false;
  }

  @VisibleForTesting
  /* package */ static boolean supportsCarrierForKodiProbeModel(
      int sampleRateHz,
      int channelMask,
      boolean stereo48Supported,
      boolean stereo192Supported,
      boolean multichannel192Supported) {
    if (channelMask == AudioFormat.CHANNEL_OUT_STEREO) {
      if (isKodiBaseIecCarrierRate(sampleRateHz)) {
        return stereo48Supported;
      }
      if (isKodiHighBitrateIecCarrierRate(sampleRateHz)) {
        return stereo48Supported && stereo192Supported;
      }
      return false;
    }
    if (channelMask == IEC61937_MULTICHANNEL_RAW_CARRIER_MASK) {
      return isKodiHighBitrateIecCarrierRate(sampleRateHz)
          && stereo48Supported
          && multichannel192Supported;
    }
    return false;
  }

  private static boolean isKodiBaseIecCarrierRate(int sampleRateHz) {
    switch (sampleRateHz) {
      case 32_000:
      case 44_100:
      case 48_000:
        return true;
      default:
        return false;
    }
  }

  private static boolean isKodiHighBitrateIecCarrierRate(int sampleRateHz) {
    switch (sampleRateHz) {
      case 88_200:
      case 96_000:
      case 176_400:
      case 192_000:
        return true;
      default:
        return false;
    }
  }

  private boolean verifyIec61937Track(int sampleRateHz, int channelMask) {
    int minBufferSize =
        AudioTrack.getMinBufferSize(sampleRateHz, channelMask, AudioFormat.ENCODING_IEC61937);
    if (minBufferSize <= 0) {
      logIecDebug(
          "verifyIec61937Track rejected"
              + " sampleRate="
              + sampleRateHz
              + " channelMask="
              + channelMask
              + " minBufferSize="
              + minBufferSize);
      return false;
    }
    AudioTrack audioTrack = null;
    try {
      AudioFormat format =
          new AudioFormat.Builder()
              .setSampleRate(sampleRateHz)
              .setChannelMask(channelMask)
              .setEncoding(AudioFormat.ENCODING_IEC61937)
              .build();
      android.media.AudioAttributes attributes =
          new android.media.AudioAttributes.Builder()
              .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MOVIE)
              .setUsage(android.media.AudioAttributes.USAGE_MEDIA)
              .build();
      audioTrack =
          new Iec61937AudioTrack(
              attributes,
              format,
              max(minBufferSize, IEC61937_PACKET_HEADER_BYTES * 256),
              AudioTrack.MODE_STREAM,
              AudioManager.AUDIO_SESSION_ID_GENERATE);
      boolean initialized = audioTrack.getState() == AudioTrack.STATE_INITIALIZED;
      if (!initialized) {
        logIecDebug(
            "verifyIec61937Track not initialized"
                + " sampleRate="
                + sampleRateHz
                + " channelMask="
                + channelMask);
      }
      return initialized;
    } catch (IllegalArgumentException | UnsupportedOperationException e) {
      logIecDebug(
          "verifyIec61937Track threw"
              + " sampleRate="
              + sampleRateHz
              + " channelMask="
              + channelMask
              + " error="
              + e.getClass().getSimpleName());
      return false;
    } finally {
      if (audioTrack != null) {
        try {
          audioTrack.release();
        } catch (RuntimeException e) {
          Log.w(TAG, "Failed releasing IEC probe track", e);
        }
      }
    }
  }

  private boolean verifyEncodedTrack(int sampleRateHz, int channelMask, int encoding) {
    int minBufferSize = AudioTrack.getMinBufferSize(sampleRateHz, channelMask, encoding);
    if (minBufferSize <= 0) {
      logIecDebug(
          "verifyEncodedTrack rejected"
              + " sampleRate="
              + sampleRateHz
              + " channelMask="
              + channelMask
              + " encoding="
              + encoding
              + " minBufferSize="
              + minBufferSize);
      return false;
    }
    AudioTrack audioTrack = null;
    try {
      AudioFormat format =
          new AudioFormat.Builder()
              .setSampleRate(sampleRateHz)
              .setChannelMask(channelMask)
              .setEncoding(encoding)
              .build();
      android.media.AudioAttributes attributes =
          new android.media.AudioAttributes.Builder()
              .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MOVIE)
              .setUsage(android.media.AudioAttributes.USAGE_MEDIA)
              .build();
      audioTrack =
          new AudioTrack(
              attributes,
              format,
              max(minBufferSize, 16 * 1024),
              AudioTrack.MODE_STREAM,
              AudioManager.AUDIO_SESSION_ID_GENERATE);
      boolean initialized = audioTrack.getState() == AudioTrack.STATE_INITIALIZED;
      if (!initialized) {
        logIecDebug(
            "verifyEncodedTrack not initialized"
                + " sampleRate="
                + sampleRateHz
                + " channelMask="
                + channelMask
                + " encoding="
                + encoding);
      }
      return initialized;
    } catch (IllegalArgumentException | UnsupportedOperationException e) {
      logIecDebug(
          "verifyEncodedTrack threw"
              + " sampleRate="
              + sampleRateHz
              + " channelMask="
              + channelMask
              + " encoding="
              + encoding
              + " error="
              + e.getClass().getSimpleName());
      return false;
    } finally {
      if (audioTrack != null) {
        try {
          audioTrack.release();
        } catch (RuntimeException e) {
          Log.w(TAG, "Failed releasing encoded probe track", e);
        }
      }
    }
  }

  private @Nullable PackerKind getDecisionPackerKind(@Nullable FireOsStreamInfo streamInfo) {
    return streamInfo != null ? getPackerKind(streamInfo) : null;
  }

  @Nullable
  private static PackerKind getPackerKind(FireOsStreamInfo streamInfo) {
    switch (streamInfo.family) {
      case AC3:
        return PackerKind.AC3;
      case E_AC3:
        return PackerKind.E_AC3;
      case DTS:
        switch (streamInfo.dtsStreamType) {
          case DTS_512:
          case DTS_1024:
          case DTS_2048:
          case DTSHD_CORE:
            return PackerKind.DTS_CORE;
          case DTSHD:
          case DTSHD_MA:
            return PackerKind.DTS_HD;
          case NONE:
          case UNKNOWN:
          default:
            return PackerKind.DTS_CORE;
        }
      case TRUEHD:
        return PackerKind.TRUEHD;
      case UNKNOWN:
      default:
        return null;
    }
  }

  private static int resolveLogicalPassthroughEncoding(PackerKind kind) {
    switch (kind) {
      case AC3:
        return C.ENCODING_AC3;
      case E_AC3:
        return C.ENCODING_E_AC3;
      case DTS_CORE:
        return C.ENCODING_DTS;
      case DTS_HD:
        return C.ENCODING_DTS_HD;
      case TRUEHD:
        return C.ENCODING_DOLBY_TRUEHD;
      default:
        throw new IllegalArgumentException("Unsupported packer kind: " + kind);
    }
  }

  /* package */ enum PackerKind {
    AC3,
    E_AC3,
    DTS_CORE,
    DTS_HD,
    TRUEHD
  }

  private enum FallbackMode {
    STRICT_FAILURE,
    STEREO_IEC,
    RAW_PASSTHROUGH,
    DTS_CORE_RAW
  }

  private static final class IecProbeConfig {
    public final int sampleRateHz;
    public final int channelMask;

    public IecProbeConfig(int sampleRateHz, int channelMask) {
      this.sampleRateHz = sampleRateHz;
      this.channelMask = channelMask;
    }

    @Override
    public boolean equals(@Nullable Object obj) {
      if (this == obj) {
        return true;
      }
      if (!(obj instanceof IecProbeConfig)) {
        return false;
      }
      IecProbeConfig other = (IecProbeConfig) obj;
      return sampleRateHz == other.sampleRateHz && channelMask == other.channelMask;
    }

    @Override
    public int hashCode() {
      return Objects.hash(sampleRateHz, channelMask);
    }
  }

  private static final class CapabilityMatrix {
    public final boolean stereo48Supported;
    public final boolean stereo192Supported;
    public final boolean rawAc3Supported;
    public final boolean rawEac3Supported;
    public final boolean multichannel192Supported;

    public CapabilityMatrix(
        boolean stereo48Supported,
        boolean stereo192Supported,
        boolean rawAc3Supported,
        boolean rawEac3Supported,
        boolean multichannel192Supported) {
      this.stereo48Supported = stereo48Supported;
      this.stereo192Supported = stereo192Supported;
      this.rawAc3Supported = rawAc3Supported;
      this.rawEac3Supported = rawEac3Supported;
      this.multichannel192Supported = multichannel192Supported;
    }

    public boolean supportsStereoCarrier(
        int sampleRateHz, FireOsIec61937AudioOutputProvider provider) {
      if (provider.shouldUseKodiFireOsIecCapabilityModel()) {
        return supportsCarrierForKodiProbeModel(
            sampleRateHz,
            AudioFormat.CHANNEL_OUT_STEREO,
            stereo48Supported,
            stereo192Supported,
            multichannel192Supported);
      }
      switch (sampleRateHz) {
        case 32_000:
        case 44_100:
        case 48_000:
          return stereo48Supported && provider.supportsIec61937Config(sampleRateHz, AudioFormat.CHANNEL_OUT_STEREO);
        case 96_000:
        case 192_000:
          return stereo192Supported && provider.supportsIec61937Config(sampleRateHz, AudioFormat.CHANNEL_OUT_STEREO);
        default:
          return provider.supportsIec61937Config(sampleRateHz, AudioFormat.CHANNEL_OUT_STEREO);
      }
    }

    public boolean supportsMultichannelCarrier(
        int sampleRateHz, FireOsIec61937AudioOutputProvider provider) {
      if (provider.shouldUseKodiFireOsIecCapabilityModel()) {
        return supportsCarrierForKodiProbeModel(
            sampleRateHz,
            IEC61937_MULTICHANNEL_RAW_CARRIER_MASK,
            stereo48Supported,
            stereo192Supported,
            multichannel192Supported);
      }
      switch (sampleRateHz) {
        case 96_000:
        case 192_000:
          return multichannel192Supported;
        default:
          return false;
      }
    }
  }

  /* package */ static final class BufferMetadata {
    public final PackerKind kind;
    public final int normalizedAccessUnitCount;
    public final int totalFrames;
    public final int inputSampleRateHz;
    public final FireOsStreamInfo streamInfo;

    public BufferMetadata(
        PackerKind kind,
        int normalizedAccessUnitCount,
        int totalFrames,
        int inputSampleRateHz,
        FireOsStreamInfo streamInfo) {
      this.kind = kind;
      this.normalizedAccessUnitCount = normalizedAccessUnitCount;
      this.totalFrames = totalFrames;
      this.inputSampleRateHz = inputSampleRateHz;
      this.streamInfo = streamInfo;
    }
  }

  /* package */ static final class PreparedEncodedPacket {
    public final BufferMetadata metadata;
    public final NormalizedPacketBatch normalizedBatch;
    public final int encodedAccessUnitCount;

    public PreparedEncodedPacket(
        BufferMetadata metadata, NormalizedPacketBatch normalizedBatch, int encodedAccessUnitCount) {
      this.metadata = metadata;
      this.normalizedBatch = normalizedBatch;
      this.encodedAccessUnitCount = encodedAccessUnitCount;
    }
  }

  private static final class EncodedProbeConfig {
    public final int sampleRateHz;
    public final int channelMask;
    public final int encoding;

    public EncodedProbeConfig(int sampleRateHz, int channelMask, int encoding) {
      this.sampleRateHz = sampleRateHz;
      this.channelMask = channelMask;
      this.encoding = encoding;
    }

    @Override
    public boolean equals(@Nullable Object obj) {
      if (this == obj) {
        return true;
      }
      if (!(obj instanceof EncodedProbeConfig)) {
        return false;
      }
      EncodedProbeConfig other = (EncodedProbeConfig) obj;
      return sampleRateHz == other.sampleRateHz
          && channelMask == other.channelMask
          && encoding == other.encoding;
    }

    @Override
    public int hashCode() {
      return Objects.hash(sampleRateHz, channelMask, encoding);
    }
  }

  private static final class CarrierPlan {
    public final PackerKind kind;
    public final EncodedBufferNormalizer normalizer;
    public final IecPacker packer;
    public final OutputConfig passthroughConfig;
    public final OutputConfig carrierConfig;
    @Nullable public final FireOsStreamInfo streamInfo;
    public final boolean allowStereoIecFallback;

    public CarrierPlan(
        PackerKind kind,
        EncodedBufferNormalizer normalizer,
        IecPacker packer,
        OutputConfig passthroughConfig,
        OutputConfig carrierConfig,
        @Nullable FireOsStreamInfo streamInfo,
        boolean allowStereoIecFallback) {
      this.kind = kind;
      this.normalizer = normalizer;
      this.packer = packer;
      this.passthroughConfig = passthroughConfig;
      this.carrierConfig = carrierConfig;
      this.streamInfo = streamInfo;
      this.allowStereoIecFallback = allowStereoIecFallback;
    }
  }

  private static final class Iec61937AudioTrackProvider
      implements DefaultAudioSink.AudioTrackProvider {

    @Override
    public AudioTrack getAudioTrack(
        AudioSink.AudioTrackConfig audioTrackConfig,
        AudioAttributes audioAttributes,
        int audioSessionId,
        @Nullable Context context) {
      AudioFormat format =
          new AudioFormat.Builder()
              .setSampleRate(audioTrackConfig.sampleRate)
              .setChannelMask(audioTrackConfig.channelConfig)
              .setEncoding(AudioFormat.ENCODING_IEC61937)
              .build();
      android.media.AudioAttributes platformAttributes =
          audioTrackConfig.tunneling
              ? getTunnelingAudioTrackAttributes()
              : audioAttributes.getPlatformAudioAttributes();
      return new Iec61937AudioTrack(
          platformAttributes,
          format,
          audioTrackConfig.bufferSize,
          AudioTrack.MODE_STREAM,
          audioSessionId);
    }

    private static android.media.AudioAttributes getTunnelingAudioTrackAttributes() {
      return new android.media.AudioAttributes.Builder()
          .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MOVIE)
          .setFlags(android.media.AudioAttributes.FLAG_HW_AV_SYNC)
          .setUsage(android.media.AudioAttributes.USAGE_MEDIA)
          .build();
    }
  }

  private static final class Iec61937AudioTrack extends AudioTrack {

    @Nullable private short[] scratchShortBuffer;

    public Iec61937AudioTrack(
        android.media.AudioAttributes attributes,
        AudioFormat format,
        int bufferSizeInBytes,
        int mode,
        int sessionId) {
      super(attributes, format, bufferSizeInBytes, mode, sessionId);
    }

    @Override
    public int write(ByteBuffer audioData, int sizeInBytes, int writeMode) {
      if ((sizeInBytes & 1) != 0) {
        logIecDebug("Iec61937AudioTrack write rejected odd size=" + sizeInBytes);
        return ERROR_BAD_VALUE;
      }
      int shortCount = sizeInBytes / 2;
      if (shortCount == 0) {
        return 0;
      }
      if (scratchShortBuffer == null || scratchShortBuffer.length < shortCount) {
        scratchShortBuffer = new short[shortCount];
      }
      int startPosition = audioData.position();
      ByteBuffer source = audioData.duplicate().order(ByteOrder.LITTLE_ENDIAN);
      source.limit(startPosition + sizeInBytes);
      source.asShortBuffer().get(checkNotNull(scratchShortBuffer), 0, shortCount);
      int shortsWritten = super.write(checkNotNull(scratchShortBuffer), 0, shortCount, writeMode);
      if (shortsWritten > 0) {
        audioData.position(startPosition + (shortsWritten * 2));
        return shortsWritten * 2;
      }
      if (shortsWritten < 0) {
        logIecDebug(
            "Iec61937AudioTrack write failed"
                + " code="
                + shortsWritten
                + " bytes="
                + sizeInBytes);
      }
      return shortsWritten;
    }

    @Override
    public int write(ByteBuffer audioData, int sizeInBytes, int writeMode, long timestampNs) {
      return write(audioData, sizeInBytes, writeMode);
    }
  }

  private interface EncodedBufferNormalizer {
    NormalizedPacketBatch normalize(ByteBuffer inputBuffer, int encodedAccessUnitCount);
  }

  private static final class NormalizedPacketBatch {
    public final PackerKind kind;
    public final int reportedAccessUnitCount;
    public final int inputSampleRateHz;
    public final List<NormalizedAccessUnit> accessUnits;

    public NormalizedPacketBatch(
        PackerKind kind,
        int reportedAccessUnitCount,
        int inputSampleRateHz,
        List<NormalizedAccessUnit> accessUnits) {
      this.kind = kind;
      this.reportedAccessUnitCount = reportedAccessUnitCount;
      this.inputSampleRateHz = inputSampleRateHz;
      this.accessUnits = accessUnits;
    }
  }

  private static final class NormalizedAccessUnit {
    public final byte[] data;
    public final int inputSampleRateHz;
    public final int sampleCount;
    public final int payloadSize;
    public final int repetitionPeriodFrames;
    public final int bitstreamMode;
    public final boolean littleEndian;
    @Nullable public final DtsUtil.DtsHeader dtsHdHeader;
    @Nullable public final DtsUtil.DtsHeader dtsUhdHeader;

    public NormalizedAccessUnit(
        byte[] data,
        int inputSampleRateHz,
        int sampleCount,
        int payloadSize,
        int repetitionPeriodFrames,
        int bitstreamMode,
        boolean littleEndian,
        @Nullable DtsUtil.DtsHeader dtsHdHeader,
        @Nullable DtsUtil.DtsHeader dtsUhdHeader) {
      this.data = data;
      this.inputSampleRateHz = inputSampleRateHz;
      this.sampleCount = sampleCount;
      this.payloadSize = payloadSize;
      this.repetitionPeriodFrames = repetitionPeriodFrames;
      this.bitstreamMode = bitstreamMode;
      this.littleEndian = littleEndian;
      this.dtsHdHeader = dtsHdHeader;
      this.dtsUhdHeader = dtsUhdHeader;
    }
  }

  private interface IecPacker {
    int getCarrierSampleRateHz();

    int getCarrierChannelMask(int passthroughChannelMask);

    int getMaximumPacketSizeBytes();

    ByteBuffer pack(NormalizedPacketBatch batch);

    ByteBuffer createPauseBurst(int millis);

    boolean usesMultichannelCarrier();

    IecPacker withMultichannelCarrier(boolean keepMultichannelCarrier);
  }

  private static final class Ac3BufferNormalizer implements EncodedBufferNormalizer {
    private final int inputSampleRateHz;
    private final boolean expectEac3;
    @Nullable private final Ac3SyncParserState parserState;

    public Ac3BufferNormalizer(
        int inputSampleRateHz, boolean expectEac3, @Nullable Ac3SyncParserState parserState) {
      this.inputSampleRateHz = inputSampleRateHz;
      this.expectEac3 = expectEac3;
      this.parserState = parserState;
    }

    @Override
    public NormalizedPacketBatch normalize(ByteBuffer inputBuffer, int encodedAccessUnitCount) {
      byte[] input = toArray(inputBuffer);
      FrameSplitResult splitResult =
          splitAc3AccessUnits(input, encodedAccessUnitCount, parserState);
      List<FrameSlice> accessUnits = splitResult.accessUnits;
      if (splitResult.skippedBytes != 0 || splitResult.trailingBytes != 0) {
        Log.w(
            TAG,
            "Dropping unsynced AC3/E-AC3 bytes"
                + " skipped="
                + splitResult.skippedBytes
                + " trailing="
                + splitResult.trailingBytes
                + " parsedUnits="
                + accessUnits.size());
      }
      ArrayList<NormalizedAccessUnit> normalizedUnits = new ArrayList<>(accessUnits.size());
      for (int i = 0; i < accessUnits.size(); i++) {
        FrameSlice accessUnit = accessUnits.get(i);
        byte[] bytes = copyRange(splitResult.source, accessUnit.offset, accessUnit.length);
        Ac3Util.SyncFrameInfo syncFrameInfo =
            Ac3Util.parseAc3SyncframeInfo(new ParsableBitArray(bytes));
        boolean isEac3 = isEac3SyncFrame(syncFrameInfo);
        if (expectEac3
            && isEac3
            && syncFrameInfo.streamType == Ac3Util.SyncFrameInfo.STREAM_TYPE_TYPE0
            && i + 1 < accessUnits.size()) {
          FrameSlice nextAccessUnit = accessUnits.get(i + 1);
          byte[] nextBytes =
              copyRange(splitResult.source, nextAccessUnit.offset, nextAccessUnit.length);
          Ac3Util.SyncFrameInfo nextSyncFrameInfo =
              Ac3Util.parseAc3SyncframeInfo(new ParsableBitArray(nextBytes));
          if (isEac3SyncFrame(nextSyncFrameInfo)
              && nextSyncFrameInfo.streamType == Ac3Util.SyncFrameInfo.STREAM_TYPE_TYPE1) {
            byte[] mergedBytes = new byte[bytes.length + nextBytes.length];
            System.arraycopy(bytes, 0, mergedBytes, 0, bytes.length);
            System.arraycopy(nextBytes, 0, mergedBytes, bytes.length, nextBytes.length);
            bytes = mergedBytes;
            i++;
          }
        }
        int effectiveSampleRateHz =
            syncFrameInfo.sampleRate != Format.NO_VALUE
                ? syncFrameInfo.sampleRate
                : inputSampleRateHz;
        int bitstreamMode = !isEac3 && bytes.length > 5 ? bytes[5] & 0x7 : 0;
        normalizedUnits.add(
            new NormalizedAccessUnit(
                bytes,
                effectiveSampleRateHz,
                syncFrameInfo.sampleCount,
                bytes.length,
                C.LENGTH_UNSET,
                bitstreamMode,
                /* littleEndian= */ false,
                /* dtsHdHeader= */ null,
                /* dtsUhdHeader= */ null));
      }
      return new NormalizedPacketBatch(
          expectEac3 ? PackerKind.E_AC3 : PackerKind.AC3,
          encodedAccessUnitCount,
          inputSampleRateHz,
          normalizedUnits);
    }
  }

  private static final class DtsBufferNormalizer implements EncodedBufferNormalizer {
    private final int inputSampleRateHz;
    private final boolean hd;
    @Nullable private final DtsSyncParserState parserState;

    public DtsBufferNormalizer(
        int inputSampleRateHz, boolean hd, @Nullable DtsSyncParserState parserState) {
      this.inputSampleRateHz = inputSampleRateHz;
      this.hd = hd;
      this.parserState = parserState;
    }

    @Override
    public NormalizedPacketBatch normalize(ByteBuffer inputBuffer, int encodedAccessUnitCount) {
      byte[] input = toArray(inputBuffer);
      FrameSplitResult splitResult =
          splitDtsAccessUnits(input, encodedAccessUnitCount, parserState);
      List<FrameSlice> accessUnits = splitResult.accessUnits;
      if (splitResult.skippedBytes != 0 || splitResult.trailingBytes != 0) {
        Log.w(
            TAG,
            "Dropping unsynced DTS bytes"
                + " skipped="
                + splitResult.skippedBytes
                + " trailing="
                + splitResult.trailingBytes
                + " parsedUnits="
                + accessUnits.size());
      }
      ArrayList<NormalizedAccessUnit> normalizedUnits = new ArrayList<>(accessUnits.size());
      for (FrameSlice accessUnit : accessUnits) {
        byte[] bytes = copyRange(splitResult.source, accessUnit.offset, accessUnit.length);
        DtsDerivedParameters parameters = deriveDtsAccessUnitParameters(bytes, inputSampleRateHz, hd);
        normalizedUnits.add(
            new NormalizedAccessUnit(
                bytes,
                parameters.sampleRateHz,
                parameters.sampleCount,
                parameters.payloadSize,
                parameters.repetitionPeriodFrames,
                /* bitstreamMode= */ 0,
                parameters.littleEndian,
                parameters.dtsHdHeader,
                parameters.dtsUhdHeader));
      }
      return new NormalizedPacketBatch(
          hd ? PackerKind.DTS_HD : PackerKind.DTS_CORE,
          encodedAccessUnitCount,
          inputSampleRateHz,
          normalizedUnits);
    }
  }

  private static final class TrueHdBufferNormalizer implements EncodedBufferNormalizer {
    private final int inputSampleRateHz;

    public TrueHdBufferNormalizer(int inputSampleRateHz) {
      this.inputSampleRateHz = inputSampleRateHz;
    }

    @Override
    public NormalizedPacketBatch normalize(ByteBuffer inputBuffer, int encodedAccessUnitCount) {
      byte[] input = toArray(inputBuffer);
      TrueHdSyncParserState parserState = new TrueHdSyncParserState();
      TrueHdSplitResult splitResult = splitTrueHdAccessUnits(input, encodedAccessUnitCount, parserState);
      ArrayList<NormalizedAccessUnit> normalizedUnits = new ArrayList<>(splitResult.accessUnits.size());
      for (ParsedTrueHdFrameSlice accessUnit : splitResult.accessUnits) {
        byte[] bytes = copyRange(splitResult.source, accessUnit.offset, accessUnit.length);
        normalizedUnits.add(
            new NormalizedAccessUnit(
                bytes,
                inputSampleRateHz,
                accessUnit.sampleCount,
                bytes.length,
                C.LENGTH_UNSET,
                /* bitstreamMode= */ 0,
                /* littleEndian= */ false,
                /* dtsHdHeader= */ null,
                /* dtsUhdHeader= */ null));
      }
      return new NormalizedPacketBatch(
          PackerKind.TRUEHD, encodedAccessUnitCount, inputSampleRateHz, normalizedUnits);
    }
  }

  private static final class Ac3IecPacker implements IecPacker {
    private final int inputSampleRateHz;

    public Ac3IecPacker(int inputSampleRateHz) {
      this.inputSampleRateHz = inputSampleRateHz;
    }

    @Override
    public int getCarrierSampleRateHz() {
      return inputSampleRateHz;
    }

    @Override
    public int getCarrierChannelMask(int passthroughChannelMask) {
      return AudioFormat.CHANNEL_OUT_STEREO;
    }

    @Override
    public int getMaximumPacketSizeBytes() {
      return IEC61937_AC3_PACKET_BYTES;
    }

    @Override
    public ByteBuffer pack(NormalizedPacketBatch batch) {
      List<ByteBuffer> packets = new ArrayList<>(batch.accessUnits.size());
      int totalSize = 0;
      for (NormalizedAccessUnit accessUnit : batch.accessUnits) {
        ByteBuffer packet = packSingleAccessUnit(accessUnit);
        totalSize += packet.remaining();
        packets.add(packet);
      }
      return joinPackets(packets, totalSize);
    }

    @Override
    public ByteBuffer createPauseBurst(int millis) {
      return createPauseBurstPacket(
          /* channelCount= */ 2,
          /* outputRateHz= */ getCarrierSampleRateHz(),
          /* repetitionPeriodFrames= */ 3,
          /* encodedRateHz= */ inputSampleRateHz,
          millis);
    }

    @Override
    public boolean usesMultichannelCarrier() {
      return false;
    }

    @Override
    public IecPacker withMultichannelCarrier(boolean keepMultichannelCarrier) {
      return this;
    }

    private static ByteBuffer packSingleAccessUnit(NormalizedAccessUnit accessUnit) {
      int packetSize = IEC61937_AC3_PACKET_BYTES;
      int payloadSize =
          min(
              accessUnit.data.length + (accessUnit.data.length & 1),
              packetSize - IEC61937_PACKET_HEADER_BYTES);
      ByteBuffer output = allocatePacket(packetSize);
      putPreamble(output, IEC61937_AC3 | (accessUnit.bitstreamMode << 8), accessUnit.data.length << 3);
      putWordSwapped(output, accessUnit.data, 0, min(accessUnit.data.length, payloadSize));
      zeroFillToLimit(output, packetSize);
      output.flip();
      return output;
    }
  }

  private static final class EAc3IecPacker implements IecPacker {
    private final int inputSampleRateHz;
    private final ArrayDeque<ByteBuffer> pendingPackets;
    private final ArrayDeque<NormalizedAccessUnit> deferredAccessUnits;
    private byte[] accumulatedFrames;
    private int accumulatedSize;
    private int accumulatedFrameCount;
    private int framesPerBurst;

    public EAc3IecPacker(int inputSampleRateHz) {
      this.inputSampleRateHz = inputSampleRateHz;
      this.pendingPackets = new ArrayDeque<>();
      this.deferredAccessUnits = new ArrayDeque<>();
      this.accumulatedFrames = new byte[0];
      this.accumulatedSize = 0;
      this.accumulatedFrameCount = 0;
      this.framesPerBurst = 0;
    }

    @Override
    public int getCarrierSampleRateHz() {
      return inputSampleRateHz * 4;
    }

    @Override
    public int getCarrierChannelMask(int passthroughChannelMask) {
      return AudioFormat.CHANNEL_OUT_STEREO;
    }

    @Override
    public int getMaximumPacketSizeBytes() {
      return IEC61937_E_AC3_PACKET_BYTES;
    }

    @Override
    public ByteBuffer pack(NormalizedPacketBatch batch) {
      while (!deferredAccessUnits.isEmpty()) {
        if (!appendAccessUnit(checkNotNull(deferredAccessUnits.pollFirst()))) {
          return drainPendingPackets();
        }
      }
      for (int i = 0; i < batch.accessUnits.size(); i++) {
        if (!appendAccessUnit(batch.accessUnits.get(i))) {
          for (int j = i + 1; j < batch.accessUnits.size(); j++) {
            deferredAccessUnits.addLast(batch.accessUnits.get(j));
          }
          break;
        }
      }
      return drainPendingPackets();
    }

    @Override
    public ByteBuffer createPauseBurst(int millis) {
      return createPauseBurstPacket(
          /* channelCount= */ 2,
          /* outputRateHz= */ getCarrierSampleRateHz(),
          /* repetitionPeriodFrames= */ 4,
          /* encodedRateHz= */ inputSampleRateHz,
          millis);
    }

    @Override
    public boolean usesMultichannelCarrier() {
      return false;
    }

    @Override
    public IecPacker withMultichannelCarrier(boolean keepMultichannelCarrier) {
      return this;
    }

    private boolean appendAccessUnit(NormalizedAccessUnit accessUnit) {
      int maxPayloadBytes = IEC61937_E_AC3_PACKET_BYTES - IEC61937_PACKET_HEADER_BYTES;
      if (accessUnit.data.length > maxPayloadBytes) {
        Log.w(
            TAG,
            "Dropping oversized E-AC3 access unit"
                + " bytes="
                + accessUnit.data.length
                + " maxPayload="
                + maxPayloadBytes);
        return true;
      }
      int frameSampleCount = max(1, accessUnit.sampleCount);
      int nextFramesPerBurst = max(1, 1_536 / frameSampleCount);
      if (framesPerBurst != 0 && framesPerBurst != nextFramesPerBurst) {
        flushAccumulatedFrames();
      }
      framesPerBurst = nextFramesPerBurst;
      int newSize = accumulatedSize + accessUnit.data.length;
      if (newSize > maxPayloadBytes) {
        flushAccumulatedFrames();
        // Kodi flushes the accumulated burst and defers this frame to a later burst.
        deferredAccessUnits.addLast(accessUnit);
        return false;
      }
      ensureAccumulatedCapacity(newSize);
      System.arraycopy(accessUnit.data, 0, accumulatedFrames, accumulatedSize, accessUnit.data.length);
      accumulatedSize += accessUnit.data.length;
      accumulatedFrameCount++;
      if (accumulatedFrameCount >= max(1, framesPerBurst)) {
        flushAccumulatedFrames();
      }
      return true;
    }

    private void flushAccumulatedFrames() {
      if (accumulatedSize <= 0) {
        accumulatedFrameCount = 0;
        return;
      }
      ByteBuffer output = allocatePacket(IEC61937_E_AC3_PACKET_BYTES);
      putPreamble(output, IEC61937_E_AC3, accumulatedSize);
      putWordSwapped(output, accumulatedFrames, 0, accumulatedSize);
      zeroFillToLimit(output, IEC61937_E_AC3_PACKET_BYTES);
      output.flip();
      pendingPackets.addLast(output);
      accumulatedSize = 0;
      accumulatedFrameCount = 0;
    }

    private void ensureAccumulatedCapacity(int requiredCapacity) {
      if (accumulatedFrames.length >= requiredCapacity) {
        return;
      }
      byte[] replacement = new byte[max(requiredCapacity, accumulatedFrames.length * 2 + 256)];
      if (accumulatedSize > 0) {
        System.arraycopy(accumulatedFrames, 0, replacement, 0, accumulatedSize);
      }
      accumulatedFrames = replacement;
    }

    private ByteBuffer drainPendingPackets() {
      if (pendingPackets.isEmpty()) {
        return EMPTY_BUFFER;
      }
      int totalSize = 0;
      for (ByteBuffer packet : pendingPackets) {
        totalSize += packet.remaining();
      }
      ByteBuffer output = allocatePacket(totalSize);
      while (!pendingPackets.isEmpty()) {
        output.put(checkNotNull(pendingPackets.removeFirst()));
      }
      output.flip();
      return output;
    }
  }

  private static final class DtsCoreIecPacker implements IecPacker {

    private final int inputSampleRateHz;

    public DtsCoreIecPacker(int inputSampleRateHz) {
      this.inputSampleRateHz = inputSampleRateHz;
    }

    @Override
    public int getCarrierSampleRateHz() {
      return inputSampleRateHz;
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
    public ByteBuffer pack(NormalizedPacketBatch batch) {
      List<ByteBuffer> packets = new ArrayList<>(batch.accessUnits.size());
      int totalSize = 0;
      for (NormalizedAccessUnit accessUnit : batch.accessUnits) {
        ByteBuffer packet = packSingleAccessUnit(accessUnit);
        totalSize += packet.remaining();
        packets.add(packet);
      }
      return joinPackets(packets, totalSize);
    }

    @Override
    public ByteBuffer createPauseBurst(int millis) {
      return createPauseBurstPacket(
          /* channelCount= */ 2,
          /* outputRateHz= */ getCarrierSampleRateHz(),
          /* repetitionPeriodFrames= */ 3,
          /* encodedRateHz= */ inputSampleRateHz,
          millis);
    }

    @Override
    public boolean usesMultichannelCarrier() {
      return false;
    }

    @Override
    public IecPacker withMultichannelCarrier(boolean keepMultichannelCarrier) {
      return this;
    }

    private static ByteBuffer packSingleAccessUnit(NormalizedAccessUnit accessUnit) {
      int dataType = getDtsCoreDataType(accessUnit.sampleCount);
      int packetSize = accessUnit.sampleCount * 4;
      int payloadSize =
          accessUnit.payloadSize > 0 && accessUnit.payloadSize < accessUnit.data.length
              ? accessUnit.payloadSize
              : accessUnit.data.length;
      boolean omitPreamble = payloadSize == packetSize;
      ByteBuffer output = allocatePacket(packetSize);
      if (!omitPreamble) {
        putPreamble(output, dataType, payloadSize << 3);
      }
      putMaybeWordSwapped(output, accessUnit.data, 0, payloadSize, !accessUnit.littleEndian);
      zeroFillToLimit(output, packetSize);
      output.flip();
      return output;
    }
  }

  private static final class DtsHdIecPacker implements IecPacker {

    private final int inputSampleRateHz;
    private final boolean keepMultichannelCarrier;
    private final int streamDtsPeriodFrames;

    public DtsHdIecPacker(
        int inputSampleRateHz, boolean keepMultichannelCarrier, int streamDtsPeriodFrames) {
      this.inputSampleRateHz = inputSampleRateHz;
      this.keepMultichannelCarrier = keepMultichannelCarrier;
      this.streamDtsPeriodFrames = streamDtsPeriodFrames;
    }

    @Override
    public int getCarrierSampleRateHz() {
      return IEC61937_DTS_HD_CARRIER_RATE_HZ;
    }

    @Override
    public int getCarrierChannelMask(int passthroughChannelMask) {
      return keepMultichannelCarrier
          ? IEC61937_MULTICHANNEL_RAW_CARRIER_MASK
          : AudioFormat.CHANNEL_OUT_STEREO;
    }

    @Override
    public int getMaximumPacketSizeBytes() {
      return IEC61937_DTS_HD_MAX_PACKET_BYTES;
    }

    @Override
    public ByteBuffer pack(NormalizedPacketBatch batch) {
      if (batch.accessUnits.size() == 1) {
        return packSingleAccessUnit(batch.accessUnits.get(0));
      }
      List<ByteBuffer> packets = new ArrayList<>(batch.accessUnits.size());
      int totalSize = 0;
      for (NormalizedAccessUnit accessUnit : batch.accessUnits) {
        ByteBuffer packet = packSingleAccessUnit(accessUnit);
        totalSize += packet.remaining();
        packets.add(packet);
      }
      return joinPackets(packets, totalSize);
    }

    @Override
    public ByteBuffer createPauseBurst(int millis) {
      return createPauseBurstPacket(
          keepMultichannelCarrier ? 8 : 2,
          getCarrierSampleRateHz(),
          /* repetitionPeriodFrames= */ 3,
          inputSampleRateHz,
          millis);
    }

    @Override
    public boolean usesMultichannelCarrier() {
      return keepMultichannelCarrier;
    }

    @Override
    public IecPacker withMultichannelCarrier(boolean keepMultichannelCarrier) {
      if (this.keepMultichannelCarrier == keepMultichannelCarrier) {
        return this;
      }
      return new DtsHdIecPacker(
          inputSampleRateHz, keepMultichannelCarrier, streamDtsPeriodFrames);
    }

    private ByteBuffer packSingleAccessUnit(NormalizedAccessUnit accessUnit) {
      int repetitionPeriod =
          streamDtsPeriodFrames != C.LENGTH_UNSET
              ? streamDtsPeriodFrames
              : (accessUnit.repetitionPeriodFrames != C.LENGTH_UNSET
              ? accessUnit.repetitionPeriodFrames
              : tryComputeDtsHdRepetitionPeriod(
                  accessUnit.sampleCount, accessUnit.inputSampleRateHz));
      if (repetitionPeriod == C.LENGTH_UNSET) {
        Log.w(
            TAG,
            "Skipping DTS-HD burst with unsupported repetition period"
                + " sampleCount="
                + accessUnit.sampleCount
                + " sampleRate="
                + accessUnit.inputSampleRateHz);
        return EMPTY_BUFFER;
      }
      int subtype = getDtsHdSubtype(repetitionPeriod);
      if (subtype == C.LENGTH_UNSET) {
        Log.w(
            TAG,
            "Skipping DTS-HD burst with invalid repetition period"
                + " repetitionPeriod="
                + repetitionPeriod);
        return EMPTY_BUFFER;
      }
      int packetSize = repetitionPeriod * 4;
      int payloadSize = accessUnit.data.length;
      if (DTS_HD_START_CODE.length + 2 + payloadSize > packetSize - IEC61937_PACKET_HEADER_BYTES
          && accessUnit.payloadSize > 0) {
        Log.w(
            TAG,
            "Skipping DTS-HD burst overflow"
                + " sampleCount="
                + accessUnit.sampleCount
                + " sampleRate="
                + accessUnit.inputSampleRateHz
                + " repetitionPeriod="
                + repetitionPeriod
                + " payloadSize="
                + payloadSize
                + " packetSize="
                + packetSize);
        return EMPTY_BUFFER;
      }
      int outBytes = DTS_HD_START_CODE.length + 2 + payloadSize;
      int lengthCode = alignTo16(outBytes + IEC61937_PACKET_HEADER_BYTES) - IEC61937_PACKET_HEADER_BYTES;
      byte[] payload = new byte[outBytes];
      System.arraycopy(DTS_HD_START_CODE, 0, payload, 0, DTS_HD_START_CODE.length);
      payload[DTS_HD_START_CODE.length] = (byte) ((payloadSize >>> 8) & 0xFF);
      payload[DTS_HD_START_CODE.length + 1] = (byte) (payloadSize & 0xFF);
      System.arraycopy(accessUnit.data, 0, payload, DTS_HD_START_CODE.length + 2, payloadSize);
      ByteBuffer output = allocatePacket(packetSize);
      putPreamble(output, IEC61937_DTSHD | (subtype << 8), lengthCode);
      putWordSwapped(output, payload, 0, payload.length);
      zeroFillToLimit(output, packetSize);
      output.flip();
      return output;
    }
  }

  private static final class TrueHdIecPacker implements IecPacker {

    private final int inputSampleRateHz;
    private final boolean keepMultichannelCarrier;
    private final TrueHdMatState state;
    private final ArrayDeque<byte[]> outputQueue;
    private byte[] matBuffer;
    private int matBufferCount;

    public TrueHdIecPacker(int inputSampleRateHz, boolean keepMultichannelCarrier) {
      this.inputSampleRateHz = inputSampleRateHz;
      this.keepMultichannelCarrier = keepMultichannelCarrier;
      this.state = new TrueHdMatState();
      this.outputQueue = new ArrayDeque<>();
      this.matBuffer = new byte[0];
      this.matBufferCount = 0;
    }

    @Override
    public int getCarrierSampleRateHz() {
      return getTrueHdCarrierRateHz(inputSampleRateHz);
    }

    @Override
    public int getCarrierChannelMask(int passthroughChannelMask) {
      return keepMultichannelCarrier
          ? IEC61937_MULTICHANNEL_RAW_CARRIER_MASK
          : AudioFormat.CHANNEL_OUT_STEREO;
    }

    @Override
    public int getMaximumPacketSizeBytes() {
      return IEC61937_TRUEHD_PACKET_BYTES;
    }

    @Override
    public ByteBuffer pack(NormalizedPacketBatch batch) {
      if (batch.reportedAccessUnitCount != Ac3Util.TRUEHD_RECHUNK_SAMPLE_COUNT) {
        Log.w(
            TAG,
            "Unexpected TrueHD access unit count="
                + batch.reportedAccessUnitCount
                + ", expected="
                + Ac3Util.TRUEHD_RECHUNK_SAMPLE_COUNT);
      }
      for (NormalizedAccessUnit accessUnit : batch.accessUnits) {
        packTrueHd(accessUnit.data);
      }
      return drainOutputFrames();
    }

    @Override
    public ByteBuffer createPauseBurst(int millis) {
      return createPauseBurstPacket(
          keepMultichannelCarrier ? 8 : 2,
          getCarrierSampleRateHz(),
          /* repetitionPeriodFrames= */ 4,
          inputSampleRateHz,
          millis);
    }

    @Override
    public boolean usesMultichannelCarrier() {
      return keepMultichannelCarrier;
    }

    @Override
    public IecPacker withMultichannelCarrier(boolean keepMultichannelCarrier) {
      if (this.keepMultichannelCarrier == keepMultichannelCarrier) {
        return this;
      }
      return new TrueHdIecPacker(inputSampleRateHz, keepMultichannelCarrier);
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
      swapWordsInPlace(matBuffer, IEC61937_PACKET_HEADER_BYTES, IEC61937_TRUEHD_PACKET_BYTES - IEC61937_PACKET_HEADER_BYTES);
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
    private final EncodedBufferNormalizer normalizer;
    private final IecPacker packer;
    private final FireOsIec61937AudioOutputProvider provider;
    @Nullable private ByteBuffer pendingInputBuffer;
    @Nullable private ByteBuffer pendingPackedBuffer;
    @Nullable private NormalizedPacketBatch pendingNormalizedBatch;
    private int pendingAccessUnitCount;
    private int lastPauseBurstDurationMs;
    private long syntheticPauseRemainingUs;
    private long lastPauseUpdateRealtimeMs;
    private boolean playing;
    private final boolean superviseAudioDelay;
    private long lastSupervisedPositionUs;
    private boolean lastSupervisedPositionValid;
    private int stuckWriteCounter;
    private long pendingWriteDurationUs;
    private long pendingWriteStartedRealtimeMs;
    private boolean pendingWriteShouldThrottle;

    public FireOsIec61937AudioOutput(
        AudioOutput audioOutput,
        PackerKind kind,
        EncodedBufferNormalizer normalizer,
        IecPacker packer,
        FireOsIec61937AudioOutputProvider provider) {
      super(audioOutput);
      this.kind = kind;
      this.normalizer = normalizer;
      this.packer = packer;
      this.provider = provider;
      this.lastPauseBurstDurationMs = C.LENGTH_UNSET;
      this.syntheticPauseRemainingUs = 0;
      this.lastPauseUpdateRealtimeMs = SystemClock.elapsedRealtime();
      this.playing = false;
      this.superviseAudioDelay = provider.shouldSuperviseFireOsIecAudioDelay();
      this.lastSupervisedPositionUs = 0;
      this.lastSupervisedPositionValid = false;
      this.stuckWriteCounter = 0;
      this.pendingWriteDurationUs = 0;
      this.pendingWriteStartedRealtimeMs = 0;
      this.pendingWriteShouldThrottle = false;
    }

    @Override
    public void play() {
      updateSyntheticPauseCompensation();
      lastPauseBurstDurationMs = C.LENGTH_UNSET;
      playing = true;
      resetSupervisedAudioDelayState();
      lastPauseUpdateRealtimeMs = SystemClock.elapsedRealtime();
      super.play();
    }

    @Override
    public long getPositionUs() {
      updateSyntheticPauseCompensation();
      return max(0L, super.getPositionUs() - syntheticPauseRemainingUs);
    }

    @Override
    public boolean write(ByteBuffer buffer, int encodedAccessUnitCount, long presentationTimeUs)
        throws WriteException {
      updateSyntheticPauseCompensation();
      if (pendingInputBuffer == null) {
        pendingInputBuffer = buffer;
        pendingAccessUnitCount = encodedAccessUnitCount;
        lastPauseBurstDurationMs = C.LENGTH_UNSET;
        try {
          @Nullable PreparedEncodedPacket preparedPacket = provider.consumePreparedPacket(buffer);
          pendingNormalizedBatch =
              preparedPacket != null && preparedPacket.encodedAccessUnitCount == encodedAccessUnitCount
                  ? preparedPacket.normalizedBatch
                  : normalizer.normalize(buffer.duplicate(), encodedAccessUnitCount);
          pendingPackedBuffer = packer.pack(checkNotNull(pendingNormalizedBatch));
        } catch (RuntimeException e) {
          logIecDebug(
              "write packer error"
                  + " kind="
                  + kind
                  + " accessUnits="
                  + encodedAccessUnitCount
                  + " inputBytes="
                  + buffer.remaining()
                  + " error="
                  + e.getClass().getSimpleName());
          provider.requestFallback(
              kind,
              packer.usesMultichannelCarrier(),
              provider.allowsStereoIecFallback(kind));
          throw new WriteException(AudioTrack.ERROR_BAD_VALUE, /* isRecoverable= */ true);
        }
        pendingWriteDurationUs = getCarrierWriteDurationUs(checkNotNull(pendingPackedBuffer).remaining());
        pendingWriteStartedRealtimeMs = SystemClock.elapsedRealtime();
        pendingWriteShouldThrottle = evaluateSupervisedAudioDelay(pendingWriteDurationUs);
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
          if (pendingWriteShouldThrottle) {
            maybeThrottleSupervisedAudioDelayWrite(
                pendingWriteDurationUs, pendingWriteStartedRealtimeMs);
          }
          buffer.position(buffer.limit());
          clearPendingState();
        }
        return fullyHandled;
      } catch (WriteException e) {
        logIecDebug(
            "write output error"
                + " kind="
                + kind
                + " errorCode="
                + e.errorCode
                + " recoverable="
                + e.isRecoverable
                + " packedRemaining="
                + (pendingPackedBuffer != null ? pendingPackedBuffer.remaining() : -1));
        if (e.isRecoverable && !isSupervisedAudioDelayReopen(e)) {
          provider.requestFallback(
              kind,
              packer.usesMultichannelCarrier(),
              provider.allowsStereoIecFallback(kind));
        }
        throw e;
      }
    }

    @Override
    public void pause() {
      updateSyntheticPauseCompensation();
      playing = false;
      resetSupervisedAudioDelayState();
      writePauseBurst(getPauseBurstDurationMs());
      super.pause();
    }

    @Override
    public void stop() {
      updateSyntheticPauseCompensation();
      playing = false;
      resetSupervisedAudioDelayState();
      writePauseBurst(getPauseBurstDurationMs());
      super.stop();
      syntheticPauseRemainingUs = 0;
      lastPauseBurstDurationMs = C.LENGTH_UNSET;
    }

    @Override
    public void flush() {
      updateSyntheticPauseCompensation();
      playing = false;
      resetSupervisedAudioDelayState();
      writePauseBurst(getPauseBurstDurationMs());
      clearPendingState();
      super.pause();
      super.flush();
      syntheticPauseRemainingUs = 0;
      lastPauseBurstDurationMs = C.LENGTH_UNSET;
      lastPauseUpdateRealtimeMs = SystemClock.elapsedRealtime();
    }

    private void writePauseBurst(int millis) {
      if (millis <= 0 || millis == lastPauseBurstDurationMs) {
        return;
      }
      ByteBuffer pauseBurst = packer.createPauseBurst(millis);
      if (!pauseBurst.hasRemaining()) {
        return;
      }
      long positionUs = getPositionUs();
      try {
        while (pauseBurst.hasRemaining()) {
          if (!super.write(pauseBurst, /* encodedAccessUnitCount= */ 1, positionUs)) {
            continue;
          }
        }
        lastPauseBurstDurationMs = millis;
        syntheticPauseRemainingUs += millis * 1_000L;
        lastPauseUpdateRealtimeMs = SystemClock.elapsedRealtime();
      } catch (WriteException e) {
        if (e.isRecoverable && !isSupervisedAudioDelayReopen(e)) {
          provider.requestFallback(
              kind,
              packer.usesMultichannelCarrier(),
              provider.allowsStereoIecFallback(kind));
        }
      }
    }

    private int getPauseBurstDurationMs() {
      long bufferFrames = getBufferSizeInFrames();
      int sampleRateHz = getSampleRate();
      if (bufferFrames <= 0 || sampleRateHz <= 0) {
        return 200;
      }
      long millis = (bufferFrames * 1_000L + sampleRateHz - 1) / sampleRateHz;
      return (int) max(50L, min((int) millis, 500));
    }

    private boolean evaluateSupervisedAudioDelay(long writeDurationUs) throws WriteException {
      if (!superviseAudioDelay || !playing || writeDurationUs <= 0) {
        return false;
      }
      long positionUs = max(0L, super.getPositionUs() - syntheticPauseRemainingUs);
      if (lastSupervisedPositionValid
          && positionUs > lastSupervisedPositionUs + SUPERVISE_AUDIO_DELAY_POSITION_TOLERANCE_US) {
        stuckWriteCounter = 0;
      } else {
        stuckWriteCounter++;
      }
      lastSupervisedPositionUs = positionUs;
      lastSupervisedPositionValid = true;

      long bufferDurationUs = getAudioTrackBufferDurationUs();
      if (bufferDurationUs <= 0) {
        return false;
      }

      long maxStuckDurationUs =
          max(
              bufferDurationUs * SUPERVISE_AUDIO_DELAY_MAX_BUFFER_MULTIPLIER,
              SUPERVISE_AUDIO_DELAY_MIN_STUCK_MS * 1_000L);
      long stuckDurationUs = stuckWriteCounter * writeDurationUs;
      if (stuckDurationUs > maxStuckDurationUs) {
        Log.w(
            TAG,
            "Supervised IEC audio delay forced sink reopen"
                + " kind="
                + kind
                + " stuckMs="
                + (stuckDurationUs / 1_000L)
                + " maxStuckMs="
                + (maxStuckDurationUs / 1_000L)
                + " bufferMs="
                + (bufferDurationUs / 1_000L));
        clearPendingState();
        resetSupervisedAudioDelayState();
        throw new WriteException(SUPERVISE_AUDIO_DELAY_REOPEN_ERROR, /* isRecoverable= */ true);
      }
      return stuckDurationUs >= bufferDurationUs;
    }

    private void maybeThrottleSupervisedAudioDelayWrite(
        long writeDurationUs, long writeStartedRealtimeMs) {
      if (writeDurationUs <= 0 || writeStartedRealtimeMs <= 0) {
        return;
      }
      long elapsedUs = max(0L, (SystemClock.elapsedRealtime() - writeStartedRealtimeMs) * 1_000L);
      long sleepUs = writeDurationUs - elapsedUs;
      if (sleepUs <= 0) {
        return;
      }
      long sleepMs = (sleepUs + 999L) / 1_000L;
      logIecDebug(
          "Supervised IEC audio delay throttling"
              + " kind="
              + kind
              + " sleepMs="
              + sleepMs
              + " stuckWrites="
              + stuckWriteCounter);
      SystemClock.sleep(sleepMs);
    }

    private long getAudioTrackBufferDurationUs() {
      long bufferFrames = getBufferSizeInFrames();
      int sampleRateHz = getSampleRate();
      if (bufferFrames <= 0 || sampleRateHz <= 0) {
        return 0L;
      }
      return (bufferFrames * 1_000_000L) / sampleRateHz;
    }

    private long getCarrierWriteDurationUs(int packetBytes) {
      int sampleRateHz = getSampleRate();
      if (packetBytes <= 0 || sampleRateHz <= 0) {
        return 0L;
      }
      int channelCount = packer.usesMultichannelCarrier() ? 8 : 2;
      int frameSizeBytes = channelCount * 2;
      long frameCount = packetBytes / frameSizeBytes;
      if (frameCount <= 0) {
        return 0L;
      }
      return (frameCount * 1_000_000L) / sampleRateHz;
    }

    private boolean isSupervisedAudioDelayReopen(WriteException error) {
      return error.isRecoverable && error.errorCode == SUPERVISE_AUDIO_DELAY_REOPEN_ERROR;
    }

    private void resetSupervisedAudioDelayState() {
      lastSupervisedPositionUs = 0;
      lastSupervisedPositionValid = false;
      stuckWriteCounter = 0;
    }

    private void clearPendingState() {
      pendingInputBuffer = null;
      pendingNormalizedBatch = null;
      pendingPackedBuffer = null;
      pendingAccessUnitCount = 0;
      pendingWriteDurationUs = 0;
      pendingWriteStartedRealtimeMs = 0;
      pendingWriteShouldThrottle = false;
    }

    private void updateSyntheticPauseCompensation() {
      long nowMs = SystemClock.elapsedRealtime();
      if (!playing || syntheticPauseRemainingUs <= 0) {
        lastPauseUpdateRealtimeMs = nowMs;
        return;
      }
      long elapsedUs = max(0L, (nowMs - lastPauseUpdateRealtimeMs) * 1_000L);
      syntheticPauseRemainingUs = max(0L, syntheticPauseRemainingUs - elapsedUs);
      lastPauseUpdateRealtimeMs = nowMs;
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

  private static int tryComputeDtsHdRepetitionPeriod(int sampleCount, int inputSampleRateHz) {
    if (sampleCount <= 0 || inputSampleRateHz <= 0) {
      return C.LENGTH_UNSET;
    }
    try {
      return computeDtsHdRepetitionPeriod(sampleCount, inputSampleRateHz);
    } catch (IllegalArgumentException e) {
      Log.w(
          TAG,
          "Unable to derive DTS-HD repetition period"
              + " sampleCount="
              + sampleCount
              + " sampleRate="
              + inputSampleRateHz,
          e);
      return C.LENGTH_UNSET;
    }
  }

  private static DtsDerivedParameters deriveDtsAccessUnitParameters(
      byte[] accessUnit, int inputSampleRateHz, boolean hd) {
    if (accessUnit.length < 4) {
      return new DtsDerivedParameters(
          inputSampleRateHz,
          /* sampleCount= */ 0,
          accessUnit.length,
          C.LENGTH_UNSET,
          /* littleEndian= */ false,
          /* dtsHdHeader= */ null,
          /* dtsUhdHeader= */ null);
    }
    int word = Util.getBigEndianInt(ByteBuffer.wrap(accessUnit, 0, 4), 0);
    @DtsUtil.FrameType int frameType = DtsUtil.getFrameType(word);
    switch (frameType) {
      case DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM:
        return deriveDtsHdAccessUnitParameters(accessUnit, inputSampleRateHz);
      case DtsUtil.FRAME_TYPE_UHD_SYNC:
      case DtsUtil.FRAME_TYPE_UHD_NON_SYNC:
        return deriveDtsUhdAccessUnitParameters(accessUnit, inputSampleRateHz);
      case DtsUtil.FRAME_TYPE_CORE:
      default:
        int sampleCount = DtsUtil.parseDtsAudioSampleCount(ByteBuffer.wrap(accessUnit));
        int payloadSize = DtsUtil.getDtsFrameSize(accessUnit);
        if (hd && payloadSize > 0 && payloadSize < accessUnit.length) {
          DtsDerivedParameters extensionParameters =
              deriveDtsHdExtensionParametersFromCombinedAccessUnit(
                  accessUnit, payloadSize, inputSampleRateHz);
          if (extensionParameters.repetitionPeriodFrames != C.LENGTH_UNSET) {
            return extensionParameters;
          }
        }
        int repetitionPeriodFrames =
            hd ? tryComputeDtsHdRepetitionPeriod(sampleCount, inputSampleRateHz) : C.LENGTH_UNSET;
        return new DtsDerivedParameters(
            inputSampleRateHz,
            sampleCount,
            payloadSize > 0 ? payloadSize : accessUnit.length,
            repetitionPeriodFrames,
            isLittleEndianDtsCoreFrame(accessUnit),
            /* dtsHdHeader= */ null,
            /* dtsUhdHeader= */ null);
    }
  }

  private static DtsDerivedParameters deriveDtsHdExtensionParametersFromCombinedAccessUnit(
      byte[] accessUnit, int coreSize, int inputSampleRateHz) {
    int extensionOffset = coreSize;
    int extensionBytesRemaining = accessUnit.length - extensionOffset;
    if (extensionBytesRemaining < 4) {
      return new DtsDerivedParameters(
          inputSampleRateHz,
          /* sampleCount= */ 0,
          accessUnit.length,
          C.LENGTH_UNSET,
          /* littleEndian= */ false,
          /* dtsHdHeader= */ null,
          /* dtsUhdHeader= */ null);
    }
    int extensionWord = Util.getBigEndianInt(ByteBuffer.wrap(accessUnit, extensionOffset, 4), 0);
    @DtsUtil.FrameType int extensionFrameType = DtsUtil.getFrameType(extensionWord);
    byte[] extensionPayload = copyRange(accessUnit, extensionOffset, extensionBytesRemaining);
    switch (extensionFrameType) {
      case DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM:
        return deriveDtsHdAccessUnitParameters(extensionPayload, inputSampleRateHz)
            .withPayloadSize(accessUnit.length);
      case DtsUtil.FRAME_TYPE_UHD_SYNC:
      case DtsUtil.FRAME_TYPE_UHD_NON_SYNC:
        return deriveDtsUhdAccessUnitParameters(extensionPayload, inputSampleRateHz)
            .withPayloadSize(accessUnit.length);
      default:
        return new DtsDerivedParameters(
            inputSampleRateHz,
            /* sampleCount= */ 0,
            accessUnit.length,
            C.LENGTH_UNSET,
            /* littleEndian= */ false,
            /* dtsHdHeader= */ null,
            /* dtsUhdHeader= */ null);
    }
  }

  private static DtsDerivedParameters deriveDtsHdAccessUnitParameters(
      byte[] accessUnit, int inputSampleRateHz) {
    try {
      int headerSize = DtsUtil.parseDtsHdHeaderSize(accessUnit);
      if (headerSize <= 0 || headerSize > accessUnit.length) {
        throw ParserException.createForMalformedContainer(
            "Invalid DTS-HD header size: " + headerSize, /* cause= */ null);
      }
      DtsUtil.DtsHeader header = DtsUtil.parseDtsHdHeader(copyRange(accessUnit, 0, headerSize));
      int effectiveSampleRateHz =
          header.sampleRate != Format.NO_VALUE ? header.sampleRate : inputSampleRateHz;
      int sampleCount = deriveSampleCountFromDurationUs(header.frameDurationUs, effectiveSampleRateHz);
      return new DtsDerivedParameters(
          effectiveSampleRateHz,
          sampleCount,
          accessUnit.length,
          tryComputeDtsHdRepetitionPeriod(sampleCount, effectiveSampleRateHz),
          /* littleEndian= */ false,
          header,
          /* dtsUhdHeader= */ null);
    } catch (ParserException | IllegalArgumentException e) {
      Log.w(TAG, "Unable to derive DTS-HD access unit parameters", e);
      return new DtsDerivedParameters(
          inputSampleRateHz,
          /* sampleCount= */ 0,
          accessUnit.length,
          C.LENGTH_UNSET,
          /* littleEndian= */ false,
          /* dtsHdHeader= */ null,
          /* dtsUhdHeader= */ null);
    }
  }

  private static DtsDerivedParameters deriveDtsUhdAccessUnitParameters(
      byte[] accessUnit, int inputSampleRateHz) {
    try {
      int headerSize = DtsUtil.parseDtsUhdHeaderSize(accessUnit);
      if (headerSize <= 0 || headerSize > accessUnit.length) {
        throw ParserException.createForMalformedContainer(
            "Invalid DTS-UHD header size: " + headerSize, /* cause= */ null);
      }
      DtsUtil.DtsHeader header =
          DtsUtil.parseDtsUhdHeader(copyRange(accessUnit, 0, headerSize), new AtomicInteger());
      int effectiveSampleRateHz =
          header.sampleRate != Format.NO_VALUE ? header.sampleRate : inputSampleRateHz;
      int sampleCount = deriveSampleCountFromDurationUs(header.frameDurationUs, effectiveSampleRateHz);
      return new DtsDerivedParameters(
          effectiveSampleRateHz,
          sampleCount,
          accessUnit.length,
          tryComputeDtsHdRepetitionPeriod(sampleCount, effectiveSampleRateHz),
          /* littleEndian= */ false,
          /* dtsHdHeader= */ null,
          header);
    } catch (ParserException | IllegalArgumentException e) {
      Log.w(TAG, "Unable to derive DTS-UHD access unit parameters", e);
      return new DtsDerivedParameters(
          inputSampleRateHz,
          /* sampleCount= */ 0,
          accessUnit.length,
          C.LENGTH_UNSET,
          /* littleEndian= */ false,
          /* dtsHdHeader= */ null,
          /* dtsUhdHeader= */ null);
    }
  }

  private static int deriveSampleCountFromDurationUs(long frameDurationUs, int sampleRateHz) {
    if (frameDurationUs == C.TIME_UNSET || sampleRateHz <= 0) {
      return 0;
    }
    return (int) ((frameDurationUs * sampleRateHz + (C.MICROS_PER_SECOND / 2)) / C.MICROS_PER_SECOND);
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
        return C.LENGTH_UNSET;
    }
  }

  private static int getTrueHdCarrierRateHz(int inputSampleRateHz) {
    switch (inputSampleRateHz) {
      case 48_000:
      case 96_000:
      case 192_000:
        return 192_000;
      case 44_100:
      case 88_200:
      case 176_400:
        return 176_400;
      default:
        return 192_000;
    }
  }

  private static boolean isEac3SyncFrame(Ac3Util.SyncFrameInfo syncFrameInfo) {
    return MimeTypes.AUDIO_E_AC3.equals(syncFrameInfo.mimeType)
        || MimeTypes.AUDIO_E_AC3_JOC.equals(syncFrameInfo.mimeType);
  }

  private static FrameSplitResult splitAc3AccessUnits(
      byte[] input, int encodedAccessUnitCount, @Nullable Ac3SyncParserState parserState) {
    if (parserState == null) {
      return splitAccessUnitsStrict(input, encodedAccessUnitCount, /* dts= */ false);
    }
    byte[] source = prependPending(input, parserState.pendingBytes);
    ArrayList<FrameSlice> accessUnits = new ArrayList<>();
    int accessUnitLimit = encodedAccessUnitCount > 0 ? encodedAccessUnitCount : Integer.MAX_VALUE;
    int offset = 0;
    int skippedBytes = 0;
    while (offset < source.length && accessUnits.size() < accessUnitLimit) {
      int bytesRemaining = source.length - offset;
      if (bytesRemaining < 6) {
        break;
      }
      if (isAc3SyncWord(source, offset)) {
        int size = parseAc3AccessUnitSize(source, offset, bytesRemaining);
        if (size > 0) {
          if (size > bytesRemaining) {
            break;
          }
          accessUnits.add(new FrameSlice(offset, size));
          offset += size;
          continue;
        }
        if (bytesRemaining < MAX_AC3_HEADER_SEARCH_BYTES) {
          break;
        }
      }
      offset++;
      skippedBytes++;
    }
    parserState.pendingBytes = copyTrailingBytes(source, offset, MAX_AC3_PENDING_BYTES);
    int trailingBytes = parserState.pendingBytes.length;
    if (accessUnits.isEmpty()) {
      Log.w(
          TAG,
          "Rejecting AC3/E-AC3 packet after parser sync failure"
              + " accessUnits="
              + encodedAccessUnitCount
              + " parsedBytes="
              + (source.length - trailingBytes)
              + "/"
              + source.length);
    }
    return new FrameSplitResult(source, accessUnits, skippedBytes, trailingBytes);
  }

  private static int parseAc3AccessUnitSize(byte[] input, int offset, int bytesRemaining) {
    if (bytesRemaining < 6) {
      return C.LENGTH_UNSET;
    }
    return Ac3Util.parseAc3SyncframeSize(copyRange(input, offset, min(bytesRemaining, 64)));
  }

  private static boolean isAc3SyncWord(byte[] input, int offset) {
    return (input[offset] & 0xFF) == 0x0B && (input[offset + 1] & 0xFF) == 0x77;
  }

  private static TrueHdSplitResult splitTrueHdAccessUnits(
      byte[] input, int encodedAccessUnitCount, TrueHdSyncParserState parserState) {
    byte[] source = prependPending(input, parserState.pendingBytes);
    ArrayList<ParsedTrueHdFrameSlice> accessUnits = new ArrayList<>();
    int accessUnitLimit = encodedAccessUnitCount > 0 ? encodedAccessUnitCount : Integer.MAX_VALUE;
    int offset = 0;
    int skippedBytes = 0;
    int possibleOffset = -1;
    parserState.needBytes = 0;
    while (accessUnits.size() < accessUnitLimit && offset < source.length) {
      int bytesRemaining = source.length - offset;
      TrueHdParseAttempt parseAttempt =
          tryParseTrueHdAccessUnit(source, offset, bytesRemaining, parserState);
      if (parseAttempt.status == TrueHdParseStatus.FRAME) {
        accessUnits.add(
            new ParsedTrueHdFrameSlice(offset, parseAttempt.frameSize, parseAttempt.sampleCount));
        offset += parseAttempt.frameSize;
        possibleOffset = -1;
        parserState.needBytes = 0;
        continue;
      }
      if (parseAttempt.status == TrueHdParseStatus.NEED_MORE) {
        // Match Kodi parser behavior: without a locked sync, keep scanning byte-wise and only
        // retain the candidate offset for the next input buffer.
        boolean shouldWaitForMore =
            parserState.hasSync
                || bytesRemaining < Ac3Util.TRUEHD_SYNCFRAME_PREFIX_LENGTH
                || isPotentialTrueHdMajorSync(source, offset);
        if (shouldWaitForMore) {
          parserState.needBytes = max(parserState.needBytes, parseAttempt.requiredBytes);
          break;
        }
        possibleOffset = offset;
        offset++;
        skippedBytes++;
        continue;
      }
      offset++;
      skippedBytes++;
    }
    int pendingOffset = possibleOffset >= 0 ? possibleOffset : offset;
    parserState.pendingBytes = copyTrailingBytes(source, pendingOffset, MAX_TRUEHD_PENDING_BYTES);
    int trailingBytes = parserState.pendingBytes.length;
    if (accessUnits.isEmpty()) {
      accessUnits.clear();
      Log.w(
          TAG,
          "Rejecting TrueHD packet after parser sync failure"
              + " accessUnits="
              + encodedAccessUnitCount
              + " parsedBytes="
              + (source.length - trailingBytes)
              + "/"
              + source.length);
      return new TrueHdSplitResult(source, accessUnits, skippedBytes, trailingBytes);
    }
    if (skippedBytes != 0 || trailingBytes != 0) {
      Log.w(
          TAG,
          "Dropping unsynced TrueHD bytes"
              + " skipped="
              + skippedBytes
              + " trailing="
              + trailingBytes
              + " parsedUnits="
              + accessUnits.size());
    }
    return new TrueHdSplitResult(source, accessUnits, skippedBytes, trailingBytes);
  }

  private static @Nullable ParsedTrueHdAccessUnitInfo parseTrueHdAccessUnitInfo(
      byte[] input, int offset, int bytesRemaining, TrueHdSyncParserState parserState) {
    TrueHdParseAttempt parseAttempt =
        tryParseTrueHdAccessUnit(input, offset, bytesRemaining, parserState);
    if (parseAttempt.status != TrueHdParseStatus.FRAME) {
      return null;
    }
    @Nullable ParsedTrueHdStreamInfo parsedStreamInfo = null;
    if ((readBigEndianInt(input, offset + 4) & 0xFFFFFFFE) == TRUEHD_FORMAT_MAJOR_SYNC
        && parserState.sampleRateHz > 0
        && parserState.channelCount > 0) {
      parsedStreamInfo = new ParsedTrueHdStreamInfo(parserState.sampleRateHz, parserState.channelCount);
    }
    return new ParsedTrueHdAccessUnitInfo(
        parseAttempt.frameSize, parseAttempt.sampleCount, parsedStreamInfo);
  }

  private static TrueHdParseAttempt tryParseTrueHdAccessUnit(
      byte[] input, int offset, int bytesRemaining, TrueHdSyncParserState parserState) {
    if (bytesRemaining < Ac3Util.TRUEHD_SYNCFRAME_PREFIX_LENGTH) {
      return TrueHdParseAttempt.needMore(Ac3Util.TRUEHD_SYNCFRAME_PREFIX_LENGTH);
    }
    int frameSize = ((input[offset] & 0x0F) << 8 | (input[offset + 1] & 0xFF)) << 1;
    if (frameSize < Ac3Util.TRUEHD_SYNCFRAME_PREFIX_LENGTH) {
      return TrueHdParseAttempt.noMatch();
    }
    if (frameSize > bytesRemaining) {
      return TrueHdParseAttempt.needMore(frameSize);
    }
    if ((readBigEndianInt(input, offset + 4) & 0xFFFFFFFE) == TRUEHD_FORMAT_MAJOR_SYNC) {
      return parseTrueHdMajorSyncAccessUnit(input, offset, bytesRemaining, frameSize, parserState);
    }
    return parseTrueHdSubframeAccessUnit(input, offset, bytesRemaining, frameSize, parserState);
  }

  private static TrueHdParseAttempt parseTrueHdMajorSyncAccessUnit(
      byte[] input,
      int offset,
      int bytesRemaining,
      int frameSize,
      TrueHdSyncParserState parserState) {
    if (bytesRemaining < 32) {
      return TrueHdParseAttempt.needMore(32);
    }
    int rateBits = (input[offset + 8] >>> 4) & 0x0F;
    if (rateBits == 0x0F) {
      return TrueHdParseAttempt.noMatch();
    }
    int majorSyncSize = getTrueHdMajorSyncSize(input, offset, bytesRemaining);
    if (majorSyncSize == C.LENGTH_UNSET || bytesRemaining < 4 + majorSyncSize) {
      return TrueHdParseAttempt.needMore(4 + max(majorSyncSize, 0));
    }
    if (!hasValidTrueHdMajorSyncCrc(input, offset, majorSyncSize)) {
      return TrueHdParseAttempt.noMatch();
    }
    int sampleRateHz = (((rateBits & 0x08) != 0) ? 44_100 : 48_000) << (rateBits & 0x07);
    int channelMap = ((input[offset + 10] & 0x1F) << 8) | (input[offset + 11] & 0xFF);
    if (channelMap == 0) {
      channelMap = ((input[offset + 9] & 0xFF) << 1) | ((input[offset + 10] >>> 7) & 0x01);
    }
    int channelCount = getTrueHdChannelCount(channelMap);
    if (channelCount <= 0) {
      return TrueHdParseAttempt.noMatch();
    }
    int sampleCount = 40 << (rateBits & 0x07);
    parserState.setMajorSync(
        rateBits,
        sampleRateHz,
        channelCount,
        sampleCount,
        (input[offset + 20] >>> 4) & 0x0F);
    return TrueHdParseAttempt.frame(frameSize, sampleCount);
  }

  private static TrueHdParseAttempt parseTrueHdSubframeAccessUnit(
      byte[] input,
      int offset,
      int bytesRemaining,
      int frameSize,
      TrueHdSyncParserState parserState) {
    if (!parserState.hasSync || parserState.sampleCount <= 0) {
      return TrueHdParseAttempt.noMatch();
    }
    int minimumParityBytes = max(4, parserState.substreams * 4);
    if (bytesRemaining < minimumParityBytes) {
      return TrueHdParseAttempt.needMore(minimumParityBytes);
    }
    if (!hasValidTrueHdSubframeParity(input, offset, bytesRemaining, parserState.substreams)) {
      parserState.resetSyncState();
      return TrueHdParseAttempt.noMatch();
    }
    return TrueHdParseAttempt.frame(frameSize, parserState.sampleCount);
  }

  private static int getTrueHdMajorSyncSize(byte[] input, int offset, int bytesRemaining) {
    if (bytesRemaining < 32) {
      return C.LENGTH_UNSET;
    }
    int majorSyncSize = 28;
    if ((input[offset + 29] & 0x01) != 0) {
      int extensionCount = (input[offset + 30] >>> 4) & 0x0F;
      majorSyncSize += 2 + extensionCount * 2;
    }
    return majorSyncSize;
  }

  private static boolean hasValidTrueHdMajorSyncCrc(
      byte[] input, int offset, int majorSyncSize) {
    int crc = computeTrueHdMajorSyncCrc(input, offset + 4, majorSyncSize - 4);
    crc ^= ((input[offset + majorSyncSize + 1] & 0xFF) << 8)
        | (input[offset + majorSyncSize] & 0xFF);
    int expectedCrc =
        ((input[offset + majorSyncSize + 3] & 0xFF) << 8)
            | (input[offset + majorSyncSize + 2] & 0xFF);
    return (crc & 0xFFFF) == expectedCrc;
  }

  private static boolean hasValidTrueHdSubframeParity(
      byte[] input, int offset, int bytesRemaining, int substreams) {
    int p = 0;
    int check = 0;
    for (int i = -1; i < substreams; i++) {
      if (p + 1 >= bytesRemaining) {
        return false;
      }
      check ^= input[offset + p++] & 0xFF;
      check ^= input[offset + p++] & 0xFF;
      if (i == -1 || (input[offset + p - 2] & 0x80) != 0) {
        if (p + 1 >= bytesRemaining) {
          return false;
        }
        check ^= input[offset + p++] & 0xFF;
        check ^= input[offset + p++] & 0xFF;
      }
    }
    return ((((check >> 4) ^ check) & 0x0F) == 0x0F);
  }

  private static int computeTrueHdMajorSyncCrc(byte[] input, int offset, int length) {
    int crc = 0;
    int end = offset + length;
    for (int i = offset; i < end; i++) {
      crc = TRUEHD_CRC_TABLE[(crc ^ (input[i] & 0xFF)) & 0xFF] ^ (crc >>> 8);
    }
    return crc & 0xFFFF;
  }

  private static int[] buildTrueHdCrcTable() {
    int[] table = new int[256];
    for (int i = 0; i < table.length; i++) {
      int c = i << 24;
      for (int j = 0; j < 8; j++) {
        c = (c << 1) ^ ((0x2D << 16) & (c >> 31));
      }
      table[i] = Integer.reverseBytes(c);
    }
    return table;
  }

  private static FrameSplitResult splitDtsAccessUnits(
      byte[] input, int encodedAccessUnitCount, @Nullable DtsSyncParserState parserState) {
    if (parserState == null) {
      return splitAccessUnitsStrict(input, encodedAccessUnitCount, /* dts= */ true);
    }
    byte[] source = prependPending(input, parserState.pendingBytes);
    ArrayList<FrameSlice> accessUnits = new ArrayList<>();
    int accessUnitLimit = encodedAccessUnitCount > 0 ? encodedAccessUnitCount : Integer.MAX_VALUE;
    int offset = 0;
    int skippedBytes = 0;
    while (offset < source.length && accessUnits.size() < accessUnitLimit) {
      int bytesRemaining = source.length - offset;
      if (bytesRemaining < 4) {
        break;
      }
      int size = parseDtsAccessUnitSize(source, offset, bytesRemaining);
      if (size > 0) {
        if (size > bytesRemaining) {
          break;
        }
        accessUnits.add(new FrameSlice(offset, size));
        offset += size;
        continue;
      }
      if (looksLikeDtsSync(source, offset) && bytesRemaining < MAX_DTS_HEADER_SEARCH_BYTES) {
        break;
      }
      offset++;
      skippedBytes++;
    }
    parserState.pendingBytes = copyTrailingBytes(source, offset, MAX_DTS_PENDING_BYTES);
    int trailingBytes = parserState.pendingBytes.length;
    if (accessUnits.isEmpty()) {
      Log.w(
          TAG,
          "Rejecting DTS packet after parser sync failure"
              + " accessUnits="
              + encodedAccessUnitCount
              + " parsedBytes="
              + (source.length - trailingBytes)
              + "/"
              + source.length);
    }
    return new FrameSplitResult(source, accessUnits, skippedBytes, trailingBytes);
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

  private static FrameSplitResult splitAccessUnitsStrict(
      byte[] input, int encodedAccessUnitCount, boolean dts) {
    ArrayList<FrameSlice> accessUnits = new ArrayList<>();
    int offset = 0;
    for (int i = 0; i < encodedAccessUnitCount && offset < input.length; i++) {
      int size =
          dts
              ? parseDtsAccessUnitSize(input, offset, input.length - offset)
              : parseAc3AccessUnitSize(input, offset, input.length - offset);
      if (size <= 0 || offset + size > input.length) {
        break;
      }
      accessUnits.add(new FrameSlice(offset, size));
      offset += size;
    }
    if (accessUnits.isEmpty() || offset != input.length) {
      accessUnits.clear();
      Log.w(
          TAG,
          (dts ? "Rejecting DTS packet after parser sync failure"
                  : "Rejecting AC3/E-AC3 packet after parser sync failure")
              + " accessUnits="
              + encodedAccessUnitCount
              + " parsedBytes="
              + offset
              + "/"
              + input.length);
    }
    return new FrameSplitResult(input, accessUnits, /* skippedBytes= */ 0, input.length - offset);
  }

  private static byte[] prependPending(byte[] input, byte[] pending) {
    if (pending.length == 0) {
      return input;
    }
    byte[] source = new byte[pending.length + input.length];
    System.arraycopy(pending, 0, source, 0, pending.length);
    System.arraycopy(input, 0, source, pending.length, input.length);
    return source;
  }

  private static byte[] copyTrailingBytes(byte[] source, int offset, int maxBytes) {
    if (offset >= source.length) {
      return new byte[0];
    }
    int pendingLength = source.length - offset;
    if (pendingLength > maxBytes) {
      offset = source.length - maxBytes;
      pendingLength = maxBytes;
    }
    return copyRange(source, offset, pendingLength);
  }

  private static boolean looksLikeDtsSync(byte[] input, int offset) {
    int firstByte = input[offset] & 0xFF;
    if (firstByte != (DTS_SYNC_FIRST_BYTE_BE & 0xFF)
        && firstByte != (DTS_SYNC_FIRST_BYTE_14B_BE & 0xFF)
        && firstByte != (DTS_SYNC_FIRST_BYTE_LE & 0xFF)
        && firstByte != (DTS_SYNC_FIRST_BYTE_14B_LE & 0xFF)) {
      return false;
    }
    int word = Util.getBigEndianInt(ByteBuffer.wrap(input, offset, 4), 0);
    @DtsUtil.FrameType int frameType = DtsUtil.getFrameType(word);
    return frameType == DtsUtil.FRAME_TYPE_CORE
        || frameType == DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM
        || frameType == DtsUtil.FRAME_TYPE_UHD_SYNC
        || frameType == DtsUtil.FRAME_TYPE_UHD_NON_SYNC;
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

  private static ByteBuffer createPauseBurstPacket(
      int channelCount,
      int outputRateHz,
      int repetitionPeriodFrames,
      int encodedRateHz,
      int millis) {
    int frameSizeBytes = channelCount * 2;
    int periodBytes = repetitionPeriodFrames * frameSizeBytes;
    if (periodBytes <= 0 || outputRateHz <= 0 || encodedRateHz <= 0 || millis <= 0) {
      return EMPTY_BUFFER;
    }
    double periodTimeMs = (double) repetitionPeriodFrames / outputRateHz * 1000.0;
    int periodsNeeded = (int) (millis / periodTimeMs);
    if (periodsNeeded <= 0) {
      periodsNeeded = 1;
    }
    int maxPeriods = IEC61937_TRUEHD_PACKET_BYTES / periodBytes;
    periodsNeeded = min(periodsNeeded, maxPeriods);
    if (periodsNeeded <= 0) {
      return EMPTY_BUFFER;
    }
    ByteBuffer output = allocatePacket(periodsNeeded * periodBytes);
    putPreamble(output, /* dataType= */ 3, /* lengthCode= */ 32);
    int gap = encodedRateHz * millis / 1000;
    output.putShort((short) gap);
    while (output.position() < periodBytes) {
      output.put((byte) 0);
    }
    int firstPeriodLimit = periodBytes;
    while (output.position() < periodsNeeded * periodBytes) {
      ByteBuffer duplicate = output.duplicate();
      duplicate.position(0);
      duplicate.limit(firstPeriodLimit);
      output.put(duplicate);
    }
    output.flip();
    return output;
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

  private static void writeLittleEndianShort(byte[] target, int offset, int value) {
    target[offset] = (byte) (value & 0xFF);
    target[offset + 1] = (byte) ((value >>> 8) & 0xFF);
  }

  private static void putMaybeWordSwapped(
      ByteBuffer output, byte[] data, int offset, int length, boolean swapWords) {
    if (swapWords) {
      putWordSwapped(output, data, offset, length);
    } else {
      output.put(data, offset, length);
      if ((length & 1) != 0) {
        output.put((byte) 0);
      }
    }
  }

  private static void putWordSwapped(ByteBuffer output, byte[] data, int offset, int length) {
    int pairs = length & ~1;
    for (int i = 0; i < pairs; i += 2) {
      output.put(data[offset + i + 1]);
      output.put(data[offset + i]);
    }
    if ((length & 1) != 0) {
      output.put((byte) 0);
      output.put(data[offset + length - 1]);
    }
  }

  private static void zeroFillToLimit(ByteBuffer output, int limit) {
    while (output.position() < limit) {
      output.put((byte) 0);
    }
  }

  private static void swapWordsInPlace(byte[] data, int offset, int length) {
    int pairs = length & ~1;
    for (int i = 0; i < pairs; i += 2) {
      byte temp = data[offset + i];
      data[offset + i] = data[offset + i + 1];
      data[offset + i + 1] = temp;
    }
  }

  private static boolean isLittleEndianDtsCoreFrame(byte[] frame) {
    if (frame.length == 0) {
      return false;
    }
    return frame[0] == DTS_SYNC_FIRST_BYTE_LE || frame[0] == DTS_SYNC_FIRST_BYTE_14B_LE;
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

  private static final class DtsDerivedParameters {
    public final int sampleRateHz;
    public final int sampleCount;
    public final int payloadSize;
    public final int repetitionPeriodFrames;
    public final boolean littleEndian;
    @Nullable public final DtsUtil.DtsHeader dtsHdHeader;
    @Nullable public final DtsUtil.DtsHeader dtsUhdHeader;

    public DtsDerivedParameters(
        int sampleRateHz,
        int sampleCount,
        int payloadSize,
        int repetitionPeriodFrames,
        boolean littleEndian,
        @Nullable DtsUtil.DtsHeader dtsHdHeader,
        @Nullable DtsUtil.DtsHeader dtsUhdHeader) {
      this.sampleRateHz = sampleRateHz;
      this.sampleCount = sampleCount;
      this.payloadSize = payloadSize;
      this.repetitionPeriodFrames = repetitionPeriodFrames;
      this.littleEndian = littleEndian;
      this.dtsHdHeader = dtsHdHeader;
      this.dtsUhdHeader = dtsUhdHeader;
    }

    public DtsDerivedParameters withPayloadSize(int payloadSize) {
      return new DtsDerivedParameters(
          sampleRateHz,
          sampleCount,
          payloadSize,
          repetitionPeriodFrames,
          littleEndian,
          dtsHdHeader,
          dtsUhdHeader);
    }
  }

  private static final class ParsedTrueHdStreamInfo {
    public final int sampleRateHz;
    public final int channelCount;

    public ParsedTrueHdStreamInfo(int sampleRateHz, int channelCount) {
      this.sampleRateHz = sampleRateHz;
      this.channelCount = channelCount;
    }
  }

  @VisibleForTesting
  /* package */ static final class TestAccessUnit {
    public final byte[] data;
    public final int sampleCount;
    public final int payloadSize;
    public final int repetitionPeriodFrames;
    public final int bitstreamMode;
    public final boolean littleEndian;

    public TestAccessUnit(
        byte[] data,
        int sampleCount,
        int payloadSize,
        int repetitionPeriodFrames,
        int bitstreamMode,
        boolean littleEndian) {
      this.data = data;
      this.sampleCount = sampleCount;
      this.payloadSize = payloadSize;
      this.repetitionPeriodFrames = repetitionPeriodFrames;
      this.bitstreamMode = bitstreamMode;
      this.littleEndian = littleEndian;
    }
  }

  private static final class FrameSlice {
    public final int offset;
    public final int length;

    public FrameSlice(int offset, int length) {
      this.offset = offset;
      this.length = length;
    }
  }

  private static final class FrameSplitResult {
    public final byte[] source;
    public final List<FrameSlice> accessUnits;
    public final int skippedBytes;
    public final int trailingBytes;

    public FrameSplitResult(
        byte[] source, List<FrameSlice> accessUnits, int skippedBytes, int trailingBytes) {
      this.source = source;
      this.accessUnits = accessUnits;
      this.skippedBytes = skippedBytes;
      this.trailingBytes = trailingBytes;
    }
  }

  private static final class Ac3SyncParserState {
    public byte[] pendingBytes = new byte[0];

    public void reset() {
      pendingBytes = new byte[0];
    }
  }

  private static final class DtsSyncParserState {
    public byte[] pendingBytes = new byte[0];

    public void reset() {
      pendingBytes = new byte[0];
    }
  }

  private enum TrueHdParseStatus {
    FRAME,
    NEED_MORE,
    NO_MATCH
  }

  private static final class TrueHdParseAttempt {
    public final TrueHdParseStatus status;
    public final int frameSize;
    public final int sampleCount;
    public final int requiredBytes;

    private TrueHdParseAttempt(
        TrueHdParseStatus status, int frameSize, int sampleCount, int requiredBytes) {
      this.status = status;
      this.frameSize = frameSize;
      this.sampleCount = sampleCount;
      this.requiredBytes = requiredBytes;
    }

    public static TrueHdParseAttempt frame(int frameSize, int sampleCount) {
      return new TrueHdParseAttempt(
          TrueHdParseStatus.FRAME, frameSize, sampleCount, /* requiredBytes= */ 0);
    }

    public static TrueHdParseAttempt needMore(int requiredBytes) {
      return new TrueHdParseAttempt(
          TrueHdParseStatus.NEED_MORE,
          /* frameSize= */ 0,
          /* sampleCount= */ 0,
          max(requiredBytes, Ac3Util.TRUEHD_SYNCFRAME_PREFIX_LENGTH));
    }

    public static TrueHdParseAttempt noMatch() {
      return new TrueHdParseAttempt(
          TrueHdParseStatus.NO_MATCH,
          /* frameSize= */ 0,
          /* sampleCount= */ 0,
          /* requiredBytes= */ 0);
    }
  }

  private static final class ParsedTrueHdFrameSlice {
    public final int offset;
    public final int length;
    public final int sampleCount;

    public ParsedTrueHdFrameSlice(int offset, int length, int sampleCount) {
      this.offset = offset;
      this.length = length;
      this.sampleCount = sampleCount;
    }
  }

  private static final class TrueHdSplitResult {
    public final byte[] source;
    public final List<ParsedTrueHdFrameSlice> accessUnits;
    public final int skippedBytes;
    public final int trailingBytes;

    public TrueHdSplitResult(
        byte[] source, List<ParsedTrueHdFrameSlice> accessUnits, int skippedBytes, int trailingBytes) {
      this.source = source;
      this.accessUnits = accessUnits;
      this.skippedBytes = skippedBytes;
      this.trailingBytes = trailingBytes;
    }
  }

  private static final class ParsedTrueHdAccessUnitInfo {
    public final int frameSize;
    public final int sampleCount;
    @Nullable public final ParsedTrueHdStreamInfo streamInfo;

    public ParsedTrueHdAccessUnitInfo(
        int frameSize, int sampleCount, @Nullable ParsedTrueHdStreamInfo streamInfo) {
      this.frameSize = frameSize;
      this.sampleCount = sampleCount;
      this.streamInfo = streamInfo;
    }
  }

  private static final class TrueHdNormalizationResult {
    public final NormalizedPacketBatch batch;

    public TrueHdNormalizationResult(NormalizedPacketBatch batch) {
      this.batch = batch;
    }
  }

  private static final class TrueHdSyncParserState {
    public boolean hasSync;
    public int substreams;
    public int rateBits;
    public int sampleRateHz;
    public int channelCount;
    public int sampleCount;
    public int needBytes;
    public byte[] pendingBytes = new byte[0];

    public TrueHdSyncParserState copy() {
      TrueHdSyncParserState copy = new TrueHdSyncParserState();
      copy.copyFrom(this);
      return copy;
    }

    public void copyFrom(TrueHdSyncParserState other) {
      hasSync = other.hasSync;
      substreams = other.substreams;
      rateBits = other.rateBits;
      sampleRateHz = other.sampleRateHz;
      channelCount = other.channelCount;
      sampleCount = other.sampleCount;
      needBytes = other.needBytes;
      pendingBytes =
          other.pendingBytes.length == 0
              ? new byte[0]
              : copyRange(other.pendingBytes, 0, other.pendingBytes.length);
    }

    public void setMajorSync(
        int rateBits, int sampleRateHz, int channelCount, int sampleCount, int substreams) {
      this.hasSync = true;
      this.rateBits = rateBits;
      this.sampleRateHz = sampleRateHz;
      this.channelCount = channelCount;
      this.sampleCount = sampleCount;
      this.substreams = substreams;
      this.needBytes = 0;
    }

    public void resetSyncState() {
      hasSync = false;
      substreams = 0;
      rateBits = 0;
      sampleRateHz = 0;
      channelCount = 0;
      sampleCount = 0;
      needBytes = 0;
    }

    public void reset() {
      resetSyncState();
      pendingBytes = new byte[0];
    }
  }
}
