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

import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.ParserException;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.extractor.DtsUtil;
import java.nio.ByteBuffer;

/** Fire OS specific DTS classifier used to select Kodi-style passthrough carrier policy. */
@UnstableApi
/* package */ final class FireOsDtsClassifier {

  private static final int DTS_PREAMBLE_XCH = 0x5A5A5A5A;
  private static final int DTS_PREAMBLE_XXCH = 0x47004A03;
  private static final int DTS_PREAMBLE_X96 = 0x1D95F262;
  private static final int DTS_PREAMBLE_XBR = 0x655E315E;
  private static final int DTS_PREAMBLE_LBR = 0x0A801921;
  private static final int DTS_PREAMBLE_XLL = 0x41A29547;
  private static final int DTS_PREAMBLE_HD = 0x64582025;

  private FireOsDtsClassifier() {}

  public static FireOsStreamInfo classify(
      byte[] accessUnit,
      int sampleCount,
      int repetitionPeriodFrames,
      int inputSampleRateHz,
      int inputChannelCount,
      FireOsStreamInfo.DtsStreamType previousStreamType,
      @Nullable DtsUtil.DtsHeader extensionHeader,
      @Nullable DtsUtil.DtsHeader uhdHeader) {
    return classifyFromHints(
        accessUnit,
        sampleCount,
        repetitionPeriodFrames,
        inputSampleRateHz,
        inputChannelCount,
        previousStreamType,
        extensionHeader != null,
        uhdHeader != null);
  }

  /* package */ static FireOsStreamInfo classifyFromHints(
      int sampleCount,
      int repetitionPeriodFrames,
      int inputSampleRateHz,
      int inputChannelCount,
      FireOsStreamInfo.DtsStreamType previousStreamType,
      boolean hasExtension,
      boolean hasUhd) {
    return classifyFromHints(
        /* accessUnit= */ new byte[0],
        sampleCount,
        repetitionPeriodFrames,
        inputSampleRateHz,
        inputChannelCount,
        previousStreamType,
        hasExtension,
        hasUhd);
  }

  /* package */ static FireOsStreamInfo classifyFromHints(
      byte[] accessUnit,
      int sampleCount,
      int repetitionPeriodFrames,
      int inputSampleRateHz,
      int inputChannelCount,
      FireOsStreamInfo.DtsStreamType previousStreamType,
      boolean hasExtension,
      boolean hasUhd) {
    FireOsStreamInfo.DtsStreamType dtsStreamType =
        classifyStreamType(
            accessUnit,
            sampleCount,
            repetitionPeriodFrames,
            previousStreamType,
            hasExtension,
            hasUhd);
    return FireOsStreamInfo.createForDts(
        /* sampleMimeType= */ null,
        dtsStreamType,
        inputSampleRateHz != Format.NO_VALUE ? inputSampleRateHz : 48_000,
        inputChannelCount != Format.NO_VALUE ? inputChannelCount : 2,
        resolveDtsPeriodFrames(dtsStreamType, repetitionPeriodFrames, sampleCount),
        "hasExtension="
            + hasExtension
            + ",hasUhd="
            + hasUhd
            + ",sampleCount="
            + sampleCount
            + ",repetitionPeriod="
            + repetitionPeriodFrames);
  }

  /* package */ static FireOsStreamInfo.DtsStreamType classifyCoreFrame(
      byte[] accessUnit) {
    int sampleCount = DtsUtil.parseDtsAudioSampleCount(ByteBuffer.wrap(accessUnit));
    return classifyCoreFrame(sampleCount);
  }

  private static FireOsStreamInfo.DtsStreamType classifyStreamType(
      byte[] accessUnit,
      int sampleCount,
      int repetitionPeriodFrames,
      FireOsStreamInfo.DtsStreamType previousStreamType,
      boolean hasExtension,
      boolean hasUhd) {
    if (hasExtension || hasUhd) {
      FireOsStreamInfo.DtsStreamType bitstreamDeclaredType =
          classifyBitstreamDeclaredType(accessUnit);
      if (bitstreamDeclaredType != FireOsStreamInfo.DtsStreamType.UNKNOWN) {
        return bitstreamDeclaredType;
      }
      return previousStreamType != FireOsStreamInfo.DtsStreamType.NONE
              ? previousStreamType
              : FireOsStreamInfo.DtsStreamType.UNKNOWN;
    }
    if (previousStreamType == FireOsStreamInfo.DtsStreamType.DTSHD
        || previousStreamType == FireOsStreamInfo.DtsStreamType.DTSHD_MA) {
      // Mirror Kodi stream-state behavior: once DTS-HD/MA is established, keep that stream type
      // instead of downgrading on core-only probe windows.
      return previousStreamType;
    }
    if (repetitionPeriodFrames != C.LENGTH_UNSET && repetitionPeriodFrames != 0 && sampleCount == 0) {
      switch (repetitionPeriodFrames) {
        case 512:
          return FireOsStreamInfo.DtsStreamType.DTS_512;
        case 1024:
          return FireOsStreamInfo.DtsStreamType.DTS_1024;
        case 2048:
          return FireOsStreamInfo.DtsStreamType.DTS_2048;
        default:
          break;
      }
    }
    return classifyCoreFrame(sampleCount);
  }

  private static FireOsStreamInfo.DtsStreamType classifyCoreFrame(int sampleCount) {
    switch (sampleCount) {
      case 512:
        return FireOsStreamInfo.DtsStreamType.DTS_512;
      case 1024:
        return FireOsStreamInfo.DtsStreamType.DTS_1024;
      case 2048:
        return FireOsStreamInfo.DtsStreamType.DTS_2048;
      default:
        return FireOsStreamInfo.DtsStreamType.UNKNOWN;
    }
  }

  private static FireOsStreamInfo.DtsStreamType classifyBitstreamDeclaredType(byte[] accessUnit) {
    @Nullable byte[] extensionFrame = findExtensionFrame(accessUnit);
    if (extensionFrame != null) {
      int marker = extractKodiDtsHdSubstreamMarker(extensionFrame);
      if (marker == DTS_PREAMBLE_XLL) {
        return FireOsStreamInfo.DtsStreamType.DTSHD_MA;
      }
      if (marker == DTS_PREAMBLE_XCH
          || marker == DTS_PREAMBLE_XXCH
          || marker == DTS_PREAMBLE_X96
          || marker == DTS_PREAMBLE_XBR
          || marker == DTS_PREAMBLE_LBR) {
        return FireOsStreamInfo.DtsStreamType.DTSHD;
      }
    }
    if (containsAnyMarker(accessUnit, DTS_PREAMBLE_XLL)) {
      return FireOsStreamInfo.DtsStreamType.DTSHD_MA;
    }
    if (containsAnyMarker(
        accessUnit,
        DTS_PREAMBLE_XCH,
        DTS_PREAMBLE_XXCH,
        DTS_PREAMBLE_X96,
        DTS_PREAMBLE_XBR,
        DTS_PREAMBLE_LBR)) {
      return FireOsStreamInfo.DtsStreamType.DTSHD;
    }
    if (extensionFrame == null) {
      return FireOsStreamInfo.DtsStreamType.UNKNOWN;
    }
    try {
      DtsUtil.DtsHdAssetInfo assetInfo = DtsUtil.parseDtsHdAssetInfo(extensionFrame);
      if (assetInfo.xllPresent || assetInfo.codingMode == 1) {
        return FireOsStreamInfo.DtsStreamType.DTSHD_MA;
      }
      if (assetInfo.lbrPresent || assetInfo.codingMode == 2) {
        return FireOsStreamInfo.DtsStreamType.DTSHD;
      }
      if (assetInfo.xbrPresent || assetInfo.xxchPresent || assetInfo.x96Present) {
        return FireOsStreamInfo.DtsStreamType.DTSHD;
      }
    } catch (ParserException | IllegalArgumentException e) {
      // Fall through to UNKNOWN.
    }
    return FireOsStreamInfo.DtsStreamType.UNKNOWN;
  }

  private static int extractKodiDtsHdSubstreamMarker(byte[] extensionFrame) {
    if (extensionFrame.length < 10) {
      return 0;
    }
    if (DtsUtil.getFrameType(ByteBuffer.wrap(extensionFrame, 0, 4).getInt())
        != DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM) {
      return 0;
    }
    boolean blownup = (extensionFrame[5] & 0x20) != 0;
    int headerSize;
    if (blownup) {
      headerSize = (((extensionFrame[5] & 0x1F) << 7) | ((extensionFrame[6] & 0xFE) >> 1)) + 1;
    } else {
      headerSize = (((extensionFrame[5] & 0x1F) << 3) | ((extensionFrame[6] & 0xE0) >> 5)) + 1;
    }
    if (headerSize <= 0 || headerSize + 4 > extensionFrame.length) {
      return 0;
    }
    return ((extensionFrame[headerSize] & 0xFF) << 24)
        | ((extensionFrame[headerSize + 1] & 0xFF) << 16)
        | ((extensionFrame[headerSize + 2] & 0xFF) << 8)
        | (extensionFrame[headerSize + 3] & 0xFF);
  }

  private static boolean containsAnyMarker(byte[] input, int... markers) {
    if (input.length < 4) {
      return false;
    }
    for (int offset = 0; offset + 3 < input.length; offset++) {
      int word =
          ((input[offset] & 0xFF) << 24)
              | ((input[offset + 1] & 0xFF) << 16)
              | ((input[offset + 2] & 0xFF) << 8)
              | (input[offset + 3] & 0xFF);
      for (int marker : markers) {
        if (word == marker) {
          return true;
        }
      }
    }
    return false;
  }

  private static @Nullable byte[] findExtensionFrame(byte[] accessUnit) {
    if (accessUnit.length < 4) {
      return null;
    }
    int sync = ByteBuffer.wrap(accessUnit, 0, 4).getInt();
    int frameType = DtsUtil.getFrameType(sync);
    if (sync == DTS_PREAMBLE_HD) {
      return accessUnit;
    }
    if (frameType != DtsUtil.FRAME_TYPE_CORE || accessUnit.length < 16) {
      return null;
    }
    int coreSize = DtsUtil.getDtsFrameSize(accessUnit);
    if (coreSize <= 0 || coreSize >= accessUnit.length || accessUnit.length - coreSize < 4) {
      return null;
    }
    int extensionSync = ByteBuffer.wrap(accessUnit, coreSize, 4).getInt();
    if (extensionSync != DTS_PREAMBLE_HD) {
      return null;
    }
    byte[] extensionFrame = new byte[accessUnit.length - coreSize];
    System.arraycopy(accessUnit, coreSize, extensionFrame, 0, extensionFrame.length);
    return extensionFrame;
  }

  private static int resolveDtsPeriodFrames(
      FireOsStreamInfo.DtsStreamType dtsStreamType, int repetitionPeriodFrames, int sampleCount) {
    switch (dtsStreamType) {
      case DTS_512:
      case DTSHD_CORE:
        return 512;
      case DTS_1024:
        return 1024;
      case DTS_2048:
        return 2048;
      case DTSHD:
      case DTSHD_MA:
        return repetitionPeriodFrames;
      case NONE:
      case UNKNOWN:
      default:
        return repetitionPeriodFrames != C.LENGTH_UNSET ? repetitionPeriodFrames : sampleCount;
    }
  }
}
