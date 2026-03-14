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

#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace androidx_media3
{

struct KodiPackedAccessUnit
{
  std::vector<uint8_t> bytes;
  size_t writeOffset{0};
  int inputBytesConsumed{0};
  int64_t ptsUs{std::numeric_limits<int64_t>::min()};
  int64_t durationUs{0};
  unsigned int outputRate{48000};
  unsigned int outputChannels{2};
  CAEStreamInfo streamInfo{};
};

class KodiIecPipeline
{
public:
  void Configure(const AEAudioFormat& requestedFormat);
  void Reset();

  // Returns bytes consumed from input. Produced IEC packets are appended to outPackets.
  int Feed(const uint8_t* data,
           int size,
           int64_t presentationTimeUs,
           std::deque<KodiPackedAccessUnit>& outPackets,
           int maxPackets = std::numeric_limits<int>::max());
  bool HasParserBacklog() const { return streamAdapter_.HasBacklog(); }
  void AcknowledgeConsumedInputBytes(int bytes);

private:
  static constexpr int64_t NO_PTS = std::numeric_limits<int64_t>::min();

  void EmitPackedPacket(const uint8_t* auData,
                        unsigned int auSize,
                        int64_t auPtsUs,
                        int64_t auDurationUs,
                        std::deque<KodiPackedAccessUnit>& outPackets);

  ActiveAE::CActiveAEMediaStreamAdapter streamAdapter_;
  CAEBitstreamPacker bitstreamPacker_;

  int64_t pendingBurstPtsUs_{NO_PTS};
  int64_t pendingBurstDurationUs_{0};
  int pendingBurstInputBytes_{0};
};

}  // namespace androidx_media3
