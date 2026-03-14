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

#include "androidjni/AudioAttributes.h"
#include "androidjni/AudioFormat.h"
#include "androidjni/AudioManager.h"
#include "androidjni/AudioTrack.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace androidx_media3
{

class KodiAudioTrackOutput
{
public:
  bool Configure(unsigned int sampleRate,
                 unsigned int channelCount,
                 int encoding,
                 bool passthrough);
  bool Play();
  void Pause();
  void Flush();
  void Release();
  int WriteNonBlocking(const uint8_t* data, int size);
  uint64_t GetPlaybackFrames64();
  bool GetTimestamp(uint64_t* framePosition, int64_t* systemTimeUs);
  int GetBufferSizeInFrames() const;
  bool IsPlaying() const;
  bool IsConfigured() const { return track_ != nullptr; }
  unsigned int SampleRate() const { return sampleRate_; }
  unsigned int ChannelCount() const { return channelCount_; }
  unsigned int FrameSizeBytes() const { return frameSizeBytes_; }

private:
  static int ChannelMaskForCount(unsigned int channelCount);

  std::unique_ptr<CJNIAudioTrack> track_;
  std::vector<char> writeBuffer_;
  uint32_t lastPlaybackHead32_{0};
  uint64_t playbackWrapCount_{0};
  unsigned int sampleRate_{0};
  unsigned int channelCount_{0};
  unsigned int frameSizeBytes_{0};
  int encoding_{CJNIAudioFormat::ENCODING_PCM_16BIT};
};

}  // namespace androidx_media3
