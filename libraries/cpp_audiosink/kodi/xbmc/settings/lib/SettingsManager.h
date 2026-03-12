/*
 *  Copyright (C) 2013-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "ISettingCallback.h"
#include "SettingDefinitions.h"

#include <memory>
#include <set>
#include <string>

using SettingsContainer = std::set<std::string, std::less<>>;

class CSettingsManager
{
public:
  CSettingsManager() = default;
  ~CSettingsManager() = default;

  void RegisterCallback(ISettingCallback* /* callback */, const SettingsContainer& /* setting_list */) {}
  void UnregisterCallback(ISettingCallback* /* callback */) {}

  void RegisterSettingOptionsFiller(const std::string& /* id */,
                                    const IntegerSettingOptionsFiller& /* filler */)
  {
  }

  void RegisterSettingOptionsFiller(const std::string& /* id */,
                                    const StringSettingOptionsFiller& /* filler */)
  {
  }

  void UnregisterSettingOptionsFiller(const std::string& /* id */) {}
};
