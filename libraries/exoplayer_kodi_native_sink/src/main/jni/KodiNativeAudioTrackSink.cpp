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

#include "KodiNativeAudioTrackSink.h"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "AEStreamInfo.h"

namespace androidx_media3 {
namespace {

constexpr int64_t kCurrentPositionNotSet = INT64_MIN / 2;
constexpr int kChannelInvalid = 0;
constexpr int kEncodingIec61937 = 13;
constexpr int kEncodingInvalid = 0;
constexpr int kEncodingPcm16Bit = 2;
constexpr int kEncodingPcmFloat = 4;
constexpr int kTrueHdSize = 61440;
constexpr int kChannelOutMono = 4;
constexpr int kChannelOutStereo = 12;
constexpr int kChannelOut5Point1 = 252;
constexpr int kChannelOut7Point1Surround = 6396;
constexpr int kModePcm = 1;
constexpr int kModePassthroughDirect = 2;
constexpr int kModePassthroughIecStereo = 3;
constexpr int kModePassthroughIecMultichannel = 4;
constexpr char kLogTag[] = "KodiNativeSink";
constexpr size_t kMovingAverageWindow = 3;
constexpr double kMaxPcmBufferSec = 0.25;
constexpr double kPeriodTimeMaxSec = 0.064;
constexpr double kPeriodTimeMinSec = 0.032;
constexpr double kTargetBufferDurationSec = 0.128;

int64_t CurrentHostCounterNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool UsesIecCarrier(const PlaybackDecision& playback_decision) {
  return playback_decision.mode == kModePassthroughIecStereo ||
         playback_decision.mode == kModePassthroughIecMultichannel;
}

}  // namespace

void KodiNativeAudioTrackSink::Configure(JNIEnv* env,
                                         int sample_rate,
                                         int channel_count,
                                         int pcm_encoding,
                                         int specified_buffer_size,
                                         int output_channel_count,
                                         int audio_session_id,
                                         float volume,
                                         const PlaybackDecision& playback_decision) {
  Release(env);
  if (playback_decision.mode == 0) {
    return;
  }

  transport_encoding_ = GetEncoding(pcm_encoding, playback_decision);
  transport_sample_rate_hz_ = GetSampleRate(sample_rate, playback_decision);
  transport_channel_config_ = GetChannelConfig(
      output_channel_count > 0 ? output_channel_count : channel_count, playback_decision);
  transport_channel_count_ = GetChannelCount(
      transport_channel_config_, output_channel_count > 0 ? output_channel_count : channel_count);
  sink_frame_size_bytes_ = GetFrameSizeBytes(transport_encoding_, transport_channel_count_);
  passthrough_ = playback_decision.mode != kModePcm;
  raw_passthrough_ = playback_decision.mode == kModePassthroughDirect;
  audio_session_id_ = audio_session_id;
  volume_ = volume;

  int channel_config = transport_channel_config_;
  bool retried = false;
  while (audio_track_ == nullptr) {
    int min_buffer_size =
        CJNIAudioTrack::GetMinBufferSize(env, transport_sample_rate_hz_, channel_config, transport_encoding_);
    if (min_buffer_size < 0) {
      LogVerbose("track configure minBuffer failed rate=%d channelConfig=%d encoding=%d result=%d",
                 transport_sample_rate_hz_, channel_config, transport_encoding_, min_buffer_size);
      transport_encoding_ = kEncodingInvalid;
      return;
    }

    if (specified_buffer_size > 0) {
      min_buffer_size = std::max(min_buffer_size, specified_buffer_size);
    }

    min_buffer_size_bytes_ = static_cast<unsigned int>(min_buffer_size);
    if (raw_passthrough_) {
      buffer_size_bytes_ = GetRawPassthroughBufferSizeBytes(
          min_buffer_size, playback_decision, &buffer_size_us_);
      min_buffer_size_bytes_ = static_cast<unsigned int>(buffer_size_bytes_);
      period_frames_ = min_buffer_size_bytes_;
      sink_frame_size_bytes_ = 1;
      audio_track_buffer_sec_ = static_cast<double>(buffer_size_us_) / 1000000.0;
    } else {
      sink_frame_size_bytes_ = static_cast<unsigned int>(
          GetFrameSizeBytes(transport_encoding_, GetChannelCount(channel_config, transport_channel_count_)));
      audio_track_buffer_sec_ =
          static_cast<double>(min_buffer_size_bytes_) / (sink_frame_size_bytes_ * transport_sample_rate_hz_);
      int periods = 1;
      if (audio_track_buffer_sec_ > kMaxPcmBufferSec) {
        const int buffer_frames = transport_sample_rate_hz_ / 4;
        min_buffer_size_bytes_ = static_cast<unsigned int>(buffer_frames) * sink_frame_size_bytes_;
        periods = 5;
      }
      audio_track_buffer_sec_ =
          static_cast<double>(min_buffer_size_bytes_) / (sink_frame_size_bytes_ * transport_sample_rate_hz_);
      while (audio_track_buffer_sec_ < kTargetBufferDurationSec) {
        min_buffer_size_bytes_ += static_cast<unsigned int>(min_buffer_size);
        periods++;
        audio_track_buffer_sec_ =
            static_cast<double>(min_buffer_size_bytes_) / (sink_frame_size_bytes_ * transport_sample_rate_hz_);
      }

      unsigned int period_size = min_buffer_size_bytes_ / std::max(1, periods);
      double period_time =
          static_cast<double>(period_size) / (sink_frame_size_bytes_ * transport_sample_rate_hz_);
      while (period_time >= kPeriodTimeMaxSec && period_size > 1) {
        period_time /= 2.0;
        period_size /= 2;
      }
      while (period_time < kPeriodTimeMinSec) {
        period_size *= 2;
        period_time *= 2.0;
      }

      period_frames_ = period_size / std::max(1U, sink_frame_size_bytes_);
      buffer_size_bytes_ = static_cast<int>(min_buffer_size_bytes_);
      buffer_size_us_ = static_cast<int64_t>(audio_track_buffer_sec_ * 1000000.0);
    }

    audio_track_buffer_sec_orig_ = audio_track_buffer_sec_;
    buffer_size_us_orig_ = buffer_size_us_;
    audio_track_ = CJNIAudioTrack::Create(
        env,
        transport_sample_rate_hz_,
        channel_config,
        transport_encoding_,
        static_cast<int>(min_buffer_size_bytes_),
        audio_session_id_);

    if (audio_track_ != nullptr &&
        audio_track_->getState(env) == CJNIAudioTrack::STATE_INITIALIZED) {
      audio_track_->pause(env);
      audio_track_->flush(env);
      audio_track_->setVolume(env, volume_);
      transport_channel_config_ = channel_config;
      transport_channel_count_ = GetChannelCount(channel_config, transport_channel_count_);
      break;
    }

    if (audio_track_ != nullptr) {
      audio_track_->release(env);
      audio_track_.reset();
    }

    if (!passthrough_) {
      if (channel_config != kChannelOutStereo && channel_config != kChannelOut5Point1) {
        channel_config = kChannelOut5Point1;
        continue;
      }
      if (channel_config != kChannelOutStereo) {
        channel_config = kChannelOutStereo;
        continue;
      }
    } else if (!retried) {
      retried = true;
      usleep(200 * 1000);
      continue;
    }

    transport_encoding_ = kEncodingInvalid;
    return;
  }

  ResetState();
  LogVerbose(
      "track configure rate=%d channelConfig=%d channelCount=%d encoding=%d bufferBytes=%d bufferUs=%lld passthrough=%d raw=%d session=%d",
      transport_sample_rate_hz_,
      transport_channel_config_,
      transport_channel_count_,
      transport_encoding_,
      buffer_size_bytes_,
      static_cast<long long>(buffer_size_us_),
      passthrough_ ? 1 : 0,
      raw_passthrough_ ? 1 : 0,
      audio_session_id_);
}

unsigned int KodiNativeAudioTrackSink::AddPackets(JNIEnv* env,
                                                  uint8_t** data,
                                                  unsigned int frames,
                                                  unsigned int offset_frames,
                                                  const CSampleBuffer& sample) {
  const bool counts_toward_media_position = sample.pool != nullptr;
  if (audio_track_ == nullptr || data == nullptr || data[0] == nullptr || sample.pkt == nullptr) {
    return INT_MAX;
  }

  drained_end_of_stream_ = false;
  if (start_media_time_us_ == kCurrentPositionNotSet) {
    start_media_time_us_ = sample.timestamp;
  }

  const int sink_frame_size = GetSinkFrameSizeBytes(sample);
  if (sink_frame_size <= 0) {
    return INT_MAX;
  }

  bool force_block = false;
  if (!raw_passthrough_ && supervise_audio_delay_enabled_ && sample.pkt->max_nb_samples > 0 &&
      transport_sample_rate_hz_ > 0) {
    const double max_stuck_delay_ms =
        std::max(audio_track_buffer_sec_orig_ * 2000.0, 400.0);
    const double stime_ms =
        static_cast<double>(sample.pkt->max_nb_samples) * 1000.0 / transport_sample_rate_hz_;
    if (static_cast<double>(stuck_counter_) * stime_ms > max_stuck_delay_ms) {
      usleep(static_cast<useconds_t>(max_stuck_delay_ms * 1000.0));
      return INT_MAX;
    }
    if (static_cast<double>(stuck_counter_) * stime_ms >= audio_track_buffer_sec_orig_ * 1000.0) {
      force_block = true;
    }
  }

  const int offset_bytes = static_cast<int>(offset_frames) * sink_frame_size;
  uint8_t* out_buffer = data[0] + offset_bytes;
  const int size_bytes = static_cast<int>(frames) * sink_frame_size;
  const double packet_duration_ms = GetPacketDurationMs(sample);

  if (frames > 0 && audio_track_->getPlayState(env) != CJNIAudioTrack::PLAYSTATE_PLAYING) {
    audio_track_->play(env);
    play_requested_ = true;
  }

  const int64_t delay_before_us = GetDelayUs(env);
  const int64_t start_time_ns = CurrentHostCounterNs();

  int written_bytes = 0;
  int size_left = size_bytes;
  bool retried = false;
  while (written_bytes < size_bytes) {
    const int loop_written = WriteToAudioTrack(env, out_buffer, size_left);
    if (loop_written < 0) {
      LogVerbose("track write error req=%d offset=%d wrote=%d delayBeforeUs=%lld ptsUs=%lld",
                 size_bytes, offset_bytes, loop_written, static_cast<long long>(delay_before_us),
                 static_cast<long long>(sample.timestamp));
      return INT_MAX;
    }

    written_bytes += loop_written;
    size_left -= loop_written;

    if (loop_written == 0) {
      if (!retried) {
        retried = true;
        double sleep_time_ms = 0.0;
        if (raw_passthrough_) {
          sleep_time_ms = packet_duration_ms;
        } else if (transport_sample_rate_hz_ > 0) {
          sleep_time_ms = 1000.0 * period_frames_ / transport_sample_rate_hz_;
        }
        if (sleep_time_ms > 0.0) {
          usleep(static_cast<useconds_t>(sleep_time_ms * 1000.0));
        }
        continue;
      }
      break;
    }

    retried = false;
    if (raw_passthrough_) {
      if (written_bytes == size_bytes) {
        duration_written_sec_ += packet_duration_ms / 1000.0;
      } else {
        break;
      }
    } else {
      duration_written_sec_ +=
          (static_cast<double>(loop_written) / sink_frame_size) / transport_sample_rate_hz_;
    }

    if (written_bytes < size_bytes) {
      out_buffer += loop_written;
    }
  }

  duration_written_us_ = static_cast<int64_t>(duration_written_sec_ * 1000000.0);
  const unsigned int written_frames = static_cast<unsigned int>(written_bytes / sink_frame_size);

  int64_t packet_frames = sample.pkt->max_nb_samples;
  int64_t accounted_frames = 0;
  if (raw_passthrough_) {
    const int64_t packet_duration_us = static_cast<int64_t>(packet_duration_ms * 1000.0);
    if (written_bytes > 0 && size_bytes > 0 && packet_duration_us > 0) {
      const int64_t accounted_duration_us =
          (packet_duration_us * static_cast<int64_t>(written_bytes)) / size_bytes;
      accounted_frames = DurationUsToSampleCount(accounted_duration_us, transport_sample_rate_hz_);
    }
  } else if (packet_frames > 0) {
    if (written_bytes == size_bytes) {
      accounted_frames = packet_frames;
    } else {
      accounted_frames =
          (packet_frames * static_cast<int64_t>(written_bytes)) / std::max(1, sample.pkt->linesize);
    }
  } else if (sink_frame_size > 0) {
    accounted_frames = written_frames;
  }

  if (accounted_frames > 0) {
    submitted_frames_ += accounted_frames;
    if (counts_toward_media_position) {
      media_frames_submitted_ += accounted_frames;
    }
  }

  const double time_to_add_ms = static_cast<double>(CurrentHostCounterNs() - start_time_ns) / 1000000.0;
  const int64_t delay_after_us = GetDelayUs(env);
  if (raw_passthrough_) {
    if (pause_ms_ > 0.0 && packet_duration_ms > 0.0) {
      const double extra_sleep_ms = packet_duration_ms / 4.0 - time_to_add_ms;
      if (extra_sleep_ms > 0.0) {
        pause_ms_ -= extra_sleep_ms;
        pause_us_ = static_cast<int64_t>(pause_ms_ * 1000.0);
        usleep(static_cast<useconds_t>(extra_sleep_ms * 1000.0));
      }
      if (pause_ms_ <= 0.0) {
        pause_ms_ = 0.0;
        pause_us_ = 0;
      }
    }
  } else if (transport_sample_rate_hz_ > 0) {
    const double period_time = period_frames_ / static_cast<double>(transport_sample_rate_hz_);
    if (delay_sec_ >= (audio_track_buffer_sec_ - period_time)) {
      const double time_should_ms = 1000.0 * written_frames / transport_sample_rate_hz_;
      const double time_off_ms = time_should_ms - time_to_add_ms;
      if (time_off_ms > 0.0) {
        usleep(static_cast<useconds_t>(time_off_ms * 500.0));
      }
    }
  }

  if (force_block && transport_sample_rate_hz_ > 0) {
    const double block_time_ms = static_cast<double>(CurrentHostCounterNs() - start_time_ns) / 1000000.0;
    const double extra_sleep_ms = (1000.0 * frames / transport_sample_rate_hz_) - block_time_ms;
    if (extra_sleep_ms > 0.0) {
      usleep(static_cast<useconds_t>(extra_sleep_ms * 1000.0));
    }
  }

  LogVerbose(
      "track write req=%d offset=%d wrote=%d accountedFrames=%lld media=%d submitted=%lld mediaSubmitted=%lld delayBeforeUs=%lld delayAfterUs=%lld ptsUs=%lld",
      size_bytes,
      offset_bytes,
      written_bytes,
      static_cast<long long>(accounted_frames),
      counts_toward_media_position ? 1 : 0,
      static_cast<long long>(submitted_frames_),
      static_cast<long long>(media_frames_submitted_),
      static_cast<long long>(delay_before_us),
      static_cast<long long>(delay_after_us),
      static_cast<long long>(sample.timestamp));
  return written_frames;
}

void KodiNativeAudioTrackSink::Play(JNIEnv* env) {
  play_requested_ = true;
  drained_end_of_stream_ = false;
  if (audio_track_ != nullptr) {
    audio_track_->play(env);
  }
}

void KodiNativeAudioTrackSink::Pause(JNIEnv* env) {
  play_requested_ = false;
  if (audio_track_ != nullptr &&
      audio_track_->getPlayState(env) == CJNIAudioTrack::PLAYSTATE_PLAYING) {
    audio_track_->pause(env);
  }
}

void KodiNativeAudioTrackSink::AddPause(JNIEnv* env, unsigned int millis) {
  if (audio_track_ == nullptr || millis == 0) {
    return;
  }

  if (audio_track_->getPlayState(env) != CJNIAudioTrack::PLAYSTATE_PAUSED) {
    audio_track_->pause(env);
  }

  const double space_ms =
      1000.0 * (audio_track_buffer_sec_ - (delay_sec_ + (pause_ms_ / 1000.0)));
  if (space_ms > millis) {
    pause_ms_ += millis;
  } else if (space_ms <= 0.0) {
    usleep(static_cast<useconds_t>(millis * 1000U));
  } else {
    const double sleep_ms = millis - space_ms;
    pause_ms_ += (millis - sleep_ms);
    usleep(static_cast<useconds_t>(sleep_ms * 1000.0));
  }
  pause_us_ = static_cast<int64_t>(pause_ms_ * 1000.0);
}

void KodiNativeAudioTrackSink::Flush(JNIEnv* env) {
  if (audio_track_ != nullptr) {
    audio_track_->pause(env);
    audio_track_->flush(env);
  }
  ResetState();
}

void KodiNativeAudioTrackSink::Stop(JNIEnv* env) {
  if (audio_track_ != nullptr) {
    audio_track_->stop(env);
  }
  ResetState();
}

void KodiNativeAudioTrackSink::Drain(JNIEnv* env) {
  if (audio_track_ == nullptr) {
    return;
  }

  audio_track_->stop(env);
  audio_track_->pause(env);
  ResetState();
  drained_end_of_stream_ = true;
}

void KodiNativeAudioTrackSink::Release(JNIEnv* env) {
  if (audio_track_ != nullptr) {
    audio_track_->pause(env);
    audio_track_->flush(env);
    audio_track_->release(env);
    audio_track_.reset();
  }

  ResetState();
  transport_encoding_ = kEncodingInvalid;
  transport_sample_rate_hz_ = 0;
  transport_channel_config_ = kChannelInvalid;
  transport_channel_count_ = 0;
  buffer_size_bytes_ = 0;
  min_buffer_size_bytes_ = 0;
  period_frames_ = 0;
  sink_frame_size_bytes_ = 0;
  audio_track_buffer_sec_ = 0.0;
  audio_track_buffer_sec_orig_ = 0.0;
  buffer_size_us_ = 0;
  buffer_size_us_orig_ = 0;
}

void KodiNativeAudioTrackSink::SetVolume(JNIEnv* env, float volume) {
  volume_ = volume;
  if (audio_track_ != nullptr) {
    audio_track_->setVolume(env, volume);
  }
}

bool KodiNativeAudioTrackSink::HasPendingData(JNIEnv* env) {
  return GetDelayUs(env) > 0;
}

bool KodiNativeAudioTrackSink::IsEnded(JNIEnv* env) {
  (void)env;
  return drained_end_of_stream_;
}

int64_t KodiNativeAudioTrackSink::GetCurrentPositionUs(JNIEnv* env) {
  if (audio_track_ == nullptr || start_media_time_us_ == kCurrentPositionNotSet) {
    return kCurrentPositionNotSet;
  }

  const int64_t played_frames =
      std::max<int64_t>(0, submitted_frames_ - DurationUsToSampleCount(GetDelayUs(env), transport_sample_rate_hz_));
  const int64_t played_media_frames = std::min<int64_t>(media_frames_submitted_, played_frames);
  const int64_t position_us =
      start_media_time_us_ + SampleCountToDurationUs(played_media_frames, transport_sample_rate_hz_);

  if (last_logged_position_us_ == kCurrentPositionNotSet ||
      std::llabs(position_us - last_logged_position_us_) >= 50000) {
    last_logged_position_us_ = position_us;
    LogVerbose(
        "track position currentUs=%lld playedFrames=%lld playedMediaFrames=%lld submitted=%lld mediaSubmitted=%lld delayUs=%lld",
        static_cast<long long>(position_us),
        static_cast<long long>(played_frames),
        static_cast<long long>(played_media_frames),
        static_cast<long long>(submitted_frames_),
        static_cast<long long>(media_frames_submitted_),
        static_cast<long long>(delay_us_));
  }
  return position_us;
}

int64_t KodiNativeAudioTrackSink::GetBufferSizeUs() const {
  return static_cast<int64_t>(audio_track_buffer_sec_ * 1000000.0);
}

int KodiNativeAudioTrackSink::GetSinkFrameSizeBytes(const CSampleBuffer& sample) const {
  if (raw_passthrough_) {
    return 1;
  }
  if (passthrough_) {
    return static_cast<int>(sink_frame_size_bytes_ > 0 ? sink_frame_size_bytes_ : 1U);
  }
  if (sample.pkt != nullptr && sample.pkt->max_nb_samples > 0 && sample.pkt->linesize > 0) {
    const int derived = sample.pkt->linesize / sample.pkt->max_nb_samples;
    if (derived > 0) {
      return derived;
    }
  }
  return static_cast<int>(sink_frame_size_bytes_ > 0 ? sink_frame_size_bytes_ : 1U);
}

void KodiNativeAudioTrackSink::ResetState() {
  submitted_frames_ = 0;
  media_frames_submitted_ = 0;
  start_media_time_us_ = kCurrentPositionNotSet;
  last_logged_position_us_ = kCurrentPositionNotSet;
  last_playback_frames_ = 0;
  playback_head_position_ = 0;
  playback_head_position_old_ = 0;
  timestamp_position_ = 0;
  stuck_counter_ = 0;
  drained_end_of_stream_ = false;
  duration_written_sec_ = 0.0;
  duration_written_us_ = 0;
  delay_sec_ = 0.0;
  delay_us_ = 0;
  hardware_delay_sec_ = 0.0;
  hardware_delay_us_ = 0;
  pause_ms_ = 0.0;
  pause_us_ = 0;
  moving_average_delay_sec_.clear();
}

int KodiNativeAudioTrackSink::GetEncoding(int pcm_encoding, const PlaybackDecision& playback_decision) const {
  if (playback_decision.mode == kModePcm) {
    return pcm_encoding != kEncodingInvalid ? pcm_encoding : kEncodingPcm16Bit;
  }
  if (UsesIecCarrier(playback_decision)) {
    return kEncodingIec61937;
  }
  return playback_decision.output_encoding;
}

int KodiNativeAudioTrackSink::GetSampleRate(int sample_rate, const PlaybackDecision& playback_decision) const {
  if (playback_decision.mode == kModePcm) {
    return sample_rate;
  }
  if (UsesIecCarrier(playback_decision)) {
    switch (playback_decision.stream_type) {
      case CAEStreamInfo::STREAM_TYPE_EAC3:
      case CAEStreamInfo::STREAM_TYPE_DTSHD:
      case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      case CAEStreamInfo::STREAM_TYPE_TRUEHD:
        return 192000;
      default:
        return sample_rate;
    }
  }
  if (playback_decision.stream_type == CAEStreamInfo::STREAM_TYPE_TRUEHD) {
    return 192000;
  }
  return sample_rate;
}

int KodiNativeAudioTrackSink::GetChannelConfig(int fallback_channel_count,
                                               const PlaybackDecision& playback_decision) const {
  if (playback_decision.mode == kModePassthroughDirect) {
    switch (playback_decision.stream_type) {
      case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      case CAEStreamInfo::STREAM_TYPE_TRUEHD:
        return kChannelOut7Point1Surround;
      default:
        return kChannelOutStereo;
    }
  }
  if (playback_decision.channel_config != kChannelInvalid) {
    return playback_decision.channel_config;
  }
  if (fallback_channel_count >= 8) {
    return kChannelOut7Point1Surround;
  }
  if (fallback_channel_count >= 6) {
    return kChannelOut5Point1;
  }
  if (fallback_channel_count == 1) {
    return kChannelOutMono;
  }
  return kChannelOutStereo;
}

int KodiNativeAudioTrackSink::GetChannelCount(int channel_config, int fallback_channel_count) const {
  switch (channel_config) {
    case kChannelOutMono:
      return 1;
    case kChannelOutStereo:
      return 2;
    case kChannelOut5Point1:
      return 6;
    case kChannelOut7Point1Surround:
      return 8;
    default:
      return std::max(1, fallback_channel_count);
  }
}

int KodiNativeAudioTrackSink::GetFrameSizeBytes(int encoding, int channel_count) const {
  if (channel_count <= 0) {
    return 0;
  }
  if (encoding == kEncodingIec61937) {
    return channel_count * 2;
  }
  if (encoding == kEncodingPcmFloat) {
    return channel_count * 4;
  }
  return channel_count * 2;
}

int KodiNativeAudioTrackSink::GetRawPassthroughBufferSizeBytes(
    int min_buffer_size, const PlaybackDecision& playback_decision, int64_t* buffer_size_us) const {
  int min_buffer = min_buffer_size;
  int64_t raw_length_us = 400000;
  switch (playback_decision.stream_type) {
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      min_buffer = 2 * kTrueHdSize;
      raw_length_us = 80000;
      break;
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
      min_buffer = 4 * 30720;
      raw_length_us = 170664;
      break;
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      min_buffer = 16 * 2012;
      raw_length_us = 170656;
      break;
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
      min_buffer = 8 * 5462;
      raw_length_us = 170664;
      break;
    case CAEStreamInfo::STREAM_TYPE_AC3:
      min_buffer = std::max(min_buffer * 3, 1536 * 8);
      raw_length_us = 256000;
      break;
    case CAEStreamInfo::STREAM_TYPE_EAC3:
      min_buffer = 2 * 10752;
      raw_length_us = 256000;
      break;
    default:
      min_buffer = std::max(min_buffer, 16384);
      raw_length_us = 400000;
      break;
  }
  if (buffer_size_us != nullptr) {
    *buffer_size_us = raw_length_us;
  }
  return min_buffer;
}

int64_t KodiNativeAudioTrackSink::SampleCountToDurationUs(int64_t sample_count, int sample_rate) const {
  if (sample_count <= 0 || sample_rate <= 0) {
    return 0;
  }
  return (sample_count * 1000000LL) / sample_rate;
}

int64_t KodiNativeAudioTrackSink::DurationUsToSampleCount(int64_t duration_us, int sample_rate) const {
  if (duration_us <= 0 || sample_rate <= 0) {
    return 0;
  }
  return (duration_us * sample_rate) / 1000000LL;
}

double KodiNativeAudioTrackSink::GetPacketDurationMs(const CSampleBuffer& sample) const {
  if (passthrough_ && sample.stream_info.m_type != CAEStreamInfo::STREAM_TYPE_NULL) {
    const double duration_ms = sample.stream_info.GetDuration();
    if (duration_ms > 0.0) {
      return duration_ms;
    }
  }
  if (sample.pkt != nullptr && sample.pkt->max_nb_samples > 0 && transport_sample_rate_hz_ > 0) {
    return static_cast<double>(sample.pkt->max_nb_samples) * 1000.0 / transport_sample_rate_hz_;
  }
  return 0.0;
}

int64_t KodiNativeAudioTrackSink::GetPlaybackFrames(JNIEnv* env) {
  if (audio_track_ == nullptr || transport_sample_rate_hz_ <= 0) {
    return 0;
  }

  const uint32_t head_pos = static_cast<uint32_t>(audio_track_->getPlaybackHeadPosition(env));
  const uint32_t minwrapvalue = UINT32_MAX - static_cast<uint32_t>(transport_sample_rate_hz_ / 10);
  const uint32_t remain = static_cast<uint32_t>(playback_head_position_ & 0xFFFFFFFFULL);
  if ((remain > head_pos) && (remain >= minwrapvalue)) {
    playback_head_position_ += (1ULL << 32);
  }
  playback_head_position_ =
      (playback_head_position_ & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(head_pos);

  if (playback_head_position_ == playback_head_position_old_) {
    stuck_counter_++;
  } else {
    stuck_counter_ = 0;
    playback_head_position_old_ = playback_head_position_;
  }

  last_playback_frames_ = static_cast<int64_t>(playback_head_position_);
  return last_playback_frames_;
}

int64_t KodiNativeAudioTrackSink::GetDelayUs(JNIEnv* env) {
  if (audio_track_ == nullptr || transport_sample_rate_hz_ <= 0) {
    return 0;
  }

  const int64_t head_frames = GetPlaybackFrames(env);
  double gone_sec = static_cast<double>(head_frames) / transport_sample_rate_hz_;
  if (gone_sec > duration_written_sec_) {
    gone_sec = duration_written_sec_;
  }

  double delay_sec = duration_written_sec_ - gone_sec;
  const int latency_ms = audio_track_->getLatency(env);
  const bool is_raw_pt = raw_passthrough_;
  if (!is_raw_pt && latency_ms != -1) {
    hardware_delay_sec_ = latency_ms / 1000.0;
    const int at_buffer = audio_track_->getBufferSizeInFrames(env);
    if (at_buffer > 0) {
      hardware_delay_sec_ -= static_cast<double>(at_buffer) / transport_sample_rate_hz_;
    }
  } else {
    uint64_t frame_position = 0;
    int64_t nano_time = 0;
    if (audio_track_->getTimestamp(env, &frame_position, &nano_time)) {
      const int64_t delta_ns = CurrentHostCounterNs() - nano_time;
      if (frame_position > 0 && delta_ns < 2LL * 1000 * 1000 * 1000) {
        uint64_t stamp_head =
            (frame_position & 0xFFFFFFFFULL) +
            static_cast<uint64_t>(delta_ns * transport_sample_rate_hz_ / 1000000000.0);
        if (stamp_head < timestamp_position_ &&
            (timestamp_position_ - stamp_head) > 0x7FFFFFFFFFFFFFFFULL) {
          uint64_t stamp = timestamp_position_;
          stamp += (1ULL << 32);
          stamp_head = (stamp & 0xFFFFFFFF00000000ULL) | stamp_head;
        }
        timestamp_position_ = stamp_head;
        const double playtime_sec = timestamp_position_ / static_cast<double>(transport_sample_rate_hz_);
        hardware_delay_sec_ = duration_written_sec_ - playtime_sec;
        if (hardware_delay_sec_ < delay_sec) {
          hardware_delay_sec_ = 0.0;
        } else {
          hardware_delay_sec_ -= delay_sec;
        }
      }
    }
  }

  delay_sec += hardware_delay_sec_;
  if (delay_sec < 0.0) {
    delay_sec = 0.0;
  }
  delay_sec += pause_ms_ / 1000.0;
  delay_sec_ = GetMovingAverageDelay(delay_sec);
  if (delay_sec_ > audio_track_buffer_sec_) {
    audio_track_buffer_sec_ = delay_sec_;
  }

  hardware_delay_us_ = static_cast<int64_t>(hardware_delay_sec_ * 1000000.0);
  delay_us_ = static_cast<int64_t>(delay_sec_ * 1000000.0);
  return delay_us_;
}

double KodiNativeAudioTrackSink::GetMovingAverageDelay(double newest_delay_sec) {
  moving_average_delay_sec_.push_back(newest_delay_sec);
  if (moving_average_delay_sec_.size() > kMovingAverageWindow) {
    moving_average_delay_sec_.pop_front();
  }

  double sum = 0.0;
  for (double delay_sample : moving_average_delay_sec_) {
    sum += delay_sample;
  }
  return moving_average_delay_sec_.empty() ? newest_delay_sec : sum / moving_average_delay_sec_.size();
}

int KodiNativeAudioTrackSink::WriteToAudioTrack(JNIEnv* env, const uint8_t* data, int size_bytes) {
  if (audio_track_ == nullptr || data == nullptr || size_bytes <= 0) {
    return 0;
  }

  if (transport_encoding_ == kEncodingPcmFloat) {
    const int float_count = size_bytes / static_cast<int>(sizeof(float));
    float_write_buffer_.resize(float_count);
    std::memcpy(float_write_buffer_.data(), data, float_count * sizeof(float));
    return audio_track_->write(env, float_write_buffer_, CJNIAudioTrack::WRITE_BLOCKING);
  }

  if (transport_encoding_ == kEncodingIec61937) {
    const int short_count = size_bytes / static_cast<int>(sizeof(int16_t));
    short_write_buffer_.resize(short_count);
    std::memcpy(short_write_buffer_.data(), data, short_count * sizeof(int16_t));
    return audio_track_->write(env, short_write_buffer_, CJNIAudioTrack::WRITE_BLOCKING);
  }

  byte_write_buffer_.resize(size_bytes);
  std::memcpy(byte_write_buffer_.data(), data, size_bytes);
  return audio_track_->write(env, byte_write_buffer_.data(), size_bytes, CJNIAudioTrack::WRITE_BLOCKING);
}

void KodiNativeAudioTrackSink::LogVerbose(const char* format, ...) const {
  if (!verbose_logging_enabled_) {
    return;
  }

  va_list args;
  va_start(args, format);
  __android_log_vprint(ANDROID_LOG_INFO, kLogTag, format, args);
  va_end(args);
}

}  // namespace androidx_media3
