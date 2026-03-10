/*
 * Copyright (C) 2026 Nuvio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
package androidx.media3.exoplayer.audio.kodi;

import android.content.Context;
import android.media.AudioDeviceInfo;
import androidx.annotation.Nullable;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.UnstableApi;

/** Java wrapper around the native Kodi capability selector. */
@UnstableApi
public final class KodiNativeCapabilitySelector {

  private static final int MIME_KIND_UNKNOWN = 0;
  private static final int MIME_KIND_AC3 = 1;
  private static final int MIME_KIND_E_AC3 = 2;
  private static final int MIME_KIND_DTS = 3;
  private static final int MIME_KIND_DTS_HD = 4;
  private static final int MIME_KIND_DTS_UHD = 5;
  private static final int MIME_KIND_TRUEHD = 6;
  private static final int MIME_KIND_PCM = 7;

  private KodiNativeCapabilitySelector() {}

  /**
   * Collects the current Android capability snapshot and evaluates the native playback decision for
   * the requested format.
   */
  public static KodiNativePlaybackDecision evaluatePlaybackDecision(
      Context context,
      AudioAttributes audioAttributes,
      @Nullable AudioDeviceInfo routedDevice,
      Format format) {
    return evaluatePlaybackDecision(
        KodiNativeCapabilitySnapshot.fromSystem(context, audioAttributes, routedDevice), format);
  }

  /**
   * Evaluates the native playback decision for the requested format and capability snapshot.
   *
   * <p>`Format.sampleMimeType` is the authoritative top-level stream-family selector for the
   * native path. Native logic may still refine subtype handling and parse frame boundaries within
   * that chosen family, but it should not reclassify the family itself.
   */
  public static KodiNativePlaybackDecision evaluatePlaybackDecision(
      KodiNativeCapabilitySnapshot snapshot, Format format) {
    if (!KodiNativeLibrary.isAvailable()) {
      return new KodiNativePlaybackDecision(
          KodiNativePlaybackDecision.MODE_UNSUPPORTED,
          C.ENCODING_INVALID,
          /* channelConfig= */ 0,
          /* streamType= */ 0,
          /* flags= */ 0);
    }
    KodiNativeUserAudioSettings userSettings = KodiNativeUserAudioSettings.fromGlobals();
    int[] result =
        nEvaluatePlaybackDecision(
            snapshot.sdkInt,
            snapshot.tv,
            snapshot.automotive,
            snapshot.routedDeviceId,
            snapshot.routedDeviceType,
            snapshot.maxChannelCount,
            snapshot.supportedEncodings,
            snapshot.ac3.supported,
            snapshot.ac3.encoding,
            snapshot.ac3.channelConfig,
            snapshot.eAc3.supported,
            snapshot.eAc3.encoding,
            snapshot.eAc3.channelConfig,
            snapshot.dts.supported,
            snapshot.dts.encoding,
            snapshot.dts.channelConfig,
            snapshot.dtsHd.supported,
            snapshot.dtsHd.encoding,
            snapshot.dtsHd.channelConfig,
            snapshot.trueHd.supported,
            snapshot.trueHd.encoding,
            snapshot.trueHd.channelConfig,
            userSettings.passthroughEnabled,
            userSettings.ac3PassthroughEnabled,
            userSettings.eac3PassthroughEnabled,
            userSettings.dtsPassthroughEnabled,
            userSettings.truehdPassthroughEnabled,
            userSettings.dtshdPassthroughEnabled,
            userSettings.dtshdCoreFallbackEnabled,
            userSettings.maxPcmChannelLayout,
            getMimeKind(format.sampleMimeType),
            format.channelCount,
            format.sampleRate);
    return new KodiNativePlaybackDecision(result[0], result[1], result[2], result[3], result[4]);
  }

  static int getMimeKind(@Nullable String sampleMimeType) {
    if (MimeTypes.AUDIO_AC3.equals(sampleMimeType)) {
      return MIME_KIND_AC3;
    }
    if (MimeTypes.AUDIO_E_AC3.equals(sampleMimeType)
        || MimeTypes.AUDIO_E_AC3_JOC.equals(sampleMimeType)) {
      return MIME_KIND_E_AC3;
    }
    if (MimeTypes.AUDIO_DTS.equals(sampleMimeType)) {
      return MIME_KIND_DTS;
    }
    if (MimeTypes.AUDIO_DTS_HD.equals(sampleMimeType)) {
      return MIME_KIND_DTS_HD;
    }
    if (MimeTypes.AUDIO_DTS_X.equals(sampleMimeType)) {
      return MIME_KIND_DTS_UHD;
    }
    if (MimeTypes.AUDIO_TRUEHD.equals(sampleMimeType)) {
      return MIME_KIND_TRUEHD;
    }
    if (MimeTypes.AUDIO_RAW.equals(sampleMimeType)) {
      return MIME_KIND_PCM;
    }
    return MIME_KIND_UNKNOWN;
  }

  private static native int[] nEvaluatePlaybackDecision(
      int sdkInt,
      boolean tv,
      boolean automotive,
      int routedDeviceId,
      int routedDeviceType,
      int maxChannelCount,
      int[] supportedEncodings,
      boolean ac3Supported,
      int ac3Encoding,
      int ac3ChannelConfig,
      boolean eAc3Supported,
      int eAc3Encoding,
      int eAc3ChannelConfig,
      boolean dtsSupported,
      int dtsEncoding,
      int dtsChannelConfig,
      boolean dtsHdSupported,
      int dtsHdEncoding,
      int dtsHdChannelConfig,
      boolean trueHdSupported,
      int trueHdEncoding,
      int trueHdChannelConfig,
      boolean passthroughEnabled,
      boolean ac3PassthroughEnabled,
      boolean eAc3PassthroughEnabled,
      boolean dtsPassthroughEnabled,
      boolean trueHdPassthroughEnabled,
      boolean dtsHdPassthroughEnabled,
      boolean dtsHdCoreFallbackEnabled,
      int maxPcmChannelLayout,
      int mimeKind,
      int channelCount,
      int sampleRate);
}
