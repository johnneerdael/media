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

#include <algorithm>
#include <cstring>

#include "AEStreamInfo.h"

namespace androidx_media3 {
namespace {

constexpr int64_t kCurrentPositionNotSet = INT64_MIN / 2;
constexpr int kChannelInvalid = 0;
constexpr int kEncodingIec61937 = 13;
constexpr int kEncodingInvalid = 0;
constexpr int kEncodingPcm16Bit = 2;
constexpr int kEncodingPcmFloat = 4;
constexpr int kChannelOutMono = 4;
constexpr int kChannelOutStereo = 12;
constexpr int kChannelOut5Point1 = 252;
constexpr int kChannelOut7Point1Surround = 6396;
constexpr int kModePcm = 1;
constexpr int kModePassthroughDirect = 2;
constexpr int kModePassthroughIecStereo = 3;
constexpr int kModePassthroughIecMultichannel = 4;

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
  transport_channel_config_ =
      GetChannelConfig(output_channel_count > 0 ? output_channel_count : channel_count, playback_decision);
  transport_channel_count_ =
      GetChannelCount(transport_channel_config_, output_channel_count > 0 ? output_channel_count : channel_count);
  transport_frame_size_bytes_ = GetFrameSizeBytes(transport_encoding_, transport_channel_count_);
  passthrough_ = playback_decision.mode != kModePcm;
  raw_passthrough_ = playback_decision.mode == kModePassthroughDirect;
  audio_session_id_ = audio_session_id;
  volume_ = volume;

  int min_buffer_size =
      CJNIAudioTrack::GetMinBufferSize(env, transport_sample_rate_hz_, transport_channel_config_, transport_encoding_);
  if (min_buffer_size < 0) {
    min_buffer_size = std::max(2048, specified_buffer_size);
  }
  buffer_size_bytes_ =
      specified_buffer_size > 0 ? std::max(specified_buffer_size, min_buffer_size) : min_buffer_size;
  audio_track_ = CJNIAudioTrack::Create(
      env,
      transport_sample_rate_hz_,
      transport_channel_config_,
      transport_encoding_,
      buffer_size_bytes_,
      audio_session_id_);
  if (audio_track_ != nullptr) {
    audio_track_->setVolume(env, volume_);
  }
  buffer_size_us_ = SampleCountToDurationUs(
      transport_frame_size_bytes_ > 0 ? buffer_size_bytes_ / transport_frame_size_bytes_ : 0,
      transport_sample_rate_hz_);
  ResetState();
}

void KodiNativeAudioTrackSink::WritePacket(JNIEnv* env,
                                           const PacketMetadata& packet,
                                           const uint8_t* data,
                                           bool counts_toward_media_position) {
  if (audio_track_ == nullptr || data == nullptr || packet.size_bytes <= 0) {
    return;
  }
  play_to_end_of_stream_requested_ = false;
  drained_end_of_stream_ = false;
  if (start_media_time_us_ == kCurrentPositionNotSet) {
    start_media_time_us_ = packet.effective_presentation_time_us;
  }
  if (play_requested_ && audio_track_->getPlayState(env) != CJNIAudioTrack::PLAYSTATE_PLAYING) {
    audio_track_->play(env);
  }
  int written_bytes = WriteToAudioTrack(env, data, packet.size_bytes);
  if (written_bytes <= 0) {
    return;
  }
  int64_t packet_frames = packet.total_frames;
  if (packet_frames <= 0 && transport_frame_size_bytes_ > 0) {
    packet_frames = written_bytes / transport_frame_size_bytes_;
  }
  if (packet_frames > 0) {
    submitted_frames_ += packet_frames;
    if (counts_toward_media_position) {
      media_frames_submitted_ += packet_frames;
    }
  }
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
  transport_frame_size_bytes_ = 0;
  buffer_size_bytes_ = 0;
  buffer_size_us_ = 0;
}

void KodiNativeAudioTrackSink::PlayToEndOfStream(JNIEnv* env) {
  play_to_end_of_stream_requested_ = true;
  MaybeDrainCompletedPlayback(env);
}

void KodiNativeAudioTrackSink::SetVolume(JNIEnv* env, float volume) {
  volume_ = volume;
  if (audio_track_ != nullptr) {
    audio_track_->setVolume(env, volume);
  }
}

bool KodiNativeAudioTrackSink::HasPendingData(JNIEnv* env) {
  MaybeDrainCompletedPlayback(env);
  return GetDelayUs(env) > 0;
}

bool KodiNativeAudioTrackSink::IsEnded(JNIEnv* env) {
  MaybeDrainCompletedPlayback(env);
  return drained_end_of_stream_;
}

int64_t KodiNativeAudioTrackSink::GetCurrentPositionUs(JNIEnv* env) {
  if (audio_track_ == nullptr || start_media_time_us_ == kCurrentPositionNotSet) {
    return kCurrentPositionNotSet;
  }
  const int64_t played_frames =
      std::max<int64_t>(0, submitted_frames_ - DurationUsToSampleCount(GetDelayUs(env), transport_sample_rate_hz_));
  const int64_t played_media_frames = std::min<int64_t>(media_frames_submitted_, played_frames);
  return start_media_time_us_ + SampleCountToDurationUs(played_media_frames, transport_sample_rate_hz_);
}

int64_t KodiNativeAudioTrackSink::GetBufferSizeUs() const {
  return buffer_size_us_;
}

void KodiNativeAudioTrackSink::ResetState() {
  submitted_frames_ = 0;
  media_frames_submitted_ = 0;
  playback_head_wrap_count_ = 0;
  last_playback_head_position_ = 0;
  start_media_time_us_ = kCurrentPositionNotSet;
  play_to_end_of_stream_requested_ = false;
  drained_end_of_stream_ = false;
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

int64_t KodiNativeAudioTrackSink::GetPlaybackFrames(JNIEnv* env) {
  if (audio_track_ == nullptr) {
    return 0;
  }
  uint64_t playback_head_position =
      static_cast<uint32_t>(audio_track_->getPlaybackHeadPosition(env));
  if (playback_head_position < last_playback_head_position_) {
    playback_head_wrap_count_++;
  }
  last_playback_head_position_ = playback_head_position;
  return playback_head_position + (playback_head_wrap_count_ << 32);
}

int64_t KodiNativeAudioTrackSink::GetDelayUs(JNIEnv* env) {
  if (audio_track_ == nullptr || transport_sample_rate_hz_ <= 0) {
    return 0;
  }
  int64_t head_frames = GetPlaybackFrames(env);
  if (head_frames > submitted_frames_) {
    head_frames = submitted_frames_;
  }
  return SampleCountToDurationUs(std::max<int64_t>(0, submitted_frames_ - head_frames), transport_sample_rate_hz_);
}

void KodiNativeAudioTrackSink::MaybeDrainCompletedPlayback(JNIEnv* env) {
  if (!play_to_end_of_stream_requested_ || drained_end_of_stream_ || audio_track_ == nullptr) {
    return;
  }
  if (GetDelayUs(env) > 0) {
    return;
  }
  audio_track_->stop(env);
  audio_track_->pause(env);
  ResetState();
  drained_end_of_stream_ = true;
}

int KodiNativeAudioTrackSink::WriteToAudioTrack(JNIEnv* env, const uint8_t* data, int size_bytes) {
  if (audio_track_ == nullptr || data == nullptr || size_bytes <= 0) {
    return 0;
  }
  if (transport_encoding_ == kEncodingIec61937) {
    const int short_count = size_bytes / 2;
    short_write_buffer_.resize(short_count);
    std::memcpy(short_write_buffer_.data(), data, short_count * sizeof(int16_t));
    return audio_track_->write(env, short_write_buffer_, CJNIAudioTrack::WRITE_BLOCKING);
  }
  if (transport_encoding_ == kEncodingPcmFloat) {
    const int float_count = size_bytes / 4;
    float_write_buffer_.resize(float_count);
    std::memcpy(float_write_buffer_.data(), data, float_count * sizeof(float));
    return audio_track_->write(env, float_write_buffer_, CJNIAudioTrack::WRITE_BLOCKING);
  }
  return audio_track_->write(env, data, size_bytes, CJNIAudioTrack::WRITE_BLOCKING);
}

}  // namespace androidx_media3
