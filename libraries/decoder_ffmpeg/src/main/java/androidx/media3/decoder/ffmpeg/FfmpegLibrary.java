/*
 * Copyright (C) 2016 The Android Open Source Project
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
package androidx.media3.decoder.ffmpeg;

import android.hardware.HardwareBuffer;
import android.view.Surface;
import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.MediaLibraryInfo;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.LibraryLoader;
import androidx.media3.common.util.Log;
import androidx.media3.common.util.UnstableApi;
import org.checkerframework.checker.nullness.qual.MonotonicNonNull;

/** Configures and queries the underlying native library. */
@UnstableApi
public final class FfmpegLibrary {

  static {
    MediaLibraryInfo.registerModule("media3.decoder.ffmpeg");
  }

  private static final String TAG = "FfmpegLibrary";

  private static final LibraryLoader LOADER =
      new LibraryLoader("ffmpegJNI") {
        @Override
        protected void loadLibrary(String name) {
          System.loadLibrary(name);
        }
      };

  private static @MonotonicNonNull String version;
  private static int inputBufferPaddingSize = C.LENGTH_UNSET;
  private static int dv5ToneMapToSdrSupport = C.LENGTH_UNSET;
  private static volatile boolean experimentalDv5ToneMapToSdrEnabled;
  private static volatile boolean experimentalDv5HardwareToneMapRpuBridgeEnabled;

  private FfmpegLibrary() {}

  /**
   * Override the names of the FFmpeg native libraries. If an application wishes to call this
   * method, it must do so before calling any other method defined by this class, and before
   * instantiating a {@link FfmpegAudioRenderer} or {@link ExperimentalFfmpegVideoRenderer}
   * instance.
   *
   * @param libraries The names of the FFmpeg native libraries.
   */
  public static void setLibraries(String... libraries) {
    LOADER.setLibraries(libraries);
  }

  /** Returns whether the underlying library is available, loading it if necessary. */
  public static boolean isAvailable() {
    return LOADER.isAvailable();
  }

  /** Returns the version of the underlying library if available, or null otherwise. */
  @Nullable
  public static String getVersion() {
    if (!isAvailable()) {
      return null;
    }
    if (version == null) {
      version = ffmpegGetVersion();
    }
    return version;
  }

  /**
   * Returns the required amount of padding for input buffers in bytes, or {@link C#LENGTH_UNSET} if
   * the underlying library is not available.
   */
  public static int getInputBufferPaddingSize() {
    if (!isAvailable()) {
      return C.LENGTH_UNSET;
    }
    if (inputBufferPaddingSize == C.LENGTH_UNSET) {
      inputBufferPaddingSize = ffmpegGetInputBufferPaddingSize();
    }
    return inputBufferPaddingSize;
  }

  /**
   * Returns whether the underlying library supports the specified MIME type.
   *
   * @param mimeType The MIME type to check.
   */
  public static boolean supportsFormat(String mimeType) {
    if (!isAvailable()) {
      return false;
    }
    @Nullable String codecName = getCodecName(mimeType);
    if (codecName == null) {
      return false;
    }
    if (!ffmpegHasDecoder(codecName)) {
      Log.w(TAG, "No " + codecName + " decoder available. Check the FFmpeg build configuration.");
      return false;
    }
    return true;
  }

  /**
   * Enables an experimental native DV5 tone-mapping path in {@link FfmpegVideoDecoder}.
   *
   * <p>This only affects newly created decoders.
   */
  public static void setExperimentalDv5ToneMapToSdrEnabled(boolean enabled) {
    experimentalDv5ToneMapToSdrEnabled = enabled;
  }

  /**
   * Enables a native RPU bridge used by the experimental DV5 hardware tone-map pipeline.
   *
   * <p>The bridge stores external RPU payloads keyed by timestamps, so native tone-map code can
   * attach them when decoded frames arrive without Dolby Vision side data.
   */
  public static void setExperimentalDv5HardwareToneMapRpuBridgeEnabled(boolean enabled) {
    experimentalDv5HardwareToneMapRpuBridgeEnabled = enabled;
    if (isAvailable()) {
      ffmpegSetExperimentalDv5HardwareToneMapRpuBridgeEnabled(enabled);
    }
  }

  /** Pushes an external DV RPU payload for a sample timestamp. */
  public static void pushExperimentalDv5HardwareRpuSample(long sampleTimeUs, byte[] rpuNalPayload) {
    if (!experimentalDv5HardwareToneMapRpuBridgeEnabled || rpuNalPayload.length == 0) {
      return;
    }
    if (!isAvailable()) {
      return;
    }
    ffmpegPushExperimentalDv5HardwareRpuSample(sampleTimeUs, rpuNalPayload);
  }

  /** Notifies native code that a frame timestamp is about to be rendered. */
  public static void notifyExperimentalDv5HardwareFramePresented(long presentationTimeUs) {
    if (!experimentalDv5HardwareToneMapRpuBridgeEnabled || !isAvailable()) {
      return;
    }
    ffmpegNotifyExperimentalDv5HardwareFramePresented(presentationTimeUs);
  }

  /**
   * Renders a MediaCodec-decoded hardware frame through the experimental native DV5 tone-map path.
   *
   * <p>Returns {@code false} if the path is disabled or unavailable.
   */
  public static boolean renderExperimentalDv5HardwareFrame(
      long presentationTimeUs,
      HardwareBuffer hardwareBuffer,
      int displayedWidth,
      int displayedHeight,
      Surface outputSurface) {
    if (!experimentalDv5HardwareToneMapRpuBridgeEnabled || !isAvailable()) {
      return false;
    }
    return ffmpegRenderExperimentalDv5HardwareFrame(
        presentationTimeUs, hardwareBuffer, displayedWidth, displayedHeight, outputSurface);
  }

  /**
   * Renders a MediaCodec-decoded hardware frame through the pure native DV5 tone-map path.
   *
   * <p>This path uses a native libplacebo renderer directly and does not route through FFmpeg
   * filter graph wrappers.
   */
  public static boolean renderExperimentalDv5HardwareFramePure(
      long presentationTimeUs,
      HardwareBuffer hardwareBuffer,
      int displayedWidth,
      int displayedHeight,
      Surface outputSurface) {
    if (!experimentalDv5HardwareToneMapRpuBridgeEnabled || !isAvailable()) {
      return false;
    }
    return ffmpegRenderExperimentalDv5HardwareFramePure(
        presentationTimeUs, hardwareBuffer, displayedWidth, displayedHeight, outputSurface);
  }

  /** Returns whether this native build includes the experimental DV5 SDR tone-mapping path. */
  public static boolean supportsExperimentalDv5ToneMapToSdr() {
    if (!isAvailable()) {
      return false;
    }
    if (dv5ToneMapToSdrSupport == C.LENGTH_UNSET) {
      dv5ToneMapToSdrSupport = ffmpegSupportsDv5ToneMapToSdr() ? 1 : 0;
    }
    return dv5ToneMapToSdrSupport == 1;
  }

  /* package */ static boolean isExperimentalDv5ToneMapToSdrEnabled() {
    return experimentalDv5ToneMapToSdrEnabled;
  }

  /**
   * Returns the name of the FFmpeg decoder that could be used to decode the format, or {@code null}
   * if it's unsupported.
   */
  @Nullable
  /* package */ static String getCodecName(String mimeType) {
    switch (mimeType) {
      case MimeTypes.AUDIO_AAC:
        return "aac";
      case MimeTypes.AUDIO_MPEG:
      case MimeTypes.AUDIO_MPEG_L1:
      case MimeTypes.AUDIO_MPEG_L2:
        return "mp3";
      case MimeTypes.AUDIO_AC3:
        return "ac3";
      case MimeTypes.AUDIO_E_AC3:
      case MimeTypes.AUDIO_E_AC3_JOC:
        return "eac3";
      case MimeTypes.AUDIO_TRUEHD:
        return "truehd";
      case MimeTypes.AUDIO_DTS:
      case MimeTypes.AUDIO_DTS_HD:
        return "dca";
      case MimeTypes.AUDIO_VORBIS:
        return "vorbis";
      case MimeTypes.AUDIO_OPUS:
        return "opus";
      case MimeTypes.AUDIO_AMR_NB:
        return "amrnb";
      case MimeTypes.AUDIO_AMR_WB:
        return "amrwb";
      case MimeTypes.AUDIO_FLAC:
        return "flac";
      case MimeTypes.AUDIO_ALAC:
        return "alac";
      case MimeTypes.AUDIO_MLAW:
        return "pcm_mulaw";
      case MimeTypes.AUDIO_ALAW:
        return "pcm_alaw";
      case MimeTypes.VIDEO_H264:
        return "h264";
      case MimeTypes.VIDEO_H265:
        return "hevc";
      case MimeTypes.VIDEO_DOLBY_VISION:
        // FFmpeg decodes Dolby Vision elementary streams through the HEVC decoder.
        return "hevc";
      case MimeTypes.VIDEO_VC1:
        return "vc1";
      case MimeTypes.VIDEO_MPEG:
        return "mpegvideo";
      case MimeTypes.VIDEO_MPEG2:
        return "mpeg2video";
      case MimeTypes.VIDEO_VP8:
        return "vp8";
      case MimeTypes.VIDEO_VP9:
        return "vp9";
      default:
        return null;
    }
  }

  private static native String ffmpegGetVersion();

  private static native int ffmpegGetInputBufferPaddingSize();

  private static native boolean ffmpegSupportsDv5ToneMapToSdr();

  private static native boolean ffmpegHasDecoder(String codecName);

  private static native void ffmpegSetExperimentalDv5HardwareToneMapRpuBridgeEnabled(
      boolean enabled);

  private static native void ffmpegPushExperimentalDv5HardwareRpuSample(
      long sampleTimeUs, byte[] rpuNalPayload);

  private static native void ffmpegNotifyExperimentalDv5HardwareFramePresented(
      long presentationTimeUs);

  private static native boolean ffmpegRenderExperimentalDv5HardwareFrame(
      long presentationTimeUs,
      HardwareBuffer hardwareBuffer,
      int displayedWidth,
      int displayedHeight,
      Surface outputSurface);

  private static native boolean ffmpegRenderExperimentalDv5HardwareFramePure(
      long presentationTimeUs,
      HardwareBuffer hardwareBuffer,
      int displayedWidth,
      int displayedHeight,
      Surface outputSurface);
}
