/*
 *  Copyright (C) 2020 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "platform/android/utils/AndroidInterfaceForCLog.h"

#include <android/log.h>
#include <dlfcn.h>
#include <spdlog/sinks/android_sink.h>
#include <spdlog/sinks/dist_sink.h>

namespace
{
constexpr auto kAndroidLogTag = "KodiCppAudioSink";

void ActivateAndroidDebugLogging()
{
  void* libHandle = dlopen("liblog.so", RTLD_LAZY);
  if (libHandle == nullptr)
    return;

  void* funcPtr = dlsym(libHandle, "__android_log_set_minimum_priority");
  if (funcPtr != nullptr)
  {
    using android_log_set_minimum_priority_func = int32_t (*)(int32_t);
    reinterpret_cast<android_log_set_minimum_priority_func>(funcPtr)(ANDROID_LOG_DEBUG);
  }

  dlclose(libHandle);
}
} // namespace

std::unique_ptr<IPlatformLog> IPlatformLog::CreatePlatformLog()
{
  ActivateAndroidDebugLogging();
  return std::make_unique<CAndroidInterfaceForCLog>();
}

void CAndroidInterfaceForCLog::AddSinks(
    std::shared_ptr<spdlog::sinks::dist_sink<std::mutex>> distributionSink) const
{
  distributionSink->add_sink(
      std::make_shared<spdlog::sinks::android_sink_st>(kAndroidLogTag));
}
