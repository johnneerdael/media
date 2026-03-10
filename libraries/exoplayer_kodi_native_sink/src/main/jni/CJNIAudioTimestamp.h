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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_TIMESTAMP_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_TIMESTAMP_H_

#include <jni.h>

#include <cstdint>

namespace androidx_media3 {

class CJNIAudioTimestamp {
 public:
  static bool EnsureIds(JNIEnv* env);

  explicit CJNIAudioTimestamp(JNIEnv* env);
  ~CJNIAudioTimestamp();

  void release(JNIEnv* env);

  jobject object() const;
  uint64_t framePosition(JNIEnv* env) const;
  int64_t nanoTime(JNIEnv* env) const;

 private:
  jobject object_;
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_TIMESTAMP_H_
