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

#include "KodiNativeSinkSession.h"

#include "KodiAndroidPassthroughEngine.h"
#include "KodiNativeAudioTrackSink.h"

namespace androidx_media3 {

KodiNativeSinkSession::KodiNativeSinkSession()
    : engine_(new KodiAndroidPassthroughEngine()),
      audio_track_sink_(std::make_unique<KodiNativeAudioTrackSink>()) {}

KodiNativeSinkSession::~KodiNativeSinkSession() { delete engine_; }

void KodiNativeSinkSession::Configure(int mime_kind,
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
                                      const PlaybackDecision& playback_decision) {
  engine_->set_verbose_logging_enabled(verbose_logging_enabled);
  engine_->Configure(mime_kind,
                     sample_rate,
                     channel_count,
                     pcm_encoding,
                     specified_buffer_size,
                     output_channel_count,
                     capability_snapshot,
                     playback_decision);
  audio_track_sink_->set_verbose_logging_enabled(verbose_logging_enabled);
  audio_track_sink_->Configure(env,
                               sample_rate,
                               channel_count,
                               pcm_encoding,
                               specified_buffer_size,
                               output_channel_count,
                               audio_session_id,
                               volume,
                               playback_decision);
}

void KodiNativeSinkSession::QueueInput(const uint8_t* data,
                                       int size,
                                       int64_t presentation_time_us,
                                       int encoded_access_unit_count) {
  engine_->QueueInput(data, size, presentation_time_us, encoded_access_unit_count);
}

void KodiNativeSinkSession::QueuePause(unsigned int millis, bool iec_bursts) {
  engine_->QueuePause(millis, iec_bursts);
}

bool KodiNativeSinkSession::DequeuePacket(PacketMetadata* packet) {
  return engine_->DequeuePacket(packet);
}

bool KodiNativeSinkSession::TakeLastDequeuedPacketData(std::vector<uint8_t>* data) {
  return engine_->TakeLastDequeuedPacketData(data);
}

void KodiNativeSinkSession::Play(JNIEnv* env) {
  engine_->Play();
  audio_track_sink_->Play(env);
}

void KodiNativeSinkSession::Pause(JNIEnv* env) {
  engine_->Pause();
  audio_track_sink_->Pause(env);
}

void KodiNativeSinkSession::Flush(JNIEnv* env) {
  engine_->Flush();
  audio_track_sink_->Flush(env);
}

void KodiNativeSinkSession::Stop(JNIEnv* env) {
  engine_->Stop();
  audio_track_sink_->Stop(env);
}

void KodiNativeSinkSession::Reset(JNIEnv* env) {
  engine_->Reset();
  audio_track_sink_->Release(env);
}

void KodiNativeSinkSession::PlayToEndOfStream(JNIEnv* env) {
  audio_track_sink_->PlayToEndOfStream(env);
}

void KodiNativeSinkSession::SetVolume(JNIEnv* env, float volume) {
  audio_track_sink_->SetVolume(env, volume);
}

bool KodiNativeSinkSession::DrainOnePacketToAudioTrack(
    JNIEnv* env, bool counts_toward_media_position, PacketMetadata* packet) {
  PacketMetadata local_packet;
  if (!engine_->DequeuePacket(&local_packet)) {
    return false;
  }
  std::vector<uint8_t> data;
  if (!engine_->TakeLastDequeuedPacketData(&data) || data.empty()) {
    return false;
  }
  audio_track_sink_->WritePacket(env, local_packet, data.data(), counts_toward_media_position);
  if (packet != nullptr) {
    *packet = local_packet;
  }
  return true;
}

int64_t KodiNativeSinkSession::GetCurrentPositionUs(JNIEnv* env) {
  return audio_track_sink_->GetCurrentPositionUs(env);
}

bool KodiNativeSinkSession::HasPendingData(JNIEnv* env) {
  return audio_track_sink_->HasPendingData(env);
}

bool KodiNativeSinkSession::IsEnded(JNIEnv* env) {
  return audio_track_sink_->IsEnded(env);
}

int64_t KodiNativeSinkSession::GetBufferSizeUs() const { return audio_track_sink_->GetBufferSizeUs(); }

int KodiNativeSinkSession::pending_packet_count() const {
  return engine_->pending_packet_count();
}

int64_t KodiNativeSinkSession::queued_input_bytes() const {
  return engine_->queued_input_bytes();
}

}  // namespace androidx_media3
