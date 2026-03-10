/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Removed Kodi logging dependencies.
 *  - Replaced FFmpeg CRC usage with local CRC16-ANSI helpers.
 *  - Omitted the unused TrueHD parser path from the native Android module.
 *  - Wrapped the implementation in the androidx_media3 namespace.
 *  - Added a parser family hint so Media3 extractor MIME classification remains authoritative.
 */

#include "AEStreamInfo.h"

#include <algorithm>
#include <cstring>

namespace androidx_media3 {

#define DTS_PREAMBLE_14BE 0x1FFFE800
#define DTS_PREAMBLE_14LE 0xFF1F00E8
#define DTS_PREAMBLE_16BE 0x7FFE8001
#define DTS_PREAMBLE_16LE 0xFE7F0180
#define DTS_PREAMBLE_HD 0x64582025
#define DTS_PREAMBLE_XCH 0x5a5a5a5a
#define DTS_PREAMBLE_XXCH 0x47004a03
#define DTS_PREAMBLE_X96K 0x1d95f262
#define DTS_PREAMBLE_XBR 0x655e315e
#define DTS_PREAMBLE_LBR 0x0a801921
#define DTS_PREAMBLE_XLL 0x41a29547
#define DTS_SFREQ_COUNT 16
#define MAX_EAC3_BLOCKS 6
#define UNKNOWN_DTS_EXTENSION 255

namespace {

const uint16_t AC3Bitrates[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                192, 224, 256, 320, 384, 448, 512, 576, 640};
const uint16_t AC3FSCod[] = {48000, 44100, 32000, 0};
const uint8_t AC3BlkCod[] = {1, 2, 3, 6};
const uint8_t AC3Channels[] = {2, 1, 2, 3, 3, 4, 4, 5};
const uint8_t DTSChannels[] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
const uint8_t THDChanMap[] = {2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};
const uint32_t DTSSampleRates[DTS_SFREQ_COUNT] = {0,     8000,  16000, 32000, 64000,  128000,
                                                  11025, 22050, 44100, 88200, 176400, 12000,
                                                  24000, 48000, 96000, 192000};

uint16_t Crc16Ansi(const uint8_t* data, unsigned int size)
{
  uint16_t crc = 0;
  for (unsigned int i = 0; i < size; ++i)
  {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x8005) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

uint16_t Crc16Poly2D(const uint8_t* data, unsigned int size)
{
  uint16_t crc = 0;
  for (unsigned int i = 0; i < size; ++i)
  {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x002D) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

}  // namespace

double CAEStreamInfo::GetDuration() const
{
  double duration = 0;
  switch (m_type)
  {
    case STREAM_TYPE_AC3:
      duration = 1536.0 / m_sampleRate;
      break;
    case STREAM_TYPE_EAC3:
      duration = 6144.0 / m_sampleRate / 4;
      break;
    case STREAM_TYPE_TRUEHD:
    {
      const int rate =
          (m_sampleRate == 48000 || m_sampleRate == 96000 || m_sampleRate == 192000) ? 192000 : 176400;
      duration = 3840.0 / rate;
      break;
    }
    case STREAM_TYPE_DTS_512:
    case STREAM_TYPE_DTSHD_CORE:
    case STREAM_TYPE_DTSHD:
    case STREAM_TYPE_DTSHD_MA:
      duration = 512.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_1024:
      duration = 1024.0 / m_sampleRate;
      break;
    case STREAM_TYPE_DTS_2048:
      duration = 2048.0 / m_sampleRate;
      break;
    default:
      break;
  }
  return duration * 1000;
}

bool CAEStreamInfo::operator==(const CAEStreamInfo& info) const
{
  return m_type == info.m_type && m_dataIsLE == info.m_dataIsLE && m_repeat == info.m_repeat;
}

CAEStreamParser::CAEStreamParser() : m_syncFunc(&CAEStreamParser::DetectType) {}

void CAEStreamParser::SetFamilyHint(FamilyHint value)
{
  m_familyHint = value;
  m_syncFunc = &CAEStreamParser::DetectType;
  m_hasSync = false;
  m_needBytes = 0;
}

void CAEStreamParser::Reset()
{
  m_skipBytes = 0;
  m_bufferSize = 0;
  m_needBytes = 0;
  m_hasSync = false;
  m_coreSize = 0;
  m_dtsBlocks = 0;
  m_fsize = 0;
  m_substreams = 0;
  m_info = {};
  m_syncFunc = &CAEStreamParser::DetectType;
}

int CAEStreamParser::AddData(uint8_t* data,
                             unsigned int size,
                             uint8_t** buffer,
                             unsigned int* bufferSize)
{
  if (size == 0)
  {
    if (bufferSize)
      *bufferSize = 0;
    return 0;
  }

  if (m_skipBytes)
  {
    const unsigned int canSkip = std::min(size, m_skipBytes);
    const unsigned int room = sizeof(m_buffer) - m_bufferSize;
    const unsigned int copy = std::min(room, canSkip);

    std::memcpy(m_buffer + m_bufferSize, data, copy);
    m_bufferSize += copy;
    m_skipBytes -= copy;

    if (m_skipBytes)
    {
      if (bufferSize)
        *bufferSize = 0;
      return copy;
    }

    GetPacket(buffer, bufferSize);
    return copy;
  }

  unsigned int consumed = 0;
  unsigned int room = sizeof(m_buffer) - m_bufferSize;
  while (true)
  {
    if (!size)
    {
      if (bufferSize)
        *bufferSize = 0;
      return consumed;
    }

    const unsigned int copy = std::min(room, size);
    std::memcpy(m_buffer + m_bufferSize, data, copy);
    m_bufferSize += copy;
    consumed += copy;
    data += copy;
    size -= copy;
    room -= copy;

    if (m_needBytes > m_bufferSize)
      continue;

    m_needBytes = 0;
    const unsigned int offset = (this->*m_syncFunc)(m_buffer, m_bufferSize);

    if (m_hasSync)
    {
      if (offset)
      {
        m_bufferSize -= offset;
        std::memmove(m_buffer, m_buffer + offset, m_bufferSize);
      }

      m_skipBytes = std::max(0, static_cast<int>(m_fsize) - static_cast<int>(m_bufferSize));
      if (m_skipBytes)
      {
        if (bufferSize)
          *bufferSize = 0;
        return consumed;
      }

      if (!m_needBytes)
        GetPacket(buffer, bufferSize);
      else if (bufferSize)
        *bufferSize = 0;

      return consumed;
    }

    m_syncFunc = &CAEStreamParser::DetectType;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_NULL;
    m_info.m_repeat = 1;

    if (m_bufferSize == sizeof(m_buffer) || offset < m_bufferSize)
    {
      m_bufferSize -= offset;
      room += offset;
      std::memmove(m_buffer, m_buffer + offset, m_bufferSize);
    }
  }
}

void CAEStreamParser::GetPacket(uint8_t** buffer, unsigned int* bufferSize)
{
  if (buffer)
  {
    unsigned int size = m_fsize;
    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE)
      size = m_coreSize;

    if (!*buffer || !bufferSize || *bufferSize < size)
    {
      delete[] *buffer;
      *buffer = new uint8_t[size];
    }

    std::memcpy(*buffer, m_buffer, size);
    if (bufferSize)
      *bufferSize = size;
  }

  m_bufferSize -= m_fsize;
  std::memmove(m_buffer, m_buffer + m_fsize, m_bufferSize);
  m_fsize = 0;
  m_coreSize = 0;
}

unsigned int CAEStreamParser::DetectType(uint8_t* data, unsigned int size)
{
  unsigned int skipped = 0;
  unsigned int possible = 0;

  while (size > 8)
  {
    if (m_familyHint != FAMILY_AC3 && m_familyHint != FAMILY_TRUEHD)
    {
      const unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
      if (header == DTS_PREAMBLE_14LE || header == DTS_PREAMBLE_14BE || header == DTS_PREAMBLE_16LE ||
          header == DTS_PREAMBLE_16BE)
      {
        const unsigned int skip = SyncDTS(data, size);
        if (m_hasSync || m_needBytes)
          return skipped + skip;
        possible = skipped;
      }
    }

    if (m_familyHint != FAMILY_DTS && m_familyHint != FAMILY_TRUEHD && data[0] == 0x0b && data[1] == 0x77)
    {
      const unsigned int skip = SyncAC3(data, size);
      if (m_hasSync || m_needBytes)
        return skipped + skip;
      possible = skipped;
    }

    if (m_familyHint != FAMILY_AC3 && m_familyHint != FAMILY_DTS &&
        data[4] == 0xf8 && data[5] == 0x72 && data[6] == 0x6f && data[7] == 0xba)
    {
      const unsigned int skip = SyncTrueHD(data, size);
      if (m_hasSync)
        return skipped + skip;
      possible = skipped;
    }

    --size;
    ++skipped;
    ++data;
  }

  return possible ? possible : skipped;
}

bool CAEStreamParser::TrySyncAC3(uint8_t* data,
                                 unsigned int size,
                                 bool resyncing,
                                 bool wantEAC3dependent)
{
  if (size < 8)
    return false;

  if (data[0] != 0x0b || data[1] != 0x77)
    return false;

  const uint8_t bsid = data[5] >> 3;
  const uint8_t acmod = data[6] >> 5;
  int8_t pos = 4;
  if ((acmod & 0x1) && (acmod != 0x1))
    pos -= 2;
  if (acmod & 0x4)
    pos -= 2;
  if (acmod == 0x2)
    pos -= 2;
  const uint8_t lfeon = pos < 0 ? ((data[7] & 0x64) ? 1 : 0) : (((data[6] >> pos) & 0x1) ? 1 : 0);

  if (bsid > 0x11 || acmod > 7)
    return false;

  if (bsid <= 10)
  {
    if (wantEAC3dependent)
      return false;

    const uint8_t fscod = data[4] >> 6;
    const uint8_t frmsizecod = data[4] & 0x3F;
    if (fscod == 3 || frmsizecod > 37)
      return false;

    const unsigned int bitRate = AC3Bitrates[frmsizecod >> 1];
    unsigned int framesize = 0;
    switch (fscod)
    {
      case 0:
        framesize = bitRate * 2;
        break;
      case 1:
        framesize = (320 * bitRate / 147 + ((frmsizecod & 1) ? 1 : 0));
        break;
      case 2:
        framesize = bitRate * 4;
        break;
      default:
        break;
    }

    m_fsize = framesize << 1;
    m_info.m_sampleRate = AC3FSCod[fscod];

    if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_AC3 && !resyncing)
      return true;

    const unsigned int fsizeMain = m_fsize;
    const unsigned int reqBytes = fsizeMain + 8;
    if (size < reqBytes)
    {
      m_needBytes = reqBytes;
      m_fsize = 0;
      return true;
    }
    m_info.m_frameSize = fsizeMain;
    if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
    {
      m_fsize += fsizeMain;
      return true;
    }

    unsigned int crc_size = (framesize <= size) ? (framesize - 1) : ((framesize >> 1) + (framesize >> 3) - 1);
    if (crc_size <= size)
    {
      if (Crc16Ansi(&data[2], crc_size * 2) != 0)
        return false;
    }

    m_hasSync = true;
    m_info.m_channels = AC3Channels[acmod] + lfeon;
    m_syncFunc = &CAEStreamParser::SyncAC3;
    m_info.m_type = CAEStreamInfo::STREAM_TYPE_AC3;
    m_info.m_frameSize += m_fsize;
    m_info.m_repeat = 1;
    return true;
  }

  const uint8_t strmtyp = data[2] >> 6;
  if (strmtyp == 3)
    return false;
  if (strmtyp != 1 && wantEAC3dependent)
    return false;

  const unsigned int framesize = (((data[2] & 0x7) << 8) | data[3]) + 1;
  const uint8_t fscod = (data[4] >> 6) & 0x3;
  const uint8_t cod = (data[4] >> 4) & 0x3;
  const uint8_t eac3Acmod = (data[4] >> 1) & 0x7;
  const uint8_t eac3Lfeon = data[4] & 0x1;
  uint8_t blocks;

  if (fscod == 0x3)
  {
    if (cod == 0x3)
      return false;
    blocks = 6;
    m_info.m_sampleRate = AC3FSCod[cod] >> 1;
  }
  else
  {
    blocks = AC3BlkCod[cod];
    m_info.m_sampleRate = AC3FSCod[fscod];
  }

  m_fsize = framesize << 1;
  m_info.m_repeat = MAX_EAC3_BLOCKS / blocks;

  if (!wantEAC3dependent)
  {
    const unsigned int fsizeMain = m_fsize;
    const unsigned int reqBytes = fsizeMain + 8;
    if (size < reqBytes)
    {
      m_needBytes = reqBytes;
      m_fsize = 0;
      return true;
    }
    m_info.m_frameSize = fsizeMain;
    if (TrySyncAC3(data + fsizeMain, size - fsizeMain, resyncing, true))
    {
      m_fsize += fsizeMain;
      return true;
    }
  }

  if (m_info.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 && m_hasSync && !resyncing)
    return true;

  m_hasSync = true;
  m_info.m_channels = AC3Channels[eac3Acmod] + eac3Lfeon;
  m_syncFunc = &CAEStreamParser::SyncAC3;
  m_info.m_type = CAEStreamInfo::STREAM_TYPE_EAC3;
  m_info.m_frameSize += m_fsize;
  return true;
}

unsigned int CAEStreamParser::SyncAC3(uint8_t* data, unsigned int size)
{
  unsigned int skip = 0;
  for (; size - skip > 7; ++skip, ++data)
  {
    const bool resyncing = skip != 0;
    if (TrySyncAC3(data, size - skip, resyncing, false))
      return skip;
  }
  m_hasSync = false;
  return skip;
}

unsigned int CAEStreamParser::SyncDTS(uint8_t* data, unsigned int size)
{
  if (size < 13)
  {
    if (m_needBytes < 13)
      m_needBytes = 14;
    return 0;
  }

  unsigned int skip = 0;
  for (; size - skip > 13; ++skip, ++data)
  {
    const unsigned int header = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    unsigned int hd_sync = 0;
    unsigned int dtsBlocks;
    unsigned int amode;
    unsigned int sfreq;
    unsigned int target_rate;
    unsigned int extension = 0;
    unsigned int ext_type = UNKNOWN_DTS_EXTENSION;
    unsigned int lfe;
    int bits;

    switch (header)
    {
      case DTS_PREAMBLE_14BE:
        if (data[4] != 0x07 || (data[5] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[5] & 0x7) << 4) | ((data[6] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[6] & 0x3) << 8) | data[7]) << 4) | ((data[8] & 0x3C) >> 2)) + 1;
        amode = ((data[8] & 0x3) << 4) | ((data[9] & 0xF0) >> 4);
        target_rate = ((data[10] & 0x3e) >> 1);
        extension = (data[11] & 0x1);
        ext_type = ((data[11] & 0xe) >> 1);
        sfreq = data[9] & 0xF;
        lfe = (data[12] & 0x18) >> 3;
        m_info.m_dataIsLE = false;
        bits = 14;
        break;
      case DTS_PREAMBLE_14LE:
        if (data[5] != 0x07 || (data[4] & 0xf0) != 0xf0)
          continue;
        dtsBlocks = (((data[4] & 0x7) << 4) | ((data[7] & 0x3C) >> 2)) + 1;
        m_fsize = (((((data[7] & 0x3) << 8) | data[6]) << 4) | ((data[9] & 0x3C) >> 2)) + 1;
        amode = ((data[9] & 0x3) << 4) | ((data[8] & 0xF0) >> 4);
        target_rate = ((data[11] & 0x3e) >> 1);
        extension = (data[10] & 0x1);
        ext_type = ((data[10] & 0xe) >> 1);
        sfreq = data[8] & 0xF;
        lfe = (data[13] & 0x18) >> 3;
        m_info.m_dataIsLE = true;
        bits = 14;
        break;
      case DTS_PREAMBLE_16BE:
        dtsBlocks = (((data[4] & 0x1) << 7) | ((data[5] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[5] & 0x3) << 8) | data[6]) << 4) | ((data[7] & 0xF0) >> 4)) + 1;
        amode = ((data[7] & 0x0F) << 2) | ((data[8] & 0xC0) >> 6);
        sfreq = (data[8] & 0x3C) >> 2;
        target_rate = ((data[8] & 0x03) << 3) | ((data[9] & 0xe0) >> 5);
        extension = (data[10] & 0x10) >> 4;
        ext_type = (data[10] & 0xe0) >> 5;
        lfe = (data[10] >> 1) & 0x3;
        m_info.m_dataIsLE = false;
        bits = 16;
        break;
      case DTS_PREAMBLE_16LE:
        dtsBlocks = (((data[5] & 0x1) << 7) | ((data[4] & 0xFC) >> 2)) + 1;
        m_fsize = (((((data[4] & 0x3) << 8) | data[7]) << 4) | ((data[6] & 0xF0) >> 4)) + 1;
        amode = ((data[6] & 0x0F) << 2) | ((data[9] & 0xC0) >> 6);
        sfreq = (data[9] & 0x3C) >> 2;
        target_rate = ((data[9] & 0x03) << 3) | ((data[8] & 0xe0) >> 5);
        extension = (data[11] & 0x10) >> 4;
        ext_type = (data[11] & 0xe0) >> 5;
        lfe = (data[11] >> 1) & 0x3;
        m_info.m_dataIsLE = true;
        bits = 16;
        break;
      default:
        continue;
    }

    if (sfreq == 0 || sfreq >= DTS_SFREQ_COUNT)
      continue;
    if (m_fsize < 96 || m_fsize > 16384)
      continue;

    CAEStreamInfo::DataType dataType{CAEStreamInfo::STREAM_TYPE_NULL};
    switch (dtsBlocks << 5)
    {
      case 512:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_512;
        break;
      case 1024:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_1024;
        break;
      case 2048:
        dataType = CAEStreamInfo::STREAM_TYPE_DTS_2048;
        break;
      default:
        break;
    }

    if (dataType == CAEStreamInfo::STREAM_TYPE_NULL)
      continue;

    if (bits == 14)
      m_fsize = m_fsize / 14 * 16;

    if (size - skip < m_fsize + 10)
    {
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_needBytes = m_fsize + 10;
      m_fsize = 0;
      return skip;
    }

    hd_sync = (data[m_fsize] << 24) | (data[m_fsize + 1] << 16) | (data[m_fsize + 2] << 8) |
              data[m_fsize + 3];
    if (hd_sync == DTS_PREAMBLE_HD)
    {
      int hd_size;
      const bool blownup = (data[m_fsize + 5] & 0x20) != 0;
      if (blownup)
        hd_size = (((data[m_fsize + 6] & 0x01) << 19) | (data[m_fsize + 7] << 11) |
                   (data[m_fsize + 8] << 3) | ((data[m_fsize + 9] & 0xe0) >> 5)) +
                  1;
      else
        hd_size = (((data[m_fsize + 6] & 0x1f) << 11) | (data[m_fsize + 7] << 3) |
                   ((data[m_fsize + 8] & 0xe0) >> 5)) +
                  1;

      int header_size;
      if (blownup)
        header_size = (((data[m_fsize + 5] & 0x1f) << 7) | ((data[m_fsize + 6] & 0xfe) >> 1)) + 1;
      else
        header_size = (((data[m_fsize + 5] & 0x1f) << 3) | ((data[m_fsize + 6] & 0xe0) >> 5)) + 1;

      hd_sync = data[m_fsize + header_size] << 24 | data[m_fsize + header_size + 1] << 16 |
                data[m_fsize + header_size + 2] << 8 | data[m_fsize + header_size + 3];

      if (m_coreOnly)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_CORE;
      else if (hd_sync == DTS_PREAMBLE_XLL)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD_MA;
      else if (hd_sync == DTS_PREAMBLE_XCH || hd_sync == DTS_PREAMBLE_XXCH ||
               hd_sync == DTS_PREAMBLE_X96K || hd_sync == DTS_PREAMBLE_XBR ||
               hd_sync == DTS_PREAMBLE_LBR)
        dataType = CAEStreamInfo::STREAM_TYPE_DTSHD;
      else
        dataType = m_info.m_type;

      m_coreSize = m_fsize;
      m_fsize += hd_size;
    }

    const unsigned int sampleRate = DTSSampleRates[sfreq];
    if (!m_hasSync || skip || dataType != m_info.m_type || sampleRate != m_info.m_sampleRate ||
        dtsBlocks != m_dtsBlocks)
    {
      m_hasSync = true;
      m_info.m_type = dataType;
      m_info.m_sampleRate = sampleRate;
      m_dtsBlocks = dtsBlocks;
      m_info.m_channels = DTSChannels[amode] + (lfe ? 1 : 0);
      m_syncFunc = &CAEStreamParser::SyncDTS;
      m_info.m_frameSize = m_fsize;
      m_info.m_repeat = 1;

      if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
      {
        m_info.m_channels += 2;
        m_info.m_dtsPeriod = (192000 * (8 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else if (dataType == CAEStreamInfo::STREAM_TYPE_DTSHD)
      {
        m_info.m_dtsPeriod = (192000 * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }
      else
      {
        m_info.m_dtsPeriod =
            (m_info.m_sampleRate * (2 >> 1)) * (m_dtsBlocks << 5) / m_info.m_sampleRate;
      }

      (void)extension;
      (void)ext_type;
      (void)target_rate;
      (void)hd_sync;
    }

    return skip;
  }

  m_hasSync = false;
  return skip;
}

unsigned int CAEStreamParser::GetTrueHDChannels(const uint16_t chanmap)
{
  int channels = 0;
  for (int i = 0; i < 13; ++i)
    channels += THDChanMap[i] * ((chanmap >> i) & 1);
  return channels;
}

unsigned int CAEStreamParser::SyncTrueHD(uint8_t* data, unsigned int size)
{
  unsigned int left = size;
  unsigned int skip = 0;

  for (; left; ++skip, ++data, --left)
  {
    if (!m_hasSync && left < 8)
      return size;

    if (left < 8)
      break;

    const uint16_t length = ((data[0] & 0x0F) << 8 | data[1]) << 1;
    const uint32_t syncword = ((((data[4] << 8) | data[5]) << 8 | data[6]) << 8) | data[7];
    if (syncword == 0xf8726fba)
    {
      if (left < 32)
        return skip;

      const int rate = (data[8] & 0xf0) >> 4;
      if (rate == 0xF)
        continue;

      unsigned int major_sync_size = 28;
      if (data[29] & 1)
      {
        const int extension_count = data[30] >> 4;
        major_sync_size += 2 + extension_count * 2;
      }

      if (left < 4 + major_sync_size)
        return skip;

      uint16_t crc = Crc16Poly2D(data + 4, major_sync_size - 4);
      crc ^= (data[4 + major_sync_size - 3] << 8) | data[4 + major_sync_size - 4];
      if (((data[4 + major_sync_size - 1] << 8) | data[4 + major_sync_size - 2]) != crc)
        continue;

      m_info.m_sampleRate = (rate & 0x8 ? 44100 : 48000) << (rate & 0x7);
      m_substreams = (data[20] & 0xF0) >> 4;

      uint16_t channel_map = ((data[10] & 0x1F) << 8) | data[11];
      if (!channel_map)
        channel_map = (data[9] << 1) | (data[10] >> 7);
      m_info.m_channels = CAEStreamParser::GetTrueHDChannels(channel_map);

      m_hasSync = true;
      m_fsize = length;
      m_info.m_type = CAEStreamInfo::STREAM_TYPE_TRUEHD;
      m_syncFunc = &CAEStreamParser::SyncTrueHD;
      m_info.m_frameSize = length;
      m_info.m_repeat = 1;
      return skip;
    }

    if (!m_hasSync)
      continue;

    if (left < static_cast<unsigned int>(m_substreams) * 4)
      return skip;

    int p = 0;
    uint8_t check = 0;
    for (int i = -1; i < m_substreams; ++i)
    {
      check ^= data[p++];
      check ^= data[p++];
      if (i == -1 || data[p - 2] & 0x80)
      {
        check ^= data[p++];
        check ^= data[p++];
      }
    }

    if ((((check >> 4) ^ check) & 0xF) != 0xF)
    {
      m_hasSync = false;
      continue;
    }

    m_fsize = length;
    return skip;
  }

  m_hasSync = false;
  return skip;
}

}  // namespace androidx_media3
