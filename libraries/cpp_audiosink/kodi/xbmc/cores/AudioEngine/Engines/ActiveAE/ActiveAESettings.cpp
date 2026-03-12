/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  Copyright (C) 2026 Nuvio
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */


#include "ActiveAESettings.h"

#include "ServiceBroker.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAE.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Sinks/AESinkAUDIOTRACK.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/SettingDefinitions.h"
#include "settings/lib/SettingsManager.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "utils/StringUtils.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>

namespace ActiveAE
{

namespace
{
constexpr int kMimeKindAc3 = 1;
constexpr int kMimeKindEAc3 = 2;
constexpr int kMimeKindDts = 3;
constexpr int kMimeKindDtsHd = 4;
constexpr int kMimeKindDtsUhd = 5;
constexpr int kMimeKindTrueHd = 6;
constexpr int kMimeKindPcm = 7;
constexpr char kDefaultIecDevice[] = "AUDIOTRACK:AudioTrack (IEC)";
std::atomic_bool g_mediaSourceExternalClockMaster{false};

CAEStreamInfo::DataType MimeKindToStreamType(int mimeKind)
{
  switch (mimeKind)
  {
    case kMimeKindAc3:
      return CAEStreamInfo::STREAM_TYPE_AC3;
    case kMimeKindEAc3:
      return CAEStreamInfo::STREAM_TYPE_EAC3;
    case kMimeKindDts:
      return CAEStreamInfo::STREAM_TYPE_DTSHD_CORE;
    case kMimeKindDtsHd:
    case kMimeKindDtsUhd:
      return CAEStreamInfo::STREAM_TYPE_DTSHD;
    case kMimeKindTrueHd:
      return CAEStreamInfo::STREAM_TYPE_TRUEHD;
    default:
      return CAEStreamInfo::STREAM_TYPE_NULL;
  }
}

AEDataFormat PcmEncodingToDataFormat(int pcmEncoding)
{
  switch (pcmEncoding)
  {
    case 2:
      return AE_FMT_S16NE;
    case 4:
      return AE_FMT_FLOAT;
    case 21:
      return AE_FMT_S24NE3;
    case 22:
      return AE_FMT_S32NE;
    case 3:
      return AE_FMT_U8;
    default:
      return AE_FMT_S16NE;
  }
}

int ChannelLayoutSettingForCount(int channelCount)
{
  switch (channelCount)
  {
    case 8:
    case 7:
      return AE_CH_LAYOUT_7_1;
    case 6:
      return AE_CH_LAYOUT_5_1;
    case 5:
      return AE_CH_LAYOUT_5_0;
    case 4:
      return AE_CH_LAYOUT_4_0;
    case 3:
      return AE_CH_LAYOUT_2_1;
    case 2:
    case 1:
    default:
      return AE_CH_LAYOUT_2_0;
  }
}
} // namespace

CActiveAESettings* CActiveAESettings::m_instance = nullptr;

CActiveAESettings::CActiveAESettings(CActiveAE &ae) : m_audioEngine(ae)
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  std::unique_lock lock(m_cs);
  m_instance = this;

  settings->GetSettingsManager()->RegisterCallback(
      this, {CSettings::SETTING_AUDIOOUTPUT_CONFIG,
             CSettings::SETTING_AUDIOOUTPUT_SAMPLERATE,
             CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH,
             CSettings::SETTING_AUDIOOUTPUT_CHANNELS,
             CSettings::SETTING_AUDIOOUTPUT_PROCESSQUALITY,
             CSettings::SETTING_AUDIOOUTPUT_ATEMPOTHRESHOLD,
             CSettings::SETTING_AUDIOOUTPUT_GUISOUNDMODE,
             CSettings::SETTING_AUDIOOUTPUT_STEREOUPMIX,
             CSettings::SETTING_AUDIOOUTPUT_AC3PASSTHROUGH,
             CSettings::SETTING_AUDIOOUTPUT_AC3TRANSCODE,
             CSettings::SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH,
             CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH,
             CSettings::SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH,
             CSettings::SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH,
             CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE,
             CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE,
             CSettings::SETTING_AUDIOOUTPUT_STREAMSILENCE,
             CSettings::SETTING_AUDIOOUTPUT_STREAMNOISE,
             CSettings::SETTING_AUDIOOUTPUT_MIXSUBLEVEL,
             CSettings::SETTING_AUDIOOUTPUT_MAINTAINORIGINALVOLUME,
             CSettings::SETTING_AUDIOOUTPUT_DTSHDCOREFALLBACK});

  settings->GetSettingsManager()->RegisterSettingOptionsFiller("aequalitylevels", SettingOptionsAudioQualityLevelsFiller);
  settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiodevices", SettingOptionsAudioDevicesFiller);
  settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiodevicespassthrough", SettingOptionsAudioDevicesPassthroughFiller);
  settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiostreamsilence", SettingOptionsAudioStreamsilenceFiller);
}

CActiveAESettings::~CActiveAESettings()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  std::unique_lock lock(m_cs);
  settings->GetSettingsManager()->UnregisterSettingOptionsFiller("aequalitylevels");
  settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiodevices");
  settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiodevicespassthrough");
  settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiostreamsilence");
  settings->GetSettingsManager()->UnregisterCallback(this);
  m_instance = nullptr;
}

void CActiveAESettings::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  std::unique_lock lock(m_cs);
  m_instance->m_audioEngine.OnSettingsChange();
}

void CActiveAESettings::SettingOptionsAudioDevicesFiller(const SettingConstPtr& setting,
                                                         std::vector<StringSettingOption>& list,
                                                         std::string& current)
{
  SettingOptionsAudioDevicesFillerGeneral(setting, list, current, false);
}

void CActiveAESettings::SettingOptionsAudioDevicesPassthroughFiller(
    const SettingConstPtr& setting, std::vector<StringSettingOption>& list, std::string& current)
{
  SettingOptionsAudioDevicesFillerGeneral(setting, list, current, true);
}

void CActiveAESettings::SettingOptionsAudioQualityLevelsFiller(
    const SettingConstPtr& /*setting*/, std::vector<IntegerSettingOption>& list, int& /*current*/)
{
  std::unique_lock lock(m_instance->m_cs);

  if (m_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_LOW))
    list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(13506),
                      AE_QUALITY_LOW);
  if (m_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_MID))
    list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(13507),
                      AE_QUALITY_MID);
  if (m_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_HIGH))
    list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(13508),
                      AE_QUALITY_HIGH);
  if (m_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_REALLYHIGH))
    list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(13509),
                      AE_QUALITY_REALLYHIGH);
  if (m_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_GPU))
    list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(38010),
                      AE_QUALITY_GPU);
}

void CActiveAESettings::SettingOptionsAudioStreamsilenceFiller(
    const SettingConstPtr& /*setting*/, std::vector<IntegerSettingOption>& list, int& /*current*/)
{
  std::unique_lock lock(m_instance->m_cs);

  list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(20422),
                    XbmcThreads::EndTime<std::chrono::minutes>::Max().count());
  list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(13551), 0);

  if (m_instance->m_audioEngine.SupportsSilenceTimeout())
  {
    list.emplace_back(
        StringUtils::Format(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(13554),
                            1),
        1);
    for (int i = 2; i <= 10; i++)
    {
      list.emplace_back(
          StringUtils::Format(
              CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(13555), i),
          i);
    }
  }
}

bool CActiveAESettings::IsSettingVisible(const std::string& condition,
                                         const std::string& value,
                                         const SettingConstPtr& setting)
{
  if (setting == NULL || value.empty())
    return false;

  std::unique_lock lock(m_instance->m_cs);
  if (!m_instance)
    return false;

  return m_instance->m_audioEngine.IsSettingVisible(value);
}

int CActiveAESettings::MimeKindFromMediaMimeType(const std::string& sampleMimeType)
{
  if (sampleMimeType == "audio/ac3")
    return kMimeKindAc3;
  if (sampleMimeType == "audio/eac3" || sampleMimeType == "audio/eac3-joc")
    return kMimeKindEAc3;
  if (sampleMimeType == "audio/vnd.dts")
    return kMimeKindDts;
  if (sampleMimeType == "audio/vnd.dts.hd")
    return kMimeKindDtsHd;
  if (sampleMimeType == "audio/vnd.dts.uhd;profile=p2")
    return kMimeKindDtsUhd;
  if (sampleMimeType == "audio/true-hd")
    return kMimeKindTrueHd;
  if (sampleMimeType == "audio/raw")
    return kMimeKindPcm;
  return 0;
}

AEAudioFormat CActiveAESettings::BuildFormatForMediaSource(const CActiveAEMediaSettings& settings)
{
  AEAudioFormat format;
  format.m_sampleRate = static_cast<unsigned int>(settings.sampleRate);

  if (settings.mimeKind == kMimeKindPcm)
  {
    format.m_dataFormat = PcmEncodingToDataFormat(settings.pcmEncoding);
    format.m_channelLayout =
        CAEChannelInfo(static_cast<AEStdChLayout>(ChannelLayoutSettingForCount(settings.channelCount)));
    format.m_frameSize =
        format.m_channelLayout.Count() * (CAEUtil::DataFormatToBits(format.m_dataFormat) / 8);
  }
  else
  {
    format.m_dataFormat = AE_FMT_RAW;
    format.m_channelLayout = AE_CH_LAYOUT_2_0;
    format.m_frameSize = 1;
    format.m_streamInfo.m_type = MimeKindToStreamType(settings.mimeKind);
    format.m_streamInfo.m_sampleRate = static_cast<unsigned int>(settings.sampleRate);
    format.m_streamInfo.m_channels = static_cast<unsigned int>(std::max(2, settings.channelCount));
    format.m_streamInfo.m_dataIsLE = true;
    format.m_streamInfo.m_repeat = 1;
    format.m_streamInfo.m_frameSize = 1;
  }

  return format;
}

std::string CActiveAESettings::SelectDeviceForMediaSource(const CActiveAEMediaSettings& settings)
{
  if (!settings.preferredDevice.empty())
    return settings.preferredDevice;
  return kDefaultIecDevice;
}

std::string CActiveAESettings::DescribeMediaSourceConfiguration(
    const CActiveAEMediaSettings& settings, const AEAudioFormat& requestedFormat)
{
  const bool passthrough = requestedFormat.m_dataFormat == AE_FMT_RAW;
  std::ostringstream stream;
  stream << "KodiActiveAEEngine::Configure passthrough=" << (passthrough ? "true" : "false")
         << " mimeKind=" << settings.mimeKind << " streamType="
         << (passthrough ? CAEUtil::StreamTypeToStr(requestedFormat.m_streamInfo.m_type) : "PCM")
         << " requestedDevice='" << SelectDeviceForMediaSource(settings) << "'";
  return stream.str();
}

void CActiveAESettings::EnsureMediaSinkRegistered()
{
  static std::once_flag sinkRegistered;
  std::call_once(sinkRegistered, []() { CAESinkAUDIOTRACK::Register(); });
}

void CActiveAESettings::ApplyForMediaSource(const CActiveAEMediaSettings& mediaSettings)
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const bool passthrough = mediaSettings.mimeKind != kMimeKindPcm;
  const std::string device = SelectDeviceForMediaSource(mediaSettings);
  g_mediaSourceExternalClockMaster.store(true);

  CServiceBroker::GetLogging().SetIecVerboseLoggingEnabled(mediaSettings.iecVerboseLogging);
  CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_superviseAudioDelay =
      mediaSettings.superviseAudioDelay;

  settings->SetString(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE, device);
  settings->SetString(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE, device);
  settings->SetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG, AE_CONFIG_AUTO);
  settings->SetInt(CSettings::SETTING_AUDIOOUTPUT_SAMPLERATE, mediaSettings.sampleRate);
  settings->SetInt(CSettings::SETTING_AUDIOOUTPUT_CHANNELS,
                   ChannelLayoutSettingForCount(mediaSettings.channelCount));
  settings->SetInt(CSettings::SETTING_AUDIOOUTPUT_PROCESSQUALITY, AE_QUALITY_HIGH);
  settings->SetInt(CSettings::SETTING_AUDIOOUTPUT_ATEMPOTHRESHOLD, 80);
  settings->SetInt(CSettings::SETTING_AUDIOOUTPUT_GUISOUNDMODE, AE_SOUND_OFF);
  settings->SetInt(CSettings::SETTING_AUDIOOUTPUT_STREAMSILENCE, 1);
  settings->SetInt(CSettings::SETTING_AUDIOOUTPUT_MIXSUBLEVEL, 100);

  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH, passthrough);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_STEREOUPMIX, false);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_AC3PASSTHROUGH, true);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_AC3TRANSCODE, false);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH, true);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH, true);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH, true);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH, true);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_STREAMNOISE, true);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_MAINTAINORIGINALVOLUME, false);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_DTSHDCOREFALLBACK, false);
  settings->SetBool(CSettings::SETTING_AUDIOOUTPUT_LOWLATENCY, false);
}

bool CActiveAESettings::IsExternalClockMasterForMediaSource()
{
  return g_mediaSourceExternalClockMaster.load();
}

void CActiveAESettings::SettingOptionsAudioDevicesFillerGeneral(
    const SettingConstPtr& setting,
    std::vector<StringSettingOption>& list,
    std::string& current,
    bool passthrough)
{
  current = std::static_pointer_cast<const CSettingString>(setting)->GetValue();
  std::string firstDevice;

  std::unique_lock lock(m_instance->m_cs);

  bool foundValue = false;
  AEDeviceList sinkList;
  m_instance->m_audioEngine.EnumerateOutputDevices(sinkList, passthrough);
  if (sinkList.empty())
    list.emplace_back("Error - no devices found", "error");
  else
  {
    for (AEDeviceList::const_iterator sink = sinkList.begin(); sink != sinkList.end(); ++sink)
    {
      if (sink == sinkList.begin())
        firstDevice = sink->second;

      list.emplace_back(sink->first, sink->second);

      if (StringUtils::EqualsNoCase(current, sink->second))
        foundValue = true;
    }
  }

  if (!foundValue)
    current = firstDevice;
}
}
