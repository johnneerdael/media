/*
 *  Copyright (C) 2010-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Extracted the passthrough policy slice from ActiveAE.cpp into a standalone helper.
 *  - Reduced the dependency surface to the native passthrough selector path.
 *  - Wrapped the declarations in the androidx_media3 namespace.
 */

#pragma once

#include "AEAudioFormat.h"
#include "ActiveAESettings.h"
#include "ActiveAESink.h"

namespace androidx_media3 {

class CActiveAEPolicy
{
public:
  CActiveAEPolicy(const CActiveAESettings::AudioSettings& settings, const CActiveAESink& sink)
      : m_settings(settings), m_sink(sink)
  {
  }

  bool SupportsRaw(AEAudioFormat& format, const CAEDeviceInfo** selected_device) const;
  bool UsesDtsCoreFallback() const;

private:
  const CActiveAESettings::AudioSettings& m_settings;
  const CActiveAESink& m_sink;
};

}  // namespace androidx_media3
