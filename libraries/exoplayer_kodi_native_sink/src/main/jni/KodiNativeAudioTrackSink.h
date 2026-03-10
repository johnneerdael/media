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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_NATIVE_AUDIO_TRACK_SINK_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_NATIVE_AUDIO_TRACK_SINK_H_

#include <jni.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "CJNIAudioTrack.h"
#include "KodiCapabilitySelector.h"
#include "KodiNativeSinkSession.h"

namespace androidx_media3 {

class KodiNativeAudioTrackSink {
 public:
  void Configure(JNIEnv* env,
                 int sample_rate,
                 int channel_count,
                 int pcm_encoding,
                 int specified_buffer_size,
                 int output_channel_count,
                 int audio_session_id,
                 float volume,
                 const PlaybackDecision& playback_decision);
  void WritePacket(JNIEnv* env,
                   const PacketMetadata& packet,
                   const uint8_t* data,
                   bool counts_toward_media_position);
  void Play(JNIEnv* env);
  void Pause(JNIEnv* env);
  void Flush(JNIEnv* env);
  void Stop(JNIEnv* env);
  void Release(JNIEnv* env);
  void PlayToEndOfStream(JNIEnv* env);
  void SetVolume(JNIEnv* env, float volume);
  bool HasPendingData(JNIEnv* env);
  bool IsEnded(JNIEnv* env);
  int64_t GetCurrentPositionUs(JNIEnv* env);
  int64_t GetBufferSizeUs() const;

 private:
  int transport_encoding_ = 0;
  int transport_sample_rate_hz_ = 0;
  int transport_channel_config_ = 0;
  int transport_channel_count_ = 0;
  int transport_frame_size_bytes_ = 0;
  int buffer_size_bytes_ = 0;
  int audio_session_id_ = 0;
  bool passthrough_ = false;
  bool raw_passthrough_ = false;
  bool play_requested_ = false;
  bool play_to_end_of_stream_requested_ = false;
  bool drained_end_of_stream_ = false;
  int64_t submitted_frames_ = 0;
  int64_t media_frames_submitted_ = 0;
  int64_t start_media_time_us_ = -1;
  int64_t buffer_size_us_ = 0;
  uint64_t playback_head_wrap_count_ = 0;
  uint64_t last_playback_head_position_ = 0;
  float volume_ = 1.0f;
  std::unique_ptr<CJNIAudioTrack> audio_track_;
  std::vector<int16_t> short_write_buffer_;
  std::vector<float> float_write_buffer_;

  void ResetState();
  int GetEncoding(int pcm_encoding, const PlaybackDecision& playback_decision) const;
  int GetSampleRate(int sample_rate, const PlaybackDecision& playback_decision) const;
  int GetChannelConfig(int fallback_channel_count, const PlaybackDecision& playback_decision) const;
  int GetChannelCount(int channel_config, int fallback_channel_count) const;
  int GetFrameSizeBytes(int encoding, int channel_count) const;
  int64_t SampleCountToDurationUs(int64_t sample_count, int sample_rate) const;
  int64_t DurationUsToSampleCount(int64_t duration_us, int sample_rate) const;
  int64_t GetPlaybackFrames(JNIEnv* env);
  int64_t GetDelayUs(JNIEnv* env);
  void MaybeDrainCompletedPlayback(JNIEnv* env);
  int WriteToAudioTrack(JNIEnv* env, const uint8_t* data, int size_bytes);
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_NATIVE_AUDIO_TRACK_SINK_H_
