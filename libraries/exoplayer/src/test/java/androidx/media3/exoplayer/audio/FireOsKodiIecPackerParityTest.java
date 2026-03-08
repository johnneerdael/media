/*
 * Copyright 2026 The Android Open Source Project
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

import static com.google.common.truth.Truth.assertThat;

import androidx.media3.common.C;
import androidx.media3.exoplayer.audio.FireOsIec61937AudioOutputProvider.PackerKind;
import androidx.media3.exoplayer.audio.FireOsIec61937AudioOutputProvider.TestAccessUnit;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.util.ArrayList;
import java.util.List;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

/** Byte-level IEC/MAT parity checks against Kodi-derived reference packers. */
@RunWith(AndroidJUnit4.class)
@Config(sdk = 33)
public final class FireOsKodiIecPackerParityTest {

  private static final int IEC61937_PACKET_SYNC_WORD_1 = 0xF872;
  private static final int IEC61937_PACKET_SYNC_WORD_2 = 0x4E1F;
  private static final int IEC61937_AC3 = 0x01;
  private static final int IEC61937_DTS1 = 0x0B;
  private static final int IEC61937_DTSHD = 0x11;
  private static final int IEC61937_E_AC3 = 0x15;
  private static final int IEC61937_TRUEHD = 0x16;
  private static final int IEC61937_AC3_PACKET_BYTES = 6_144;
  private static final int IEC61937_E_AC3_PACKET_BYTES = 24_576;
  private static final int IEC61937_TRUEHD_PACKET_BYTES = 61_440;
  private static final int IEC61937_TRUEHD_LENGTH_CODE = 61_424;
  private static final int TRUEHD_FORMAT_MAJOR_SYNC = 0xF8726FBA;
  private static final int TRUEHD_MAT_BUFFER_LIMIT = IEC61937_TRUEHD_PACKET_BYTES - 24;
  private static final int TRUEHD_MAT_MIDDLE_POSITION = 30_708 + 8;
  private static final byte[] DTS_HD_START_CODE =
      new byte[] {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, (byte) 0xFE, (byte) 0xFE};
  private static final byte[] TRUEHD_SYNCFRAME_HEADER =
      new byte[] {
        (byte) 0xC0, 0x75, 0x04, (byte) 0xD8, (byte) 0xF8, 0x72, 0x6F, (byte) 0xBA, 0x00,
        (byte) 0x97, (byte) 0xC0, 0x0F, (byte) 0xB7, 0x52, 0x00, 0x00
      };
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

  @Test
  public void packAc3_matchesKodiReferenceBytes() {
    byte[] accessUnit = new byte[] {0x11, 0x22, 0x33, 0x44, 0x55, (byte) 0xA6, 0x77};

    byte[] actual =
        FireOsIec61937AudioOutputProvider.packAccessUnitsForTesting(
            PackerKind.AC3,
            /* inputSampleRateHz= */ 48_000,
            /* reportedAccessUnitCount= */ 1,
            /* keepMultichannelCarrier= */ false,
            /* streamDtsPeriodFrames= */ C.LENGTH_UNSET,
            new TestAccessUnit(
                accessUnit,
                /* sampleCount= */ 1_536,
                accessUnit.length,
                C.LENGTH_UNSET,
                accessUnit[5] & 0x7,
                /* littleEndian= */ false));

    assertThat(actual).isEqualTo(packKodiAc3(accessUnit));
  }

  @Test
  public void packEac3_matchesKodiReferenceBytes() {
    byte[] accessUnit =
        new byte[] {0x01, 0x23, 0x45, 0x67, (byte) 0x89, (byte) 0xAB, (byte) 0xCD, 0x55, 0x66};

    byte[] actual =
        FireOsIec61937AudioOutputProvider.packAccessUnitsForTesting(
            PackerKind.E_AC3,
            /* inputSampleRateHz= */ 48_000,
            /* reportedAccessUnitCount= */ 1,
            /* keepMultichannelCarrier= */ false,
            /* streamDtsPeriodFrames= */ C.LENGTH_UNSET,
            new TestAccessUnit(
                accessUnit,
                /* sampleCount= */ 1_536,
                accessUnit.length,
                C.LENGTH_UNSET,
                /* bitstreamMode= */ 0,
                /* littleEndian= */ false));

    assertThat(actual).isEqualTo(packKodiEac3(accessUnit));
  }

  @Test
  public void packDtsCore_matchesKodiReferenceBytes() {
    byte[] accessUnit =
        new byte[] {
          (byte) 0x7F, (byte) 0xFE, (byte) 0x80, 0x01, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70
        };

    byte[] actual =
        FireOsIec61937AudioOutputProvider.packAccessUnitsForTesting(
            PackerKind.DTS_CORE,
            /* inputSampleRateHz= */ 48_000,
            /* reportedAccessUnitCount= */ 1,
            /* keepMultichannelCarrier= */ false,
            /* streamDtsPeriodFrames= */ C.LENGTH_UNSET,
            new TestAccessUnit(
                accessUnit,
                /* sampleCount= */ 512,
                accessUnit.length,
                C.LENGTH_UNSET,
                /* bitstreamMode= */ 0,
                /* littleEndian= */ false));

    assertThat(actual).isEqualTo(packKodiDtsCore(accessUnit, /* sampleCount= */ 512, false));
  }

  @Test
  public void packDtsHd_matchesKodiReferenceBytes() {
    byte[] accessUnit =
        new byte[] {
          0x01, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, (byte) 0x80, (byte) 0x90, (byte) 0xA0
        };

    byte[] actual =
        FireOsIec61937AudioOutputProvider.packAccessUnitsForTesting(
            PackerKind.DTS_HD,
            /* inputSampleRateHz= */ 48_000,
            /* reportedAccessUnitCount= */ 1,
            /* keepMultichannelCarrier= */ false,
            /* streamDtsPeriodFrames= */ 4_096,
            new TestAccessUnit(
                accessUnit,
                /* sampleCount= */ 4_096,
                accessUnit.length,
                /* repetitionPeriodFrames= */ 4_096,
                /* bitstreamMode= */ 0,
                /* littleEndian= */ false));

    assertThat(actual).isEqualTo(packKodiDtsHd(accessUnit, /* repetitionPeriodFrames= */ 4_096));
  }

  @Test
  public void packTrueHdMatchesKodiReferenceBytes() {
    List<byte[]> syncframes = new ArrayList<>();
    TestAccessUnit[] accessUnits = new TestAccessUnit[30];
    for (int i = 0; i < accessUnits.length; i++) {
      byte[] syncframe =
          createTrueHdSyncframe(/* frameSizeBytes= */ 120, /* frameTime= */ i * 40);
      syncframes.add(syncframe);
      accessUnits[i] =
          new TestAccessUnit(
              syncframe,
              /* sampleCount= */ 40,
              syncframe.length,
              C.LENGTH_UNSET,
              /* bitstreamMode= */ 0,
              /* littleEndian= */ false);
    }

    byte[] actual =
        FireOsIec61937AudioOutputProvider.packAccessUnitsForTesting(
            PackerKind.TRUEHD,
            /* inputSampleRateHz= */ 48_000,
            /* reportedAccessUnitCount= */ accessUnits.length,
            /* keepMultichannelCarrier= */ true,
            /* streamDtsPeriodFrames= */ C.LENGTH_UNSET,
            accessUnits);
    byte[] expected = new KodiReferenceMatPacker().pack(syncframes);

    assertThat(actual.length).isGreaterThan(0);
    assertThat(actual).isEqualTo(expected);
  }

  private static byte[] packKodiAc3(byte[] accessUnit) {
    byte[] output = new byte[IEC61937_AC3_PACKET_BYTES];
    writePreamble(output, IEC61937_AC3 | ((accessUnit[5] & 0x7) << 8), accessUnit.length << 3);
    writeWordSwapped(output, 8, accessUnit, accessUnit.length);
    return output;
  }

  private static byte[] packKodiEac3(byte[] accessUnit) {
    byte[] output = new byte[IEC61937_E_AC3_PACKET_BYTES];
    writePreamble(output, IEC61937_E_AC3, accessUnit.length);
    writeWordSwapped(output, 8, accessUnit, accessUnit.length);
    return output;
  }

  private static byte[] packKodiDtsCore(byte[] accessUnit, int sampleCount, boolean littleEndian) {
    int frameSize = sampleCount * 4;
    byte[] output = new byte[frameSize];
    int payloadOffset = 0;
    if (accessUnit.length != frameSize) {
      writePreamble(output, IEC61937_DTS1, accessUnit.length << 3);
      payloadOffset = 8;
    }
    writeMaybeWordSwapped(output, payloadOffset, accessUnit, accessUnit.length, !littleEndian);
    return output;
  }

  private static byte[] packKodiDtsHd(byte[] accessUnit, int repetitionPeriodFrames) {
    int subtype = getKodiDtsHdSubtype(repetitionPeriodFrames);
    int burstSize = repetitionPeriodFrames * 4;
    byte[] payload = new byte[DTS_HD_START_CODE.length + 2 + accessUnit.length];
    System.arraycopy(DTS_HD_START_CODE, 0, payload, 0, DTS_HD_START_CODE.length);
    payload[DTS_HD_START_CODE.length] = (byte) ((accessUnit.length >>> 8) & 0xFF);
    payload[DTS_HD_START_CODE.length + 1] = (byte) (accessUnit.length & 0xFF);
    System.arraycopy(accessUnit, 0, payload, DTS_HD_START_CODE.length + 2, accessUnit.length);

    byte[] output = new byte[burstSize];
    writePreamble(
        output,
        IEC61937_DTSHD | (subtype << 8),
        alignTo16(payload.length + 8) - 8);
    writeWordSwapped(output, 8, payload, payload.length);
    return output;
  }

  private static byte[] createTrueHdSyncframe(int frameSizeBytes, int frameTime) {
    byte[] frame = new byte[frameSizeBytes];
    System.arraycopy(TRUEHD_SYNCFRAME_HEADER, 0, frame, 0, TRUEHD_SYNCFRAME_HEADER.length);
    int frameSizeWords = frameSizeBytes / 2;
    frame[0] =
        (byte)
            ((TRUEHD_SYNCFRAME_HEADER[0] & 0xF0) | ((frameSizeWords >>> 8) & 0x0F));
    frame[1] = (byte) (frameSizeWords & 0xFF);
    frame[2] = (byte) ((frameTime >>> 8) & 0xFF);
    frame[3] = (byte) (frameTime & 0xFF);
    return frame;
  }

  private static void writePreamble(byte[] output, int dataType, int lengthCode) {
    writeLittleEndianShort(output, 0, IEC61937_PACKET_SYNC_WORD_1);
    writeLittleEndianShort(output, 2, IEC61937_PACKET_SYNC_WORD_2);
    writeLittleEndianShort(output, 4, dataType);
    writeLittleEndianShort(output, 6, lengthCode);
  }

  private static void writeMaybeWordSwapped(
      byte[] output, int outputOffset, byte[] data, int length, boolean swapWords) {
    if (!swapWords) {
      System.arraycopy(data, 0, output, outputOffset, length);
      if ((length & 1) != 0) {
        output[outputOffset + length] = 0;
      }
      return;
    }
    writeWordSwapped(output, outputOffset, data, length);
  }

  private static void writeWordSwapped(byte[] output, int outputOffset, byte[] data, int length) {
    int pairs = length & ~1;
    for (int i = 0; i < pairs; i += 2) {
      output[outputOffset + i] = data[i + 1];
      output[outputOffset + i + 1] = data[i];
    }
    if ((length & 1) != 0) {
      output[outputOffset + pairs] = 0;
      output[outputOffset + pairs + 1] = data[length - 1];
    }
  }

  private static int getKodiDtsHdSubtype(int repetitionPeriodFrames) {
    switch (repetitionPeriodFrames) {
      case 512:
        return 0;
      case 1_024:
        return 1;
      case 2_048:
        return 2;
      case 4_096:
        return 3;
      case 8_192:
        return 4;
      case 16_384:
        return 5;
      default:
        throw new IllegalArgumentException(
            "Unsupported DTS-HD repetition period: " + repetitionPeriodFrames);
    }
  }

  private static int alignTo16(int value) {
    return (value + 15) & ~15;
  }

  private static int align(int value, int alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
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

  private static void writeLittleEndianShort(byte[] target, int offset, int value) {
    target[offset] = (byte) (value & 0xFF);
    target[offset + 1] = (byte) ((value >>> 8) & 0xFF);
  }

  private static final class KodiReferenceMatPacker {
    private final List<byte[]> outputFrames;
    private byte[] matBuffer;
    private int matBufferCount;
    private boolean initialized;
    private int rateBits;
    private int previousFrameTime;
    private boolean previousFrameTimeValid;
    private int previousMatFrameSize;
    private int currentMatFrameSize;
    private int padding;

    public KodiReferenceMatPacker() {
      outputFrames = new ArrayList<>();
      matBuffer = new byte[0];
      matBufferCount = 0;
    }

    public byte[] pack(List<byte[]> syncframes) {
      for (byte[] syncframe : syncframes) {
        packTrueHd(syncframe);
      }
      int totalBytes = 0;
      for (byte[] outputFrame : outputFrames) {
        totalBytes += outputFrame.length;
      }
      byte[] output = new byte[totalBytes];
      int offset = 0;
      for (byte[] outputFrame : outputFrames) {
        System.arraycopy(outputFrame, 0, output, offset, outputFrame.length);
        offset += outputFrame.length;
      }
      return output;
    }

    private void packTrueHd(byte[] syncframe) {
      if (syncframe.length < 10) {
        return;
      }
      if (readBigEndianInt(syncframe, 4) == TRUEHD_FORMAT_MAJOR_SYNC) {
        rateBits = (syncframe[8] >>> 4) & 0x0F;
      } else if (!previousFrameTimeValid) {
        return;
      }

      int frameTime = readUnsignedShort(syncframe, 2);
      int spaceSize = 0;
      if (previousFrameTimeValid) {
        spaceSize = ((frameTime - previousFrameTime) & 0xFFFF) * (64 >> (rateBits & 7));
      }
      if (previousFrameTimeValid && spaceSize < previousMatFrameSize) {
        spaceSize = align(previousMatFrameSize, 64 >> (rateBits & 7));
      }
      padding += spaceSize - previousMatFrameSize;
      if (padding > IEC61937_TRUEHD_PACKET_BYTES * 5) {
        initialized = true;
        rateBits = 0;
        previousFrameTime = 0;
        previousFrameTimeValid = false;
        previousMatFrameSize = 0;
        currentMatFrameSize = 0;
        padding = 0;
        matBuffer = new byte[0];
        matBufferCount = 0;
        outputFrames.clear();
        return;
      }

      previousFrameTime = frameTime;
      previousFrameTimeValid = true;

      if (matBufferCount == 0) {
        writeHeader();
        if (!initialized) {
          initialized = true;
          currentMatFrameSize = 0;
        }
      }

      while (padding > 0) {
        writePadding();
        if (padding == 0 && matBufferCount != IEC61937_TRUEHD_PACKET_BYTES) {
          break;
        }
        if (matBufferCount == IEC61937_TRUEHD_PACKET_BYTES) {
          flushPacket();
          writeHeader();
        }
      }

      int remaining = fillDataBuffer(syncframe, 0, syncframe.length, /* paddingFill= */ false);
      if (remaining != 0 || matBufferCount == IEC61937_TRUEHD_PACKET_BYTES) {
        flushPacket();
        if (remaining > 0) {
          writeHeader();
          int written = syncframe.length - remaining;
          remaining = fillDataBuffer(syncframe, written, remaining, /* paddingFill= */ false);
          if (remaining != 0) {
            throw new IllegalStateException("Remaining TrueHD data after MAT flush");
          }
        }
      }

      previousMatFrameSize = currentMatFrameSize;
      currentMatFrameSize = 0;
    }

    private void writeHeader() {
      matBuffer = new byte[IEC61937_TRUEHD_PACKET_BYTES];
      System.arraycopy(TRUEHD_MAT_START_CODE, 0, matBuffer, 8, TRUEHD_MAT_START_CODE.length);
      int headerSize = 8 + TRUEHD_MAT_START_CODE.length;
      matBufferCount = headerSize;
      currentMatFrameSize += headerSize;
      if (padding > 0) {
        if (padding > headerSize) {
          padding -= headerSize;
          currentMatFrameSize = 0;
        } else {
          currentMatFrameSize = headerSize - padding;
          padding = 0;
        }
      }
    }

    private void writePadding() {
      if (padding == 0) {
        return;
      }
      int remaining = fillDataBuffer(null, 0, padding, /* paddingFill= */ true);
      if (remaining >= 0) {
        padding = remaining;
        currentMatFrameSize = 0;
      } else {
        padding = 0;
        currentMatFrameSize = -remaining;
      }
    }

    private int fillDataBuffer(
        byte[] data, int dataOffset, int size, boolean paddingFill) {
      if (matBufferCount >= TRUEHD_MAT_BUFFER_LIMIT) {
        return size;
      }
      int remaining = size;
      if (matBufferCount <= TRUEHD_MAT_MIDDLE_POSITION
          && matBufferCount + size > TRUEHD_MAT_MIDDLE_POSITION) {
        int bytesBefore = TRUEHD_MAT_MIDDLE_POSITION - matBufferCount;
        appendData(data, dataOffset, bytesBefore, paddingFill);
        remaining -= bytesBefore;
        appendLiteral(TRUEHD_MAT_MIDDLE_CODE);
        if (paddingFill) {
          remaining -= TRUEHD_MAT_MIDDLE_CODE.length;
        }
        if (remaining > 0) {
          int nextDataOffset = paddingFill ? dataOffset : dataOffset + bytesBefore;
          remaining = fillDataBuffer(data, nextDataOffset, remaining, paddingFill);
        }
        return remaining;
      }
      if (matBufferCount + size >= TRUEHD_MAT_BUFFER_LIMIT) {
        int bytesBefore = TRUEHD_MAT_BUFFER_LIMIT - matBufferCount;
        appendData(data, dataOffset, bytesBefore, paddingFill);
        remaining -= bytesBefore;
        appendLiteral(TRUEHD_MAT_END_CODE);
        if (paddingFill) {
          remaining -= TRUEHD_MAT_END_CODE.length;
        }
        return remaining;
      }
      appendData(data, dataOffset, size, paddingFill);
      return 0;
    }

    private void appendData(byte[] data, int dataOffset, int size, boolean paddingFill) {
      if (size <= 0) {
        return;
      }
      if (!paddingFill) {
        System.arraycopy(data, dataOffset, matBuffer, matBufferCount, size);
      }
      currentMatFrameSize += size;
      matBufferCount += size;
    }

    private void appendLiteral(byte[] data) {
      System.arraycopy(data, 0, matBuffer, matBufferCount, data.length);
      currentMatFrameSize += data.length;
      matBufferCount += data.length;
    }

    private void flushPacket() {
      if (matBufferCount == 0) {
        return;
      }
      if (matBufferCount != IEC61937_TRUEHD_PACKET_BYTES) {
        throw new IllegalStateException("Incomplete MAT packet");
      }
      byte[] outputFrame = new byte[IEC61937_TRUEHD_PACKET_BYTES];
      System.arraycopy(matBuffer, 0, outputFrame, 0, outputFrame.length);
      swapWordsInPlace(outputFrame, 8, outputFrame.length - 8);
      writePreamble(outputFrame, IEC61937_TRUEHD, IEC61937_TRUEHD_LENGTH_CODE);
      outputFrames.add(outputFrame);
      matBuffer = new byte[0];
      matBufferCount = 0;
    }

    private static void swapWordsInPlace(byte[] data, int offset, int length) {
      int pairs = length & ~1;
      for (int i = 0; i < pairs; i += 2) {
        byte temp = data[offset + i];
        data[offset + i] = data[offset + i + 1];
        data[offset + i + 1] = temp;
      }
    }
  }
}
