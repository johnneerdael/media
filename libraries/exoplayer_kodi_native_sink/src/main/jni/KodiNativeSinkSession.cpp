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

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <random>
#include <thread>
#include <utility>

#include "KodiNativeAudioTrackSink.h"

namespace androidx_media3 {
namespace {

using namespace std::chrono_literals;

constexpr int kModePcm = 1;
constexpr int kModePassthroughDirect = 2;
constexpr int kModePassthroughIecStereo = 3;
constexpr int kModePassthroughIecMultichannel = 4;

constexpr std::chrono::milliseconds kDefaultTimeout = 1000ms;
constexpr std::chrono::milliseconds kConfiguredIdleTimeout = 10s;
constexpr std::chrono::milliseconds kSyncTimeout = 5s;

constexpr int kSinkParentStates[] = {
    -1,
    0,
    0,
    2,
    2,
    2,
    2,
};

void EndianSwap16Buf(uint16_t* dst, uint16_t* src, unsigned int size) {
  for (unsigned int i = 0; i < size; ++i, ++dst, ++src) {
    *dst = ((*src & 0xFF00) >> 8) | ((*src & 0x00FF) << 8);
  }
}

bool UsesIecCarrier(const PlaybackDecision& playback_decision) {
  return playback_decision.mode == kModePassthroughIecStereo ||
         playback_decision.mode == kModePassthroughIecMultichannel;
}

bool S16NeedsByteSwap(AEDataFormat in, AEDataFormat out) {
#ifdef __BIG_ENDIAN__
  constexpr AEDataFormat native_format = AE_FMT_S16BE;
#else
  constexpr AEDataFormat native_format = AE_FMT_S16LE;
#endif

  if (in == AE_FMT_S16NE || in == AE_FMT_RAW) {
    in = native_format;
  }
  if (out == AE_FMT_S16NE || out == AE_FMT_RAW) {
    out = native_format;
  }
  return in != out;
}

int BytesPerSampleForPcmEncoding(int pcm_encoding) {
  switch (pcm_encoding) {
    case 2:
      return 2;
    case 4:
      return 4;
    case 536870912:
      return 3;
    case 805306368:
      return 4;
    default:
      return 2;
  }
}

template <typename Payload>
Payload* GetPayload(actor::Message* msg) {
  if (msg == nullptr || msg->payload_obj == nullptr) {
    return nullptr;
  }
  auto* wrapper = static_cast<actor::CPayloadWrap<Payload>*>(msg->payload_obj.get());
  return wrapper != nullptr ? wrapper->GetPayload() : nullptr;
}

}  // namespace

KodiNativeSinkSession::KodiNativeSinkSession()
    : audio_track_sink_(std::make_unique<KodiNativeAudioTrackSink>()),
      control_port_(&out_msg_event_, &out_msg_event_),
      data_port_(&out_msg_event_, &out_msg_event_),
      worker_thread_(&KodiNativeSinkSession::ThreadMain, this) {}

KodiNativeSinkSession::~KodiNativeSinkSession() {
  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::RESET, nullptr, &reply, kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
  StopWorker();
}

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
                                      bool supervise_audio_delay_enabled,
                                      const CapabilitySnapshot& capability_snapshot,
                                      const PlaybackDecision& playback_decision) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  ConfigurePayload payload;
  payload.mime_kind = mime_kind;
  payload.sample_rate = sample_rate;
  payload.channel_count = channel_count;
  payload.pcm_encoding = pcm_encoding;
  payload.specified_buffer_size = specified_buffer_size;
  payload.output_channel_count = output_channel_count;
  payload.audio_session_id = audio_session_id;
  payload.volume = volume;
  payload.verbose_logging_enabled = verbose_logging_enabled;
  payload.supervise_audio_delay_enabled = supervise_audio_delay_enabled;
  payload.capability_snapshot = capability_snapshot;
  payload.playback_decision = playback_decision;

  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::CONFIGURE,
                  new actor::CPayloadWrap<ConfigurePayload>(payload),
                  &reply,
                  kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::QueueInput(const uint8_t* data,
                                       int size,
                                       int64_t presentation_time_us,
                                       int encoded_access_unit_count) {
  BufferPayload payload;
  if (data != nullptr && size > 0) {
    payload.data.assign(data, data + size);
  }
  payload.presentation_time_us = presentation_time_us;
  payload.encoded_access_unit_count = encoded_access_unit_count;

  actor::Message* reply = nullptr;
  SendDataSync(SessionDataProtocol::QUEUE_INPUT,
               new actor::CPayloadWrap<BufferPayload>(payload),
               &reply,
               kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::QueuePause(JNIEnv* env, unsigned int millis, bool iec_bursts) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  PausePayload payload;
  payload.millis = millis;
  payload.iec_bursts = iec_bursts;

  actor::Message* reply = nullptr;
  SendDataSync(SessionDataProtocol::PAUSE_BURST,
               new actor::CPayloadWrap<PausePayload>(payload),
               &reply,
               kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

bool KodiNativeSinkSession::HandleBuffer(JNIEnv* env,
                                         const uint8_t* data,
                                         int size,
                                         int64_t presentation_time_us,
                                         int encoded_access_unit_count) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  BufferPayload payload;
  if (data != nullptr && size > 0) {
    payload.data.assign(data, data + size);
  }
  payload.presentation_time_us = presentation_time_us;
  payload.encoded_access_unit_count = encoded_access_unit_count;

  actor::Message* reply = nullptr;
  const bool sent =
      SendDataSync(SessionDataProtocol::SAMPLE, new actor::CPayloadWrap<BufferPayload>(payload),
                   &reply, kSyncTimeout);
  bool handled = false;
  if (sent && reply != nullptr && reply->data != nullptr &&
      reply->payload_size == sizeof(handled)) {
    std::memcpy(&handled, reply->data, sizeof(handled));
  }
  if (reply != nullptr) {
    reply->Release();
  }
  return handled;
}

bool KodiNativeSinkSession::QueuePauseToSink(JNIEnv* env,
                                             unsigned int millis,
                                             bool iec_bursts) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  PausePayload payload;
  payload.millis = millis;
  payload.iec_bursts = iec_bursts;

  actor::Message* reply = nullptr;
  const bool sent =
      SendDataSync(SessionDataProtocol::PAUSE_BURST, new actor::CPayloadWrap<PausePayload>(payload),
                   &reply, kSyncTimeout);
  bool handled = false;
  if (sent && reply != nullptr && reply->data != nullptr &&
      reply->payload_size == sizeof(handled)) {
    std::memcpy(&handled, reply->data, sizeof(handled));
  }
  if (reply != nullptr) {
    reply->Release();
  }
  return handled;
}

void KodiNativeSinkSession::Play(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::PLAY, nullptr, &reply, kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::Pause(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::PAUSE, nullptr, &reply, kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::Flush(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::FLUSH, nullptr, &reply, kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::Stop(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::STOP, nullptr, &reply, kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::Reset(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::RESET, nullptr, &reply, kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::Drain(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  SendDataSync(SessionDataProtocol::DRAIN, nullptr, &reply, kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::SetVolume(JNIEnv* env, float volume) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }
  configured_volume_ = volume;

  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::VOLUME, nullptr, &reply, kSyncTimeout);
  if (reply == nullptr) {
    configured_volume_ = volume;
    return;
  }
  reply->Release();
}

void KodiNativeSinkSession::SetAppFocused(JNIEnv* env, bool app_focused) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  BoolPayload payload;
  payload.value = app_focused;
  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::APPFOCUSED,
                  new actor::CPayloadWrap<BoolPayload>(payload),
                  &reply,
                  kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::SetSilenceTimeoutMinutes(int silence_timeout_minutes) {
  IntPayload payload;
  payload.value = silence_timeout_minutes;
  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::SETSILENCETIMEOUT,
                  new actor::CPayloadWrap<IntPayload>(payload),
                  &reply,
                  kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

void KodiNativeSinkSession::SetStreamNoise(bool stream_noise) {
  BoolPayload payload;
  payload.value = stream_noise;
  actor::Message* reply = nullptr;
  SendControlSync(SessionControlProtocol::SETNOISETYPE,
                  new actor::CPayloadWrap<BoolPayload>(payload),
                  &reply,
                  kSyncTimeout);
  if (reply != nullptr) {
    reply->Release();
  }
}

int64_t KodiNativeSinkSession::GetCurrentPositionUs(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  int64_t position_us = 0;
  if (SendControlSync(SessionControlProtocol::GET_CURRENT_POSITION, nullptr, &reply, kSyncTimeout) &&
      reply != nullptr && reply->data != nullptr && reply->payload_size == sizeof(position_us)) {
    std::memcpy(&position_us, reply->data, sizeof(position_us));
  }
  if (reply != nullptr) {
    reply->Release();
  }
  return position_us;
}

bool KodiNativeSinkSession::HasPendingData(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  bool has_pending_data = false;
  if (SendControlSync(SessionControlProtocol::HAS_PENDING_DATA, nullptr, &reply, kSyncTimeout) &&
      reply != nullptr && reply->data != nullptr &&
      reply->payload_size == sizeof(has_pending_data)) {
    std::memcpy(&has_pending_data, reply->data, sizeof(has_pending_data));
  }
  if (reply != nullptr) {
    reply->Release();
  }
  return has_pending_data;
}

bool KodiNativeSinkSession::IsEnded(JNIEnv* env) {
  if (env != nullptr) {
    env->GetJavaVM(&java_vm_);
  }

  actor::Message* reply = nullptr;
  bool is_ended = false;
  if (SendControlSync(SessionControlProtocol::IS_ENDED, nullptr, &reply, kSyncTimeout) &&
      reply != nullptr && reply->data != nullptr && reply->payload_size == sizeof(is_ended)) {
    std::memcpy(&is_ended, reply->data, sizeof(is_ended));
  }
  if (reply != nullptr) {
    reply->Release();
  }
  return is_ended;
}

int64_t KodiNativeSinkSession::GetBufferSizeUs() {
  actor::Message* reply = nullptr;
  int64_t buffer_size_us = 0;
  if (SendControlSync(SessionControlProtocol::GET_BUFFER_SIZE, nullptr, &reply, kSyncTimeout) &&
      reply != nullptr && reply->data != nullptr &&
      reply->payload_size == sizeof(buffer_size_us)) {
    std::memcpy(&buffer_size_us, reply->data, sizeof(buffer_size_us));
  }
  if (reply != nullptr) {
    reply->Release();
  }
  return buffer_size_us;
}

void KodiNativeSinkSession::StateMachine(int signal, actor::Protocol* port, actor::Message* msg) {
  for (int state = state_; state >= 0; state = kSinkParentStates[state]) {
    switch (state) {
      case kStateTop:
        if (port == &control_port_) {
          switch (signal) {
            case SessionControlProtocol::CONFIGURE: {
              ConfigurePayload* payload = GetPayload<ConfigurePayload>(msg);
              if (payload == nullptr) {
                ext_error_ = true;
                state_ = kStateUnconfigured;
                msg->Reply(SessionControlProtocol::ERR);
                return;
              }
              ext_error_ = false;
              ext_silence_timer_.Set(0ms);
              streaming_ = false;
              ReturnBuffers();
              ConfigureOnWorker(*payload);
              if (!ext_error_) {
                state_ = kStateConfiguredIdle;
                ext_timeout_ = kConfiguredIdleTimeout;
                msg->Reply(SessionControlProtocol::ACC);
              } else {
                state_ = kStateUnconfigured;
                ext_timeout_ = kDefaultTimeout;
                msg->Reply(SessionControlProtocol::ERR);
              }
              return;
            }
            case SessionControlProtocol::RESET:
              ReturnBuffers();
              ResetOnWorker();
              state_ = kStateUnconfigured;
              ext_timeout_ = kDefaultTimeout;
              msg->Reply(SessionControlProtocol::ACC);
              return;
            case SessionControlProtocol::FLUSH:
              ReturnBuffers();
              FlushOnWorker();
              state_ = IsConfiguredLocked() ? kStateConfiguredIdle : kStateUnconfigured;
              ext_timeout_ = IsConfiguredLocked() ? kConfiguredIdleTimeout : kDefaultTimeout;
              msg->Reply(SessionControlProtocol::ACC);
              return;
            case SessionControlProtocol::STOP:
              ReturnBuffers();
              StopOnWorker();
              state_ = IsConfiguredLocked() ? kStateConfiguredIdle : kStateUnconfigured;
              ext_timeout_ = IsConfiguredLocked() ? kConfiguredIdleTimeout : kDefaultTimeout;
              msg->Reply(SessionControlProtocol::ACC);
              return;
            case SessionControlProtocol::VOLUME: {
              SetVolumeOnWorker(configured_volume_);
              msg->Reply(SessionControlProtocol::ACC);
              return;
            }
            case SessionControlProtocol::APPFOCUSED: {
              BoolPayload* payload = GetPayload<BoolPayload>(msg);
              ext_app_focused_ = payload != nullptr ? payload->value : true;
              SetSilenceTimer();
              ext_timeout_ = 0ms;
              msg->Reply(SessionControlProtocol::ACC);
              return;
            }
            case SessionControlProtocol::SETSILENCETIMEOUT: {
              IntPayload* payload = GetPayload<IntPayload>(msg);
              silence_timeout_ =
                  std::chrono::minutes(std::max(0, payload != nullptr ? payload->value : 0));
              SetSilenceTimer();
              msg->Reply(SessionControlProtocol::ACC);
              return;
            }
            case SessionControlProtocol::SETNOISETYPE: {
              BoolPayload* payload = GetPayload<BoolPayload>(msg);
              stream_noise_enabled_ = payload != nullptr ? payload->value : true;
              PrepareSilenceSample();
              msg->Reply(SessionControlProtocol::ACC);
              return;
            }
            case SessionControlProtocol::GET_CURRENT_POSITION: {
              const int64_t value = GetCurrentPositionUsOnWorker();
              msg->Reply(SessionControlProtocol::ACC, const_cast<int64_t*>(&value), sizeof(value));
              return;
            }
            case SessionControlProtocol::HAS_PENDING_DATA: {
              const bool value = HasPendingDataOnWorker();
              msg->Reply(SessionControlProtocol::ACC, const_cast<bool*>(&value), sizeof(value));
              return;
            }
            case SessionControlProtocol::IS_ENDED: {
              const bool value = IsEndedOnWorker();
              msg->Reply(SessionControlProtocol::ACC, const_cast<bool*>(&value), sizeof(value));
              return;
            }
            case SessionControlProtocol::GET_BUFFER_SIZE: {
              const int64_t value = GetBufferSizeUsOnWorker();
              msg->Reply(SessionControlProtocol::ACC, const_cast<int64_t*>(&value), sizeof(value));
              return;
            }
            default:
              break;
          }
        } else if (port == &data_port_) {
          switch (signal) {
            case SessionDataProtocol::DRAIN:
              DrainOnWorker();
              state_ = IsConfiguredLocked() ? kStateConfiguredIdle : kStateUnconfigured;
              ext_timeout_ = IsConfiguredLocked() ? kConfiguredIdleTimeout : kDefaultTimeout;
              msg->Reply(SessionDataProtocol::ACC);
              return;
            default:
              break;
          }
        }
        break;

      case kStateUnconfigured:
        if (port == nullptr) {
          if (signal == SessionControlProtocol::TIMEOUT) {
            ext_timeout_ = kDefaultTimeout;
            return;
          }
        } else if (port == &control_port_) {
          switch (signal) {
            case SessionControlProtocol::PLAY:
            case SessionControlProtocol::PAUSE:
              msg->Reply(SessionControlProtocol::ACC);
              return;
            default:
              break;
          }
        } else if (port == &data_port_) {
          switch (signal) {
            case SessionDataProtocol::QUEUE_INPUT:
            case SessionDataProtocol::PAUSE_BURST: {
              const bool handled = false;
              msg->Reply(SessionDataProtocol::ACC, const_cast<bool*>(&handled), sizeof(handled));
              ext_timeout_ = 0ms;
              return;
            }
            case SessionDataProtocol::SAMPLE: {
              BufferPayload* payload = GetPayload<BufferPayload>(msg);
              if (payload != nullptr && configured_sample_rate_ > 0) {
                const int frame_bytes =
                    std::max(1, configured_channel_count_) *
                    BytesPerSampleForPcmEncoding(configured_pcm_encoding_);
                const int frames =
                    frame_bytes > 0 ? static_cast<int>(payload->data.size()) / frame_bytes : 0;
                if (frames > 0) {
                  std::this_thread::sleep_for(
                      std::chrono::milliseconds(1000LL * frames / configured_sample_rate_));
                }
              }
              const bool handled = false;
              msg->Reply(SessionDataProtocol::ACC, const_cast<bool*>(&handled), sizeof(handled));
              ext_timeout_ = 0ms;
              return;
            }
            default:
              break;
          }
        }
        break;

      case kStateConfigured:
        if (port == &control_port_) {
          switch (signal) {
            case SessionControlProtocol::PLAY:
              PlayOnWorker();
              SetSilenceTimer();
              if (!ext_silence_timer_.IsTimePast()) {
                state_ = kStateConfiguredSilence;
              }
              ext_timeout_ = 0ms;
              msg->Reply(SessionControlProtocol::ACC);
              return;
            case SessionControlProtocol::PAUSE:
              PauseOnWorker();
              SetSilenceTimer();
              if (!ext_silence_timer_.IsTimePast()) {
                state_ = kStateConfiguredSilence;
              }
              ext_timeout_ = 0ms;
              msg->Reply(SessionControlProtocol::ACC);
              return;
            default:
              break;
          }
        } else if (port == &data_port_) {
          switch (signal) {
            case SessionDataProtocol::QUEUE_INPUT: {
              BufferPayload* payload = GetPayload<BufferPayload>(msg);
              if (payload != nullptr) {
                QueueInputOnWorker(*payload);
              }
              msg->Reply(SessionDataProtocol::ACC);
              return;
            }
            case SessionDataProtocol::PAUSE_BURST: {
              PausePayload* payload = GetPayload<PausePayload>(msg);
              const bool handled = payload != nullptr && QueuePauseToSinkOnWorker(*payload);
              msg->Reply(SessionDataProtocol::ACC, const_cast<bool*>(&handled), sizeof(handled));
              if (!handled && !ext_error_) {
                ext_timeout_ = 0ms;
              } else if (ext_error_) {
                state_ = kStateConfiguredSuspend;
                ext_timeout_ = 0ms;
              } else {
                state_ = kStateConfiguredPlay;
                ext_timeout_ =
                    std::chrono::milliseconds(std::max<int64_t>(1, GetBufferSizeUsOnWorker() / 2000));
                ext_silence_timer_.Set(ext_silence_timeout_);
              }
              return;
            }
            case SessionDataProtocol::SAMPLE: {
              BufferPayload* payload = GetPayload<BufferPayload>(msg);
              const bool handled = payload != nullptr && HandleBufferOnWorker(*payload);
              msg->Reply(SessionDataProtocol::ACC, const_cast<bool*>(&handled), sizeof(handled));
              if (!handled && !ext_error_) {
                ext_timeout_ = 0ms;
              } else if (ext_error_) {
                state_ = kStateConfiguredSuspend;
                ext_timeout_ = 0ms;
              } else {
                state_ = kStateConfiguredPlay;
                ext_timeout_ =
                    std::chrono::milliseconds(std::max<int64_t>(1, GetBufferSizeUsOnWorker() / 2000));
                ext_silence_timer_.Set(ext_silence_timeout_);
              }
              return;
            }
            default:
              break;
          }
        }
        break;

      case kStateConfiguredSuspend:
        if (port == &control_port_) {
          switch (signal) {
            case SessionControlProtocol::PLAY:
              PlayOnWorker();
              SetSilenceTimer();
              ext_timeout_ = 0ms;
              msg->Reply(SessionControlProtocol::ACC);
              return;
            case SessionControlProtocol::PAUSE:
              PauseOnWorker();
              SetSilenceTimer();
              ext_timeout_ = 0ms;
              msg->Reply(SessionControlProtocol::ACC);
              return;
            default:
              break;
          }
        } else if (port == &data_port_) {
          switch (signal) {
            case SessionDataProtocol::QUEUE_INPUT: {
              BufferPayload* payload = GetPayload<BufferPayload>(msg);
              if (payload != nullptr) {
                QueueInputOnWorker(*payload);
              }
              msg->Reply(SessionDataProtocol::ACC);
              return;
            }
            case SessionDataProtocol::PAUSE_BURST:
            case SessionDataProtocol::SAMPLE:
              ext_error_ = false;
              OpenSink();
              if (!ext_error_) {
                bool attached = false;
                JNIEnv* env = GetWorkerEnv(&attached);
                if (env != nullptr) {
                  (void)OutputSamples(env, &sample_of_silence_);
                } else {
                  ext_error_ = true;
                }
                DetachWorkerEnvIfNeeded(attached);
              }
              if (!ext_error_) {
                state_ = kStateConfiguredPlay;
                ext_timeout_ = 0ms;
                state_machine_self_trigger_ = true;
              } else {
                state_ = kStateUnconfigured;
                ext_timeout_ = kDefaultTimeout;
                const bool handled = false;
                msg->Reply(SessionDataProtocol::ERR, const_cast<bool*>(&handled), sizeof(handled));
              }
              return;
            case SessionDataProtocol::DRAIN:
              msg->Reply(SessionDataProtocol::ACC);
              return;
            default:
              break;
          }
        } else if (port == nullptr) {
          if (signal == SessionControlProtocol::TIMEOUT) {
            ext_timeout_ = kConfiguredIdleTimeout;
            return;
          }
        }
        break;

      case kStateConfiguredIdle:
        if (port == &data_port_) {
          switch (signal) {
            case SessionDataProtocol::QUEUE_INPUT: {
              BufferPayload* payload = GetPayload<BufferPayload>(msg);
              if (payload != nullptr) {
                QueueInputOnWorker(*payload);
              }
              msg->Reply(SessionDataProtocol::ACC);
              return;
            }
            case SessionDataProtocol::PAUSE_BURST:
            case SessionDataProtocol::SAMPLE: {
              bool attached = false;
              JNIEnv* env = GetWorkerEnv(&attached);
              if (env != nullptr) {
                (void)OutputSamples(env, &sample_of_silence_);
              } else {
                ext_error_ = true;
              }
              DetachWorkerEnvIfNeeded(attached);
              state_ = ext_error_ ? kStateConfiguredSuspend : kStateConfiguredPlay;
              ext_timeout_ = 0ms;
              if (!ext_error_) {
                state_machine_self_trigger_ = true;
              } else {
                const bool handled = false;
                msg->Reply(SessionDataProtocol::ERR, const_cast<bool*>(&handled), sizeof(handled));
              }
              return;
            }
            default:
              break;
          }
        } else if (port == nullptr) {
          if (signal == SessionControlProtocol::TIMEOUT) {
            bool attached = false;
            JNIEnv* env = GetWorkerEnv(&attached);
            if (env != nullptr) {
              audio_track_sink_->Release(env);
            }
            DetachWorkerEnvIfNeeded(attached);
            state_ = kStateConfiguredSuspend;
            ext_timeout_ = kConfiguredIdleTimeout;
            return;
          }
        }
        break;

      case kStateConfiguredPlay:
        if (port == nullptr) {
          if (signal == SessionControlProtocol::TIMEOUT) {
            if (!ext_silence_timer_.IsTimePast()) {
              state_ = kStateConfiguredSilence;
              ext_timeout_ = 0ms;
            } else {
              bool attached = false;
              JNIEnv* env = GetWorkerEnv(&attached);
              if (env != nullptr) {
                audio_track_sink_->Drain(env);
              }
              DetachWorkerEnvIfNeeded(attached);
              state_ = kStateConfiguredIdle;
              ext_timeout_ = ext_app_focused_ ? kConfiguredIdleTimeout : 0ms;
            }
            return;
          }
        }
        break;

      case kStateConfiguredSilence:
        if (port == nullptr) {
          if (signal == SessionControlProtocol::TIMEOUT) {
            bool attached = false;
            JNIEnv* env = GetWorkerEnv(&attached);
            if (env != nullptr) {
              (void)OutputSamples(env, &sample_of_silence_);
            } else {
              ext_error_ = true;
            }
            DetachWorkerEnvIfNeeded(attached);
            if (ext_error_) {
              bool release_attached = false;
              JNIEnv* release_env = GetWorkerEnv(&release_attached);
              if (release_env != nullptr) {
                audio_track_sink_->Release(release_env);
              }
              DetachWorkerEnvIfNeeded(release_attached);
              state_ = kStateConfiguredSuspend;
            } else {
              state_ = kStateConfiguredPlay;
            }
            ext_timeout_ = 0ms;
            return;
          }
        }
        break;

      default:
        break;
    }
  }

  if (msg != nullptr && msg->is_sync) {
    if (port == &control_port_) {
      msg->Reply(SessionControlProtocol::ERR);
    } else if (port == &data_port_) {
      msg->Reply(SessionDataProtocol::ERR);
    }
  }
}

void KodiNativeSinkSession::ReturnBuffers() {
  actor::Message* msg = nullptr;
  while (data_port_.ReceiveOutMessage(&msg)) {
    if (msg == nullptr) {
      continue;
    }
    switch (msg->signal) {
      case SessionDataProtocol::SAMPLE:
      case SessionDataProtocol::PAUSE_BURST: {
        const bool handled = false;
        msg->Reply(SessionDataProtocol::ACC, const_cast<bool*>(&handled), sizeof(handled));
        break;
      }
      default:
        msg->Reply(SessionDataProtocol::ACC);
        break;
    }
    msg->Release();
  }
}

void KodiNativeSinkSession::SetSilenceTimer() {
  if (streaming_) {
    ext_silence_timeout_ = EndTime<std::chrono::milliseconds>::Max();
  } else if (ext_app_focused_) {
    const bool no_silence_on_pause =
        !need_iec_pack_ && configured_playback_decision_.mode != kModePcm &&
        (current_stream_info_.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA ||
         current_stream_info_.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD);
    ext_silence_timeout_ = no_silence_on_pause
                               ? std::chrono::milliseconds::zero()
                               : std::chrono::duration_cast<std::chrono::milliseconds>(
                                     silence_timeout_);
  } else {
    ext_silence_timeout_ = std::chrono::milliseconds::zero();
  }
  ext_silence_timer_.Set(ext_silence_timeout_);
}

CAEStreamInfo KodiNativeSinkSession::MakeConfiguredStreamInfo() const {
  CAEStreamInfo info;
  info.m_type = static_cast<CAEStreamInfo::DataType>(configured_playback_decision_.stream_type);
  info.m_sampleRate = static_cast<unsigned int>(std::max(0, configured_sample_rate_));
  info.m_channels = static_cast<unsigned int>(std::max(0, configured_channel_count_));
  info.m_dataIsLE = true;
  return info;
}

void KodiNativeSinkSession::PrepareSilenceSample() {
  sample_of_silence_.pool = nullptr;
  sample_of_silence_.timestamp = 0;
  sample_of_silence_.pkt_start_offset = 0;
  sample_of_silence_.stream_info = current_stream_info_;

  if (configured_playback_decision_.mode != kModePcm) {
    sample_of_silence_.pkt->SetPauseBurst(
        static_cast<int>(std::max(0.0, current_stream_info_.GetDuration())), 0);
    return;
  }

  const int channel_count =
      std::max(1, configured_output_channel_count_ > 0 ? configured_output_channel_count_
                                                       : configured_channel_count_);
  const int frame_count =
      std::max(1U, audio_track_sink_->GetFramesPerWrite()) > INT_MAX
          ? INT_MAX
          : static_cast<int>(std::max(1U, audio_track_sink_->GetFramesPerWrite()));
  const int bytes_per_sample = BytesPerSampleForPcmEncoding(configured_pcm_encoding_);
  const int bytes_per_frame = std::max(1, channel_count * bytes_per_sample);
  const int size_bytes = frame_count * bytes_per_frame;
  pcm_silence_bytes_.assign(size_bytes, 0);
  if (stream_noise_enabled_) {
    std::minstd_rand random_engine(0x4B4F4449u);
    switch (configured_pcm_encoding_) {
      case 4: {
        const int sample_count = size_bytes / static_cast<int>(sizeof(float));
        for (int i = 0; i < sample_count; ++i) {
          const float unit = static_cast<float>(random_engine() & 0xFFFF) / 32768.0f - 1.0f;
          const float value = unit * 0.00001f;
          std::memcpy(pcm_silence_bytes_.data() + (i * static_cast<int>(sizeof(value))), &value,
                      sizeof(value));
        }
        break;
      }
      case 536870912: {
        for (int i = 0; i + 2 < size_bytes; i += 3) {
          const int value = static_cast<int>(random_engine() % 17) - 8;
          pcm_silence_bytes_[i] = static_cast<uint8_t>(value & 0xFF);
          pcm_silence_bytes_[i + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
          pcm_silence_bytes_[i + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        }
        break;
      }
      case 805306368: {
        const int sample_count = size_bytes / static_cast<int>(sizeof(int32_t));
        for (int i = 0; i < sample_count; ++i) {
          const int32_t value = static_cast<int32_t>(random_engine() % 257) - 128;
          std::memcpy(pcm_silence_bytes_.data() + (i * static_cast<int>(sizeof(value))), &value,
                      sizeof(value));
        }
        break;
      }
      case 2:
      default: {
        const int sample_count = size_bytes / static_cast<int>(sizeof(int16_t));
        for (int i = 0; i < sample_count; ++i) {
          const int16_t value = static_cast<int16_t>(static_cast<int>(random_engine() % 17) - 8);
          std::memcpy(pcm_silence_bytes_.data() + (i * static_cast<int>(sizeof(value))), &value,
                      sizeof(value));
        }
        break;
      }
    }
  }
  sample_of_silence_.pkt->SetData(pcm_silence_bytes_.data(), size_bytes, frame_count, frame_count);
}

void KodiNativeSinkSession::ConfigureCodec(int mime_kind) {
  (void)mime_kind;
  passthrough_codec_.reset();
  const auto codec_stream_type =
      static_cast<CAEStreamInfo::DataType>(configured_playback_decision_.stream_type);
  if (codec_stream_type == CAEStreamInfo::STREAM_TYPE_NULL ||
      configured_playback_decision_.mode == kModePcm) {
    return;
  }
  passthrough_codec_ = std::make_unique<CDVDAudioCodecPassthrough>(
      configured_playback_decision_.mode == kModePassthroughDirect, codec_stream_type);
  if (!passthrough_codec_->Open()) {
    passthrough_codec_.reset();
  }
}

bool KodiNativeSinkSession::UsesKodiPassthroughCodec() const {
  return passthrough_codec_ != nullptr;
}

bool KodiNativeSinkSession::IsConfiguredLocked() const {
  return state_ != kStateUnconfigured;
}

bool KodiNativeSinkSession::CanAcceptInputLocked() const {
  return IsConfiguredLocked();
}

bool KodiNativeSinkSession::HasCodecBufferedDataLocked() const {
  return passthrough_codec_ != nullptr && passthrough_codec_->GetBufferSize() > 0;
}

void KodiNativeSinkSession::ResetStreamingStateLocked(bool reset_codec) {
  if (reset_codec && passthrough_codec_ != nullptr) {
    passthrough_codec_->Reset();
  }
  bitstream_packer_.Reset();
  current_stream_info_ = MakeConfiguredStreamInfo();
  streaming_ = false;
  pending_pause_iec_bursts_ = true;
  swap_state_ = kCheckSwap;
  PrepareSilenceSample();
}

void KodiNativeSinkSession::ThreadMain() {
  actor::Message* msg = nullptr;
  actor::Protocol* port = nullptr;
  bool got_msg = false;
  EndTime<> timer;

  state_ = kStateUnconfigured;
  ext_timeout_ = kDefaultTimeout;
  state_machine_self_trigger_ = false;
  ext_app_focused_ = true;
  ext_error_ = false;
  streaming_ = false;

  while (!worker_stop_) {
    got_msg = false;
    timer.Set(ext_timeout_);

    if (state_machine_self_trigger_ && msg != nullptr) {
      state_machine_self_trigger_ = false;
      StateMachine(msg->signal, port, msg);
      if (!state_machine_self_trigger_) {
        msg->Release();
        msg = nullptr;
        port = nullptr;
      }
      continue;
    }
    if (control_port_.ReceiveOutMessage(&msg)) {
      got_msg = true;
      port = &control_port_;
    } else if (data_port_.ReceiveOutMessage(&msg)) {
      got_msg = true;
      port = &data_port_;
    }

    if (got_msg) {
      StateMachine(msg->signal, port, msg);
      if (!state_machine_self_trigger_) {
        msg->Release();
        msg = nullptr;
        port = nullptr;
      }
      continue;
    }

    if (out_msg_event_.Wait(ext_timeout_)) {
      ext_timeout_ = timer.GetTimeLeft();
      continue;
    }

    msg = control_port_.GetMessage();
    msg->signal = SessionControlProtocol::TIMEOUT;
    port = nullptr;
    StateMachine(msg->signal, port, msg);
    if (!state_machine_self_trigger_) {
      msg->Release();
      msg = nullptr;
    }
  }

  ResetOnWorker();
}

void KodiNativeSinkSession::StopWorker() {
  if (worker_stop_) {
    return;
  }
  worker_stop_ = true;
  out_msg_event_.Set();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  control_port_.Purge();
  data_port_.Purge();
}

bool KodiNativeSinkSession::SendControlSync(int signal,
                                            actor::CPayloadWrapBase* payload,
                                            actor::Message** reply,
                                            std::chrono::milliseconds timeout) {
  if (reply == nullptr) {
    delete payload;
    return false;
  }
  if (payload != nullptr) {
    return control_port_.SendOutMessageSync(signal, reply, timeout, payload);
  }
  return control_port_.SendOutMessageSync(signal, reply, timeout, nullptr, 0);
}

bool KodiNativeSinkSession::SendDataSync(int signal,
                                         actor::CPayloadWrapBase* payload,
                                         actor::Message** reply,
                                         std::chrono::milliseconds timeout) {
  if (reply == nullptr) {
    delete payload;
    return false;
  }
  if (payload != nullptr) {
    return data_port_.SendOutMessageSync(signal, reply, timeout, payload);
  }
  return data_port_.SendOutMessageSync(signal, reply, timeout, nullptr, 0);
}

JNIEnv* KodiNativeSinkSession::GetWorkerEnv(bool* attached) const {
  if (attached != nullptr) {
    *attached = false;
  }
  if (java_vm_ == nullptr) {
    return nullptr;
  }

  JNIEnv* env = nullptr;
  const jint get_env_result =
      java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (get_env_result == JNI_OK) {
    return env;
  }
  if (get_env_result != JNI_EDETACHED) {
    return nullptr;
  }
  if (java_vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
    return nullptr;
  }
  if (attached != nullptr) {
    *attached = true;
  }
  return env;
}

void KodiNativeSinkSession::DetachWorkerEnvIfNeeded(bool attached) const {
  if (attached && java_vm_ != nullptr) {
    java_vm_->DetachCurrentThread();
  }
}

bool KodiNativeSinkSession::OpenSink() {
  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  if (env == nullptr || configured_playback_decision_.mode == 0) {
    ext_error_ = true;
    DetachWorkerEnvIfNeeded(attached);
    return false;
  }

  need_iec_pack_ = UsesIecCarrier(configured_playback_decision_);
  sink_data_format_ =
      configured_playback_decision_.mode == kModePcm && configured_pcm_encoding_ == 4 ? AE_FMT_FLOAT
                                                                                       : AE_FMT_S16NE;
  if (current_stream_info_.m_type == CAEStreamInfo::STREAM_TYPE_NULL) {
    current_stream_info_ = MakeConfiguredStreamInfo();
  }
  pending_pause_iec_bursts_ = true;
  swap_state_ = kCheckSwap;

  audio_track_sink_->set_verbose_logging_enabled(verbose_logging_enabled_);
  audio_track_sink_->set_supervise_audio_delay_enabled(supervise_audio_delay_enabled_);
  audio_track_sink_->Configure(env,
                               configured_sample_rate_,
                               configured_channel_count_,
                               configured_pcm_encoding_,
                               configured_specified_buffer_size_,
                               configured_output_channel_count_,
                               configured_audio_session_id_,
                               configured_volume_,
                               configured_playback_decision_);
  PrepareSilenceSample();
  ext_error_ = false;
  DetachWorkerEnvIfNeeded(attached);
  return true;
}

void KodiNativeSinkSession::ConfigureOnWorker(const ConfigurePayload& payload) {
  configured_sample_rate_ = payload.sample_rate;
  configured_channel_count_ = payload.channel_count;
  configured_pcm_encoding_ = payload.pcm_encoding;
  configured_mime_kind_ = payload.mime_kind;
  configured_specified_buffer_size_ = payload.specified_buffer_size;
  configured_output_channel_count_ = payload.output_channel_count;
  configured_audio_session_id_ = payload.audio_session_id;
  configured_volume_ = payload.volume;
  verbose_logging_enabled_ = payload.verbose_logging_enabled;
  supervise_audio_delay_enabled_ = payload.supervise_audio_delay_enabled;
  configured_playback_decision_ = payload.playback_decision;
  (void)payload.capability_snapshot;

  ConfigureCodec(configured_mime_kind_);
  ResetStreamingStateLocked(false);
  OpenSink();
}

void KodiNativeSinkSession::QueueInputOnWorker(const BufferPayload& payload) {
  if (!CanAcceptInputLocked() || !UsesKodiPassthroughCodec() || payload.data.empty()) {
    return;
  }
  DemuxPacket packet{payload.data.data(), static_cast<int>(payload.data.size()),
                     static_cast<double>(payload.presentation_time_us)};
  passthrough_codec_->AddData(packet);
}

bool KodiNativeSinkSession::HandleBufferOnWorker(const BufferPayload& payload) {
  if (!CanAcceptInputLocked() || payload.data.empty()) {
    return false;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  if (env == nullptr) {
    return false;
  }

  if (configured_playback_decision_.mode == kModePcm) {
    auto sample = std::make_unique<CSampleBuffer>();
    sample->Acquire();
    sample->pool = &buffer_pool_;
    sample->timestamp = payload.presentation_time_us;
    sample->stream_info = current_stream_info_;
    const int output_channel_count =
        std::max(1, configured_output_channel_count_ > 0 ? configured_output_channel_count_
                                                         : configured_channel_count_);
    const int bytes_per_frame =
        output_channel_count * BytesPerSampleForPcmEncoding(configured_pcm_encoding_);
    const int pcm_frames =
        bytes_per_frame > 0 ? static_cast<int>(payload.data.size()) / bytes_per_frame : 0;
    sample->pkt->SetData(payload.data.data(), static_cast<int>(payload.data.size()), pcm_frames,
                         pcm_frames);
    const bool handled = OutputSamples(env, sample.get());
    sample->Return();
    DetachWorkerEnvIfNeeded(attached);
    return handled;
  }

  if (UsesKodiPassthroughCodec()) {
    DemuxPacket packet{payload.data.data(), static_cast<int>(payload.data.size()),
                       static_cast<double>(payload.presentation_time_us)};
    passthrough_codec_->AddData(packet);
  }

  bool handled = true;
  while (true) {
    auto sample = std::make_unique<CSampleBuffer>();
    sample->Acquire();
    sample->pool = &buffer_pool_;
    if (!UsesKodiPassthroughCodec()) {
      sample->Return();
      break;
    }

    DVDAudioFrame frame;
    if (!passthrough_codec_->GetData(frame)) {
      sample->Return();
      break;
    }

    sample->timestamp = frame.hasTimestamp && frame.pts != DVD_NOPTS_VALUE
                            ? static_cast<int64_t>(frame.pts)
                            : payload.presentation_time_us;
    sample->stream_info = frame.format.m_streamInfo;
    sample->pkt->SetData(frame.data, static_cast<int>(frame.nb_frames),
                         static_cast<int>(frame.nb_frames), static_cast<int>(frame.nb_frames));
    handled &= OutputSamples(env, sample.get());
    sample->Return();
    if (!handled) {
      break;
    }
  }

  DetachWorkerEnvIfNeeded(attached);
  return handled;
}

bool KodiNativeSinkSession::QueuePauseToSinkOnWorker(const PausePayload& payload) {
  if (!CanAcceptInputLocked()) {
    return false;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  if (env == nullptr) {
    return false;
  }

  auto sample = std::make_unique<CSampleBuffer>();
  sample->Acquire();
  sample->stream_info = current_stream_info_;
  sample->pkt->SetPauseBurst(static_cast<int>(payload.millis), 0);
  pending_pause_iec_bursts_ = payload.iec_bursts;
  const bool handled = OutputSamples(env, sample.get());
  sample->Return();
  DetachWorkerEnvIfNeeded(attached);
  return handled;
}

void KodiNativeSinkSession::QueuePauseOnWorker(const PausePayload& payload) {
  (void)QueuePauseToSinkOnWorker(payload);
}

void KodiNativeSinkSession::PlayOnWorker() {
  if (!IsConfiguredLocked()) {
    return;
  }
  streaming_ = true;
}

void KodiNativeSinkSession::PauseOnWorker() {
  if (!IsConfiguredLocked()) {
    return;
  }
  streaming_ = false;
}

void KodiNativeSinkSession::FlushOnWorker() {
  if (!IsConfiguredLocked()) {
    return;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  ResetStreamingStateLocked(true);
  if (env != nullptr) {
    audio_track_sink_->Flush(env);
  }
  DetachWorkerEnvIfNeeded(attached);
}

void KodiNativeSinkSession::StopOnWorker() {
  if (!IsConfiguredLocked()) {
    return;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  ResetStreamingStateLocked(true);
  if (env != nullptr) {
    audio_track_sink_->Stop(env);
  }
  DetachWorkerEnvIfNeeded(attached);
}

void KodiNativeSinkSession::ResetOnWorker() {
  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);

  ResetStreamingStateLocked(false);
  passthrough_codec_.reset();
  if (env != nullptr) {
    audio_track_sink_->Release(env);
  }
  java_vm_ = nullptr;
  state_ = kStateUnconfigured;
  DetachWorkerEnvIfNeeded(attached);
}

void KodiNativeSinkSession::DrainOnWorker() {
  if (!IsConfiguredLocked()) {
    return;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  if (env == nullptr) {
    ext_error_ = true;
    return;
  }

  while (DrainOnePacketToAudioTrack(env)) {
  }
  audio_track_sink_->Drain(env);
  streaming_ = false;
  DetachWorkerEnvIfNeeded(attached);
}

void KodiNativeSinkSession::SetVolumeOnWorker(float volume) {
  configured_volume_ = volume;
  if (!IsConfiguredLocked()) {
    return;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  if (env != nullptr) {
    audio_track_sink_->SetVolume(env, volume);
  }
  DetachWorkerEnvIfNeeded(attached);
}

int64_t KodiNativeSinkSession::GetCurrentPositionUsOnWorker() {
  if (!IsConfiguredLocked()) {
    return 0;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  if (env == nullptr) {
    return 0;
  }

  const int64_t position_us = audio_track_sink_->GetCurrentPositionUs(env);
  DetachWorkerEnvIfNeeded(attached);
  return position_us;
}

bool KodiNativeSinkSession::HasPendingDataOnWorker() {
  if (HasCodecBufferedDataLocked()) {
    return true;
  }
  if (!IsConfiguredLocked()) {
    return false;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  if (env == nullptr) {
    return false;
  }

  const bool has_pending_data = audio_track_sink_->HasPendingData(env);
  DetachWorkerEnvIfNeeded(attached);
  return has_pending_data;
}

bool KodiNativeSinkSession::IsEndedOnWorker() {
  if (HasCodecBufferedDataLocked() || !IsConfiguredLocked()) {
    return false;
  }

  bool attached = false;
  JNIEnv* env = GetWorkerEnv(&attached);
  if (env == nullptr) {
    return false;
  }

  const bool is_ended = audio_track_sink_->IsEnded(env);
  DetachWorkerEnvIfNeeded(attached);
  return is_ended;
}

int64_t KodiNativeSinkSession::GetBufferSizeUsOnWorker() const {
  return audio_track_sink_->GetBufferSizeUs();
}

bool KodiNativeSinkSession::DrainOnePacketToAudioTrack(JNIEnv* env) {
  if (!IsConfiguredLocked() || !UsesKodiPassthroughCodec()) {
    return false;
  }

  auto sample = std::make_unique<CSampleBuffer>();
  sample->Acquire();
  sample->pool = &buffer_pool_;

  DVDAudioFrame frame;
  if (!passthrough_codec_->GetData(frame)) {
    sample->Return();
    return false;
  }

  sample->timestamp =
      frame.hasTimestamp && frame.pts != DVD_NOPTS_VALUE ? static_cast<int64_t>(frame.pts) : 0;
  sample->stream_info = frame.format.m_streamInfo;
  sample->pkt->SetData(frame.data, static_cast<int>(frame.nb_frames),
                       static_cast<int>(frame.nb_frames), static_cast<int>(frame.nb_frames));
  const bool handled = OutputSamples(env, sample.get());
  sample->Return();
  return handled;
}

void KodiNativeSinkSession::SwapInit(CSampleBuffer* samples) {
  (void)samples;
  if (configured_playback_decision_.mode != kModePcm &&
      S16NeedsByteSwap(AE_FMT_S16NE, sink_data_format_)) {
    swap_state_ = kNeedByteSwap;
  } else {
    swap_state_ = kSkipSwap;
  }
}

bool KodiNativeSinkSession::OutputSamples(JNIEnv* env, CSampleBuffer* samples) {
  if (!IsConfiguredLocked() || samples == nullptr || samples->pkt == nullptr) {
    ext_error_ = true;
    return false;
  }

  uint8_t** buffer = samples->pkt->data;
  uint8_t* pack_buffer = nullptr;
  unsigned int frames = samples->pkt->nb_samples;
  unsigned int total_frames = frames;
  unsigned int max_frames = 0;
  int retry = 0;
  unsigned int written = 0;
  bool skip_swap = false;

  if (configured_playback_decision_.mode != kModePcm) {
    if (need_iec_pack_) {
      if (frames > 0) {
        current_stream_info_ = samples->stream_info;
        sample_of_silence_.stream_info = current_stream_info_;
        sample_of_silence_.pkt->SetPauseBurst(
            static_cast<int>(std::max(0.0, current_stream_info_.GetDuration())), 0);
        bitstream_packer_.Reset();
        bitstream_packer_.Pack(current_stream_info_, buffer[0], frames);
      } else if (samples->pkt->pause_burst_ms > 0) {
        const bool burst =
            pending_pause_iec_bursts_ && streaming_ && (bitstream_packer_.GetBuffer()[0] != 0);
        if (!bitstream_packer_.PackPause(current_stream_info_,
                                         static_cast<unsigned int>(samples->pkt->pause_burst_ms),
                                         burst)) {
          skip_swap = true;
        }
      } else {
        bitstream_packer_.Reset();
      }

      const unsigned int size = bitstream_packer_.GetSize();
      pack_buffer = bitstream_packer_.GetBuffer();
      buffer = &pack_buffer;
      total_frames = size / std::max(1, audio_track_sink_->GetSinkFrameSizeBytes(*samples));
      frames = total_frames;
      samples->pkt->SetData(pack_buffer, static_cast<int>(size), static_cast<int>(total_frames),
                            static_cast<int>(total_frames));

      switch (swap_state_) {
        case kSkipSwap:
          break;
        case kNeedByteSwap:
          if (!skip_swap) {
            EndianSwap16Buf(reinterpret_cast<uint16_t*>(buffer[0]),
                           reinterpret_cast<uint16_t*>(buffer[0]), size / 2);
          }
          break;
        case kCheckSwap:
          SwapInit(samples);
          if (swap_state_ == kNeedByteSwap) {
            EndianSwap16Buf(reinterpret_cast<uint16_t*>(buffer[0]),
                           reinterpret_cast<uint16_t*>(buffer[0]), size / 2);
          }
          break;
      }
    } else if (samples->pkt->pause_burst_ms > 0) {
      audio_track_sink_->AddPause(env, static_cast<unsigned int>(samples->pkt->pause_burst_ms));
      pending_pause_iec_bursts_ = true;
      return true;
    }
  }

  while (frames > 0) {
    max_frames = std::min(frames, std::max(1U, audio_track_sink_->GetFramesPerWrite()));
    written = audio_track_sink_->AddPackets(env, buffer, max_frames, total_frames - frames, *samples);
    if (written == 0) {
      usleep(static_cast<useconds_t>(
          500000LL * std::max(1U, audio_track_sink_->GetFramesPerWrite()) /
          std::max(1, audio_track_sink_->GetTransportSampleRateHz())));
      retry++;
      if (retry > 4) {
        ext_error_ = true;
        return false;
      }
      continue;
    }
    if (written > max_frames || written == static_cast<unsigned int>(INT_MAX)) {
      ext_error_ = true;
      return false;
    }
    frames -= written;
  }

  ext_error_ = false;
  if (samples->pkt->pause_burst_ms == 0) {
    streaming_ = true;
  }
  pending_pause_iec_bursts_ = true;
  return true;
}

}  // namespace androidx_media3
