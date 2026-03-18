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

namespace androidx_media3
{

bool KodiTrueHdAudioTrackOutput::Configure(unsigned int sampleRate,
                                           unsigned int channelCount,
                                           int encoding,
                                           bool passthrough)
{
  CLog::Log(LOGINFO,
            "KodiTrueHdAudioTrackOutput::Configure sampleRate={} channelCount={} encoding={} "
            "passthrough={}",
            sampleRate,
            channelCount,
            encoding,
            passthrough);
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
  return output_.WriteNonBlocking(data, size);
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
