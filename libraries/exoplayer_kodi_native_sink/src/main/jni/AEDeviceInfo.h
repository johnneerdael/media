/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Wrapped the declarations in the androidx_media3 namespace.
 *  - Kept this file staged for the native capability model port.
 */

#pragma once

#include "AEChannelInfo.h"
#include "AEStreamInfo.h"

#include <string>
#include <vector>

namespace androidx_media3 {

typedef std::vector<unsigned int> AESampleRateList;
typedef std::vector<enum AEDataFormat> AEDataFormatList;
typedef std::vector<CAEStreamInfo::DataType> AEDataTypeList;

enum AEDeviceType
{
  AE_DEVTYPE_PCM,
  AE_DEVTYPE_IEC958,
  AE_DEVTYPE_HDMI,
  AE_DEVTYPE_DP
};

class CAEDeviceInfo
{
public:
  std::string m_deviceName;
  std::string m_displayName;
  std::string m_displayNameExtra;
  enum AEDeviceType m_deviceType;
  CAEChannelInfo m_channels;
  AESampleRateList m_sampleRates;
  AEDataFormatList m_dataFormats;
  AEDataTypeList m_streamTypes;

  bool m_wantsIECPassthrough;
  bool m_onlyPassthrough{false};
  bool m_onlyPCM{false};

  operator std::string();
  static std::string DeviceTypeToString(enum AEDeviceType deviceType);
  std::string GetFriendlyName() const;
  std::string ToDeviceString(const std::string& driver) const;
};

typedef std::vector<CAEDeviceInfo> AEDeviceInfoList;

}  // namespace androidx_media3
