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

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace androidx_media3
{

class KodiTrueHdFfmpegMatPacker
{
public:
  KodiTrueHdFfmpegMatPacker();

  void Reset();
  bool PackTrueHd(const uint8_t* data, int size);
  std::vector<uint8_t> GetOutputFrame();

private:
  static constexpr int kMatPacketOffset = 61440;
  static constexpr int kMatFrameSize = 61424;
  static constexpr int kIecHeaderBytes = 8;

  struct MatCode
  {
    unsigned int pos;
    const uint8_t* code;
    unsigned int len;
  };

  void ResetActiveBuffer();
  std::vector<uint8_t> BuildOutputFrame() const;
  static uint16_t ReadBe16(const uint8_t* data);
  static uint32_t ReadBe24(const uint8_t* data);

  int trueHdSamplesPerFrame_{0};
  uint16_t trueHdPrevTime_{0};
  int trueHdPrevSize_{0};
  std::array<std::vector<uint8_t>, 2> buffers_;
  int bufferIndex_{0};
  int bufferFilled_{0};
  std::deque<std::vector<uint8_t>> outputQueue_;
};

}  // namespace androidx_media3
