/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "settings/lib/Setting.h"
#include "settings/lib/SettingDefinitions.h"
#include "settings/lib/SettingsManager.h"

#include <memory>
#include <string>
#include <unordered_map>

class CSettings
{
public:
  static constexpr const char* SETTING_AUDIOOUTPUT_CONFIG = "audiooutput.config";
  static constexpr const char* SETTING_AUDIOOUTPUT_SAMPLERATE = "audiooutput.samplerate";
  static constexpr const char* SETTING_AUDIOOUTPUT_PASSTHROUGH = "audiooutput.passthrough";
  static constexpr const char* SETTING_AUDIOOUTPUT_CHANNELS = "audiooutput.channels";
  static constexpr const char* SETTING_AUDIOOUTPUT_PROCESSQUALITY = "audiooutput.processquality";
  static constexpr const char* SETTING_AUDIOOUTPUT_ATEMPOTHRESHOLD =
      "audiooutput.atempothreshold";
  static constexpr const char* SETTING_AUDIOOUTPUT_GUISOUNDMODE = "audiooutput.guisoundmode";
  static constexpr const char* SETTING_AUDIOOUTPUT_STEREOUPMIX = "audiooutput.stereoupmix";
  static constexpr const char* SETTING_AUDIOOUTPUT_AC3PASSTHROUGH =
      "audiooutput.ac3passthrough";
  static constexpr const char* SETTING_AUDIOOUTPUT_AC3TRANSCODE = "audiooutput.ac3transcode";
  static constexpr const char* SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH =
      "audiooutput.eac3passthrough";
  static constexpr const char* SETTING_AUDIOOUTPUT_DTSPASSTHROUGH =
      "audiooutput.dtspassthrough";
  static constexpr const char* SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH =
      "audiooutput.truehdpassthrough";
  static constexpr const char* SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH =
      "audiooutput.dtshdpassthrough";
  static constexpr const char* SETTING_AUDIOOUTPUT_AUDIODEVICE = "audiooutput.audiodevice";
  static constexpr const char* SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE =
      "audiooutput.passthroughdevice";
  static constexpr const char* SETTING_AUDIOOUTPUT_STREAMSILENCE =
      "audiooutput.streamsilence";
  static constexpr const char* SETTING_AUDIOOUTPUT_STREAMNOISE = "audiooutput.streamnoise";
  static constexpr const char* SETTING_AUDIOOUTPUT_MIXSUBLEVEL = "audiooutput.mixsublevel";
  static constexpr const char* SETTING_AUDIOOUTPUT_MAINTAINORIGINALVOLUME =
      "audiooutput.maintainoriginalvolume";
  static constexpr const char* SETTING_AUDIOOUTPUT_DTSHDCOREFALLBACK =
      "audiooutput.dtshdcorefallback";
  static constexpr const char* SETTING_AUDIOOUTPUT_LOWLATENCY = "audiooutput.lowlatency";

  CSettings()
    : m_settingsManager(std::make_shared<CSettingsManager>())
  {
  }

  std::string GetString(const std::string& id) const
  {
    auto it = m_stringSettings.find(id);
    return it == m_stringSettings.end() ? std::string() : it->second;
  }

  int GetInt(const std::string& id) const
  {
    auto it = m_intSettings.find(id);
    return it == m_intSettings.end() ? 0 : it->second;
  }

  bool GetBool(const std::string& id) const
  {
    auto it = m_boolSettings.find(id);
    return it != m_boolSettings.end() && it->second;
  }

  void SetString(const std::string& id, const std::string& value) { m_stringSettings[id] = value; }
  void SetInt(const std::string& id, int value) { m_intSettings[id] = value; }
  void SetBool(const std::string& id, bool value) { m_boolSettings[id] = value; }

  void Save() {}

  std::shared_ptr<CSettingsManager> GetSettingsManager() const { return m_settingsManager; }

private:
  std::unordered_map<std::string, std::string> m_stringSettings;
  std::unordered_map<std::string, int> m_intSettings;
  std::unordered_map<std::string, bool> m_boolSettings;
  std::shared_ptr<CSettingsManager> m_settingsManager;
};
