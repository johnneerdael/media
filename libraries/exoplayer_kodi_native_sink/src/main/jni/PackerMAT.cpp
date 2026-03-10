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

#include "PackerMAT.h"

#include <array>
#include <cassert>
#include <cstring>
#include <utility>

namespace androidx_media3 {
namespace
{
constexpr uint32_t FORMAT_MAJOR_SYNC = 0xf8726fba;

constexpr auto BURST_HEADER_SIZE = 8;
constexpr auto MAT_BUFFER_SIZE = 61440;
constexpr auto MAT_BUFFER_LIMIT = MAT_BUFFER_SIZE - 24;
constexpr auto MAT_POS_MIDDLE = 30708 + BURST_HEADER_SIZE;

constexpr std::array<uint8_t, 20> mat_start_code = {0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01,
                                                    0x01, 0x80, 0x00, 0x56, 0xA5, 0x3B, 0xF4,
                                                    0x81, 0x83, 0x49, 0x80, 0x77, 0xE0};

constexpr std::array<uint8_t, 12> mat_middle_code = {0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA,
                                                     0x82, 0x83, 0x49, 0x80, 0x77, 0xE0};

constexpr std::array<uint8_t, 24> mat_end_code = {0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x11,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

inline uint32_t ReadBigEndian32(const uint8_t* data)
{
  return ((data[0] & 0xFF) << 24) | ((data[1] & 0xFF) << 16) | ((data[2] & 0xFF) << 8) |
         (data[3] & 0xFF);
}

inline uint16_t ReadBigEndian16(const uint8_t* data)
{
  return static_cast<uint16_t>(((data[0] & 0xFF) << 8) | (data[1] & 0xFF));
}

inline uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

CPackerMAT::CPackerMAT()
{
  m_buffer.reserve(MAT_BUFFER_SIZE);
}

void CPackerMAT::Reset()
{
  m_state = {};
  m_state.init = true;
  m_buffer.clear();
  m_bufferCount = 0;
  m_outputQueue.clear();
}

bool CPackerMAT::PackTrueHD(const uint8_t* data, int size)
{
  if (size < 10)
    return false;

  if (ReadBigEndian32(data + 4) == FORMAT_MAJOR_SYNC)
  {
    m_state.ratebits = data[8] >> 4;
  }
  else if (!m_state.prevFrametimeValid)
  {
    return false;
  }

  const uint16_t frameTime = ReadBigEndian16(data + 2);
  uint32_t spaceSize = 0;

  if (m_state.prevFrametimeValid)
    spaceSize = static_cast<uint16_t>(frameTime - m_state.prevFrametime) *
                (64 >> (m_state.ratebits & 7));

  assert(!m_state.prevFrametimeValid || spaceSize >= m_state.prevMatFramesize);

  if (spaceSize < m_state.prevMatFramesize)
    spaceSize = AlignUp(m_state.prevMatFramesize, (64 >> (m_state.ratebits & 7)));

  m_state.padding += (spaceSize - m_state.prevMatFramesize);

  if (m_state.padding > MAT_BUFFER_SIZE * 5)
  {
    Reset();
    return false;
  }

  m_state.prevFrametime = frameTime;
  m_state.prevFrametimeValid = true;

  if (GetCount() == 0)
  {
    WriteHeader();

    if (m_state.init == false)
    {
      m_state.init = true;
      m_state.matFramesize = 0;
    }
  }

  while (m_state.padding > 0)
  {
    WritePadding();

    assert(m_state.padding == 0 || GetCount() == MAT_BUFFER_SIZE);

    if (GetCount() == MAT_BUFFER_SIZE)
    {
      FlushPacket();
      WriteHeader();
    }
  }

  int remaining = FillDataBuffer(data, size, MATDataType::DATA);

  if (remaining || GetCount() == MAT_BUFFER_SIZE)
  {
    FlushPacket();

    if (remaining)
    {
      WriteHeader();
      remaining = FillDataBuffer(data + (size - remaining), remaining, MATDataType::DATA);
      assert(remaining == 0);
    }
  }

  m_state.prevMatFramesize = m_state.matFramesize;
  m_state.matFramesize = 0;

  return !m_outputQueue.empty();
}

std::vector<uint8_t> CPackerMAT::GetOutputFrame()
{
  std::vector<uint8_t> buffer;

  if (m_outputQueue.empty())
    return {};

  buffer = std::move(m_outputQueue.front());
  m_outputQueue.pop_front();
  return buffer;
}

void CPackerMAT::WriteHeader()
{
  m_buffer.resize(MAT_BUFFER_SIZE);

  const size_t size = BURST_HEADER_SIZE + mat_start_code.size();

  std::memcpy(m_buffer.data() + BURST_HEADER_SIZE, mat_start_code.data(), mat_start_code.size());
  m_bufferCount = static_cast<uint32_t>(size);

  m_state.matFramesize += static_cast<uint32_t>(size);

  if (m_state.padding > 0)
  {
    if (m_state.padding > size)
    {
      m_state.padding -= static_cast<uint32_t>(size);
      m_state.matFramesize = 0;
    }
    else
    {
      m_state.matFramesize = static_cast<uint32_t>(size) - m_state.padding;
      m_state.padding = 0;
    }
  }
}

void CPackerMAT::WritePadding()
{
  if (m_state.padding == 0)
    return;

  const int remaining = FillDataBuffer(nullptr, static_cast<int>(m_state.padding), MATDataType::PADDING);

  if (remaining >= 0)
  {
    m_state.padding = static_cast<uint32_t>(remaining);
    m_state.matFramesize = 0;
  }
  else
  {
    m_state.padding = 0;
    m_state.matFramesize = static_cast<uint32_t>(-remaining);
  }
}

void CPackerMAT::AppendData(const uint8_t* data, int size, MATDataType type)
{
  if (type == MATDataType::DATA)
    std::memcpy(m_buffer.data() + m_bufferCount, data, size);

  m_state.matFramesize += static_cast<uint32_t>(size);
  m_bufferCount += static_cast<uint32_t>(size);
}

int CPackerMAT::FillDataBuffer(const uint8_t* data, int size, MATDataType type)
{
  if (GetCount() >= MAT_BUFFER_LIMIT)
    return size;

  int remaining = size;

  if (GetCount() <= MAT_POS_MIDDLE && GetCount() + size > MAT_POS_MIDDLE)
  {
    int nBytesBefore = MAT_POS_MIDDLE - static_cast<int>(GetCount());
    AppendData(data, nBytesBefore, type);
    remaining -= nBytesBefore;

    AppendData(mat_middle_code.data(), static_cast<int>(mat_middle_code.size()), MATDataType::DATA);

    if (type == MATDataType::PADDING)
      remaining -= static_cast<int>(mat_middle_code.size());

    if (remaining > 0)
      remaining = FillDataBuffer(data != nullptr ? data + nBytesBefore : nullptr, remaining, type);

    return remaining;
  }

  if (GetCount() + size >= MAT_BUFFER_LIMIT)
  {
    int nBytesBefore = MAT_BUFFER_LIMIT - static_cast<int>(GetCount());
    AppendData(data, nBytesBefore, type);
    remaining -= nBytesBefore;

    AppendData(mat_end_code.data(), static_cast<int>(mat_end_code.size()), MATDataType::DATA);

    assert(GetCount() == MAT_BUFFER_SIZE);

    if (type == MATDataType::PADDING)
      remaining -= static_cast<int>(mat_end_code.size());

    return remaining;
  }

  AppendData(data, size, type);
  return 0;
}

void CPackerMAT::FlushPacket()
{
  if (GetCount() == 0)
    return;

  assert(GetCount() == MAT_BUFFER_SIZE);
  m_outputQueue.emplace_back(std::move(m_buffer));
  m_buffer.clear();
  m_bufferCount = 0;
}

}  // namespace androidx_media3
