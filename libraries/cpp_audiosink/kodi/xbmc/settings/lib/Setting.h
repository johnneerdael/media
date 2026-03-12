/*
 *  Copyright (C) 2013-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <memory>
#include <string>

class CSetting;
using SettingPtr = std::shared_ptr<CSetting>;
using SettingConstPtr = std::shared_ptr<const CSetting>;

class CSetting
{
public:
  explicit CSetting(std::string id = {}) : m_id(std::move(id)) {}
  virtual ~CSetting() = default;

  const std::string& GetId() const { return m_id; }

private:
  std::string m_id;
};

class CSettingString : public CSetting
{
public:
  CSettingString(std::string id = {}, std::string value = {})
    : CSetting(std::move(id)), m_value(std::move(value))
  {
  }

  const std::string& GetValue() const { return m_value; }
  void SetValue(std::string value) { m_value = std::move(value); }

private:
  std::string m_value;
};
