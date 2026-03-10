/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Wrapped the declarations in the androidx_media3 namespace.
 *  - Simplified string rendering so this staged port does not depend on AEUtil yet.
 */

#include "AEDeviceInfo.h"

#include <sstream>

namespace androidx_media3 {

CAEDeviceInfo::operator std::string()
{
  std::stringstream ss;
  ss << "m_deviceName      : " << m_deviceName << '\n';
  ss << "m_displayName     : " << m_displayName << '\n';
  ss << "m_displayNameExtra: " << m_displayNameExtra << '\n';
  ss << "m_deviceType      : " << DeviceTypeToString(m_deviceType) << '\n';
  ss << "m_channels        : " << static_cast<std::string>(m_channels) << '\n';
  ss << "m_sampleRates     : ";
  for (auto it = m_sampleRates.begin(); it != m_sampleRates.end(); ++it)
  {
    if (it != m_sampleRates.begin())
      ss << ',';
    ss << *it;
  }
  ss << '\n';
  ss << "m_streamTypes     : " << m_streamTypes.size() << '\n';
  return ss.str();
}

std::string CAEDeviceInfo::DeviceTypeToString(enum AEDeviceType deviceType)
{
  switch (deviceType)
  {
    case AE_DEVTYPE_PCM:
      return "AE_DEVTYPE_PCM";
    case AE_DEVTYPE_IEC958:
      return "AE_DEVTYPE_IEC958";
    case AE_DEVTYPE_HDMI:
      return "AE_DEVTYPE_HDMI";
    case AE_DEVTYPE_DP:
      return "AE_DEVTYPE_DP";
  }
  return "INVALID";
}

std::string CAEDeviceInfo::GetFriendlyName() const
{
  return (m_deviceName != m_displayName) ? m_displayName : m_displayNameExtra;
}

std::string CAEDeviceInfo::ToDeviceString(const std::string& driver) const
{
  std::string device = driver.empty() ? m_deviceName : driver + ":" + m_deviceName;
  const std::string fn = GetFriendlyName();
  if (!fn.empty())
    device += "|" + fn;
  return device;
}

}  // namespace androidx_media3
