#pragma once

#include <jni.h>

#include <cstdint>

#include "androidjni/Context.h"

namespace jni
{

class CJNIActivityManager
{
public:
  class MemoryInfo
  {
  public:
    MemoryInfo();
    ~MemoryInfo();

    MemoryInfo(const MemoryInfo&) = delete;
    MemoryInfo& operator=(const MemoryInfo&) = delete;

    int64_t totalMem() const;
    int64_t availMem() const;
    jobject get_raw() const;

  private:
    jobject m_object = nullptr;
  };

  explicit CJNIActivityManager(jobject object);
  ~CJNIActivityManager();

  CJNIActivityManager(const CJNIActivityManager&) = delete;
  CJNIActivityManager& operator=(const CJNIActivityManager&) = delete;

  void getMemoryInfo(MemoryInfo& info) const;

private:
  jobject m_object = nullptr;
};

} // namespace jni

using CJNIActivityManager = jni::CJNIActivityManager;
