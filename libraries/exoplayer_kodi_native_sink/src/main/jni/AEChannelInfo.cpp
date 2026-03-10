/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Wrapped the implementation in the androidx_media3 namespace.
 *  - Trimmed the implementation to the subset required by AEBitstreamPacker.
 */

#include "AEChannelInfo.h"

#include <sstream>

namespace androidx_media3 {

CAEChannelInfo::CAEChannelInfo()
{
  Reset();
}

bool CAEChannelInfo::operator==(const CAEChannelInfo& rhs) const
{
  if (m_channelCount != rhs.m_channelCount)
    return false;

  for (unsigned int i = 0; i < m_channelCount; ++i)
  {
    if (m_channels[i] != rhs.m_channels[i])
      return false;
  }

  return true;
}

bool CAEChannelInfo::operator!=(const CAEChannelInfo& rhs) const
{
  return !(*this == rhs);
}

CAEChannelInfo& CAEChannelInfo::operator+=(const AEChannel& rhs)
{
  if (rhs == AE_CH_NULL || m_channelCount >= AE_CH_MAX)
    return *this;

  m_channels[m_channelCount++] = rhs;
  return *this;
}

CAEChannelInfo::operator std::string() const
{
  if (m_channelCount == 0)
    return "NULL";

  std::ostringstream stream;
  for (unsigned int i = 0; i < m_channelCount; ++i)
  {
    if (i != 0)
      stream << ", ";
    stream << static_cast<int>(m_channels[i]);
  }
  return stream.str();
}

void CAEChannelInfo::Reset()
{
  m_channelCount = 0;
  for (unsigned int i = 0; i < AE_CH_MAX; ++i)
    m_channels[i] = AE_CH_NULL;
}

}  // namespace androidx_media3
