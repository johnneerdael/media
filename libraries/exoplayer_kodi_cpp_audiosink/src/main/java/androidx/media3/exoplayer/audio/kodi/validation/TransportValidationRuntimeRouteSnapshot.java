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
package androidx.media3.exoplayer.audio.kodi.validation;

import androidx.annotation.Nullable;

public final class TransportValidationRuntimeRouteSnapshot {
  @Nullable private final String deviceName;
  @Nullable private final String encoding;
  private final int sampleRate;
  @Nullable private final String channelMask;
  @Nullable private final Boolean directPlaybackSupported;
  @Nullable private final Integer audioTrackState;

  public TransportValidationRuntimeRouteSnapshot(
      @Nullable String deviceName,
      @Nullable String encoding,
      int sampleRate,
      @Nullable String channelMask,
      @Nullable Boolean directPlaybackSupported,
      @Nullable Integer audioTrackState) {
    this.deviceName = deviceName;
    this.encoding = encoding;
    this.sampleRate = sampleRate;
    this.channelMask = channelMask;
    this.directPlaybackSupported = directPlaybackSupported;
    this.audioTrackState = audioTrackState;
  }

  @Nullable
  public String getDeviceName() {
    return deviceName;
  }

  @Nullable
  public String getEncoding() {
    return encoding;
  }

  public int getSampleRate() {
    return sampleRate;
  }

  @Nullable
  public String getChannelMask() {
    return channelMask;
  }

  @Nullable
  public Boolean getDirectPlaybackSupported() {
    return directPlaybackSupported;
  }

  @Nullable
  public Integer getAudioTrackState() {
    return audioTrackState;
  }
}
