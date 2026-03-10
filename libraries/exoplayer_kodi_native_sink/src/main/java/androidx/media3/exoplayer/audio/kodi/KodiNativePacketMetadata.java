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

/** Metadata describing one native session output packet. */
@UnstableApi
public final class KodiNativePacketMetadata {

  public static final int KIND_NONE = 0;
  public static final int KIND_PASSTHROUGH_DIRECT = 1;
  public static final int KIND_IEC61937 = 2;
  public static final int KIND_PCM = 3;

  public final int kind;
  public final int sizeBytes;
  public final long totalFrames;
  public final int normalizedAccessUnits;
  public final long effectivePresentationTimeUs;

  public KodiNativePacketMetadata(
      int kind,
      int sizeBytes,
      long totalFrames,
      int normalizedAccessUnits,
      long effectivePresentationTimeUs) {
    this.kind = kind;
    this.sizeBytes = sizeBytes;
    this.totalFrames = totalFrames;
    this.normalizedAccessUnits = normalizedAccessUnits;
    this.effectivePresentationTimeUs = effectivePresentationTimeUs;
  }
}
