/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Wrapped the declarations in the androidx_media3 namespace.
 *  - Trimmed the interface to the subset required by AEBitstreamPacker.
 */

#pragma once

#include "AEChannelData.h"

#include <string>

namespace androidx_media3 {

class CAEChannelInfo
{
public:
  CAEChannelInfo();
  ~CAEChannelInfo() = default;
  CAEChannelInfo(const CAEChannelInfo&) = default;
  CAEChannelInfo& operator=(const CAEChannelInfo&) = default;

  bool operator==(const CAEChannelInfo& rhs) const;
  bool operator!=(const CAEChannelInfo& rhs) const;
  CAEChannelInfo& operator+=(const AEChannel& rhs);
  operator std::string() const;
  void Reset();
  inline unsigned int Count() const { return m_channelCount; }

private:
  unsigned int m_channelCount;
  AEChannel m_channels[AE_CH_MAX];
};

}  // namespace androidx_media3
