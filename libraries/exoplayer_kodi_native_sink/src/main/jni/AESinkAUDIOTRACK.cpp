/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Reduced the implementation to capability enumeration semantics for the native selector.
 *  - Replaced live AudioTrack probing with Media3-provided capability snapshots.
 *  - Wrapped the definitions in the androidx_media3 namespace.
 */

#include "AESinkAUDIOTRACK.h"

#include "KodiCapabilitySelector.h"

namespace androidx_media3 {
namespace {

void AddSampleRate(CAEDeviceInfo& info, unsigned int sample_rate)
{
  for (unsigned int existing : info.m_sampleRates)
  {
    if (existing == sample_rate)
      return;
  }
  info.m_sampleRates.push_back(sample_rate);
}

void AddStreamType(CAEDeviceInfo& info, CAEStreamInfo::DataType type)
{
  for (CAEStreamInfo::DataType existing : info.m_streamTypes)
  {
    if (existing == type)
      return;
  }
  info.m_streamTypes.push_back(type);
}

void PopulatePcmFormats(CAEDeviceInfo& info)
{
  info.m_dataFormats.push_back(AE_FMT_S16LE);
  info.m_dataFormats.push_back(AE_FMT_FLOAT);
}

void AddRawSupport(CAEDeviceInfo& info,
                   const ProbeResult& probe,
                   CAEStreamInfo::DataType type,
                   unsigned int sample_rate)
{
  if (!probe.supported)
    return;
  AddStreamType(info, type);
  AddSampleRate(info, sample_rate);
}

void AddIecSupport(CAEDeviceInfo& info,
                   const ProbeResult& probe,
                   CAEStreamInfo::DataType type,
                   unsigned int sample_rate)
{
  if (!probe.supported)
    return;
  AddStreamType(info, type);
  AddSampleRate(info, sample_rate);
}

}  // namespace

void CAESinkAUDIOTRACK::EnumerateDevicesEx(const CapabilitySnapshot& snapshot,
                                           AEDeviceInfoList& list)
{
  list.clear();

  CAEDeviceInfo raw_device;
  raw_device.m_deviceName = kRawDeviceName;
  raw_device.m_displayName = kRawDeviceName;
  raw_device.m_deviceType = snapshot.max_channel_count > 2 ? AE_DEVTYPE_HDMI : AE_DEVTYPE_PCM;
  raw_device.m_wantsIECPassthrough = false;
  raw_device.m_channels += snapshot.max_channel_count > 6 ? AE_CH_BL : AE_CH_FL;
  PopulatePcmFormats(raw_device);
  AddSampleRate(raw_device, 48000);
  AddSampleRate(raw_device, 96000);
  AddSampleRate(raw_device, 192000);
  AddRawSupport(raw_device, snapshot.ac3, CAEStreamInfo::STREAM_TYPE_AC3, 48000);
  AddRawSupport(raw_device, snapshot.eac3, CAEStreamInfo::STREAM_TYPE_EAC3, 48000);
  AddRawSupport(raw_device, snapshot.dts, CAEStreamInfo::STREAM_TYPE_DTSHD_CORE, 48000);
  if (!raw_device.m_streamTypes.empty() || !raw_device.m_dataFormats.empty())
    list.push_back(raw_device);

  CAEDeviceInfo iec_device;
  iec_device.m_deviceName = kIecDeviceName;
  iec_device.m_displayName = kIecDeviceName;
  iec_device.m_deviceType = AE_DEVTYPE_HDMI;
  iec_device.m_wantsIECPassthrough = true;
  iec_device.m_channels += AE_CH_FL;
  iec_device.m_channels += AE_CH_FR;
  PopulatePcmFormats(iec_device);
  AddSampleRate(iec_device, 48000);
  AddIecSupport(iec_device, snapshot.ac3, CAEStreamInfo::STREAM_TYPE_AC3, 48000);
  AddIecSupport(iec_device, snapshot.eac3, CAEStreamInfo::STREAM_TYPE_EAC3, 192000);
  AddIecSupport(iec_device, snapshot.dts, CAEStreamInfo::STREAM_TYPE_DTSHD_CORE, 48000);
  AddIecSupport(iec_device, snapshot.dtshd, CAEStreamInfo::STREAM_TYPE_DTSHD, 192000);
  AddIecSupport(iec_device, snapshot.dtshd, CAEStreamInfo::STREAM_TYPE_DTSHD_MA, 192000);
  AddIecSupport(iec_device, snapshot.truehd, CAEStreamInfo::STREAM_TYPE_TRUEHD, 192000);
  if (!iec_device.m_streamTypes.empty())
    list.push_back(iec_device);
}

}  // namespace androidx_media3
