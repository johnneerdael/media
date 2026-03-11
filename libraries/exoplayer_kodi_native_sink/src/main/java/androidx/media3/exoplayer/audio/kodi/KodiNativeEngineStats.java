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

import androidx.annotation.Nullable;
import androidx.media3.common.Format;
import androidx.media3.common.util.UnstableApi;

/** Minimal runtime stats mirror for the Kodi-backed audio sink. */
@UnstableApi
public final class KodiNativeEngineStats {

  public static final KodiNativeEngineStats EMPTY =
      new KodiNativeEngineStats(
          /* usingNativeTransport= */ false,
          /* appFocused= */ true,
          /* suspended= */ false,
          /* pendingData= */ false,
          /* ended= */ false,
          /* positionUs= */ 0,
          /* bufferSizeUs= */ 0,
          /* volume= */ 1f,
          /* format= */ null,
          /* capabilitySnapshot= */ null,
          /* playbackDecision= */ null);

  public final boolean usingNativeTransport;
  public final boolean appFocused;
  public final boolean suspended;
  public final boolean pendingData;
  public final boolean ended;
  public final long positionUs;
  public final long bufferSizeUs;
  public final float volume;
  @Nullable public final Format format;
  @Nullable public final KodiNativeCapabilitySnapshot capabilitySnapshot;
  @Nullable public final KodiNativePlaybackDecision playbackDecision;

  public KodiNativeEngineStats(
      boolean usingNativeTransport,
      boolean appFocused,
      boolean suspended,
      boolean pendingData,
      boolean ended,
      long positionUs,
      long bufferSizeUs,
      float volume,
      @Nullable Format format,
      @Nullable KodiNativeCapabilitySnapshot capabilitySnapshot,
      @Nullable KodiNativePlaybackDecision playbackDecision) {
    this.usingNativeTransport = usingNativeTransport;
    this.appFocused = appFocused;
    this.suspended = suspended;
    this.pendingData = pendingData;
    this.ended = ended;
    this.positionUs = positionUs;
    this.bufferSizeUs = bufferSizeUs;
    this.volume = volume;
    this.format = format;
    this.capabilitySnapshot = capabilitySnapshot;
    this.playbackDecision = playbackDecision;
  }
}
