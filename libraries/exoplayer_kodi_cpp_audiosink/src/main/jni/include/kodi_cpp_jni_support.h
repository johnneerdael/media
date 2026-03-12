#pragma once

#include <jni.h>

JavaVM* kodi_cpp_get_java_vm();
void kodi_cpp_set_java_vm(JavaVM* vm);
JNIEnv* kodi_cpp_get_env();
