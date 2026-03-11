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

#include "CJNIAudioTrack.h"

#include <vector>

#include "CJNIAudioAttributes.h"
#include "CJNIAudioFormat.h"

namespace androidx_media3 {
namespace {

struct CachedIds {
  jclass audio_track_class = nullptr;
  jclass audio_attributes_builder_class = nullptr;
  jclass audio_format_builder_class = nullptr;
  jclass audio_timestamp_class = nullptr;
  jmethodID audio_track_ctor = nullptr;
  jmethodID get_state = nullptr;
  jmethodID get_play_state = nullptr;
  jmethodID play = nullptr;
  jmethodID pause = nullptr;
  jmethodID stop = nullptr;
  jmethodID flush = nullptr;
  jmethodID release = nullptr;
  jmethodID set_volume = nullptr;
  jmethodID write_bytes = nullptr;
  jmethodID write_shorts = nullptr;
  jmethodID write_floats = nullptr;
  jmethodID get_playback_head_position = nullptr;
  jmethodID get_latency = nullptr;
  jmethodID get_buffer_size_in_frames = nullptr;
  jmethodID get_timestamp = nullptr;
  jmethodID get_min_buffer_size = nullptr;
};

CachedIds& GetCachedIds() {
  static CachedIds ids;
  return ids;
}

bool EnsureIds(JNIEnv* env) {
  CachedIds& ids = GetCachedIds();
  if (ids.audio_track_class != nullptr) {
    return true;
  }

  auto load_class = [env](const char* name) -> jclass {
    jclass local = env->FindClass(name);
    if (local == nullptr) {
      return nullptr;
    }
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
  };

  ids.audio_track_class = load_class("android/media/AudioTrack");
  if (ids.audio_track_class == nullptr || !CJNIAudioAttributesBuilder::EnsureIds(env) ||
      !CJNIAudioFormatBuilder::EnsureIds(env) || !CJNIAudioTimestamp::EnsureIds(env)) {
    return false;
  }

  ids.audio_track_ctor = env->GetMethodID(
      ids.audio_track_class,
      "<init>",
      "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V");
  ids.get_state = env->GetMethodID(ids.audio_track_class, "getState", "()I");
  ids.get_play_state = env->GetMethodID(ids.audio_track_class, "getPlayState", "()I");
  ids.play = env->GetMethodID(ids.audio_track_class, "play", "()V");
  ids.pause = env->GetMethodID(ids.audio_track_class, "pause", "()V");
  ids.stop = env->GetMethodID(ids.audio_track_class, "stop", "()V");
  ids.flush = env->GetMethodID(ids.audio_track_class, "flush", "()V");
  ids.release = env->GetMethodID(ids.audio_track_class, "release", "()V");
  ids.set_volume = env->GetMethodID(ids.audio_track_class, "setVolume", "(F)I");
  ids.write_bytes = env->GetMethodID(ids.audio_track_class, "write", "([BIII)I");
  ids.write_shorts = env->GetMethodID(ids.audio_track_class, "write", "([SIII)I");
  ids.write_floats = env->GetMethodID(ids.audio_track_class, "write", "([FIII)I");
  ids.get_playback_head_position =
      env->GetMethodID(ids.audio_track_class, "getPlaybackHeadPosition", "()I");
  ids.get_latency = env->GetMethodID(ids.audio_track_class, "getLatency", "()I");
  ids.get_buffer_size_in_frames =
      env->GetMethodID(ids.audio_track_class, "getBufferSizeInFrames", "()I");
  ids.get_timestamp =
      env->GetMethodID(ids.audio_track_class, "getTimestamp", "(Landroid/media/AudioTimestamp;)Z");
  ids.get_min_buffer_size =
      env->GetStaticMethodID(ids.audio_track_class, "getMinBufferSize", "(III)I");
  return ids.audio_track_ctor != nullptr && ids.write_bytes != nullptr;
}

}  // namespace

int CJNIAudioTrack::GetMinBufferSize(JNIEnv* env, int sample_rate, int channel_mask, int encoding) {
  if (!EnsureIds(env)) {
    return -1;
  }
  return env->CallStaticIntMethod(
      GetCachedIds().audio_track_class,
      GetCachedIds().get_min_buffer_size,
      sample_rate,
      channel_mask,
      encoding);
}

std::unique_ptr<CJNIAudioTrack> CJNIAudioTrack::Create(
    JNIEnv* env,
    int sample_rate,
    int channel_mask,
    int encoding,
    int buffer_size,
    int audio_session_id) {
  if (!EnsureIds(env)) {
    return nullptr;
  }
  CJNIAudioAttributesBuilder attributes_builder(env);
  attributes_builder.setUsage(env, CJNIAudioAttributesBuilder::USAGE_MEDIA);
  attributes_builder.setContentType(env, CJNIAudioAttributesBuilder::CONTENT_TYPE_MUSIC);
  jobject attributes = attributes_builder.build(env);
  CJNIAudioFormatBuilder format_builder(env);
  format_builder.setChannelMask(env, channel_mask);
  format_builder.setEncoding(env, encoding);
  format_builder.setSampleRate(env, sample_rate);
  jobject format = format_builder.build(env);
  std::unique_ptr<CJNIAudioTimestamp> timestamp = std::make_unique<CJNIAudioTimestamp>(env);
  if (attributes == nullptr || format == nullptr || timestamp->object() == nullptr) {
    if (attributes != nullptr) env->DeleteLocalRef(attributes);
    if (format != nullptr) env->DeleteLocalRef(format);
    return nullptr;
  }
  jobject local_track = env->NewObject(
      GetCachedIds().audio_track_class,
      GetCachedIds().audio_track_ctor,
      attributes,
      format,
      buffer_size,
      MODE_STREAM,
      audio_session_id == 0 ? AUDIO_SESSION_ID_GENERATE : audio_session_id);
  env->DeleteLocalRef(attributes);
  env->DeleteLocalRef(format);
  if (local_track == nullptr) {
    return nullptr;
  }
  jobject global_track = env->NewGlobalRef(local_track);
  env->DeleteLocalRef(local_track);
  if (global_track == nullptr) {
    if (global_track != nullptr) env->DeleteGlobalRef(global_track);
    return nullptr;
  }
  return std::unique_ptr<CJNIAudioTrack>(new CJNIAudioTrack(global_track, std::move(timestamp)));
}

CJNIAudioTrack::CJNIAudioTrack(jobject audio_track, std::unique_ptr<CJNIAudioTimestamp> audio_timestamp)
    : audio_track_(audio_track), audio_timestamp_(std::move(audio_timestamp)) {}

CJNIAudioTrack::~CJNIAudioTrack() = default;

int CJNIAudioTrack::getState(JNIEnv* env) const {
  return env->CallIntMethod(audio_track_, GetCachedIds().get_state);
}

int CJNIAudioTrack::getPlayState(JNIEnv* env) const {
  return env->CallIntMethod(audio_track_, GetCachedIds().get_play_state);
}

void CJNIAudioTrack::play(JNIEnv* env) const {
  env->CallVoidMethod(audio_track_, GetCachedIds().play);
}

void CJNIAudioTrack::pause(JNIEnv* env) const {
  env->CallVoidMethod(audio_track_, GetCachedIds().pause);
}

void CJNIAudioTrack::stop(JNIEnv* env) const {
  env->CallVoidMethod(audio_track_, GetCachedIds().stop);
}

void CJNIAudioTrack::flush(JNIEnv* env) const {
  env->CallVoidMethod(audio_track_, GetCachedIds().flush);
}

void CJNIAudioTrack::release(JNIEnv* env) {
  if (audio_track_ != nullptr) {
    env->CallVoidMethod(audio_track_, GetCachedIds().release);
    env->DeleteGlobalRef(audio_track_);
    audio_track_ = nullptr;
  }
  if (audio_timestamp_ != nullptr) {
    audio_timestamp_->release(env);
    audio_timestamp_.reset();
  }
}

int CJNIAudioTrack::setVolume(JNIEnv* env, float volume) const {
  return env->CallIntMethod(audio_track_, GetCachedIds().set_volume, volume);
}

int CJNIAudioTrack::write(JNIEnv* env, const uint8_t* data, int size_bytes, int write_mode) {
  if (data == nullptr || size_bytes <= 0) {
    return 0;
  }
  jbyteArray array = env->NewByteArray(size_bytes);
  if (array == nullptr) {
    return -1;
  }
  env->SetByteArrayRegion(array, 0, size_bytes, reinterpret_cast<const jbyte*>(data));
  int written =
      env->CallIntMethod(audio_track_, GetCachedIds().write_bytes, array, 0, size_bytes, write_mode);
  env->DeleteLocalRef(array);
  return written;
}

int CJNIAudioTrack::write(JNIEnv* env, const std::vector<int16_t>& data, int write_mode) {
  if (data.empty()) {
    return 0;
  }
  jshortArray array = env->NewShortArray(data.size());
  if (array == nullptr) {
    return -1;
  }
  env->SetShortArrayRegion(array, 0, data.size(), reinterpret_cast<const jshort*>(data.data()));
  int written =
      env->CallIntMethod(audio_track_, GetCachedIds().write_shorts, array, 0, data.size(), write_mode);
  env->DeleteLocalRef(array);
  return written > 0 ? written * static_cast<int>(sizeof(int16_t)) : written;
}

int CJNIAudioTrack::write(JNIEnv* env, const std::vector<float>& data, int write_mode) {
  if (data.empty()) {
    return 0;
  }
  jfloatArray array = env->NewFloatArray(data.size());
  if (array == nullptr) {
    return -1;
  }
  env->SetFloatArrayRegion(array, 0, data.size(), reinterpret_cast<const jfloat*>(data.data()));
  int written =
      env->CallIntMethod(audio_track_, GetCachedIds().write_floats, array, 0, data.size(), write_mode);
  env->DeleteLocalRef(array);
  return written > 0 ? written * static_cast<int>(sizeof(float)) : written;
}

int CJNIAudioTrack::getPlaybackHeadPosition(JNIEnv* env) const {
  return env->CallIntMethod(audio_track_, GetCachedIds().get_playback_head_position);
}

int CJNIAudioTrack::getLatency(JNIEnv* env) const {
  if (GetCachedIds().get_latency == nullptr) {
    return -1;
  }
  return env->CallIntMethod(audio_track_, GetCachedIds().get_latency);
}

int CJNIAudioTrack::getBufferSizeInFrames(JNIEnv* env) const {
  if (GetCachedIds().get_buffer_size_in_frames == nullptr) {
    return -1;
  }
  return env->CallIntMethod(audio_track_, GetCachedIds().get_buffer_size_in_frames);
}

bool CJNIAudioTrack::getTimestamp(JNIEnv* env, uint64_t* frame_position, int64_t* nano_time) const {
  const jboolean ok =
      env->CallBooleanMethod(audio_track_, GetCachedIds().get_timestamp, audio_timestamp_->object());
  if (ok != JNI_TRUE) {
    return false;
  }
  if (frame_position != nullptr) {
    *frame_position = audio_timestamp_->framePosition(env);
  }
  if (nano_time != nullptr) {
    *nano_time = audio_timestamp_->nanoTime(env);
  }
  return true;
}

}  // namespace androidx_media3
