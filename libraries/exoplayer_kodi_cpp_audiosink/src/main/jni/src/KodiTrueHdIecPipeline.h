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

#pragma once

#include "cores/AudioEngine/Engines/ActiveAE/ActiveAEStream.h"
#include "cores/AudioEngine/Utils/AEBitstreamPacker.h"
#include "cores/AudioEngine/Utils/PackerMAT.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace androidx_media3
{

struct KodiTrueHdPackedUnit
{
  std::vector<uint8_t> bytes;
  size_t writeOffset{0};
  int inputBytesConsumed{0};
  int64_t ptsUs{std::numeric_limits<int64_t>::min()};
  int64_t durationUs{0};
  int sourceAccessUnitCount{0};
  unsigned int outputRate{192000};
  unsigned int outputChannels{8};
  uint16_t burstInfo{0x0016};
  uint16_t payloadLengthCode{61424};
  int matFrameSizeBytes{61424};
  int burstSizeBytes{61440};
  int paddingBytes{0};
};

class KodiTrueHdIecPipeline
{
public:
  void Configure(const AEAudioFormat& requestedFormat, bool verboseLogging);
  void Reset();

  int Feed(const uint8_t* data,
           int size,
           int64_t presentationTimeUs,
           std::deque<KodiTrueHdPackedUnit>& outPackets,
           int maxPackets = std::numeric_limits<int>::max());
  bool HasParserBacklog() const { return streamAdapter_.HasBacklog(); }
  void AcknowledgeConsumedInputBytes(int bytes);

private:
  static constexpr int64_t NO_PTS = std::numeric_limits<int64_t>::min();
  static constexpr int kMatFramePayloadLengthCode = 61424;
  static constexpr int kMatBurstSizeBytes = 61440;
  static constexpr int kTrueHdFrameOffsetBytes = 2560;

  void EmitPackedMatFrames(std::deque<KodiTrueHdPackedUnit>& outPackets, int maxPackets);

  ActiveAE::CActiveAEMediaStreamAdapter streamAdapter_;
  CPackerMAT matPacker_;
  bool verboseLogging_{false};

  int64_t pendingBurstPtsUs_{NO_PTS};
  int64_t pendingBurstDurationUs_{0};
  int pendingBurstInputBytes_{0};
  int pendingBurstAccessUnitCount_{0};
};

}  // namespace androidx_media3
