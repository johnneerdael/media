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

import android.media.AudioFormat;
import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.UnstableApi;

/** Fire OS specific encoded stream contract used to drive passthrough policy deterministically. */
@UnstableApi
public final class FireOsStreamInfo {

  public enum StreamFamily {
    AC3,
    E_AC3,
    DTS,
    TRUEHD,
    UNKNOWN
  }

  public enum DtsStreamType {
    NONE,
    DTS_512,
    DTS_1024,
    DTS_2048,
    DTSHD_CORE,
    DTSHD,
    DTSHD_MA,
    UNKNOWN
  }

  public final @Nullable String sampleMimeType;
  public final StreamFamily family;
  public final DtsStreamType dtsStreamType;
  public final int inputSampleRateHz;
  public final int inputChannelCount;
  public final int dtsPeriodFrames;
  public final int outputRateHz;
  public final int outputChannelCount;
  public final int logicalPassthroughChannelMask;
  public final boolean preferMultichannelIecCarrier;
  public final boolean allowDtsCoreRawFallback;
  public final boolean passthroughCandidate;
  public final boolean highBitrateCandidate;
  public final String diagnostics;

  public FireOsStreamInfo(
      @Nullable String sampleMimeType,
      StreamFamily family,
      DtsStreamType dtsStreamType,
      int inputSampleRateHz,
      int inputChannelCount,
      int dtsPeriodFrames,
      int outputRateHz,
      int outputChannelCount,
      int logicalPassthroughChannelMask,
      boolean preferMultichannelIecCarrier,
      boolean allowDtsCoreRawFallback,
      boolean passthroughCandidate,
      boolean highBitrateCandidate,
      String diagnostics) {
    this.sampleMimeType = sampleMimeType;
    this.family = family;
    this.dtsStreamType = dtsStreamType;
    this.inputSampleRateHz = inputSampleRateHz;
    this.inputChannelCount = inputChannelCount;
    this.dtsPeriodFrames = dtsPeriodFrames;
    this.outputRateHz = outputRateHz;
    this.outputChannelCount = outputChannelCount;
    this.logicalPassthroughChannelMask = logicalPassthroughChannelMask;
    this.preferMultichannelIecCarrier = preferMultichannelIecCarrier;
    this.allowDtsCoreRawFallback = allowDtsCoreRawFallback;
    this.passthroughCandidate = passthroughCandidate;
    this.highBitrateCandidate = highBitrateCandidate;
    this.diagnostics = diagnostics;
  }

  public boolean matchesFormat(Format format) {
    return sampleMimeType != null && sampleMimeType.equals(format.sampleMimeType);
  }

  public String summary() {
    return "family="
        + family
        + ",dtsType="
        + dtsStreamType
        + ",sampleRate="
        + inputSampleRateHz
        + ",channels="
        + inputChannelCount
        + ",dtsPeriod="
        + dtsPeriodFrames
        + ",outputRate="
        + outputRateHz
        + ",outputChannels="
        + outputChannelCount
        + ",logicalMask="
        + logicalPassthroughChannelMask
        + ",multiIec="
        + preferMultichannelIecCarrier
        + ",dtsCoreRawFallback="
        + allowDtsCoreRawFallback
        + ",highBitrate="
        + highBitrateCandidate
        + ",diagnostics="
        + diagnostics;
  }

  public static FireOsStreamInfo createForAc3Family(
      @Nullable String sampleMimeType, int inputSampleRateHz, int inputChannelCount) {
    StreamFamily family =
        MimeTypes.AUDIO_AC3.equals(sampleMimeType) ? StreamFamily.AC3 : StreamFamily.E_AC3;
    int outputRateHz =
        family == StreamFamily.AC3
            ? inputSampleRateHz
            : (inputSampleRateHz != Format.NO_VALUE ? inputSampleRateHz * 4 : 192_000);
    return new FireOsStreamInfo(
        sampleMimeType,
        family,
        DtsStreamType.NONE,
        inputSampleRateHz,
        inputChannelCount,
        C.LENGTH_UNSET,
        outputRateHz,
        /* outputChannelCount= */ 2,
        AudioFormat.CHANNEL_OUT_STEREO,
        /* preferMultichannelIecCarrier= */ false,
        /* allowDtsCoreRawFallback= */ false,
        /* passthroughCandidate= */ true,
        /* highBitrateCandidate= */ false,
        "ac3-family");
  }

  public static FireOsStreamInfo createForTrueHd(int inputSampleRateHz, int inputChannelCount) {
    int outputRateHz = getTrueHdOutputRate(inputSampleRateHz);
    return new FireOsStreamInfo(
        MimeTypes.AUDIO_TRUEHD,
        StreamFamily.TRUEHD,
        DtsStreamType.NONE,
        inputSampleRateHz,
        inputChannelCount,
        C.LENGTH_UNSET,
        outputRateHz,
        /* outputChannelCount= */ 8,
        AudioFormat.CHANNEL_OUT_7POINT1_SURROUND,
        /* preferMultichannelIecCarrier= */ true,
        /* allowDtsCoreRawFallback= */ false,
        /* passthroughCandidate= */ true,
        /* highBitrateCandidate= */ inputSampleRateHz >= 96_000,
        "truehd");
  }

  public static FireOsStreamInfo createForDts(
      @Nullable String sampleMimeType,
      DtsStreamType dtsStreamType,
      int inputSampleRateHz,
      int inputChannelCount,
      int dtsPeriodFrames,
      String diagnostics) {
    int outputRateHz = getDtsOutputRate(dtsStreamType, inputSampleRateHz);
    int outputChannelCount = getDtsOutputChannelCount(dtsStreamType);
    return new FireOsStreamInfo(
        sampleMimeType != null ? sampleMimeType : MimeTypes.AUDIO_DTS,
        StreamFamily.DTS,
        dtsStreamType,
        inputSampleRateHz,
        inputChannelCount,
        dtsPeriodFrames,
        outputRateHz,
        outputChannelCount,
        outputChannelCount >= 8
            ? AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
            : AudioFormat.CHANNEL_OUT_STEREO,
        /* preferMultichannelIecCarrier= */ outputChannelCount >= 8,
        /* allowDtsCoreRawFallback= */ dtsStreamType != DtsStreamType.DTS_512
            && dtsStreamType != DtsStreamType.DTS_1024
            && dtsStreamType != DtsStreamType.DTS_2048
            && dtsStreamType != DtsStreamType.DTSHD_CORE,
        /* passthroughCandidate= */ true,
        /* highBitrateCandidate= */ outputRateHz >= 192_000 || outputChannelCount >= 8,
        diagnostics);
  }

  public static FireOsStreamInfo createForUnknown(@Nullable String sampleMimeType) {
    return new FireOsStreamInfo(
        sampleMimeType,
        StreamFamily.UNKNOWN,
        DtsStreamType.UNKNOWN,
        /* inputSampleRateHz= */ C.RATE_UNSET_INT,
        /* inputChannelCount= */ Format.NO_VALUE,
        /* dtsPeriodFrames= */ C.LENGTH_UNSET,
        /* outputRateHz= */ C.RATE_UNSET_INT,
        /* outputChannelCount= */ Format.NO_VALUE,
        AudioFormat.CHANNEL_OUT_STEREO,
        /* preferMultichannelIecCarrier= */ false,
        /* allowDtsCoreRawFallback= */ false,
        /* passthroughCandidate= */ false,
        /* highBitrateCandidate= */ false,
        "unknown");
  }

  private static int getDtsOutputRate(DtsStreamType dtsStreamType, int inputSampleRateHz) {
    switch (dtsStreamType) {
      case DTS_512:
      case DTS_1024:
      case DTS_2048:
      case DTSHD_CORE:
        return inputSampleRateHz != Format.NO_VALUE ? inputSampleRateHz : 48_000;
      case DTSHD:
      case DTSHD_MA:
        return 192_000;
      case NONE:
      case UNKNOWN:
      default:
        return 48_000;
    }
  }

  private static int getDtsOutputChannelCount(DtsStreamType dtsStreamType) {
    switch (dtsStreamType) {
      case DTSHD_MA:
        return 8;
      case DTS_512:
      case DTS_1024:
      case DTS_2048:
      case DTSHD_CORE:
      case DTSHD:
      case NONE:
      case UNKNOWN:
      default:
        return 2;
    }
  }

  private static int getTrueHdOutputRate(int inputSampleRateHz) {
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
}
