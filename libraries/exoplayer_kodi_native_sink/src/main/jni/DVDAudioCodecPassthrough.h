/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Reduced the dependency surface to the native passthrough module.
 *  - Removed Kodi process/codec base classes and exposed a self-contained codec wrapper.
 *  - Wrapped the declarations in the androidx_media3 namespace.
 */

#pragma once

#include "AEAudioFormat.h"
#include "AEStreamInfo.h"
#include "PackerMAT.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace androidx_media3 {

constexpr int64_t DVD_TIME_BASE = 1000000;
constexpr double DVD_MSEC_TO_TIME(double x) { return x * DVD_TIME_BASE / 1000.0; }
constexpr double DVD_NOPTS_VALUE = static_cast<double>(0xFFF0000000000000ULL);

struct DemuxPacket
{
  const uint8_t* pData = nullptr;
  int iSize = 0;
  double pts = DVD_NOPTS_VALUE;
};

struct DVDAudioFrame
{
  uint8_t* data = nullptr;
  double pts = DVD_NOPTS_VALUE;
  bool hasTimestamp = false;
  double duration = 0;
  unsigned int nb_frames = 0;
  unsigned int framesOut = 0;
  unsigned int framesize = 1;
  unsigned int planes = 1;
  AEAudioFormat format;
  int bits_per_sample = 8;
  bool passthrough = true;
};

class CDVDAudioCodecPassthrough
{
public:
  CDVDAudioCodecPassthrough(bool device_is_raw, CAEStreamInfo::DataType stream_type);
  ~CDVDAudioCodecPassthrough();

  bool Open();
  void Dispose();
  bool AddData(const DemuxPacket& packet);
  bool GetData(DVDAudioFrame& frame);
  void Reset();
  AEAudioFormat GetFormat() const { return m_format; }
  std::string GetName() const { return m_codecName; }
  int GetBufferSize() const;

private:
  int GetData(uint8_t** dst);
  unsigned int PackTrueHD();

  CAEStreamParser m_parser;
  uint8_t* m_buffer = nullptr;
  unsigned int m_bufferSize = 0;
  unsigned int m_dataSize = 0;
  AEAudioFormat m_format;
  uint8_t* m_backlogBuffer = nullptr;
  unsigned int m_backlogBufferSize = 0;
  unsigned int m_backlogSize = 0;
  double m_currentPts = DVD_NOPTS_VALUE;
  double m_nextPts = DVD_NOPTS_VALUE;
  std::string m_codecName;

  std::unique_ptr<CPackerMAT> m_packerMAT;
  std::vector<uint8_t> m_trueHDBuffer;
  unsigned int m_trueHDoffset = 0;
  unsigned int m_trueHDframes = 0;
  bool m_deviceIsRAW{false};
};

}  // namespace androidx_media3
