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

public final class TransportValidationRuntimeEvent {
  public final long elapsedRealtimeMs;
  public final String type;
  public final long value;
  @Nullable public final String detail;

  public TransportValidationRuntimeEvent(
      long elapsedRealtimeMs, String type, long value, @Nullable String detail) {
    this.elapsedRealtimeMs = elapsedRealtimeMs;
    this.type = type;
    this.value = value;
    this.detail = detail;
  }
}
