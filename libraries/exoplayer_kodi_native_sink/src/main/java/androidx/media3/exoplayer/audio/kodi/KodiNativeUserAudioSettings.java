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

import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.audio.AudioCapabilities;

/** User-configurable Kodi-style passthrough settings consumed by the native IEC path. */
@UnstableApi
public final class KodiNativeUserAudioSettings {

  public final boolean passthroughEnabled;
  public final boolean ac3PassthroughEnabled;
  public final boolean eac3PassthroughEnabled;
  public final boolean dtsPassthroughEnabled;
  public final boolean truehdPassthroughEnabled;
  public final boolean dtshdPassthroughEnabled;
  public final boolean dtshdCoreFallbackEnabled;
  public final int maxPcmChannelLayout;

  public KodiNativeUserAudioSettings(
      boolean passthroughEnabled,
      boolean ac3PassthroughEnabled,
      boolean eac3PassthroughEnabled,
      boolean dtsPassthroughEnabled,
      boolean truehdPassthroughEnabled,
      boolean dtshdPassthroughEnabled,
      boolean dtshdCoreFallbackEnabled,
      int maxPcmChannelLayout) {
    this.passthroughEnabled = passthroughEnabled;
    this.ac3PassthroughEnabled = ac3PassthroughEnabled;
    this.eac3PassthroughEnabled = eac3PassthroughEnabled;
    this.dtsPassthroughEnabled = dtsPassthroughEnabled;
    this.truehdPassthroughEnabled = truehdPassthroughEnabled;
    this.dtshdPassthroughEnabled = dtshdPassthroughEnabled;
    this.dtshdCoreFallbackEnabled = dtshdCoreFallbackEnabled;
    this.maxPcmChannelLayout = maxPcmChannelLayout;
  }

  public static KodiNativeUserAudioSettings fromGlobals() {
    return new KodiNativeUserAudioSettings(
        AudioCapabilities.isExperimentalFireOsIecPassthroughEnabled(),
        AudioCapabilities.isIecPackerAc3PassthroughEnabled(),
        AudioCapabilities.isIecPackerEac3PassthroughEnabled(),
        AudioCapabilities.isIecPackerDtsPassthroughEnabled(),
        AudioCapabilities.isIecPackerTruehdPassthroughEnabled(),
        AudioCapabilities.isIecPackerDtshdPassthroughEnabled(),
        AudioCapabilities.isIecPackerDtshdCoreFallbackEnabled(),
        AudioCapabilities.getIecPackerMaxPcmChannelLayout());
  }
}
