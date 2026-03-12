#pragma once

#include <jni.h>

#include "kodi_cpp_jni_support.h"

inline JNIEnv* xbmc_jnienv()
{
  return kodi_cpp_get_env();
}
