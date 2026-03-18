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

#include "KodiTrueHdAudioTrackOutput.h"

#include "utils/log.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace androidx_media3
{

namespace
{
uint32_t HashBytes(const uint8_t* data, size_t size)
{
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i)
  {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

std::string HexPreview(const uint8_t* data, size_t size, size_t maxBytes)
{
  if (data == nullptr || size == 0 || maxBytes == 0)
    return "";

  const size_t count = std::min(size, maxBytes);
  std::string result;
  result.reserve(count * 3);
  char chunk[4];
  for (size_t i = 0; i < count; ++i)
  {
    std::snprintf(chunk, sizeof(chunk), "%02X", data[i]);
    if (!result.empty())
      result.push_back(' ');
    result.append(chunk);
  }
  return result;
}
}  // namespace

void KodiTrueHdAudioTrackOutput::SetVerboseLogging(bool verboseLogging)
{
  verboseLogging_ = verboseLogging;
}

bool KodiTrueHdAudioTrackOutput::Configure(unsigned int sampleRate,
                                           unsigned int channelCount,
                                           int encoding,
                                           bool passthrough)
{
  if (verboseLogging_)
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAudioTrackOutput::Configure sampleRate={} channelCount={} encoding={} "
              "passthrough={}",
              sampleRate,
              channelCount,
              encoding,
              passthrough);
  }
  return output_.Configure(sampleRate, channelCount, encoding, passthrough);
}

bool KodiTrueHdAudioTrackOutput::Play()
{
  return output_.Play();
}

void KodiTrueHdAudioTrackOutput::Pause()
{
  output_.Pause();
}

void KodiTrueHdAudioTrackOutput::Flush()
{
  output_.Flush();
}

void KodiTrueHdAudioTrackOutput::Release()
{
  output_.Release();
}

int KodiTrueHdAudioTrackOutput::WriteNonBlocking(const uint8_t* data, int size)
{
  const int written = output_.WriteNonBlocking(data, size);
  if (verboseLogging_ && data != nullptr && size > 0)
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAudioTrackOutput::WriteNonBlocking size={} written={} crc=0x{:08x} "
              "preview={}",
              size,
              written,
              HashBytes(data, static_cast<size_t>(size)),
              HexPreview(data, static_cast<size_t>(size), 32));
  }
  return written;
}

uint64_t KodiTrueHdAudioTrackOutput::GetPlaybackFrames64()
{
  return output_.GetPlaybackFrames64();
}

bool KodiTrueHdAudioTrackOutput::GetTimestamp(uint64_t* framePosition, int64_t* systemTimeUs)
{
  return output_.GetTimestamp(framePosition, systemTimeUs);
}

int KodiTrueHdAudioTrackOutput::GetBufferSizeInFrames() const
{
  return output_.GetBufferSizeInFrames();
}

bool KodiTrueHdAudioTrackOutput::IsPlaying() const
{
  return output_.IsPlaying();
}

bool KodiTrueHdAudioTrackOutput::IsConfigured() const
{
  return output_.IsConfigured();
}

unsigned int KodiTrueHdAudioTrackOutput::SampleRate() const
{
  return output_.SampleRate();
}

unsigned int KodiTrueHdAudioTrackOutput::ChannelCount() const
{
  return output_.ChannelCount();
}

unsigned int KodiTrueHdAudioTrackOutput::FrameSizeBytes() const
{
  return output_.FrameSizeBytes();
}

}  // namespace androidx_media3
