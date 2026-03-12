#pragma once

#include <jni.h>

#include "androidjni/JNIThreading.h"

namespace jni
{

class CJNIContext
{
public:
  static constexpr const char* ACTIVITY_SERVICE = "activity";

  static jobject getSystemService(const char* name);
};

} // namespace jni

using CJNIContext = jni::CJNIContext;
