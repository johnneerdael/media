#pragma once

#include <jni.h>

#include <vector>

namespace jni
{

class CJNIByteBuffer
{
public:
  CJNIByteBuffer() = default;
  explicit CJNIByteBuffer(jobject object);
  ~CJNIByteBuffer();

  CJNIByteBuffer(const CJNIByteBuffer&) = delete;
  CJNIByteBuffer& operator=(const CJNIByteBuffer&) = delete;
  CJNIByteBuffer(CJNIByteBuffer&& other) noexcept;
  CJNIByteBuffer& operator=(CJNIByteBuffer&& other) noexcept;

  static CJNIByteBuffer wrap(const std::vector<char>& data);
  jobject get_raw() const;

private:
  jobject m_object = nullptr;
};

} // namespace jni

using CJNIByteBuffer = jni::CJNIByteBuffer;
