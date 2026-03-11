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
#include <thread>
#include <vector>

#include "AEAudioFormat.h"
#include "AEBitstreamPacker.h"
#include "DVDAudioCodecPassthrough.h"
#include "KodiActorProtocol.h"
#include "KodiActiveAEBufferCompat.h"
#include "KodiCapabilitySelector.h"
#include "KodiEndTime.h"
#include "KodiEvent.h"

namespace androidx_media3 {

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
                 bool supervise_audio_delay_enabled,
                 const CapabilitySnapshot& capability_snapshot,
                 const PlaybackDecision& playback_decision);
  void QueueInput(const uint8_t* data,
                  int size,
                  int64_t presentation_time_us,
                  int encoded_access_unit_count);
  void QueuePause(JNIEnv* env, unsigned int millis, bool iec_bursts);
  bool HandleBuffer(JNIEnv* env,
                    const uint8_t* data,
                    int size,
                    int64_t presentation_time_us,
                    int encoded_access_unit_count);
  bool QueuePauseToSink(JNIEnv* env, unsigned int millis, bool iec_bursts);
  void Play(JNIEnv* env);
  void Pause(JNIEnv* env);
  void Flush(JNIEnv* env);
  void Stop(JNIEnv* env);
  void Reset(JNIEnv* env);
  void Drain(JNIEnv* env);
  void SetVolume(JNIEnv* env, float volume);
  void SetAppFocused(JNIEnv* env, bool app_focused);
  void SetSilenceTimeoutMinutes(int silence_timeout_minutes);
  void SetStreamNoise(bool stream_noise);
  bool DrainOnePacketToAudioTrack(JNIEnv* env);
  int64_t GetCurrentPositionUs(JNIEnv* env);
  bool HasPendingData(JNIEnv* env);
  bool IsEnded(JNIEnv* env);
  int64_t GetBufferSizeUs();

 private:
  struct ConfigurePayload {
    int mime_kind = 0;
    int sample_rate = 0;
    int channel_count = 0;
    int pcm_encoding = 0;
    int specified_buffer_size = 0;
    int output_channel_count = 0;
    int audio_session_id = 0;
    float volume = 1.0f;
    bool verbose_logging_enabled = false;
    bool supervise_audio_delay_enabled = false;
    CapabilitySnapshot capability_snapshot = {};
    PlaybackDecision playback_decision = {};
  };

  struct BufferPayload {
    std::vector<uint8_t> data;
    int64_t presentation_time_us = 0;
    int encoded_access_unit_count = 0;
  };

  struct PausePayload {
    unsigned int millis = 0;
    bool iec_bursts = true;
  };

  struct BoolPayload {
    bool value = false;
  };

  struct IntPayload {
    int value = 0;
  };

  class SessionControlProtocol : public actor::Protocol {
   public:
    SessionControlProtocol(CEvent* in_event, CEvent* out_event)
        : actor::Protocol("SinkControlPort", in_event, out_event) {}

    enum OutSignal {
      CONFIGURE,
      RESET,
      PLAY,
      PAUSE,
      FLUSH,
      STOP,
      VOLUME,
      APPFOCUSED,
      SETSILENCETIMEOUT,
      SETNOISETYPE,
      GET_CURRENT_POSITION,
      HAS_PENDING_DATA,
      IS_ENDED,
      GET_BUFFER_SIZE,
      TIMEOUT,
    };

    enum InSignal {
      ACC,
      ERR,
    };
  };

  class SessionDataProtocol : public actor::Protocol {
   public:
    SessionDataProtocol(CEvent* in_event, CEvent* out_event)
        : actor::Protocol("SinkDataPort", in_event, out_event) {}

    enum OutSignal {
      QUEUE_INPUT = 0,
      SAMPLE,
      PAUSE_BURST,
      DRAIN,
    };

    enum InSignal {
      RETURNSAMPLE,
      ACC,
      ERR,
    };
  };

  std::unique_ptr<KodiNativeAudioTrackSink> audio_track_sink_;
  CActiveAEBufferPool buffer_pool_;
  CSampleBuffer sample_of_silence_;
  std::vector<uint8_t> pcm_silence_bytes_;
  int configured_sample_rate_ = 0;
  int configured_channel_count_ = 0;
  int configured_pcm_encoding_ = 0;
  int configured_mime_kind_ = 0;
  int configured_specified_buffer_size_ = 0;
  int configured_output_channel_count_ = 0;
  int configured_audio_session_id_ = 0;
  float configured_volume_ = 1.0f;
  bool verbose_logging_enabled_ = false;
  bool supervise_audio_delay_enabled_ = false;
  PlaybackDecision configured_playback_decision_ = {};
  CAEBitstreamPacker bitstream_packer_;
  bool need_iec_pack_ = false;
  CAEStreamInfo current_stream_info_ = {};
  std::unique_ptr<CDVDAudioCodecPassthrough> passthrough_codec_;
  AEDataFormat sink_data_format_ = AE_FMT_INVALID;
  bool streaming_ = false;
  bool pending_pause_iec_bursts_ = true;
  JavaVM* java_vm_ = nullptr;
  CEvent out_msg_event_;
  SessionControlProtocol control_port_;
  SessionDataProtocol data_port_;
  std::thread worker_thread_;
  bool worker_stop_ = false;
  bool state_machine_self_trigger_ = false;
  bool ext_error_ = false;
  bool ext_app_focused_ = true;
  std::chrono::milliseconds ext_timeout_{std::chrono::milliseconds(1000)};
  std::chrono::milliseconds ext_silence_timeout_{std::chrono::milliseconds::zero()};
  std::chrono::minutes silence_timeout_{std::chrono::minutes(1)};
  EndTime<> ext_silence_timer_;
  int state_ = 0;
  bool stream_noise_enabled_ = true;

  enum SinkStates {
    kStateTop = 0,
    kStateUnconfigured,
    kStateConfigured,
    kStateConfiguredSuspend,
    kStateConfiguredIdle,
    kStateConfiguredPlay,
    kStateConfiguredSilence,
  };

  enum SwapState {
    kCheckSwap,
    kNeedByteSwap,
    kSkipSwap,
  } swap_state_ = kCheckSwap;

  void StateMachine(int signal, actor::Protocol* port, actor::Message* msg);
  void ReturnBuffers();
  void SetSilenceTimer();
  CAEStreamInfo MakeConfiguredStreamInfo() const;
  void PrepareSilenceSample();
  void ConfigureCodec(int mime_kind);
  bool UsesKodiPassthroughCodec() const;
  bool IsConfiguredLocked() const;
  bool CanAcceptInputLocked() const;
  bool HasCodecBufferedDataLocked() const;
  void ResetStreamingStateLocked(bool reset_codec);
  void ThreadMain();
  void StopWorker();
  bool SendControlSync(int signal,
                       actor::CPayloadWrapBase* payload,
                       actor::Message** reply,
                       std::chrono::milliseconds timeout);
  bool SendDataSync(int signal,
                    actor::CPayloadWrapBase* payload,
                    actor::Message** reply,
                    std::chrono::milliseconds timeout);
  JNIEnv* GetWorkerEnv(bool* attached) const;
  void DetachWorkerEnvIfNeeded(bool attached) const;
  bool OpenSink();
  void ConfigureOnWorker(const ConfigurePayload& payload);
  void QueueInputOnWorker(const BufferPayload& payload);
  bool HandleBufferOnWorker(const BufferPayload& payload);
  bool QueuePauseToSinkOnWorker(const PausePayload& payload);
  void QueuePauseOnWorker(const PausePayload& payload);
  void PlayOnWorker();
  void PauseOnWorker();
  void FlushOnWorker();
  void StopOnWorker();
  void ResetOnWorker();
  void DrainOnWorker();
  void SetVolumeOnWorker(float volume);
  int64_t GetCurrentPositionUsOnWorker();
  bool HasPendingDataOnWorker();
  bool IsEndedOnWorker();
  int64_t GetBufferSizeUsOnWorker() const;
  void SwapInit(CSampleBuffer* samples);
  bool OutputSamples(JNIEnv* env, CSampleBuffer* samples);
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_NATIVE_SINK_SESSION_H_
