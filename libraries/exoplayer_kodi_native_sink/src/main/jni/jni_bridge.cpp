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

#include <jni.h>

#include <array>
#include <cstdint>
#include <vector>

#include "KodiCapabilitySelector.h"
#include "KodiNativeSinkSession.h"

namespace {

constexpr jint kSmokeTestSentinel = 0x4B4F4449;
constexpr jintArray MakePlaybackDecisionArray(
    JNIEnv* env, const androidx_media3::PlaybackDecision& decision) {
  std::array<jint, 5> values = {decision.mode,
                                decision.output_encoding,
                                decision.channel_config,
                                decision.stream_type,
                                decision.flags};
  jintArray result = env->NewIntArray(values.size());
  if (result == nullptr) {
    return nullptr;
  }
  env->SetIntArrayRegion(result, 0, values.size(), values.data());
  return result;
}

constexpr jlongArray MakePacketMetadataArray(
    JNIEnv* env, const androidx_media3::PacketMetadata& packet) {
  std::array<jlong, 5> values = {packet.kind,
                                 packet.size_bytes,
                                 packet.total_frames,
                                 packet.normalized_access_units,
                                 packet.effective_presentation_time_us};
  jlongArray result = env->NewLongArray(values.size());
  if (result == nullptr) {
    return nullptr;
  }
  env->SetLongArrayRegion(result, 0, values.size(), values.data());
  return result;
}

androidx_media3::KodiNativeSinkSession* AsSession(jlong native_handle) {
  return reinterpret_cast<androidx_media3::KodiNativeSinkSession*>(native_handle);
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeLibrary_nGetSmokeTestValue(
    JNIEnv* env, jclass clazz) {
  (void)env;
  (void)clazz;
  return kSmokeTestSentinel;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeCapabilitySelector_nEvaluatePlaybackDecision(
    JNIEnv* env,
    jclass clazz,
    jint sdk_int,
    jboolean tv,
    jboolean automotive,
    jint routed_device_id,
    jint routed_device_type,
    jint max_channel_count,
    jintArray supported_encodings,
    jboolean ac3_supported,
    jint ac3_encoding,
    jint ac3_channel_config,
    jboolean eac3_supported,
    jint eac3_encoding,
    jint eac3_channel_config,
    jboolean dts_supported,
    jint dts_encoding,
    jint dts_channel_config,
    jboolean dtshd_supported,
    jint dtshd_encoding,
    jint dtshd_channel_config,
    jboolean truehd_supported,
    jint truehd_encoding,
    jint truehd_channel_config,
    jboolean passthrough_enabled,
    jboolean ac3_passthrough_enabled,
    jboolean eac3_passthrough_enabled,
    jboolean dts_passthrough_enabled,
    jboolean truehd_passthrough_enabled,
    jboolean dtshd_passthrough_enabled,
    jboolean dtshd_core_fallback_enabled,
    jint max_pcm_channel_layout,
    jint mime_kind,
    jint channel_count,
    jint sample_rate) {
  (void)clazz;
  (void)supported_encodings;
  const androidx_media3::CapabilitySnapshot snapshot = {
      /*sdk_int=*/sdk_int,
      /*tv=*/tv == JNI_TRUE,
      /*automotive=*/automotive == JNI_TRUE,
      /*routed_device_id=*/routed_device_id,
      /*routed_device_type=*/routed_device_type,
      /*max_channel_count=*/max_channel_count,
      /*ac3=*/{ac3_supported == JNI_TRUE, ac3_encoding, ac3_channel_config},
      /*eac3=*/{eac3_supported == JNI_TRUE, eac3_encoding, eac3_channel_config},
      /*dts=*/{dts_supported == JNI_TRUE, dts_encoding, dts_channel_config},
      /*dtshd=*/{dtshd_supported == JNI_TRUE, dtshd_encoding, dtshd_channel_config},
      /*truehd=*/{truehd_supported == JNI_TRUE, truehd_encoding, truehd_channel_config},
  };
  const androidx_media3::UserAudioSettings user_settings = {
      /*passthrough_enabled=*/passthrough_enabled == JNI_TRUE,
      /*ac3_passthrough_enabled=*/ac3_passthrough_enabled == JNI_TRUE,
      /*eac3_passthrough_enabled=*/eac3_passthrough_enabled == JNI_TRUE,
      /*dts_passthrough_enabled=*/dts_passthrough_enabled == JNI_TRUE,
      /*truehd_passthrough_enabled=*/truehd_passthrough_enabled == JNI_TRUE,
      /*dtshd_passthrough_enabled=*/dtshd_passthrough_enabled == JNI_TRUE,
      /*dtshd_core_fallback_enabled=*/dtshd_core_fallback_enabled == JNI_TRUE,
      /*max_pcm_channel_layout=*/max_pcm_channel_layout,
  };
  return MakePlaybackDecisionArray(
      env,
      androidx_media3::EvaluatePlaybackDecision(
          snapshot, user_settings, mime_kind, channel_count, sample_rate));
}

extern "C" JNIEXPORT jlong JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nCreate(
    JNIEnv* env, jclass clazz) {
  (void)env;
  (void)clazz;
  return reinterpret_cast<jlong>(new androidx_media3::KodiNativeSinkSession());
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nConfigure(
    JNIEnv* env,
    jclass clazz,
    jlong native_handle,
    jint mime_kind,
    jint sample_rate,
    jint channel_count,
    jint pcm_encoding,
    jint specified_buffer_size,
    jint output_channel_count,
    jint audio_session_id,
    jfloat volume,
    jint sdk_int,
    jboolean tv,
    jboolean automotive,
    jint routed_device_id,
    jint routed_device_type,
    jint max_channel_count,
    jboolean ac3_supported,
    jint ac3_encoding,
    jint ac3_channel_config,
    jboolean eac3_supported,
    jint eac3_encoding,
    jint eac3_channel_config,
    jboolean dts_supported,
    jint dts_encoding,
    jint dts_channel_config,
    jboolean dtshd_supported,
    jint dtshd_encoding,
    jint dtshd_channel_config,
    jboolean truehd_supported,
    jint truehd_encoding,
    jint truehd_channel_config,
    jint playback_mode,
    jint playback_encoding,
    jint playback_channel_config,
    jint playback_stream_type,
    jint playback_flags) {
  (void)clazz;
  const androidx_media3::CapabilitySnapshot capability_snapshot = {
      /*sdk_int=*/sdk_int,
      /*tv=*/tv == JNI_TRUE,
      /*automotive=*/automotive == JNI_TRUE,
      /*routed_device_id=*/routed_device_id,
      /*routed_device_type=*/routed_device_type,
      /*max_channel_count=*/max_channel_count,
      /*ac3=*/{ac3_supported == JNI_TRUE, ac3_encoding, ac3_channel_config},
      /*eac3=*/{eac3_supported == JNI_TRUE, eac3_encoding, eac3_channel_config},
      /*dts=*/{dts_supported == JNI_TRUE, dts_encoding, dts_channel_config},
      /*dtshd=*/{dtshd_supported == JNI_TRUE, dtshd_encoding, dtshd_channel_config},
      /*truehd=*/{truehd_supported == JNI_TRUE, truehd_encoding, truehd_channel_config},
  };
  const androidx_media3::PlaybackDecision playback_decision = {
      /*mode=*/playback_mode,
      /*output_encoding=*/playback_encoding,
      /*channel_config=*/playback_channel_config,
      /*stream_type=*/playback_stream_type,
      /*flags=*/playback_flags,
  };
  AsSession(native_handle)
      ->Configure(
          mime_kind,
          env,
          sample_rate,
          channel_count,
          pcm_encoding,
          specified_buffer_size,
          output_channel_count,
          audio_session_id,
          volume,
          capability_snapshot,
          playback_decision);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nQueueInput(
    JNIEnv* env,
    jclass clazz,
    jlong native_handle,
    jobject buffer,
    jint offset,
    jint size,
    jlong presentation_time_us,
    jint encoded_access_unit_count) {
  (void)clazz;
  const auto* base =
      static_cast<const uint8_t*>(buffer != nullptr ? env->GetDirectBufferAddress(buffer) : nullptr);
  const uint8_t* data = base != nullptr ? base + offset : nullptr;
  AsSession(native_handle)
      ->QueueInput(data, size, static_cast<int64_t>(presentation_time_us), encoded_access_unit_count);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nQueuePause(
    JNIEnv* env,
    jclass clazz,
    jlong native_handle,
    jint millis,
    jboolean iec_bursts) {
  (void)env;
  (void)clazz;
  AsSession(native_handle)->QueuePause(static_cast<unsigned int>(millis), iec_bursts == JNI_TRUE);
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nDequeuePacketMetadata(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)clazz;
  androidx_media3::PacketMetadata packet;
  if (!AsSession(native_handle)->DequeuePacket(&packet)) {
    return nullptr;
  }
  return MakePacketMetadataArray(env, packet);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nDequeuePacketBytes(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)clazz;
  std::vector<uint8_t> data;
  if (!AsSession(native_handle)->TakeLastDequeuedPacketData(&data)) {
    return nullptr;
  }
  jbyteArray result = env->NewByteArray(data.size());
  if (result == nullptr) {
    return nullptr;
  }
  env->SetByteArrayRegion(
      result, 0, data.size(), reinterpret_cast<const jbyte*>(data.data()));
  return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nGetPendingPacketCount(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)env;
  (void)clazz;
  return AsSession(native_handle)->pending_packet_count();
}

extern "C" JNIEXPORT jlong JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nGetQueuedInputBytes(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)env;
  (void)clazz;
  return static_cast<jlong>(AsSession(native_handle)->queued_input_bytes());
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nPlay(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Play(env);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nPause(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Pause(env);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nFlush(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Flush(env);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nStop(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Stop(env);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nReset(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Reset(env);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nPlayToEndOfStream(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)clazz;
  AsSession(native_handle)->PlayToEndOfStream(env);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nSetVolume(
    JNIEnv* env, jclass clazz, jlong native_handle, jfloat volume) {
  (void)clazz;
  AsSession(native_handle)->SetVolume(env, volume);
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nDrainOnePacketToAudioTrack(
    JNIEnv* env, jclass clazz, jlong native_handle, jboolean counts_toward_media_position) {
  (void)clazz;
  androidx_media3::PacketMetadata packet;
  if (!AsSession(native_handle)
           ->DrainOnePacketToAudioTrack(
               env, counts_toward_media_position == JNI_TRUE, &packet)) {
    return nullptr;
  }
  return MakePacketMetadataArray(env, packet);
}

extern "C" JNIEXPORT jlong JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nGetCurrentPositionUs(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)clazz;
  return static_cast<jlong>(AsSession(native_handle)->GetCurrentPositionUs(env));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nHasPendingData(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)clazz;
  return AsSession(native_handle)->HasPendingData(env) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nIsEnded(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)clazz;
  return AsSession(native_handle)->IsEnded(env) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nGetBufferSizeUs(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)env;
  (void)clazz;
  return static_cast<jlong>(AsSession(native_handle)->GetBufferSizeUs());
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeSinkSession_nRelease(
    JNIEnv* env, jclass clazz, jlong native_handle) {
  (void)clazz;
  AsSession(native_handle)->Reset(env);
  delete AsSession(native_handle);
}
