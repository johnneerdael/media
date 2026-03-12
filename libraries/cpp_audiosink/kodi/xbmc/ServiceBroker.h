/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/GlobalsHandling.h"

#include <memory>

class CDataCacheCore;
class IAE;
class CResourcesComponent;
class CSettingsComponent;
class CWinSystemBase;
class CLog;

class CServiceBroker
{
public:
  CServiceBroker();
  ~CServiceBroker();

  static CLog& GetLogging();

  static void RegisterSettingsComponent(const std::shared_ptr<CSettingsComponent>& settings);
  static void UnregisterSettingsComponent();
  static std::shared_ptr<CSettingsComponent> GetSettingsComponent();

  static void RegisterAE(IAE* ae);
  static void UnregisterAE();
  static IAE* GetActiveAE();

  static CDataCacheCore& GetDataCacheCore();

  static void RegisterResourcesComponent(std::unique_ptr<CResourcesComponent> resources);
  static void UnregisterResourcesComponent();
  static CResourcesComponent& GetResourcesComponent();

  static void RegisterWinSystem(CWinSystemBase* winsystem);
  static void UnregisterWinSystem();
  static CWinSystemBase* GetWinSystem();

private:
  std::unique_ptr<CLog> m_logging;
  std::unique_ptr<CResourcesComponent> m_pResourcesComponent;
  std::shared_ptr<CSettingsComponent> m_pSettingsComponent;
  std::unique_ptr<CDataCacheCore> m_pDataCacheCore;
  CWinSystemBase* m_pWinSystem = nullptr;
  IAE* m_pActiveAE = nullptr;
};

XBMC_GLOBAL_REF(CServiceBroker, g_serviceBroker);
#define g_serviceBroker XBMC_GLOBAL_USE(CServiceBroker)
