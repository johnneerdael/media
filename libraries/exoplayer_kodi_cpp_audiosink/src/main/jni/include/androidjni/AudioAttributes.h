#pragma once

#include <jni.h>

namespace jni
{

class CJNIAudioAttributes
{
public:
  static int USAGE_MEDIA;
  static int CONTENT_TYPE_MUSIC;
};

class CJNIAudioAttributesBuilder
{
public:
  CJNIAudioAttributesBuilder();
  ~CJNIAudioAttributesBuilder();

  void setUsage(int usage) const;
  void setContentType(int contentType) const;
  jobject build() const;

private:
  jobject m_builder = nullptr;
};

} // namespace jni

using CJNIAudioAttributes = jni::CJNIAudioAttributes;
using CJNIAudioAttributesBuilder = jni::CJNIAudioAttributesBuilder;
