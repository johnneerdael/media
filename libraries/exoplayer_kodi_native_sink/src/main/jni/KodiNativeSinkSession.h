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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_NATIVE_SINK_SESSION_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_NATIVE_SINK_SESSION_H_

#include <jni.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "KodiCapabilitySelector.h"

namespace androidx_media3 {

struct PacketMetadata {
  int kind;
  int size_bytes;
  int64_t total_frames;
  int normalized_access_units;
  int64_t effective_presentation_time_us;
};

class KodiAndroidPassthroughEngine;
class KodiNativeAudioTrackSink;

class KodiNativeSinkSession {
 public:
  KodiNativeSinkSession();
  ~KodiNativeSinkSession();

  void Configure(int mime_kind,
                 JNIEnv* env,
                 int sample_rate,
                 int channel_count,
                 int pcm_encoding,
                 int specified_buffer_size,
                 int output_channel_count,
                 int audio_session_id,
                 float volume,
                 bool verbose_logging_enabled,
                 const CapabilitySnapshot& capability_snapshot,
                 const PlaybackDecision& playback_decision);
  void QueueInput(const uint8_t* data,
                  int size,
                  int64_t presentation_time_us,
                  int encoded_access_unit_count);
  void QueuePause(unsigned int millis, bool iec_bursts);
  bool DequeuePacket(PacketMetadata* packet);
  bool TakeLastDequeuedPacketData(std::vector<uint8_t>* data);
  void Play(JNIEnv* env);
  void Pause(JNIEnv* env);
  void Flush(JNIEnv* env);
  void Stop(JNIEnv* env);
  void Reset(JNIEnv* env);
  void PlayToEndOfStream(JNIEnv* env);
  void SetVolume(JNIEnv* env, float volume);
  bool DrainOnePacketToAudioTrack(JNIEnv* env, bool counts_toward_media_position, PacketMetadata* packet);
  int64_t GetCurrentPositionUs(JNIEnv* env);
  bool HasPendingData(JNIEnv* env);
  bool IsEnded(JNIEnv* env);
  int64_t GetBufferSizeUs() const;
  int pending_packet_count() const;
  int64_t queued_input_bytes() const;

 private:
  KodiAndroidPassthroughEngine* engine_;
  std::unique_ptr<KodiNativeAudioTrackSink> audio_track_sink_;
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_NATIVE_SINK_SESSION_H_
