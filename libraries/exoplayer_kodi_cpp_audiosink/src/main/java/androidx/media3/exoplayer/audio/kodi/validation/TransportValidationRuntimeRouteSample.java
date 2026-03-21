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

public final class TransportValidationRuntimeRouteSample {
  private final long elapsedRealtimeMs;
  private final TransportValidationRuntimeRouteSnapshot snapshot;

  public TransportValidationRuntimeRouteSample(
      long elapsedRealtimeMs, TransportValidationRuntimeRouteSnapshot snapshot) {
    this.elapsedRealtimeMs = elapsedRealtimeMs;
    this.snapshot = snapshot;
  }

  public long getElapsedRealtimeMs() {
    return elapsedRealtimeMs;
  }

  public TransportValidationRuntimeRouteSnapshot getSnapshot() {
    return snapshot;
  }
}
