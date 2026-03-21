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

#include "KodiTrueHdFfmpegMatPacker.h"

#include <algorithm>
#include <cstring>

namespace androidx_media3
{

namespace
{
constexpr uint32_t kFormatMajorSync = 0xF8726F;

constexpr std::array<uint8_t, 20> kMatStartCode = {
    0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00,
    0x56, 0xA5, 0x3B, 0xF4, 0x81, 0x83, 0x49, 0x80, 0x77, 0xE0};
constexpr std::array<uint8_t, 12> kMatMiddleCode = {
    0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA, 0x82, 0x83, 0x49, 0x80, 0x77, 0xE0};
constexpr std::array<uint8_t, 16> kMatEndCode = {
    0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x11};
}  // namespace

KodiTrueHdFfmpegMatPacker::KodiTrueHdFfmpegMatPacker()
{
  Reset();
}

void KodiTrueHdFfmpegMatPacker::Reset()
{
  trueHdSamplesPerFrame_ = 0;
  trueHdPrevTime_ = 0;
  trueHdPrevSize_ = 0;
  bufferIndex_ = 0;
  bufferFilled_ = 0;
  outputQueue_.clear();
  buffers_[0].assign(kMatFrameSize, 0);
  buffers_[1].assign(kMatFrameSize, 0);
}

bool KodiTrueHdFfmpegMatPacker::PackTrueHd(const uint8_t* data, int size)
{
  if (data == nullptr || size < 10)
    return false;

  if (ReadBe24(data + 4) == kFormatMajorSync)
  {
    int ratebits = 0;
    if (data[7] == 0xBA)
      ratebits = data[8] >> 4;
    else if (data[7] == 0xBB)
      ratebits = data[9] >> 4;
    else
      return false;

    trueHdSamplesPerFrame_ = 40 << (ratebits & 3);
  }

  if (trueHdSamplesPerFrame_ == 0)
    return false;

  static constexpr MatCode kMatCodes[] = {
      {0, kMatStartCode.data(), static_cast<unsigned int>(kMatStartCode.size())},
      {30708, kMatMiddleCode.data(), static_cast<unsigned int>(kMatMiddleCode.size())},
      {kMatFrameSize - static_cast<int>(kMatEndCode.size()),
       kMatEndCode.data(),
       static_cast<unsigned int>(kMatEndCode.size())},
  };

  uint8_t* hdBuf = buffers_[bufferIndex_].data();
  const uint16_t inputTiming = ReadBe16(data + 2);
  int paddingRemaining = 0;
  int totalFrameSize = size;
  const uint8_t* dataPtr = data;
  int dataRemaining = size;
  bool havePacket = false;

  if (trueHdPrevSize_ > 0)
  {
    const uint16_t deltaSamples = static_cast<uint16_t>(inputTiming - trueHdPrevTime_);
    const int deltaBytes = deltaSamples * 2560 / trueHdSamplesPerFrame_;
    paddingRemaining = deltaBytes - trueHdPrevSize_;
    if (paddingRemaining < 0 || paddingRemaining >= kMatFrameSize / 2)
      paddingRemaining = 0;
  }

  size_t nextCodeIndex = 0;
  while (nextCodeIndex < std::size(kMatCodes) &&
         bufferFilled_ > static_cast<int>(kMatCodes[nextCodeIndex].pos))
  {
    ++nextCodeIndex;
  }
  if (nextCodeIndex >= std::size(kMatCodes))
    return false;

  while (paddingRemaining > 0 || dataRemaining > 0 ||
         static_cast<int>(kMatCodes[nextCodeIndex].pos) == bufferFilled_)
  {
    if (static_cast<int>(kMatCodes[nextCodeIndex].pos) == bufferFilled_)
    {
      int codeLenRemaining = static_cast<int>(kMatCodes[nextCodeIndex].len);
      std::memcpy(hdBuf + kMatCodes[nextCodeIndex].pos,
                  kMatCodes[nextCodeIndex].code,
                  kMatCodes[nextCodeIndex].len);
      bufferFilled_ += static_cast<int>(kMatCodes[nextCodeIndex].len);

      ++nextCodeIndex;
      if (nextCodeIndex == std::size(kMatCodes))
      {
        nextCodeIndex = 0;
        havePacket = true;
        outputQueue_.push_back(BuildOutputFrame());
        bufferIndex_ ^= 1;
        ResetActiveBuffer();
        hdBuf = buffers_[bufferIndex_].data();
        codeLenRemaining += kMatPacketOffset - kMatFrameSize;
      }

      if (paddingRemaining > 0)
      {
        const int countedAsPadding = std::min(paddingRemaining, codeLenRemaining);
        paddingRemaining -= countedAsPadding;
        codeLenRemaining -= countedAsPadding;
      }
      if (codeLenRemaining > 0)
        totalFrameSize += codeLenRemaining;
    }

    if (paddingRemaining > 0)
    {
      const int paddingToInsert =
          std::min(static_cast<int>(kMatCodes[nextCodeIndex].pos) - bufferFilled_,
                   paddingRemaining);
      std::memset(hdBuf + bufferFilled_, 0, paddingToInsert);
      bufferFilled_ += paddingToInsert;
      paddingRemaining -= paddingToInsert;
      if (paddingRemaining > 0)
        continue;
    }

    if (dataRemaining > 0)
    {
      const int dataToInsert =
          std::min(static_cast<int>(kMatCodes[nextCodeIndex].pos) - bufferFilled_, dataRemaining);
      std::memcpy(hdBuf + bufferFilled_, dataPtr, dataToInsert);
      bufferFilled_ += dataToInsert;
      dataPtr += dataToInsert;
      dataRemaining -= dataToInsert;
    }
  }

  trueHdPrevSize_ = totalFrameSize;
  trueHdPrevTime_ = inputTiming;
  return havePacket;
}

std::vector<uint8_t> KodiTrueHdFfmpegMatPacker::GetOutputFrame()
{
  if (outputQueue_.empty())
    return {};
  auto buffer = std::move(outputQueue_.front());
  outputQueue_.pop_front();
  return buffer;
}

void KodiTrueHdFfmpegMatPacker::ResetActiveBuffer()
{
  std::fill(buffers_[bufferIndex_].begin(), buffers_[bufferIndex_].end(), 0);
  bufferFilled_ = 0;
}

std::vector<uint8_t> KodiTrueHdFfmpegMatPacker::BuildOutputFrame() const
{
  std::vector<uint8_t> frame(kIecHeaderBytes + kMatFrameSize, 0);
  std::memcpy(frame.data() + kIecHeaderBytes, buffers_[bufferIndex_].data(), kMatFrameSize);
  return frame;
}

uint16_t KodiTrueHdFfmpegMatPacker::ReadBe16(const uint8_t* data)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                               static_cast<uint16_t>(data[1]));
}

uint32_t KodiTrueHdFfmpegMatPacker::ReadBe24(const uint8_t* data)
{
  return (static_cast<uint32_t>(data[0]) << 16) |
         (static_cast<uint32_t>(data[1]) << 8) |
         static_cast<uint32_t>(data[2]);
}

}  // namespace androidx_media3
