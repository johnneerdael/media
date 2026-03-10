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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_ATTRIBUTES_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_ATTRIBUTES_H_

#include <jni.h>

namespace androidx_media3 {

class CJNIAudioAttributesBuilder {
 public:
  static constexpr int USAGE_MEDIA = 1;
  static constexpr int CONTENT_TYPE_MUSIC = 2;

  static bool EnsureIds(JNIEnv* env);

  explicit CJNIAudioAttributesBuilder(JNIEnv* env);
  ~CJNIAudioAttributesBuilder();

  void setUsage(JNIEnv* env, int usage) const;
  void setContentType(JNIEnv* env, int content_type) const;
  jobject build(JNIEnv* env) const;

 private:
  jobject builder_;
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_ATTRIBUTES_H_
