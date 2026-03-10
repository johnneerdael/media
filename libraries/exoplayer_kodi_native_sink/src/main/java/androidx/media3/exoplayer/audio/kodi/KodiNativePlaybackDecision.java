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

import android.media.AudioFormat;
import androidx.annotation.IntDef;
import androidx.media3.common.C;
import androidx.media3.common.util.UnstableApi;
import java.lang.annotation.Documented;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/** Decision returned by the native capability selector for a requested input format. */
@UnstableApi
public final class KodiNativePlaybackDecision {

  public static final int MODE_UNSUPPORTED = 0;
  public static final int MODE_PCM = 1;
  public static final int MODE_PASSTHROUGH_DIRECT = 2;
  public static final int MODE_PASSTHROUGH_IEC_STEREO = 3;
  public static final int MODE_PASSTHROUGH_IEC_MULTICHANNEL = 4;

  public static final int STREAM_TYPE_NULL = 0;
  public static final int STREAM_TYPE_AC3 = 1;
  public static final int STREAM_TYPE_DTS_512 = 2;
  public static final int STREAM_TYPE_DTS_1024 = 3;
  public static final int STREAM_TYPE_DTS_2048 = 4;
  public static final int STREAM_TYPE_DTSHD = 5;
  public static final int STREAM_TYPE_DTSHD_CORE = 6;
  public static final int STREAM_TYPE_EAC3 = 7;
  public static final int STREAM_TYPE_MLP = 8;
  public static final int STREAM_TYPE_TRUEHD = 9;
  public static final int STREAM_TYPE_DTSHD_MA = 10;

  @Documented
  @Retention(RetentionPolicy.SOURCE)
  @IntDef({
    MODE_UNSUPPORTED,
    MODE_PCM,
    MODE_PASSTHROUGH_DIRECT,
    MODE_PASSTHROUGH_IEC_STEREO,
    MODE_PASSTHROUGH_IEC_MULTICHANNEL
  })
  public @interface Mode {}

  public final @Mode int mode;
  public final @C.Encoding int outputEncoding;
  public final int channelConfig;
  public final int streamType;
  public final int flags;

  public KodiNativePlaybackDecision(
      @Mode int mode,
      @C.Encoding int outputEncoding,
      int channelConfig,
      int streamType,
      int flags) {
    this.mode = mode;
    this.outputEncoding = outputEncoding;
    this.channelConfig = channelConfig;
    this.streamType = streamType;
    this.flags = flags;
  }

  public boolean isPassthrough() {
    return mode == MODE_PASSTHROUGH_DIRECT
        || mode == MODE_PASSTHROUGH_IEC_STEREO
        || mode == MODE_PASSTHROUGH_IEC_MULTICHANNEL;
  }

  public boolean usesIecCarrier() {
    return mode == MODE_PASSTHROUGH_IEC_STEREO || mode == MODE_PASSTHROUGH_IEC_MULTICHANNEL;
  }

  public boolean usesMultichannelCarrier() {
    return mode == MODE_PASSTHROUGH_IEC_MULTICHANNEL
        || channelConfig == AudioFormat.CHANNEL_OUT_7POINT1_SURROUND;
  }

  @Override
  public String toString() {
    return "KodiNativePlaybackDecision[mode="
        + mode
        + ", outputEncoding="
        + outputEncoding
        + ", channelConfig="
        + channelConfig
        + ", streamType="
        + streamType
        + ", flags="
        + flags
        + "]";
  }
}
