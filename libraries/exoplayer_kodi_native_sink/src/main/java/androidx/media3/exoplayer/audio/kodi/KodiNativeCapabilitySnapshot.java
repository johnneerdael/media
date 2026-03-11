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
import android.media.AudioFormat;
import android.os.Build;
import android.util.Pair;
import androidx.annotation.Nullable;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.common.util.Util;
import androidx.media3.exoplayer.audio.AudioCapabilities;
import java.util.Arrays;
import java.util.Objects;

/**
 * Snapshot of Android audio capability facts used by the native Kodi capability selector.
 *
 * <p>This is intentionally a transport object for native evaluation, not a second Java decision
 * engine.
 */
@UnstableApi
public final class KodiNativeCapabilitySnapshot {

  private static final int[] SURROUND_ENCODINGS_TO_CHECK =
      new int[] {
        C.ENCODING_AC3,
        C.ENCODING_E_AC3,
        C.ENCODING_E_AC3_JOC,
        C.ENCODING_DTS,
        C.ENCODING_DTS_HD,
        C.ENCODING_DTS_UHD_P2,
        C.ENCODING_DOLBY_TRUEHD
      };

  public final int sdkInt;
  public final boolean tv;
  public final boolean automotive;
  public final int routedDeviceId;
  public final int routedDeviceType;
  public final int maxChannelCount;
  public final int[] supportedEncodings;
  public final ProbeResult ac3;
  public final ProbeResult eAc3;
  public final ProbeResult dts;
  public final ProbeResult dtsHd;
  public final ProbeResult trueHd;

  public KodiNativeCapabilitySnapshot(
      int sdkInt,
      boolean tv,
      boolean automotive,
      int routedDeviceId,
      int routedDeviceType,
      int maxChannelCount,
      int[] supportedEncodings,
      ProbeResult ac3,
      ProbeResult eAc3,
      ProbeResult dts,
      ProbeResult dtsHd,
      ProbeResult trueHd) {
    this.sdkInt = sdkInt;
    this.tv = tv;
    this.automotive = automotive;
    this.routedDeviceId = routedDeviceId;
    this.routedDeviceType = routedDeviceType;
    this.maxChannelCount = maxChannelCount;
    this.supportedEncodings = Arrays.copyOf(supportedEncodings, supportedEncodings.length);
    this.ac3 = ac3;
    this.eAc3 = eAc3;
    this.dts = dts;
    this.dtsHd = dtsHd;
    this.trueHd = trueHd;
  }

  /** Builds a capability snapshot from the current Android audio routing state. */
  public static KodiNativeCapabilitySnapshot fromSystem(
      Context context,
      AudioAttributes audioAttributes,
      @Nullable AudioDeviceInfo routedDevice) {
    AudioCapabilities capabilities =
        AudioCapabilities.getCapabilities(context, audioAttributes, routedDevice);
    int[] supportedEncodings = getSupportedEncodings(capabilities);
    return new KodiNativeCapabilitySnapshot(
        Build.VERSION.SDK_INT,
        Util.isTv(context),
        Util.isAutomotive(context),
        routedDevice != null ? routedDevice.getId() : C.INDEX_UNSET,
        routedDevice != null ? routedDevice.getType() : AudioDeviceInfo.TYPE_UNKNOWN,
        capabilities.getMaxChannelCount(),
        supportedEncodings,
        buildProbeResult(capabilities, audioAttributes, MimeTypes.AUDIO_AC3, /* channelCount= */ 6),
        buildProbeResult(
            capabilities, audioAttributes, MimeTypes.AUDIO_E_AC3, /* channelCount= */ 8),
        buildProbeResult(capabilities, audioAttributes, MimeTypes.AUDIO_DTS, /* channelCount= */ 6),
        buildProbeResult(
            capabilities, audioAttributes, MimeTypes.AUDIO_DTS_HD, /* channelCount= */ 8),
        buildProbeResult(
            capabilities, audioAttributes, MimeTypes.AUDIO_TRUEHD, /* channelCount= */ 8));
  }

  @Override
  public boolean equals(@Nullable Object obj) {
    if (this == obj) {
      return true;
    }
    if (!(obj instanceof KodiNativeCapabilitySnapshot)) {
      return false;
    }
    KodiNativeCapabilitySnapshot other = (KodiNativeCapabilitySnapshot) obj;
    return sdkInt == other.sdkInt
        && tv == other.tv
        && automotive == other.automotive
        && routedDeviceId == other.routedDeviceId
        && routedDeviceType == other.routedDeviceType
        && maxChannelCount == other.maxChannelCount
        && Arrays.equals(supportedEncodings, other.supportedEncodings)
        && Objects.equals(ac3, other.ac3)
        && Objects.equals(eAc3, other.eAc3)
        && Objects.equals(dts, other.dts)
        && Objects.equals(dtsHd, other.dtsHd)
        && Objects.equals(trueHd, other.trueHd);
  }

  @Override
  public int hashCode() {
    int result =
        Objects.hash(
            sdkInt, tv, automotive, routedDeviceId, routedDeviceType, maxChannelCount, ac3, eAc3,
            dts, dtsHd, trueHd);
    result = 31 * result + Arrays.hashCode(supportedEncodings);
    return result;
  }

  @Override
  public String toString() {
    return "KodiNativeCapabilitySnapshot[sdk="
        + sdkInt
        + ", tv="
        + tv
        + ", automotive="
        + automotive
        + ", routedDeviceId="
        + routedDeviceId
        + ", routedDeviceType="
        + routedDeviceType
        + ", maxChannelCount="
        + maxChannelCount
        + ", supportedEncodings="
        + Arrays.toString(supportedEncodings)
        + ", ac3="
        + ac3
        + ", eAc3="
        + eAc3
        + ", dts="
        + dts
        + ", dtsHd="
        + dtsHd
        + ", trueHd="
        + trueHd
        + "]";
  }

  private static int[] getSupportedEncodings(AudioCapabilities capabilities) {
    int count = 0;
    for (int encoding : SURROUND_ENCODINGS_TO_CHECK) {
      if (capabilities.supportsEncoding(encoding)) {
        count++;
      }
    }
    int[] encodings = new int[count];
    int index = 0;
    for (int encoding : SURROUND_ENCODINGS_TO_CHECK) {
      if (capabilities.supportsEncoding(encoding)) {
        encodings[index++] = encoding;
      }
    }
    return encodings;
  }

  private static ProbeResult buildProbeResult(
      AudioCapabilities capabilities,
      AudioAttributes audioAttributes,
      @Nullable String sampleMimeType,
      int channelCount) {
    Format format =
        new Format.Builder()
            .setSampleMimeType(sampleMimeType)
            .setSampleRate(48_000)
            .setChannelCount(channelCount)
            .build();
    @Nullable Pair<Integer, Integer> config =
        capabilities.getEncodingAndChannelConfigForPassthrough(format, audioAttributes);
    return new ProbeResult(
        config != null,
        config != null ? config.first : C.ENCODING_INVALID,
        config != null ? config.second : AudioFormat.CHANNEL_INVALID);
  }

  /** Probe result for a specific passthrough family under the current route and attributes. */
  @UnstableApi
  public static final class ProbeResult {
    public final boolean supported;
    public final @C.Encoding int encoding;
    public final int channelConfig;

    public ProbeResult(boolean supported, @C.Encoding int encoding, int channelConfig) {
      this.supported = supported;
      this.encoding = encoding;
      this.channelConfig = channelConfig;
    }

    @Override
    public boolean equals(@Nullable Object obj) {
      if (this == obj) {
        return true;
      }
      if (!(obj instanceof ProbeResult)) {
        return false;
      }
      ProbeResult other = (ProbeResult) obj;
      return supported == other.supported
          && encoding == other.encoding
          && channelConfig == other.channelConfig;
    }

    @Override
    public int hashCode() {
      return Objects.hash(supported, encoding, channelConfig);
    }

    @Override
    public String toString() {
      return "ProbeResult[supported="
          + supported
          + ", encoding="
          + encoding
          + ", channelConfig="
          + channelConfig
          + "]";
    }
  }
}
