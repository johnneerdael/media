/*
 *  Copyright (C) 2013-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class CSetting;

struct IntegerSettingOption
{
  IntegerSettingOption(const std::string& _label, int _value) : label(_label), value(_value) {}

  std::string label;
  std::string label2;
  int value = 0;
};

struct StringSettingOption
{
  StringSettingOption(const std::string& _label, const std::string& _value)
    : label(_label), value(_value)
  {
  }

  std::string label;
  std::string label2;
  std::string value;
};

using IntegerSettingOptions = std::vector<IntegerSettingOption>;
using StringSettingOptions = std::vector<StringSettingOption>;

using IntegerSettingOptionsFiller = std::function<void(
    const std::shared_ptr<const CSetting>& setting, IntegerSettingOptions& list, int& current)>;
using StringSettingOptionsFiller =
    std::function<void(const std::shared_ptr<const CSetting>& setting,
                       StringSettingOptions& list,
                       std::string& current)>;
