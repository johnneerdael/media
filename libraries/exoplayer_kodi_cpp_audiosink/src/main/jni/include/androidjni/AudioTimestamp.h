#pragma once

#include <jni.h>

#include <cstdint>

namespace jni
{

class CJNIAudioTimestamp
{
public:
  CJNIAudioTimestamp();
  ~CJNIAudioTimestamp();

  jobject get_raw() const;
  uint64_t get_framePosition() const;
  int64_t get_nanoTime() const;

private:
  jobject m_object = nullptr;
};

} // namespace jni

using CJNIAudioTimestamp = jni::CJNIAudioTimestamp;
