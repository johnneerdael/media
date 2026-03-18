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

#include "KodiAudioTrackOutput.h"

#include <cstdint>

namespace androidx_media3
{

class KodiTrueHdAudioTrackOutput
{
public:
  bool Configure(unsigned int sampleRate,
                 unsigned int channelCount,
                 int encoding,
                 bool passthrough);
  void SetVerboseLogging(bool verboseLogging);
  bool Play();
  void Pause();
  void Flush();
  void Release();
  int WriteNonBlocking(const uint8_t* data, int size);
  uint64_t GetPlaybackFrames64();
  bool GetTimestamp(uint64_t* framePosition, int64_t* systemTimeUs);
  int GetBufferSizeInFrames() const;
  bool IsPlaying() const;
  bool IsConfigured() const;
  unsigned int SampleRate() const;
  unsigned int ChannelCount() const;
  unsigned int FrameSizeBytes() const;

private:
  KodiAudioTrackOutput output_;
  bool verboseLogging_{false};
};

}  // namespace androidx_media3
