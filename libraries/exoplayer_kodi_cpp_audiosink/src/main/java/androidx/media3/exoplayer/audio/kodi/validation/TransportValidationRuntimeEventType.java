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

public final class TransportValidationRuntimeEventType {
  public static final String AUDIO_UNDERRUN = "audio_underrun";
  public static final String AUDIOTRACK_RESTART = "audiotrack_restart";
  public static final String AUDIO_WRITE_SUCCESS = "audio_write_success";
  public static final String AUDIO_WRITE_ZERO = "audio_write_zero";
  public static final String AUDIO_WRITE_PARTIAL = "audio_write_partial";
  public static final String AUDIO_WRITE_DEFERRED = "audio_write_deferred";
  public static final String ROUTE_SNAPSHOT = "route_snapshot";
  public static final String ROUTE_REOPEN_CANDIDATE = "route_reopen_candidate";
  public static final String PLAYBACK_HEAD_POSITION = "playback_head_position";

  private TransportValidationRuntimeEventType() {}
}
