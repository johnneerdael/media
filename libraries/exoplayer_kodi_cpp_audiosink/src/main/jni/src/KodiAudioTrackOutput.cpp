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

#include "KodiAudioTrackOutput.h"

#include "utils/log.h"

#include <algorithm>
#include <cstring>

namespace androidx_media3
{

int KodiAudioTrackOutput::ChannelMaskForCount(unsigned int channelCount)
{
  if (channelCount > 6)
    return CJNIAudioFormat::CHANNEL_OUT_7POINT1_SURROUND;
  if (channelCount > 2)
    return CJNIAudioFormat::CHANNEL_OUT_5POINT1;
  return CJNIAudioFormat::CHANNEL_OUT_STEREO;
}

bool KodiAudioTrackOutput::Configure(unsigned int sampleRate,
                                     unsigned int channelCount,
                                     int encoding,
                                     bool passthrough)
{
  const unsigned int safeSampleRate = sampleRate > 0 ? sampleRate : 48000;
  const unsigned int safeChannels = std::max(1u, channelCount);

  int safeEncoding = encoding;
  if (passthrough)
  {
    safeEncoding = CJNIAudioFormat::ENCODING_IEC61937 > 0 ? CJNIAudioFormat::ENCODING_IEC61937
                                                           : CJNIAudioFormat::ENCODING_PCM_16BIT;
  }
  else if (safeEncoding <= 0)
  {
    safeEncoding = CJNIAudioFormat::ENCODING_PCM_16BIT;
  }

  const int channelMask = ChannelMaskForCount(safeChannels);
  int minBufferSize = CJNIAudioTrack::getMinBufferSize(safeSampleRate, channelMask, safeEncoding);
  if (minBufferSize <= 0)
    minBufferSize = static_cast<int>(safeSampleRate * safeChannels * 2 / 4); // ~250ms fallback
  const int targetBufferSize = std::max(minBufferSize, minBufferSize * 2);

  CJNIAudioAttributesBuilder attrBuilder;
  attrBuilder.setUsage(CJNIAudioAttributes::USAGE_MEDIA);
  attrBuilder.setContentType(CJNIAudioAttributes::CONTENT_TYPE_MUSIC);

  CJNIAudioFormatBuilder fmtBuilder;
  fmtBuilder.setChannelMask(channelMask);
  fmtBuilder.setEncoding(safeEncoding);
  fmtBuilder.setSampleRate(static_cast<int>(safeSampleRate));

  auto newTrack = std::make_unique<CJNIAudioTrack>(
      attrBuilder.build(),
      fmtBuilder.build(),
      targetBufferSize,
      CJNIAudioTrack::MODE_STREAM,
      CJNIAudioManager::AUDIO_SESSION_ID_GENERATE);
  if (!newTrack || newTrack->getState() != CJNIAudioTrack::STATE_INITIALIZED)
  {
    CLog::Log(LOGERROR,
              "KodiAudioTrackOutput::Configure failed sampleRate={} channels={} encoding={}",
              safeSampleRate,
              safeChannels,
              safeEncoding);
    return false;
  }

  newTrack->pause();
  newTrack->flush();

  track_ = std::move(newTrack);
  sampleRate_ = safeSampleRate;
  channelCount_ = safeChannels;
  encoding_ = safeEncoding;
  frameSizeBytes_ = safeChannels * (safeEncoding == CJNIAudioFormat::ENCODING_PCM_FLOAT ? 4 : 2);
  lastPlaybackHead32_ = 0;
  playbackWrapCount_ = 0;
  restartFrameOffset_ = 0;
  lastTimestampFramePosition_ = 0;
  lastObservedUnderrunCount_ = track_->getUnderrunCount();
  accumulatedUnderrunCount_ = 0;
  restartCount_ = 0;
  return true;
}

bool KodiAudioTrackOutput::Play()
{
  if (!track_)
    return false;

  track_->play();
  return track_->getState() == CJNIAudioTrack::STATE_INITIALIZED &&
         track_->getPlayState() == CJNIAudioTrack::PLAYSTATE_PLAYING;
}

void KodiAudioTrackOutput::Pause()
{
  if (track_)
    track_->pause();
}

void KodiAudioTrackOutput::Flush()
{
  if (!track_)
    return;
  track_->pause();
  track_->flush();
  lastPlaybackHead32_ = 0;
  playbackWrapCount_ = 0;
  restartFrameOffset_ = 0;
  lastTimestampFramePosition_ = 0;
  lastObservedUnderrunCount_ = track_->getUnderrunCount();
}

void KodiAudioTrackOutput::Release()
{
  if (track_)
  {
    track_->release();
    track_.reset();
  }
  sampleRate_ = 0;
  channelCount_ = 0;
  frameSizeBytes_ = 0;
  encoding_ = CJNIAudioFormat::ENCODING_PCM_16BIT;
  lastPlaybackHead32_ = 0;
  playbackWrapCount_ = 0;
  restartFrameOffset_ = 0;
  lastTimestampFramePosition_ = 0;
  lastObservedUnderrunCount_ = -1;
  accumulatedUnderrunCount_ = 0;
  restartCount_ = 0;
  writeBuffer_.clear();
}

int KodiAudioTrackOutput::WriteNonBlocking(const uint8_t* data, int size)
{
  if (!track_ || data == nullptr || size <= 0)
    return 0;

  if (static_cast<int>(writeBuffer_.size()) < size)
    writeBuffer_.resize(size);
  std::memcpy(writeBuffer_.data(), data, size);
  return track_->write(writeBuffer_, 0, size, CJNIAudioTrack::WRITE_NON_BLOCKING);
}

uint64_t KodiAudioTrackOutput::GetPlaybackFrames64()
{
  if (!track_)
    return 0;

  const uint32_t head32 = static_cast<uint32_t>(track_->getPlaybackHeadPosition());
  if (head32 < lastPlaybackHead32_)
  {
    const uint32_t backwardDelta = lastPlaybackHead32_ - head32;
    // Natural 32-bit wrap only happens near UINT32 rollover. Small backward jumps are
    // typically AudioTrack reset/recreation artifacts on direct/offload outputs.
    if (backwardDelta > 0x40000000u)
      ++playbackWrapCount_;
    else
    {
      restartFrameOffset_ += lastPlaybackHead32_;
      ++restartCount_;
    }
  }
  lastPlaybackHead32_ = head32;

  return restartFrameOffset_ + ((playbackWrapCount_ << 32) | head32);
}

bool KodiAudioTrackOutput::GetTimestamp(uint64_t* framePosition, int64_t* systemTimeUs)
{
  if (!track_ || framePosition == nullptr || systemTimeUs == nullptr)
    return false;

  CJNIAudioTimestamp ts;
  if (!track_->getTimestamp(ts))
    return false;

  const uint64_t adjustedFramePosition = restartFrameOffset_ + ts.get_framePosition();
  if (adjustedFramePosition < lastTimestampFramePosition_)
    *framePosition = lastTimestampFramePosition_;
  else
    *framePosition = adjustedFramePosition;
  lastTimestampFramePosition_ = *framePosition;
  *systemTimeUs = ts.get_nanoTime() / 1000;
  return true;
}

int KodiAudioTrackOutput::GetBufferSizeInFrames() const
{
  if (!track_)
    return 0;
  return track_->getBufferSizeInFrames();
}

int KodiAudioTrackOutput::GetUnderrunCount() const
{
  if (!track_)
    return accumulatedUnderrunCount_;
  const int currentUnderrunCount = track_->getUnderrunCount();
  if (currentUnderrunCount < 0)
    return accumulatedUnderrunCount_;
  if (lastObservedUnderrunCount_ < 0)
  {
    lastObservedUnderrunCount_ = currentUnderrunCount;
    return accumulatedUnderrunCount_;
  }
  if (currentUnderrunCount > lastObservedUnderrunCount_)
    accumulatedUnderrunCount_ += currentUnderrunCount - lastObservedUnderrunCount_;
  lastObservedUnderrunCount_ = currentUnderrunCount;
  return accumulatedUnderrunCount_;
}

bool KodiAudioTrackOutput::IsPlaying() const
{
  return track_ != nullptr && track_->getPlayState() == CJNIAudioTrack::PLAYSTATE_PLAYING;
}

}  // namespace androidx_media3
