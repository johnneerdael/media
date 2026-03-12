#pragma once

#include <jni.h>

namespace jni
{

class CJNIAudioFormat
{
public:
  static int CHANNEL_OUT_FRONT_LEFT;
  static int CHANNEL_OUT_FRONT_RIGHT;
  static int CHANNEL_OUT_FRONT_CENTER;
  static int CHANNEL_OUT_LOW_FREQUENCY;
  static int CHANNEL_OUT_BACK_LEFT;
  static int CHANNEL_OUT_BACK_RIGHT;
  static int CHANNEL_OUT_SIDE_LEFT;
  static int CHANNEL_OUT_SIDE_RIGHT;
  static int CHANNEL_OUT_FRONT_LEFT_OF_CENTER;
  static int CHANNEL_OUT_FRONT_RIGHT_OF_CENTER;
  static int CHANNEL_OUT_BACK_CENTER;
  static int CHANNEL_OUT_STEREO;
  static int CHANNEL_OUT_5POINT1;
  static int CHANNEL_OUT_7POINT1_SURROUND;

  static int ENCODING_PCM_16BIT;
  static int ENCODING_PCM_FLOAT;
  static int ENCODING_AC3;
  static int ENCODING_E_AC3;
  static int ENCODING_DTS;
  static int ENCODING_DTS_HD;
  static int ENCODING_DOLBY_TRUEHD;
  static int ENCODING_IEC61937;
};

class CJNIAudioFormatBuilder
{
public:
  CJNIAudioFormatBuilder();
  ~CJNIAudioFormatBuilder();

  void setChannelMask(int channelMask) const;
  void setEncoding(int encoding) const;
  void setSampleRate(int sampleRate) const;
  jobject build() const;

private:
  jobject m_builder = nullptr;
};

} // namespace jni

using CJNIAudioFormat = jni::CJNIAudioFormat;
using CJNIAudioFormatBuilder = jni::CJNIAudioFormatBuilder;
