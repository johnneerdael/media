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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_TRACK_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_TRACK_H_

#include <jni.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "CJNIAudioTimestamp.h"

namespace androidx_media3 {

class CJNIAudioTrack {
 public:
  static constexpr int MODE_STREAM = 1;
  static constexpr int WRITE_BLOCKING = 0;
  static constexpr int PLAYSTATE_STOPPED = 1;
  static constexpr int PLAYSTATE_PAUSED = 2;
  static constexpr int PLAYSTATE_PLAYING = 3;
  static constexpr int STATE_UNINITIALIZED = 0;
  static constexpr int STATE_INITIALIZED = 1;
  static constexpr int AUDIO_SESSION_ID_GENERATE = 0;
  static constexpr int USAGE_MEDIA = 1;
  static constexpr int CONTENT_TYPE_MUSIC = 2;

  static int GetMinBufferSize(JNIEnv* env, int sample_rate, int channel_mask, int encoding);
  static std::unique_ptr<CJNIAudioTrack> Create(
      JNIEnv* env,
      int sample_rate,
      int channel_mask,
      int encoding,
      int buffer_size,
      int audio_session_id);

  ~CJNIAudioTrack();

  int getState(JNIEnv* env) const;
  int getPlayState(JNIEnv* env) const;
  void play(JNIEnv* env) const;
  void pause(JNIEnv* env) const;
  void stop(JNIEnv* env) const;
  void flush(JNIEnv* env) const;
  void release(JNIEnv* env);
  int setVolume(JNIEnv* env, float volume) const;
  int write(JNIEnv* env, const uint8_t* data, int size_bytes, int write_mode);
  int write(JNIEnv* env, const std::vector<int16_t>& data, int write_mode);
  int write(JNIEnv* env, const std::vector<float>& data, int write_mode);
  int getPlaybackHeadPosition(JNIEnv* env) const;
  int getLatency(JNIEnv* env) const;
  int getBufferSizeInFrames(JNIEnv* env) const;
  bool getTimestamp(JNIEnv* env, uint64_t* frame_position, int64_t* nano_time) const;

 private:
  explicit CJNIAudioTrack(jobject audio_track, std::unique_ptr<CJNIAudioTimestamp> audio_timestamp);

  jobject audio_track_;
  std::unique_ptr<CJNIAudioTimestamp> audio_timestamp_;
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_CJNI_AUDIO_TRACK_H_
