/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Removed Kodi logging dependencies.
 *  - Replaced FFmpeg integer/align helpers with local equivalents.
 *  - Added Reset() for Android session lifecycle integration.
 *  - Wrapped the class in the androidx_media3 namespace for JNI module isolation.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace androidx_media3 {

enum class MATDataType
{
  PADDING,
  DATA,
};

class CPackerMAT
{
public:
  CPackerMAT();
  ~CPackerMAT() = default;

  bool PackTrueHD(const uint8_t* data, int size);
  std::vector<uint8_t> GetOutputFrame();
  void Reset();

private:
  struct MATState
  {
    bool init;
    int ratebits;
    uint16_t prevFrametime;
    bool prevFrametimeValid;
    uint32_t matFramesize;
    uint32_t prevMatFramesize;
    uint32_t padding;
  };

  void WriteHeader();
  void WritePadding();
  void AppendData(const uint8_t* data, int size, MATDataType type);
  uint32_t GetCount() const { return m_bufferCount; }
  int FillDataBuffer(const uint8_t* data, int size, MATDataType type);
  void FlushPacket();

  MATState m_state{};
  uint32_t m_bufferCount{0};
  std::vector<uint8_t> m_buffer;
  std::deque<std::vector<uint8_t>> m_outputQueue;
};

}  // namespace androidx_media3
