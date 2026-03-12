/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Utils/AEAudioFormat.h"
#include "settings/lib/ISettingCallback.h"
#include "threads/CriticalSection.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class CSetting;
class CAEStreamInfo;
struct IntegerSettingOption;
struct StringSettingOption;

namespace ActiveAE
{
class CActiveAE;

struct CActiveAEMediaSettings
{
  int mimeKind = 0;
  int sampleRate = 0;
  int channelCount = 0;
  int pcmEncoding = 0;
  std::string preferredDevice;
  float volume = 1.0f;
  bool superviseAudioDelay = false;
  bool iecVerboseLogging = false;
};

class CActiveAESettings : public ISettingCallback
{
public:
  CActiveAESettings(CActiveAE &ae);
  ~CActiveAESettings() override;

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

  static void SettingOptionsAudioDevicesFiller(const std::shared_ptr<const CSetting>& setting,
                                               std::vector<StringSettingOption>& list,
                                               std::string& current);
  static void SettingOptionsAudioDevicesPassthroughFiller(
      const std::shared_ptr<const CSetting>& setting,
      std::vector<StringSettingOption>& list,
      std::string& current);
  static void SettingOptionsAudioQualityLevelsFiller(const std::shared_ptr<const CSetting>& setting,
                                                     std::vector<IntegerSettingOption>& list,
                                                     int& current);
  static void SettingOptionsAudioStreamsilenceFiller(const std::shared_ptr<const CSetting>& setting,
                                                     std::vector<IntegerSettingOption>& list,
                                                     int& current);
  static bool IsSettingVisible(const std::string& condition,
                               const std::string& value,
                               const std::shared_ptr<const CSetting>& setting);
  static int MimeKindFromMediaMimeType(const std::string& sampleMimeType);
  static AEAudioFormat BuildFormatForMediaSource(const CActiveAEMediaSettings& settings);
  static std::string SelectDeviceForMediaSource(const CActiveAEMediaSettings& settings);
  static std::string DescribeMediaSourceConfiguration(const CActiveAEMediaSettings& settings,
                                                      const AEAudioFormat& requestedFormat);
  static void EnsureMediaSinkRegistered();
  static void ApplyForMediaSource(const CActiveAEMediaSettings& settings);

protected:
  static void SettingOptionsAudioDevicesFillerGeneral(
      const std::shared_ptr<const CSetting>& setting,
      std::vector<StringSettingOption>& list,
      std::string& current,
      bool passthrough);

  CActiveAE &m_audioEngine;
  CCriticalSection m_cs;
  static CActiveAESettings* m_instance;
};
};
