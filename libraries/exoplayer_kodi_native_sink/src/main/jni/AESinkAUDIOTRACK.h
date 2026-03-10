/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Reduced the dependency surface to the native passthrough module.
 *  - Replaced Android JNI AudioTrack access with capability snapshot translation helpers.
 *  - Wrapped the declarations in the androidx_media3 namespace.
 */

#pragma once

#include "AEDeviceInfo.h"

namespace androidx_media3 {

struct CapabilitySnapshot;

class CAESinkAUDIOTRACK
{
public:
  static constexpr const char* kRawDeviceName = "AudioTrack (RAW)";
  static constexpr const char* kIecDeviceName = "AudioTrack (IEC)";

  static void EnumerateDevicesEx(const CapabilitySnapshot& snapshot, AEDeviceInfoList& list);
};

}  // namespace androidx_media3
