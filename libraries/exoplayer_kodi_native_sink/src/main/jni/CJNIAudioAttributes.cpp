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

#include "CJNIAudioAttributes.h"

namespace androidx_media3 {
namespace {

struct CachedIds {
  jclass builder_class = nullptr;
  jmethodID ctor = nullptr;
  jmethodID set_usage = nullptr;
  jmethodID set_content_type = nullptr;
  jmethodID build = nullptr;
};

CachedIds& GetCachedIds() {
  static CachedIds ids;
  return ids;
}

}  // namespace

bool CJNIAudioAttributesBuilder::EnsureIds(JNIEnv* env) {
  CachedIds& ids = GetCachedIds();
  if (ids.builder_class != nullptr) {
    return true;
  }
  jclass local = env->FindClass("android/media/AudioAttributes$Builder");
  if (local == nullptr) {
    return false;
  }
  ids.builder_class = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  ids.ctor = env->GetMethodID(ids.builder_class, "<init>", "()V");
  ids.set_usage = env->GetMethodID(
      ids.builder_class, "setUsage", "(I)Landroid/media/AudioAttributes$Builder;");
  ids.set_content_type = env->GetMethodID(
      ids.builder_class, "setContentType", "(I)Landroid/media/AudioAttributes$Builder;");
  ids.build = env->GetMethodID(ids.builder_class, "build", "()Landroid/media/AudioAttributes;");
  return ids.ctor != nullptr && ids.set_usage != nullptr && ids.set_content_type != nullptr &&
         ids.build != nullptr;
}

CJNIAudioAttributesBuilder::CJNIAudioAttributesBuilder(JNIEnv* env) : builder_(nullptr) {
  if (!EnsureIds(env)) {
    return;
  }
  builder_ = env->NewObject(GetCachedIds().builder_class, GetCachedIds().ctor);
}

CJNIAudioAttributesBuilder::~CJNIAudioAttributesBuilder() = default;

void CJNIAudioAttributesBuilder::setUsage(JNIEnv* env, int usage) const {
  if (builder_ != nullptr) {
    env->CallObjectMethod(builder_, GetCachedIds().set_usage, usage);
  }
}

void CJNIAudioAttributesBuilder::setContentType(JNIEnv* env, int content_type) const {
  if (builder_ != nullptr) {
    env->CallObjectMethod(builder_, GetCachedIds().set_content_type, content_type);
  }
}

jobject CJNIAudioAttributesBuilder::build(JNIEnv* env) const {
  if (builder_ == nullptr) {
    return nullptr;
  }
  return env->CallObjectMethod(builder_, GetCachedIds().build);
}

}  // namespace androidx_media3
