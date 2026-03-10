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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_FORMAT_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_FORMAT_H_

#include <jni.h>

namespace androidx_media3 {

class CJNIAudioFormatBuilder {
 public:
  static bool EnsureIds(JNIEnv* env);

  explicit CJNIAudioFormatBuilder(JNIEnv* env);
  ~CJNIAudioFormatBuilder();

  void setChannelMask(JNIEnv* env, int channel_mask) const;
  void setEncoding(JNIEnv* env, int encoding) const;
  void setSampleRate(JNIEnv* env, int sample_rate) const;
  jobject build(JNIEnv* env) const;

 private:
  jobject builder_;
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_FORMAT_H_
