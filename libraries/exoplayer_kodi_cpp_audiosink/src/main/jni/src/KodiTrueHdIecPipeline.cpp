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

#include "cores/AudioEngine/Utils/AEPackIEC61937.h"

#include <algorithm>

namespace
{
constexpr uint16_t IEC61937_PREAMBLE1 = 0xF872;
constexpr uint16_t IEC61937_PREAMBLE2 = 0x4E1F;

void SwapEndianInPlace(uint8_t* data, size_t size)
{
  for (size_t i = 0; i + 1 < size; i += 2)
    std::swap(data[i], data[i + 1]);
}
}  // namespace

namespace androidx_media3
{

void KodiTrueHdIecPipeline::Configure(const AEAudioFormat& requestedFormat)
{
  streamAdapter_.Configure(requestedFormat);
  matPacker_ = CPackerMAT();
  pendingBurstPtsUs_ = NO_PTS;
  pendingBurstDurationUs_ = 0;
  pendingBurstInputBytes_ = 0;
  pendingBurstAccessUnitCount_ = 0;
}

void KodiTrueHdIecPipeline::Reset()
{
  streamAdapter_.Reset();
  matPacker_ = CPackerMAT();
  pendingBurstPtsUs_ = NO_PTS;
  pendingBurstDurationUs_ = 0;
  pendingBurstInputBytes_ = 0;
  pendingBurstAccessUnitCount_ = 0;
}

int KodiTrueHdIecPipeline::Feed(const uint8_t* data,
                                int size,
                                int64_t presentationTimeUs,
                                std::deque<KodiTrueHdPackedUnit>& outPackets,
                                int maxPackets)
{
  if (data == nullptr || size <= 0 || maxPackets <= 0)
    return 0;

  int consumedTotal = 0;
  const uint8_t* current = data;
  int remaining = size;
  int64_t currentPtsUs = presentationTimeUs;

  while (remaining > 0 && static_cast<int>(outPackets.size()) < maxPackets)
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

    if (auSize > IEC61937_DATA_OFFSET && auData != nullptr)
    {
      if (pendingBurstPtsUs_ == NO_PTS && auPtsUs != NO_PTS)
        pendingBurstPtsUs_ = auPtsUs;
      if (auDurationUs > 0)
        pendingBurstDurationUs_ += auDurationUs;
      ++pendingBurstAccessUnitCount_;

      matPacker_.PackTrueHD(
          auData + IEC61937_DATA_OFFSET, static_cast<int>(auSize - IEC61937_DATA_OFFSET));
      EmitPackedMatFrames(outPackets, maxPackets);
    }

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

void KodiTrueHdIecPipeline::EmitPackedMatFrames(std::deque<KodiTrueHdPackedUnit>& outPackets,
                                                int maxPackets)
{
  while (static_cast<int>(outPackets.size()) < maxPackets)
  {
    std::vector<uint8_t> matFrame = matPacker_.GetOutputFrame();
    if (matFrame.empty())
      return;

    if (matFrame.size() < IEC61937_DATA_OFFSET)
      continue;

    auto* header = reinterpret_cast<uint16_t*>(matFrame.data());
    header[0] = IEC61937_PREAMBLE1;
    header[1] = IEC61937_PREAMBLE2;
    header[2] = 0x0016;
    header[3] = static_cast<uint16_t>(kMatFramePayloadLengthCode);

#ifndef __BIG_ENDIAN__
    SwapEndianInPlace(matFrame.data() + IEC61937_DATA_OFFSET, matFrame.size() - IEC61937_DATA_OFFSET);
#endif

    KodiTrueHdPackedUnit packet;
    packet.bytes = std::move(matFrame);
    packet.inputBytesConsumed = pendingBurstInputBytes_;
    packet.ptsUs = pendingBurstPtsUs_;
    packet.durationUs = pendingBurstDurationUs_;
    packet.sourceAccessUnitCount = pendingBurstAccessUnitCount_;
    packet.outputRate = 192000;
    packet.outputChannels = 8;
    packet.burstInfo = 0x0016;
    packet.payloadLengthCode = kMatFramePayloadLengthCode;
    packet.matFrameSizeBytes = kMatFramePayloadLengthCode;
    packet.burstSizeBytes = kMatBurstSizeBytes;
    packet.paddingBytes = std::max(0, kMatBurstSizeBytes - IEC61937_DATA_OFFSET - kMatFramePayloadLengthCode);
    outPackets.emplace_back(std::move(packet));

    if (pendingBurstPtsUs_ != NO_PTS && pendingBurstDurationUs_ > 0)
      pendingBurstPtsUs_ += pendingBurstDurationUs_;
    pendingBurstDurationUs_ = 0;
    pendingBurstInputBytes_ = 0;
    pendingBurstAccessUnitCount_ = 0;
  }
}

}  // namespace androidx_media3
