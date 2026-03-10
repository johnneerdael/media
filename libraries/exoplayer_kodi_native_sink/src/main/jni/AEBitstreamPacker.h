/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Wrapped the declarations in the androidx_media3 namespace.
 *  - Added standard C++ includes needed for the JNI module build.
 */

#pragma once

#include "AEChannelInfo.h"
#include "AEPackIEC61937.h"

#include <cstdint>
#include <vector>

namespace androidx_media3 {

class CAEStreamInfo;

class CAEBitstreamPacker
{
public:
  CAEBitstreamPacker();
  ~CAEBitstreamPacker();

  void Pack(CAEStreamInfo& info, uint8_t* data, int size);
  bool PackPause(CAEStreamInfo& info, unsigned int millis, bool iecBursts);
  void Reset();
  uint8_t* GetBuffer();
  unsigned int GetSize() const;
  static unsigned int GetOutputRate(const CAEStreamInfo& info);
  static CAEChannelInfo GetOutputChannelMap(const CAEStreamInfo& info);

private:
  void PackDTSHD(CAEStreamInfo& info, uint8_t* data, int size);
  void PackEAC3(CAEStreamInfo& info, uint8_t* data, int size);

  std::vector<uint8_t> m_dtsHD;
  unsigned int m_dtsHDSize = 0;

  std::vector<uint8_t> m_eac3;
  unsigned int m_eac3Size = 0;
  unsigned int m_eac3FramesCount = 0;
  unsigned int m_eac3FramesPerBurst = 0;

  unsigned int m_dataSize = 0;
  uint8_t m_packedBuffer[MAX_IEC61937_PACKET];
  unsigned int m_pauseDuration = 0;
};

}  // namespace androidx_media3
