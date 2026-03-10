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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ANDROID_PASSTHROUGH_ENGINE_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ANDROID_PASSTHROUGH_ENGINE_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "AEAudioFormat.h"
#include "AEBitstreamPacker.h"
#include "AEStreamInfo.h"
#include "DVDAudioCodecPassthrough.h"
#include "KodiCapabilitySelector.h"
#include "KodiNativeSinkSession.h"

namespace androidx_media3 {

class KodiAndroidPassthroughEngine {
 public:
  void set_verbose_logging_enabled(bool verbose_logging_enabled) {
    verbose_logging_enabled_ = verbose_logging_enabled;
  }
  void Configure(int mime_kind,
                 int sample_rate,
                 int channel_count,
                 int pcm_encoding,
                 int specified_buffer_size,
                 int output_channel_count,
                 const CapabilitySnapshot& capability_snapshot,
                 const PlaybackDecision& playback_decision);
  void QueueInput(const uint8_t* data,
                  int size,
                  int64_t presentation_time_us,
                  int encoded_access_unit_count);
  void QueuePause(unsigned int millis, bool iec_bursts);
  bool DequeuePacket(PacketMetadata* packet);
  bool TakeLastDequeuedPacketData(std::vector<uint8_t>* data);
  void Play();
  void Pause();
  void Flush();
  void Stop();
  void Reset();
  int pending_packet_count() const;
  int64_t queued_input_bytes() const;

 private:
  int sample_rate_ = 0;
  int channel_count_ = 0;
  int pcm_encoding_ = 0;
  int specified_buffer_size_ = 0;
  int output_channel_count_ = 0;
  PlaybackDecision playback_decision_ = {};
  bool verbose_logging_enabled_ = false;
  bool playing_ = false;
  int64_t queued_input_bytes_ = 0;
  CAEBitstreamPacker bitstream_packer_;
  std::unique_ptr<CDVDAudioCodecPassthrough> passthrough_codec_;
  AEAudioFormat audio_format_ = {};
  CAEStreamInfo stream_info_ = {};
  std::vector<uint8_t> last_dequeued_packet_data_;

  struct PendingPacket {
    PacketMetadata metadata;
    std::vector<uint8_t> data;
  };

  std::deque<PendingPacket> pending_packets_;

  bool ConfigureStreamInfo();
  bool UsesKodiPassthroughCodec() const;
  void QueueCopiedOutput(int kind,
                         const uint8_t* data,
                         int size,
                         int64_t presentation_time_us,
                         int encoded_access_unit_count,
                         int64_t total_frames);
  void QueueCodecFrames(const uint8_t* data,
                        int size,
                        int64_t presentation_time_us,
                        int encoded_access_unit_count);
  void QueuePackedOutput(int64_t presentation_time_us, int encoded_access_unit_count);
  void LogVerbose(const char* format, ...) const;
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ANDROID_PASSTHROUGH_ENGINE_H_
