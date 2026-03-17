#pragma once

#include <jni.h>

#include <cstdint>
#include <vector>

#include "androidjni/AudioAttributes.h"
#include "androidjni/AudioFormat.h"
#include "androidjni/AudioTimestamp.h"
#include "androidjni/ByteBuffer.h"

namespace jni
{

class CJNIAudioTrack
{
public:
  static int MODE_STREAM;
  static int WRITE_BLOCKING;
  static int WRITE_NON_BLOCKING;
  static int PLAYSTATE_STOPPED;
  static int PLAYSTATE_PAUSED;
  static int PLAYSTATE_PLAYING;
  static int STATE_UNINITIALIZED;
  static int STATE_INITIALIZED;
  static int ENCAPSULATION_MODE_NONE;
  static int ENCAPSULATION_MODE_ELEMENTARY_STREAM;

  CJNIAudioTrack(jobject attributes,
                 jobject format,
                 int bufferSize,
                 int mode,
                 int audioSessionId,
                 int encapsulationMode = 0);
  ~CJNIAudioTrack();

  int getState() const;
  int getPlayState() const;
  void play() const;
  void pause() const;
  void stop() const;
  void flush() const;
  void release();
  int setVolume(float volume) const;
  int write(const std::vector<float>& data, int offset, int size, int writeMode);
  int write(const std::vector<int16_t>& data, int offset, int size, int writeMode);
  int write(const std::vector<char>& data, int offset, int size, int writeMode);
  int write(jobject byteBuffer, int size, int writeMode, int64_t timestamp);
  int getPlaybackHeadPosition() const;
  int getLatency() const;
  int getBufferSizeInFrames() const;
  bool getTimestamp(CJNIAudioTimestamp& timestamp) const;

  static int getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat);
  static int getNativeOutputSampleRate(int streamType);

private:
  jobject m_audioTrack = nullptr;
};

} // namespace jni

using CJNIAudioTrack = jni::CJNIAudioTrack;
using CJNIAudioTimestamp = jni::CJNIAudioTimestamp;
using CJNIByteBuffer = jni::CJNIByteBuffer;
