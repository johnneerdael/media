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
import androidx.media3.common.MimeTypes;
import androidx.media3.common.ParserException;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.extractor.DtsUtil;
import java.nio.ByteBuffer;

/** Fire OS specific DTS classifier used to select Kodi-style passthrough carrier policy. */
@UnstableApi
/* package */ final class FireOsDtsClassifier {

  private FireOsDtsClassifier() {}

  public static FireOsStreamInfo classify(
      Format format,
      byte[] accessUnit,
      int sampleCount,
      int repetitionPeriodFrames,
      int inputSampleRateHz,
      int inputChannelCount,
      FireOsStreamInfo.DtsStreamType previousStreamType,
      @Nullable DtsUtil.DtsHeader extensionHeader,
      @Nullable DtsUtil.DtsHeader uhdHeader) {
    return classifyFromHints(
        format,
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
      Format format,
      int sampleCount,
      int repetitionPeriodFrames,
      int inputSampleRateHz,
      int inputChannelCount,
      FireOsStreamInfo.DtsStreamType previousStreamType,
      boolean hasExtension,
      boolean hasUhd) {
    return classifyFromHints(
        format,
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
      Format format,
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
            format,
            accessUnit,
            sampleCount,
            repetitionPeriodFrames,
            previousStreamType,
            hasExtension,
            hasUhd);
    return FireOsStreamInfo.createForDts(
        format.sampleMimeType,
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
      @Nullable String sampleMimeType, byte[] accessUnit) {
    int sampleCount = DtsUtil.parseDtsAudioSampleCount(ByteBuffer.wrap(accessUnit));
    return classifyCoreFrame(sampleMimeType, sampleCount);
  }

  private static FireOsStreamInfo.DtsStreamType classifyStreamType(
      Format format,
      byte[] accessUnit,
      int sampleCount,
      int repetitionPeriodFrames,
      FireOsStreamInfo.DtsStreamType previousStreamType,
      boolean hasExtension,
      boolean hasUhd) {
    @Nullable String sampleMimeType = format.sampleMimeType;
    if (hasExtension || hasUhd || MimeTypes.AUDIO_DTS_EXPRESS.equals(sampleMimeType)
        || MimeTypes.AUDIO_DTS_X.equals(sampleMimeType)) {
      FireOsStreamInfo.DtsStreamType bitstreamDeclaredType =
          classifyBitstreamDeclaredType(accessUnit);
      if (bitstreamDeclaredType != FireOsStreamInfo.DtsStreamType.UNKNOWN) {
        return bitstreamDeclaredType;
      }
      return previousStreamType != FireOsStreamInfo.DtsStreamType.NONE
              ? previousStreamType
              : FireOsStreamInfo.DtsStreamType.UNKNOWN;
    }
    if (MimeTypes.AUDIO_DTS_HD.equals(sampleMimeType)) {
      return FireOsStreamInfo.DtsStreamType.DTSHD_CORE;
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
    return classifyCoreFrame(sampleMimeType, sampleCount);
  }

  private static FireOsStreamInfo.DtsStreamType classifyCoreFrame(
      @Nullable String sampleMimeType, int sampleCount) {
    if (MimeTypes.AUDIO_DTS_HD.equals(sampleMimeType)) {
      return FireOsStreamInfo.DtsStreamType.DTSHD_CORE;
    }
    switch (sampleCount) {
      case 512:
        return FireOsStreamInfo.DtsStreamType.DTS_512;
      case 1024:
        return FireOsStreamInfo.DtsStreamType.DTS_1024;
      case 2048:
      default:
        return FireOsStreamInfo.DtsStreamType.DTS_2048;
    }
  }

  private static FireOsStreamInfo.DtsStreamType classifyBitstreamDeclaredType(byte[] accessUnit) {
    @Nullable byte[] extensionFrame = findExtensionFrame(accessUnit);
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
      // Fall through to codec hints or UNKNOWN.
    }
    return FireOsStreamInfo.DtsStreamType.UNKNOWN;
  }

  private static @Nullable byte[] findExtensionFrame(byte[] accessUnit) {
    if (accessUnit.length < 4) {
      return null;
    }
    int frameType = DtsUtil.getFrameType(ByteBuffer.wrap(accessUnit, 0, 4).getInt());
    if (frameType == DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM) {
      return accessUnit;
    }
    if (frameType != DtsUtil.FRAME_TYPE_CORE || accessUnit.length < 16) {
      return null;
    }
    int coreSize = DtsUtil.getDtsFrameSize(accessUnit);
    if (coreSize <= 0 || coreSize >= accessUnit.length || accessUnit.length - coreSize < 4) {
      return null;
    }
    int extensionType = DtsUtil.getFrameType(ByteBuffer.wrap(accessUnit, coreSize, 4).getInt());
    if (extensionType != DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM) {
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
