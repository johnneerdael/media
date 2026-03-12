/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "commons/ilog.h"

class CAdvancedSettings
{
public:
  bool m_AllowMultiChannelFloat = false;
  bool m_superviseAudioDelay = false;
  float m_limiterHold = 0.025f;
  float m_limiterRelease = 0.1f;
  int m_logLevel = LOG_LEVEL_DEBUG;
};
