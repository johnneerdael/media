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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CAPABILITY_SELECTOR_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CAPABILITY_SELECTOR_H_

namespace androidx_media3 {

struct ProbeResult {
  bool supported;
  int encoding;
  int channel_config;
};

struct CapabilitySnapshot {
  int sdk_int;
  bool tv;
  bool automotive;
  int routed_device_id;
  int routed_device_type;
  int max_channel_count;
  ProbeResult ac3;
  ProbeResult eac3;
  ProbeResult dts;
  ProbeResult dtshd;
  ProbeResult truehd;
};

struct UserAudioSettings {
  bool passthrough_enabled;
  bool ac3_passthrough_enabled;
  bool eac3_passthrough_enabled;
  bool dts_passthrough_enabled;
  bool truehd_passthrough_enabled;
  bool dtshd_passthrough_enabled;
  bool dtshd_core_fallback_enabled;
  int max_pcm_channel_layout;
};

struct PlaybackDecision {
  int mode;
  int output_encoding;
  int channel_config;
  int stream_type;
  int flags;
};

enum MimeKind {
  kMimeKindUnknown = 0,
  kMimeKindAc3 = 1,
  kMimeKindEAc3 = 2,
  kMimeKindDts = 3,
  kMimeKindDtsHd = 4,
  kMimeKindDtsUhd = 5,
  kMimeKindTrueHd = 6,
  kMimeKindPcm = 7,
};

PlaybackDecision EvaluatePlaybackDecision(
    const CapabilitySnapshot& snapshot,
    const UserAudioSettings& user_settings,
    int mime_kind,
    int channel_count,
    int sample_rate);

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CAPABILITY_SELECTOR_H_
