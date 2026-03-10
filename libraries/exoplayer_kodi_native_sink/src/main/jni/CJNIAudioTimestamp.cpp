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

#include "CJNIAudioTimestamp.h"

namespace androidx_media3 {
namespace {

struct CachedIds {
  jclass timestamp_class = nullptr;
  jmethodID ctor = nullptr;
  jfieldID frame_position = nullptr;
  jfieldID nano_time = nullptr;
};

CachedIds& GetCachedIds() {
  static CachedIds ids;
  return ids;
}

}  // namespace

bool CJNIAudioTimestamp::EnsureIds(JNIEnv* env) {
  CachedIds& ids = GetCachedIds();
  if (ids.timestamp_class != nullptr) {
    return true;
  }
  jclass local = env->FindClass("android/media/AudioTimestamp");
  if (local == nullptr) {
    return false;
  }
  ids.timestamp_class = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  ids.ctor = env->GetMethodID(ids.timestamp_class, "<init>", "()V");
  ids.frame_position = env->GetFieldID(ids.timestamp_class, "framePosition", "J");
  ids.nano_time = env->GetFieldID(ids.timestamp_class, "nanoTime", "J");
  return ids.ctor != nullptr && ids.frame_position != nullptr && ids.nano_time != nullptr;
}

CJNIAudioTimestamp::CJNIAudioTimestamp(JNIEnv* env) : object_(nullptr) {
  if (!EnsureIds(env)) {
    return;
  }
  jobject local = env->NewObject(GetCachedIds().timestamp_class, GetCachedIds().ctor);
  if (local != nullptr) {
    object_ = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
  }
}

CJNIAudioTimestamp::~CJNIAudioTimestamp() = default;

void CJNIAudioTimestamp::release(JNIEnv* env) {
  if (object_ != nullptr) {
    env->DeleteGlobalRef(object_);
    object_ = nullptr;
  }
}

jobject CJNIAudioTimestamp::object() const {
  return object_;
}

uint64_t CJNIAudioTimestamp::framePosition(JNIEnv* env) const {
  return object_ != nullptr
      ? static_cast<uint64_t>(env->GetLongField(object_, GetCachedIds().frame_position))
      : 0;
}

int64_t CJNIAudioTimestamp::nanoTime(JNIEnv* env) const {
  return object_ != nullptr
      ? static_cast<int64_t>(env->GetLongField(object_, GetCachedIds().nano_time))
      : 0;
}

}  // namespace androidx_media3
