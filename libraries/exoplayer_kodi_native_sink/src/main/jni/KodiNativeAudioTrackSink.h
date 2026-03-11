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
#include <deque>
#include <memory>
#include <vector>

#include "CJNIAudioTrack.h"
#include "KodiCapabilitySelector.h"
#include "KodiActiveAEBufferCompat.h"

namespace androidx_media3 {

class KodiNativeAudioTrackSink {
 public:
  void set_verbose_logging_enabled(bool verbose_logging_enabled) {
    verbose_logging_enabled_ = verbose_logging_enabled;
  }
  void set_supervise_audio_delay_enabled(bool supervise_audio_delay_enabled) {
    supervise_audio_delay_enabled_ = supervise_audio_delay_enabled;
  }
  void Configure(JNIEnv* env,
                 int sample_rate,
                 int channel_count,
                 int pcm_encoding,
                 int specified_buffer_size,
                 int output_channel_count,
                 int audio_session_id,
                 float volume,
                 const PlaybackDecision& playback_decision);
  unsigned int AddPackets(JNIEnv* env,
                          uint8_t** data,
                          unsigned int frames,
                          unsigned int offset_frames,
                          const CSampleBuffer& sample);
  void Play(JNIEnv* env);
  void Pause(JNIEnv* env);
  void AddPause(JNIEnv* env, unsigned int millis);
  void Flush(JNIEnv* env);
  void Stop(JNIEnv* env);
  void Drain(JNIEnv* env);
  void Release(JNIEnv* env);
  void SetVolume(JNIEnv* env, float volume);
  bool HasPendingData(JNIEnv* env);
  bool IsEnded(JNIEnv* env);
  int64_t GetCurrentPositionUs(JNIEnv* env);
  int64_t GetBufferSizeUs() const;
  int GetTransportSampleRateHz() const { return transport_sample_rate_hz_; }
  unsigned int GetFramesPerWrite() const { return period_frames_; }
  int GetSinkFrameSizeBytes(const CSampleBuffer& sample) const;

 private:
  int transport_encoding_ = 0;
  int transport_sample_rate_hz_ = 0;
  int transport_channel_config_ = 0;
  int transport_channel_count_ = 0;
  int buffer_size_bytes_ = 0;
  unsigned int min_buffer_size_bytes_ = 0;
  unsigned int period_frames_ = 0;
  unsigned int sink_frame_size_bytes_ = 0;
  int audio_session_id_ = 0;
  bool verbose_logging_enabled_ = false;
  bool supervise_audio_delay_enabled_ = false;
  bool passthrough_ = false;
  bool raw_passthrough_ = false;
  bool play_requested_ = false;
  bool drained_end_of_stream_ = false;
  int64_t submitted_frames_ = 0;
  int64_t media_frames_submitted_ = 0;
  int64_t start_media_time_us_ = -1;
  int64_t buffer_size_us_ = 0;
  int64_t buffer_size_us_orig_ = 0;
  int64_t last_logged_position_us_ = -1;
  int64_t last_playback_frames_ = 0;
  int64_t duration_written_us_ = 0;
  int64_t delay_us_ = 0;
  int64_t hardware_delay_us_ = 0;
  int64_t pause_us_ = 0;
  int stuck_counter_ = 0;
  uint64_t playback_head_position_ = 0;
  uint64_t playback_head_position_old_ = 0;
  uint64_t timestamp_position_ = 0;
  double duration_written_sec_ = 0.0;
  double audio_track_buffer_sec_ = 0.0;
  double audio_track_buffer_sec_orig_ = 0.0;
  double delay_sec_ = 0.0;
  double hardware_delay_sec_ = 0.0;
  double pause_ms_ = 0.0;
  float volume_ = 1.0f;
  std::unique_ptr<CJNIAudioTrack> audio_track_;
  std::vector<int16_t> short_write_buffer_;
  std::vector<float> float_write_buffer_;
  std::vector<uint8_t> byte_write_buffer_;
  std::deque<double> moving_average_delay_sec_;

  void ResetState();
  int GetEncoding(int pcm_encoding, const PlaybackDecision& playback_decision) const;
  int GetSampleRate(int sample_rate, const PlaybackDecision& playback_decision) const;
  int GetChannelConfig(int fallback_channel_count, const PlaybackDecision& playback_decision) const;
  int GetChannelCount(int channel_config, int fallback_channel_count) const;
  int GetFrameSizeBytes(int encoding, int channel_count) const;
  int GetRawPassthroughBufferSizeBytes(int min_buffer_size,
                                       const PlaybackDecision& playback_decision,
                                       int64_t* buffer_size_us) const;
  int64_t SampleCountToDurationUs(int64_t sample_count, int sample_rate) const;
  int64_t DurationUsToSampleCount(int64_t duration_us, int sample_rate) const;
  double GetPacketDurationMs(const CSampleBuffer& sample) const;
  int64_t GetPlaybackFrames(JNIEnv* env);
  int64_t GetDelayUs(JNIEnv* env);
  double GetMovingAverageDelay(double newest_delay_sec);
  int WriteToAudioTrack(JNIEnv* env, const uint8_t* data, int size_bytes);
  void LogVerbose(const char* format, ...) const;
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_NATIVE_AUDIO_TRACK_SINK_H_
