/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <memory>

class CAdvancedSettings;
class CSettings;

class CSettingsComponent
{
public:
  std::shared_ptr<CAdvancedSettings> GetAdvancedSettings() const;
  std::shared_ptr<CSettings> GetSettings() const;
};
