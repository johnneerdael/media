/*
 * Copyright (C) 2026 Nuvio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "KodiTrueHdIecPipeline.h"

#include "utils/log.h"

#include <algorithm>

namespace androidx_media3
{

namespace
{
bool IsTrueHdPassthrough(const ActiveAE::CActiveAEMediaStreamAdapter& streamAdapter)
{
  return streamAdapter.GetRequestedFormat().m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD;
}
}  // namespace

void KodiTrueHdIecPipeline::Configure(const AEAudioFormat& requestedFormat)
{
  streamAdapter_.Configure(requestedFormat);
  bitstreamPacker_.Reset();
  matPacker_ = std::make_unique<KodiTrueHdFfmpegMatPacker>();
  pendingBurstPtsUs_ = NO_PTS;
  pendingBurstDurationUs_ = 0;
  pendingBurstInputBytes_ = 0;
}

void KodiTrueHdIecPipeline::Reset()
{
  streamAdapter_.Reset();
  bitstreamPacker_.Reset();
  matPacker_ = std::make_unique<KodiTrueHdFfmpegMatPacker>();
  pendingBurstPtsUs_ = NO_PTS;
  pendingBurstDurationUs_ = 0;
  pendingBurstInputBytes_ = 0;
}

int KodiTrueHdIecPipeline::Feed(const uint8_t* data,
                          int size,
                          int64_t presentationTimeUs,
                          KodiPackedAccessUnit* outPacket,
                          bool* emittedPacket)
{
  if (IsTrueHdPassthrough(streamAdapter_))
    return FeedTrueHd(data, size, presentationTimeUs, outPacket, emittedPacket);

  return FeedBaseline(data, size, presentationTimeUs, outPacket, emittedPacket);
}

int KodiTrueHdIecPipeline::FeedBaseline(const uint8_t* data,
                                  int size,
                                  int64_t presentationTimeUs,
                                  KodiPackedAccessUnit* outPacket,
                                  bool* emittedPacket)
{
  if (data == nullptr || size <= 0)
    return 0;
  if (outPacket == nullptr || emittedPacket == nullptr)
    return 0;

  int consumedTotal = 0;
  const uint8_t* current = data;
  int remaining = size;
  int64_t currentPtsUs = presentationTimeUs;
  *emittedPacket = false;

  while (remaining > 0 && !*emittedPacket)
  {
    const uint8_t* auData = nullptr;
    unsigned int auSize = 0;
    int64_t auPtsUs = NO_PTS;
    int64_t auDurationUs = 0;

    const int consumed = streamAdapter_.FeedPassthrough(
        current, remaining, currentPtsUs, &auData, &auSize, &auPtsUs, &auDurationUs);

    if (consumed > 0)
    {
      current += consumed;
      remaining -= consumed;
      consumedTotal += consumed;
      pendingBurstInputBytes_ += consumed;
      currentPtsUs = NO_PTS;
    }

    if (auSize > 0 && auData != nullptr)
    {
      EmitPackedPacket(auData, auSize, auPtsUs, auDurationUs, outPacket, emittedPacket);
    }

    if (consumed <= 0)
      break;
  }

  return consumedTotal;
}

int KodiTrueHdIecPipeline::FeedTrueHd(const uint8_t* data,
                                int size,
                                int64_t presentationTimeUs,
                                KodiPackedAccessUnit* outPacket,
                                bool* emittedPacket)
{
  if (outPacket == nullptr || emittedPacket == nullptr)
    return 0;

  *emittedPacket = false;

  if (DrainTrueHdBacklog(outPacket, emittedPacket))
    return 0;

  if (data == nullptr || size <= 0)
    return 0;

  int consumedTotal = 0;
  const uint8_t* current = data;
  int remaining = size;
  int64_t currentPtsUs = presentationTimeUs;

  while (remaining > 0 && !*emittedPacket)
  {
    const uint8_t* auData = nullptr;
    unsigned int auSize = 0;
    int64_t auPtsUs = NO_PTS;
    int64_t auDurationUs = 0;

    const int consumed = streamAdapter_.FeedPassthrough(
        current, remaining, currentPtsUs, &auData, &auSize, &auPtsUs, &auDurationUs);

    if (consumed > 0)
    {
      current += consumed;
      remaining -= consumed;
      consumedTotal += consumed;
      pendingBurstInputBytes_ += consumed;
      currentPtsUs = NO_PTS;
    }

    if (auSize > 0 && auData != nullptr)
    {
      EmitPackedPacket(auData, auSize, auPtsUs, auDurationUs, outPacket, emittedPacket);
    }

    DrainTrueHdBacklog(outPacket, emittedPacket);

    if (consumed <= 0)
      break;
  }

  return consumedTotal;
}

void KodiTrueHdIecPipeline::AcknowledgeConsumedInputBytes(int bytes)
{
  if (bytes <= 0)
    return;
  pendingBurstInputBytes_ = std::max(0, pendingBurstInputBytes_ - bytes);
}

void KodiTrueHdIecPipeline::EmitPackedPacket(const uint8_t* auData,
                                       unsigned int auSize,
                                       int64_t auPtsUs,
                                       int64_t auDurationUs,
                                       KodiPackedAccessUnit* outPacket,
                                       bool* emittedPacket)
{
  AEAudioFormat resolved = streamAdapter_.GetResolvedFormat();
  CAEStreamInfo info = resolved.m_streamInfo;
  if (info.m_type == CAEStreamInfo::STREAM_TYPE_NULL || info.m_sampleRate == 0)
    return;
  if (outPacket == nullptr || emittedPacket == nullptr)
    return;

  if (pendingBurstPtsUs_ == NO_PTS && auPtsUs != NO_PTS)
    pendingBurstPtsUs_ = auPtsUs;
  if (auDurationUs > 0)
    pendingBurstDurationUs_ += auDurationUs;

  const uint8_t* packedInputData = auData;
  int packedInputSize = static_cast<int>(auSize);
  std::vector<uint8_t> matFrame;
  if (info.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    if (!matPacker_)
      matPacker_ = std::make_unique<KodiTrueHdFfmpegMatPacker>();
    if (!matPacker_->PackTrueHd(auData, packedInputSize))
      return;
    matFrame = matPacker_->GetOutputFrame();
    if (matFrame.empty())
      return;
    packedInputData = matFrame.data();
    packedInputSize = static_cast<int>(matFrame.size());
  }

  bitstreamPacker_.Pack(info,
                        const_cast<uint8_t*>(packedInputData),
                        packedInputSize);
  const unsigned int packedSize = bitstreamPacker_.GetSize();
  if (packedSize == 0)
    return;

  outPacket->bytes.assign(bitstreamPacker_.GetBuffer(), bitstreamPacker_.GetBuffer() + packedSize);
  outPacket->writeOffset = 0;
  outPacket->inputBytesConsumed = pendingBurstInputBytes_;
  outPacket->ptsUs = pendingBurstPtsUs_;
  outPacket->durationUs = pendingBurstDurationUs_ > 0 ? pendingBurstDurationUs_ : auDurationUs;
  outPacket->outputRate = CAEBitstreamPacker::GetOutputRate(info);
  outPacket->outputChannels =
      std::max<unsigned int>(1u, CAEBitstreamPacker::GetOutputChannelMap(info).Count());
  outPacket->streamInfo = info;
  *emittedPacket = true;

  if (pendingBurstPtsUs_ != NO_PTS && pendingBurstDurationUs_ > 0)
    pendingBurstPtsUs_ += pendingBurstDurationUs_;
  pendingBurstDurationUs_ = 0;
  pendingBurstInputBytes_ = 0;
}

bool KodiTrueHdIecPipeline::DrainTrueHdBacklog(KodiPackedAccessUnit* outPacket, bool* emittedPacket)
{
  if (!IsTrueHdPassthrough(streamAdapter_) || !streamAdapter_.HasBacklog() || outPacket == nullptr ||
      emittedPacket == nullptr)
  {
    return false;
  }

  const uint8_t* backlogData = nullptr;
  unsigned int backlogSize = 0;
  int64_t backlogPtsUs = NO_PTS;
  int64_t backlogDurationUs = 0;
  if (!streamAdapter_.DrainBacklog(
          &backlogData, &backlogSize, &backlogPtsUs, &backlogDurationUs) ||
      backlogSize == 0 || backlogData == nullptr)
  {
    return false;
  }

  EmitPackedPacket(backlogData, backlogSize, backlogPtsUs, backlogDurationUs, outPacket, emittedPacket);
  return *emittedPacket;
}

}  // namespace androidx_media3
