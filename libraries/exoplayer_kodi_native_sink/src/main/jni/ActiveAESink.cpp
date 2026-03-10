/*
 *  Copyright (C) 2010-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Reduced the implementation to capability and IEC-packing semantics used by the native
 *    selector.
 *  - Removed Kodi thread, buffer, and live AudioTrack control logic.
 *  - Wrapped the definitions in the androidx_media3 namespace.
 */

#include "ActiveAESink.h"

#include "AESinkAUDIOTRACK.h"
#include "KodiCapabilitySelector.h"

#include <algorithm>

namespace androidx_media3 {

void CActiveAESink::EnumerateOutputDevices(const CapabilitySnapshot& snapshot)
{
  CAESinkAUDIOTRACK::EnumerateDevicesEx(snapshot, m_deviceInfoList);
}

bool CActiveAESink::HasPassthroughDevice() const
{
  for (const CAEDeviceInfo& info : m_deviceInfoList)
  {
    if (info.m_deviceType != AE_DEVTYPE_PCM && !info.m_streamTypes.empty())
      return true;
  }
  return false;
}

AEDeviceType CActiveAESink::GetDeviceType(const std::string& device) const
{
  for (const CAEDeviceInfo& info : m_deviceInfoList)
  {
    if (info.m_deviceName == device)
      return info.m_deviceType;
  }
  return AE_DEVTYPE_PCM;
}

bool CActiveAESink::SupportsFormat(const std::string& device, AEAudioFormat& format) const
{
  for (const CAEDeviceInfo& info : m_deviceInfoList)
  {
    if (info.m_deviceName != device)
      continue;

    const bool is_raw = format.m_dataFormat == AE_FMT_RAW;
    bool format_exists = false;
    unsigned int sample_rate = format.m_sampleRate;

    if (is_raw && info.m_wantsIECPassthrough)
    {
      switch (format.m_streamInfo.m_type)
      {
        case CAEStreamInfo::STREAM_TYPE_EAC3:
          sample_rate = 192000;
          break;
        case CAEStreamInfo::STREAM_TYPE_TRUEHD:
          sample_rate = 192000;
          break;
        case CAEStreamInfo::STREAM_TYPE_DTSHD:
        case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
          sample_rate = 192000;
          break;
        default:
          break;
      }
      format_exists =
          std::find(info.m_streamTypes.begin(), info.m_streamTypes.end(), format.m_streamInfo.m_type) !=
          info.m_streamTypes.end();
    }
    else if (is_raw && !info.m_wantsIECPassthrough)
    {
      sample_rate = 48000;
      format_exists =
          std::find(info.m_streamTypes.begin(), info.m_streamTypes.end(), format.m_streamInfo.m_type) !=
          info.m_streamTypes.end();
    }
    else
    {
      format_exists =
          std::find(info.m_dataFormats.begin(), info.m_dataFormats.end(), format.m_dataFormat) !=
          info.m_dataFormats.end();
    }

    if (!format_exists)
      return false;

    return std::find(info.m_sampleRates.begin(), info.m_sampleRates.end(), sample_rate) !=
           info.m_sampleRates.end();
  }

  return false;
}

bool CActiveAESink::FindSupportingPassthroughDevice(const std::string& preferred_device,
                                                    AEAudioFormat& format,
                                                    const CAEDeviceInfo** selected_device) const
{
  if (selected_device != nullptr)
    *selected_device = nullptr;

  auto try_device = [&](const CAEDeviceInfo& info) -> bool {
    AEAudioFormat probe_format = format;
    if (!SupportsFormat(info.m_deviceName, probe_format))
      return false;
    if (selected_device != nullptr)
      *selected_device = &info;
    return true;
  };

  for (const CAEDeviceInfo& info : m_deviceInfoList)
  {
    if (info.m_deviceName == preferred_device && try_device(info))
      return true;
  }

  for (const CAEDeviceInfo& info : m_deviceInfoList)
  {
    if (info.m_deviceName != preferred_device && try_device(info))
      return true;
  }

  return false;
}

bool CActiveAESink::NeedIecPack(const std::string& device) const
{
  for (const CAEDeviceInfo& info : m_deviceInfoList)
  {
    if (info.m_deviceName == device)
      return info.m_wantsIECPassthrough;
  }
  return true;
}

}  // namespace androidx_media3
