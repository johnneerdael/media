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

#include "CJNIAudioFormat.h"

namespace androidx_media3 {
namespace {

struct CachedIds {
  jclass builder_class = nullptr;
  jmethodID ctor = nullptr;
  jmethodID set_channel_mask = nullptr;
  jmethodID set_encoding = nullptr;
  jmethodID set_sample_rate = nullptr;
  jmethodID build = nullptr;
};

CachedIds& GetCachedIds() {
  static CachedIds ids;
  return ids;
}

}  // namespace

bool CJNIAudioFormatBuilder::EnsureIds(JNIEnv* env) {
  CachedIds& ids = GetCachedIds();
  if (ids.builder_class != nullptr) {
    return true;
  }
  jclass local = env->FindClass("android/media/AudioFormat$Builder");
  if (local == nullptr) {
    return false;
  }
  ids.builder_class = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  ids.ctor = env->GetMethodID(ids.builder_class, "<init>", "()V");
  ids.set_channel_mask = env->GetMethodID(
      ids.builder_class, "setChannelMask", "(I)Landroid/media/AudioFormat$Builder;");
  ids.set_encoding = env->GetMethodID(
      ids.builder_class, "setEncoding", "(I)Landroid/media/AudioFormat$Builder;");
  ids.set_sample_rate = env->GetMethodID(
      ids.builder_class, "setSampleRate", "(I)Landroid/media/AudioFormat$Builder;");
  ids.build = env->GetMethodID(ids.builder_class, "build", "()Landroid/media/AudioFormat;");
  return ids.ctor != nullptr && ids.set_channel_mask != nullptr && ids.set_encoding != nullptr &&
         ids.set_sample_rate != nullptr && ids.build != nullptr;
}

CJNIAudioFormatBuilder::CJNIAudioFormatBuilder(JNIEnv* env) : builder_(nullptr) {
  if (!EnsureIds(env)) {
    return;
  }
  builder_ = env->NewObject(GetCachedIds().builder_class, GetCachedIds().ctor);
}

CJNIAudioFormatBuilder::~CJNIAudioFormatBuilder() = default;

void CJNIAudioFormatBuilder::setChannelMask(JNIEnv* env, int channel_mask) const {
  if (builder_ != nullptr) {
    env->CallObjectMethod(builder_, GetCachedIds().set_channel_mask, channel_mask);
  }
}

void CJNIAudioFormatBuilder::setEncoding(JNIEnv* env, int encoding) const {
  if (builder_ != nullptr) {
    env->CallObjectMethod(builder_, GetCachedIds().set_encoding, encoding);
  }
}

void CJNIAudioFormatBuilder::setSampleRate(JNIEnv* env, int sample_rate) const {
  if (builder_ != nullptr) {
    env->CallObjectMethod(builder_, GetCachedIds().set_sample_rate, sample_rate);
  }
}

jobject CJNIAudioFormatBuilder::build(JNIEnv* env) const {
  if (builder_ == nullptr) {
    return nullptr;
  }
  return env->CallObjectMethod(builder_, GetCachedIds().build);
}

}  // namespace androidx_media3
