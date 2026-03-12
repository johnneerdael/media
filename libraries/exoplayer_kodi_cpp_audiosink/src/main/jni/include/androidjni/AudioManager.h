#pragma once

namespace jni
{

class CJNIAudioManager
{
public:
  static int AUDIO_SESSION_ID_GENERATE;
  static int STREAM_MUSIC;
};

} // namespace jni

using CJNIAudioManager = jni::CJNIAudioManager;
