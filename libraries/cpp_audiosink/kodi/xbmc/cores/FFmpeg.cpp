/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/FFmpeg.h"

#include "utils/log.h"

namespace FFMPEG_HELP_TOOLS
{

std::string FFMpegErrorToString(int err)
{
  std::string text;
  text.resize(AV_ERROR_MAX_STRING_SIZE);
  av_strerror(err, text.data(), AV_ERROR_MAX_STRING_SIZE);
  return text;
}

} // namespace FFMPEG_HELP_TOOLS

FFmpegExtraData::FFmpegExtraData(size_t size)
  : m_data(reinterpret_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE))),
    m_size(size)
{
  if (!m_data)
    throw std::bad_alloc();
}

FFmpegExtraData::FFmpegExtraData(const uint8_t* data, size_t size)
  : FFmpegExtraData(size)
{
  if (size > 0)
    std::memcpy(m_data, data, size);
}

FFmpegExtraData::~FFmpegExtraData()
{
  av_free(m_data);
}

FFmpegExtraData::FFmpegExtraData(const FFmpegExtraData& e)
  : FFmpegExtraData(e.m_size)
{
  if (m_size > 0)
    std::memcpy(m_data, e.m_data, m_size);
}

FFmpegExtraData::FFmpegExtraData(FFmpegExtraData&& other) noexcept
  : FFmpegExtraData()
{
  std::swap(m_data, other.m_data);
  std::swap(m_size, other.m_size);
}

FFmpegExtraData& FFmpegExtraData::operator=(const FFmpegExtraData& other)
{
  if (this != &other)
  {
    if (m_size >= other.m_size && other.m_size > 0)
    {
      std::memcpy(m_data, other.m_data, other.m_size);
      m_size = other.m_size;
    }
    else
    {
      FFmpegExtraData extraData(other);
      *this = std::move(extraData);
    }
  }
  return *this;
}

FFmpegExtraData& FFmpegExtraData::operator=(FFmpegExtraData&& other) noexcept
{
  if (this != &other)
  {
    std::swap(m_data, other.m_data);
    std::swap(m_size, other.m_size);
  }
  return *this;
}

bool FFmpegExtraData::operator==(const FFmpegExtraData& other) const
{
  return m_size == other.m_size && (m_size == 0 || std::memcmp(m_data, other.m_data, m_size) == 0);
}

bool FFmpegExtraData::operator!=(const FFmpegExtraData& other) const
{
  return !(*this == other);
}

uint8_t* FFmpegExtraData::TakeData()
{
  auto tmp = m_data;
  m_data = nullptr;
  m_size = 0;
  return tmp;
}

FFmpegExtraData GetPacketExtradata(const AVPacket* pkt, const AVCodecParameters* codecPar)
{
  constexpr int FF_MAX_EXTRADATA_SIZE = ((1 << 28) - AV_INPUT_BUFFER_PADDING_SIZE);

  if (!pkt)
    return {};

  AVCodecID codecId = codecPar->codec_id;

  if (codecId != AV_CODEC_ID_MPEG1VIDEO && codecId != AV_CODEC_ID_MPEG2VIDEO &&
      codecId != AV_CODEC_ID_H264 && codecId != AV_CODEC_ID_HEVC &&
      codecId != AV_CODEC_ID_MPEG4 && codecId != AV_CODEC_ID_VC1 &&
      codecId != AV_CODEC_ID_AV1 && codecId != AV_CODEC_ID_AVS2 &&
      codecId != AV_CODEC_ID_AVS3 && codecId != AV_CODEC_ID_CAVS)
  {
    return {};
  }

  const AVBitStreamFilter* f = av_bsf_get_by_name("extract_extradata");
  if (!f)
    return {};

  AVBSFContext* bsf = nullptr;
  int ret = av_bsf_alloc(f, &bsf);
  if (ret < 0)
    return {};

  ret = avcodec_parameters_copy(bsf->par_in, codecPar);
  if (ret < 0)
  {
    av_bsf_free(&bsf);
    return {};
  }

  ret = av_bsf_init(bsf);
  if (ret < 0)
  {
    av_bsf_free(&bsf);
    return {};
  }

  AVPacket* dstPkt = av_packet_alloc();
  if (!dstPkt)
  {
    CLog::LogF(LOGERROR, "failed to allocate packet");
    av_bsf_free(&bsf);
    return {};
  }
  AVPacket* pktRef = dstPkt;

  ret = av_packet_ref(pktRef, pkt);
  if (ret < 0)
  {
    av_bsf_free(&bsf);
    av_packet_free(&dstPkt);
    return {};
  }

  ret = av_bsf_send_packet(bsf, pktRef);
  if (ret < 0)
  {
    av_packet_unref(pktRef);
    av_bsf_free(&bsf);
    av_packet_free(&dstPkt);
    return {};
  }

  FFmpegExtraData extraData;
  ret = 0;
  while (ret >= 0)
  {
    ret = av_bsf_receive_packet(bsf, pktRef);
    if (ret < 0)
    {
      if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        break;

      continue;
    }

    size_t retExtraDataSize = 0;
    uint8_t* retExtraData =
        av_packet_get_side_data(pktRef, AV_PKT_DATA_NEW_EXTRADATA, &retExtraDataSize);
    if (retExtraData && retExtraDataSize > 0 && retExtraDataSize < FF_MAX_EXTRADATA_SIZE)
    {
      try
      {
        extraData = FFmpegExtraData(retExtraData, retExtraDataSize);
      }
      catch (const std::bad_alloc&)
      {
        CLog::LogF(LOGERROR, "failed to allocate {} bytes for extradata", retExtraDataSize);
        av_packet_unref(pktRef);
        av_bsf_free(&bsf);
        av_packet_free(&dstPkt);
        return {};
      }

      CLog::LogF(LOGDEBUG, "fetching extradata, extradata_size({})", retExtraDataSize);
      av_packet_unref(pktRef);
      break;
    }

    av_packet_unref(pktRef);
  }

  av_bsf_free(&bsf);
  av_packet_free(&dstPkt);
  return extraData;
}
