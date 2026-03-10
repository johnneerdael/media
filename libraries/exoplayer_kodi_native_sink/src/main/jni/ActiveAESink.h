/*
 *  Copyright (C) 2010-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Reduced the dependency surface to sink capability selection helpers.
 *  - Removed Kodi thread, protocol, and live sink control machinery.
 *  - Wrapped the declarations in the androidx_media3 namespace.
 */

#pragma once

#include "AEAudioFormat.h"
#include "AEDeviceInfo.h"

#include <string>

namespace androidx_media3 {

struct CapabilitySnapshot;

class CActiveAESink
{
public:
  void EnumerateOutputDevices(const CapabilitySnapshot& snapshot);
  bool HasPassthroughDevice() const;
  AEDeviceType GetDeviceType(const std::string& device) const;
  bool SupportsFormat(const std::string& device, AEAudioFormat& format) const;
  bool FindSupportingPassthroughDevice(const std::string& preferred_device,
                                       AEAudioFormat& format,
                                       const CAEDeviceInfo** selected_device) const;
  bool NeedIecPack(const std::string& device) const;

private:
  AEDeviceInfoList m_deviceInfoList;
};

}  // namespace androidx_media3
