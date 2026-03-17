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

namespace
{
constexpr int kTrueHdKodiPacketBytes = 61440;
constexpr double kKodiPassthroughTargetBufferSeconds = 0.128;
}

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
                                     bool passthrough,
                                     bool truehdPassthrough)
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
  int targetBufferSize = std::max(minBufferSize, minBufferSize * 2);
  if (passthrough && truehdPassthrough)
  {
    const int sinkFrameSizeBytes = static_cast<int>(safeChannels) * static_cast<int>(sizeof(int16_t));
    if (sinkFrameSizeBytes > 0 && safeSampleRate > 0)
    {
      double bufferSeconds =
          static_cast<double>(minBufferSize) / (sinkFrameSizeBytes * safeSampleRate);
      targetBufferSize = minBufferSize;
      while (bufferSeconds <= kKodiPassthroughTargetBufferSeconds)
      {
        targetBufferSize += minBufferSize;
        bufferSeconds =
            static_cast<double>(targetBufferSize) / (sinkFrameSizeBytes * safeSampleRate);
      }
    }
    targetBufferSize = std::max(targetBufferSize, 2 * kTrueHdKodiPacketBytes);
  }

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
      CJNIAudioManager::AUDIO_SESSION_ID_GENERATE,
      CJNIAudioTrack::ENCAPSULATION_MODE_NONE);
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
  passthroughIec_ = passthrough;
  truehdPassthrough_ = passthrough && truehdPassthrough;
  frameSizeBytes_ = safeChannels * (safeEncoding == CJNIAudioFormat::ENCODING_PCM_FLOAT ? 4 : 2);
  lastPlaybackHead32_ = 0;
  playbackWrapCount_ = 0;
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
  passthroughIec_ = false;
  truehdPassthrough_ = false;
  lastPlaybackHead32_ = 0;
  playbackWrapCount_ = 0;
  writeBuffer_.clear();
  writeShortBuffer_.clear();
}

int KodiAudioTrackOutput::WriteNonBlocking(const uint8_t* data, int size)
{
  if (!track_ || data == nullptr || size <= 0)
    return 0;

  if (passthroughIec_)
  {
    const int sampleCount = size / static_cast<int>(sizeof(int16_t));
    if (sampleCount <= 0)
      return 0;
    if (static_cast<int>(writeShortBuffer_.size()) < sampleCount)
      writeShortBuffer_.resize(sampleCount);
    std::memcpy(writeShortBuffer_.data(), data, static_cast<size_t>(sampleCount) * sizeof(int16_t));
    const int writtenSamples =
        track_->write(writeShortBuffer_, 0, sampleCount, CJNIAudioTrack::WRITE_NON_BLOCKING);
    return writtenSamples > 0 ? writtenSamples * static_cast<int>(sizeof(int16_t)) : writtenSamples;
  }

  if (static_cast<int>(writeBuffer_.size()) < size)
    writeBuffer_.resize(size);
  std::memcpy(writeBuffer_.data(), data, size);
  return track_->write(writeBuffer_, 0, size, CJNIAudioTrack::WRITE_NON_BLOCKING);
}

int KodiAudioTrackOutput::WriteBlocking(const uint8_t* data, int size)
{
  if (!track_ || data == nullptr || size <= 0)
    return 0;

  if (passthroughIec_)
  {
    const int sampleCount = size / static_cast<int>(sizeof(int16_t));
    if (sampleCount <= 0)
      return 0;
    if (static_cast<int>(writeShortBuffer_.size()) < sampleCount)
      writeShortBuffer_.resize(sampleCount);
    std::memcpy(writeShortBuffer_.data(), data, static_cast<size_t>(sampleCount) * sizeof(int16_t));
    const int writtenSamples =
        track_->write(writeShortBuffer_, 0, sampleCount, CJNIAudioTrack::WRITE_BLOCKING);
    return writtenSamples > 0 ? writtenSamples * static_cast<int>(sizeof(int16_t)) : writtenSamples;
  }

  if (static_cast<int>(writeBuffer_.size()) < size)
    writeBuffer_.resize(size);
  std::memcpy(writeBuffer_.data(), data, size);
  return track_->write(writeBuffer_, 0, size, CJNIAudioTrack::WRITE_BLOCKING);
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
  }
  lastPlaybackHead32_ = head32;

  return (playbackWrapCount_ << 32) | head32;
}

bool KodiAudioTrackOutput::GetTimestamp(uint64_t* framePosition, int64_t* systemTimeUs)
{
  if (!track_ || framePosition == nullptr || systemTimeUs == nullptr)
    return false;

  CJNIAudioTimestamp ts;
  if (!track_->getTimestamp(ts))
    return false;

  *framePosition = ts.get_framePosition();
  *systemTimeUs = ts.get_nanoTime() / 1000;
  return true;
}

int KodiAudioTrackOutput::GetBufferSizeInFrames() const
{
  if (!track_)
    return 0;
  return track_->getBufferSizeInFrames();
}

bool KodiAudioTrackOutput::IsPlaying() const
{
  return track_ != nullptr && track_->getPlayState() == CJNIAudioTrack::PLAYSTATE_PLAYING;
}

}  // namespace androidx_media3
