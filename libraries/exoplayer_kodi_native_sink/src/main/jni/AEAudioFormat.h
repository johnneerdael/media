/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Wrapped the declarations in the androidx_media3 namespace.
 */

#pragma once

#include "AEChannelInfo.h"
#include "AEStreamInfo.h"

#define AE_IS_PLANAR(x) ((x) >= AE_FMT_U8P && (x) <= AE_FMT_FLOATP)

namespace androidx_media3 {

struct AEAudioFormat
{
  enum AEDataFormat m_dataFormat;
  unsigned int m_sampleRate;
  CAEChannelInfo m_channelLayout;
  unsigned int m_frames;
  unsigned int m_frameSize;
  CAEStreamInfo m_streamInfo;

  AEAudioFormat()
  {
    m_dataFormat = AE_FMT_INVALID;
    m_sampleRate = 0;
    m_frames = 0;
    m_frameSize = 0;
  }

  bool operator==(const AEAudioFormat& fmt) const
  {
    return m_dataFormat == fmt.m_dataFormat && m_sampleRate == fmt.m_sampleRate &&
           m_channelLayout == fmt.m_channelLayout && m_frames == fmt.m_frames &&
           m_frameSize == fmt.m_frameSize && m_streamInfo == fmt.m_streamInfo;
  }
};

}  // namespace androidx_media3
