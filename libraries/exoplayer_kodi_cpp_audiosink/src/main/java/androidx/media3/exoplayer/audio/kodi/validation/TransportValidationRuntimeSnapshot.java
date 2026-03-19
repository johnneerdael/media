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
import java.util.List;

public final class TransportValidationRuntimeSnapshot {
  private final String sampleId;
  private final String codecFamily;
  private final List<TransportValidationRuntimeBurst> packerInputBursts;
  private final List<TransportValidationRuntimeBurst> packedBursts;
  private final List<TransportValidationRuntimeBurst> audioTrackWriteBursts;
  @Nullable private final TransportValidationRuntimeRouteSnapshot routeSnapshot;

  public TransportValidationRuntimeSnapshot(
      String sampleId,
      String codecFamily,
      List<TransportValidationRuntimeBurst> packerInputBursts,
      List<TransportValidationRuntimeBurst> packedBursts,
      List<TransportValidationRuntimeBurst> audioTrackWriteBursts,
      @Nullable TransportValidationRuntimeRouteSnapshot routeSnapshot) {
    this.sampleId = sampleId;
    this.codecFamily = codecFamily;
    this.packerInputBursts = packerInputBursts;
    this.packedBursts = packedBursts;
    this.audioTrackWriteBursts = audioTrackWriteBursts;
    this.routeSnapshot = routeSnapshot;
  }

  public String getSampleId() {
    return sampleId;
  }

  public String getCodecFamily() {
    return codecFamily;
  }

  public List<TransportValidationRuntimeBurst> getPackerInputBursts() {
    return packerInputBursts;
  }

  public List<TransportValidationRuntimeBurst> getPackedBursts() {
    return packedBursts;
  }

  public List<TransportValidationRuntimeBurst> getAudioTrackWriteBursts() {
    return audioTrackWriteBursts;
  }

  @Nullable
  public TransportValidationRuntimeRouteSnapshot getRouteSnapshot() {
    return routeSnapshot;
  }
}
