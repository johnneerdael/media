/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Removed Kodi logging dependencies.
 *  - Wrapped the implementation in the androidx_media3 namespace.
 */

#include "AEBitstreamPacker.h"

#include "AEStreamInfo.h"

#include <cstring>

namespace androidx_media3 {
namespace {
constexpr auto BURST_HEADER_SIZE = 8;
constexpr auto EAC3_MAX_BURST_PAYLOAD_SIZE = 24576 - BURST_HEADER_SIZE;
}  // namespace

CAEBitstreamPacker::CAEBitstreamPacker()
{
  Reset();
}

CAEBitstreamPacker::~CAEBitstreamPacker() = default;

void CAEBitstreamPacker::Pack(CAEStreamInfo& info, uint8_t* data, int size)
{
  m_pauseDuration = 0;
  switch (info.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_dataSize = CAEPackIEC61937::PackTrueHD(
          data + IEC61937_DATA_OFFSET, size - IEC61937_DATA_OFFSET, m_packedBuffer);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      PackDTSHD(info, data, size);
      break;

    case CAEStreamInfo::STREAM_TYPE_AC3:
      m_dataSize = CAEPackIEC61937::PackAC3(data, size, m_packedBuffer);
      break;

    case CAEStreamInfo::STREAM_TYPE_EAC3:
      PackEAC3(info, data, size);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
      m_dataSize = CAEPackIEC61937::PackDTS_512(data, size, m_packedBuffer, info.m_dataIsLE);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
      m_dataSize = CAEPackIEC61937::PackDTS_1024(data, size, m_packedBuffer, info.m_dataIsLE);
      break;

    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
      m_dataSize = CAEPackIEC61937::PackDTS_2048(data, size, m_packedBuffer, info.m_dataIsLE);
      break;

    default:
      m_dataSize = 0;
      break;
  }
}

bool CAEBitstreamPacker::PackPause(CAEStreamInfo& info, unsigned int millis, bool iecBursts)
{
  if (m_pauseDuration == millis)
    return false;

  switch (info.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
    case CAEStreamInfo::STREAM_TYPE_EAC3:
      m_dataSize = CAEPackIEC61937::PackPause(m_packedBuffer, millis,
                                              GetOutputChannelMap(info).Count() * 2,
                                              GetOutputRate(info), 4, info.m_sampleRate);
      m_pauseDuration = millis;
      break;

    case CAEStreamInfo::STREAM_TYPE_AC3:
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
      m_dataSize = CAEPackIEC61937::PackPause(m_packedBuffer, millis,
                                              GetOutputChannelMap(info).Count() * 2,
                                              GetOutputRate(info), 3, info.m_sampleRate);
      m_pauseDuration = millis;
      break;

    default:
      m_dataSize = 0;
      break;
  }

  if (!iecBursts)
    std::memset(m_packedBuffer, 0, m_dataSize);

  return m_dataSize > 0;
}

unsigned int CAEBitstreamPacker::GetSize() const
{
  return m_dataSize;
}

uint8_t* CAEBitstreamPacker::GetBuffer()
{
  return m_packedBuffer;
}

void CAEBitstreamPacker::Reset()
{
  m_dtsHD.clear();
  m_dtsHDSize = 0;
  m_eac3.clear();
  m_eac3Size = 0;
  m_eac3FramesCount = 0;
  m_eac3FramesPerBurst = 0;
  m_dataSize = 0;
  m_pauseDuration = 0;
  m_packedBuffer[0] = 0;
}

void CAEBitstreamPacker::PackDTSHD(CAEStreamInfo& info, uint8_t* data, int size)
{
  static const uint8_t dtshd_start_code[10] = {
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xfe};
  const unsigned int dataSize = sizeof(dtshd_start_code) + 2 + size;

  if (dataSize > m_dtsHDSize)
  {
    m_dtsHDSize = dataSize;
    m_dtsHD.resize(dataSize);
    std::memcpy(m_dtsHD.data(), dtshd_start_code, sizeof(dtshd_start_code));
  }

  m_dtsHD[sizeof(dtshd_start_code) + 0] = (static_cast<uint16_t>(size) & 0xFF00) >> 8;
  m_dtsHD[sizeof(dtshd_start_code) + 1] = (static_cast<uint16_t>(size) & 0x00FF);
  std::memcpy(m_dtsHD.data() + sizeof(dtshd_start_code) + 2, data, size);

  m_dataSize =
      CAEPackIEC61937::PackDTSHD(m_dtsHD.data(), dataSize, m_packedBuffer, info.m_dtsPeriod);
}

void CAEBitstreamPacker::PackEAC3(CAEStreamInfo& info, uint8_t* data, int size)
{
  const unsigned int framesPerBurst = info.m_repeat;

  if (m_eac3FramesPerBurst != framesPerBurst)
  {
    m_eac3Size = 0;
    m_eac3FramesPerBurst = framesPerBurst;
  }

  if (m_eac3FramesPerBurst == 1)
  {
    m_dataSize = CAEPackIEC61937::PackEAC3(data, size, m_packedBuffer);
  }
  else
  {
    if (m_eac3.empty())
      m_eac3.resize(EAC3_MAX_BURST_PAYLOAD_SIZE);

    const unsigned int newsize = m_eac3Size + size;
    const bool overrun = newsize > EAC3_MAX_BURST_PAYLOAD_SIZE;

    if (!overrun)
    {
      std::memcpy(m_eac3.data() + m_eac3Size, data, size);
      m_eac3Size = newsize;
      m_eac3FramesCount++;
    }

    if (m_eac3FramesCount >= m_eac3FramesPerBurst || overrun)
    {
      m_dataSize = CAEPackIEC61937::PackEAC3(m_eac3.data(), m_eac3Size, m_packedBuffer);
      m_eac3Size = 0;
      m_eac3FramesCount = 0;
    }
  }
}

unsigned int CAEBitstreamPacker::GetOutputRate(const CAEStreamInfo& info)
{
  unsigned int rate;
  switch (info.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      rate = info.m_sampleRate;
      break;
    case CAEStreamInfo::STREAM_TYPE_EAC3:
      rate = info.m_sampleRate * 4;
      break;
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      if (info.m_sampleRate == 48000 || info.m_sampleRate == 96000 || info.m_sampleRate == 192000)
        rate = 192000;
      else
        rate = 176400;
      break;
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      rate = info.m_sampleRate;
      break;
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      rate = 192000;
      break;
    default:
      rate = 48000;
      break;
  }
  return rate;
}

CAEChannelInfo CAEBitstreamPacker::GetOutputChannelMap(const CAEStreamInfo& info)
{
  int channels = 2;
  switch (info.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
    case CAEStreamInfo::STREAM_TYPE_EAC3:
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
      channels = 2;
      break;

    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      channels = 8;
      break;

    default:
      break;
  }

  CAEChannelInfo channelMap;
  for (int i = 0; i < channels; ++i)
    channelMap += AE_CH_RAW;

  return channelMap;
}

}  // namespace androidx_media3
