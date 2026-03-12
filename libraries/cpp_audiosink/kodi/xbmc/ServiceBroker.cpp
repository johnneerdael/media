/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ServiceBroker.h"

#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/DataCacheCore.h"
#include "resources/ResourcesComponent.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

#include <memory>

CServiceBroker::CServiceBroker()
  : m_logging(std::make_unique<CLog>()),
    m_pSettingsComponent(std::make_shared<CSettingsComponent>()),
    m_pResourcesComponent(std::make_unique<CResourcesComponent>()),
    m_pDataCacheCore(std::make_unique<CDataCacheCore>())
{
}

CServiceBroker::~CServiceBroker() = default;

CLog& CServiceBroker::GetLogging()
{
  return *(g_serviceBroker.m_logging);
}

void CServiceBroker::RegisterSettingsComponent(const std::shared_ptr<CSettingsComponent>& settings)
{
  g_serviceBroker.m_pSettingsComponent = settings;
}

void CServiceBroker::UnregisterSettingsComponent()
{
  g_serviceBroker.m_pSettingsComponent.reset();
}

std::shared_ptr<CSettingsComponent> CServiceBroker::GetSettingsComponent()
{
  return g_serviceBroker.m_pSettingsComponent;
}

void CServiceBroker::RegisterAE(IAE* ae)
{
  g_serviceBroker.m_pActiveAE = ae;
}

void CServiceBroker::UnregisterAE()
{
  g_serviceBroker.m_pActiveAE = nullptr;
}

IAE* CServiceBroker::GetActiveAE()
{
  return g_serviceBroker.m_pActiveAE;
}

CDataCacheCore& CServiceBroker::GetDataCacheCore()
{
  return *(g_serviceBroker.m_pDataCacheCore);
}

void CServiceBroker::RegisterResourcesComponent(std::unique_ptr<CResourcesComponent> resources)
{
  g_serviceBroker.m_pResourcesComponent = std::move(resources);
}

void CServiceBroker::UnregisterResourcesComponent()
{
  g_serviceBroker.m_pResourcesComponent.reset();
}

CResourcesComponent& CServiceBroker::GetResourcesComponent()
{
  return *(g_serviceBroker.m_pResourcesComponent);
}

void CServiceBroker::RegisterWinSystem(CWinSystemBase* winsystem)
{
  g_serviceBroker.m_pWinSystem = winsystem;
}

void CServiceBroker::UnregisterWinSystem()
{
  g_serviceBroker.m_pWinSystem = nullptr;
}

CWinSystemBase* CServiceBroker::GetWinSystem()
{
  return g_serviceBroker.m_pWinSystem;
}

namespace
{

std::shared_ptr<CAdvancedSettings> g_advanced_settings = std::make_shared<CAdvancedSettings>();
std::shared_ptr<CSettings> g_audio_settings = std::make_shared<CSettings>();

} // namespace

std::shared_ptr<CAdvancedSettings> CSettingsComponent::GetAdvancedSettings() const
{
  return g_advanced_settings;
}

std::shared_ptr<CSettings> CSettingsComponent::GetSettings() const
{
  return g_audio_settings;
}
