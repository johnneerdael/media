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

#include "KodiTrueHdAEEngine.h"

#include "androidjni/AudioFormat.h"

#include <algorithm>
#include "utils/log.h"

namespace androidx_media3
{

namespace
{
int64_t NowUs()
{
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void KodiTrueHdAEEngine::MarkReleasePendingLocked()
{
  releasePendingUntilUs_ = NowUs() + RELEASE_PENDING_HOLD_US;
}

bool KodiTrueHdAEEngine::IsReleasePendingLocked(int64_t nowUs) const
{
  return releasePendingUntilUs_ != CURRENT_POSITION_NOT_SET && nowUs < releasePendingUntilUs_;
}

bool KodiTrueHdAEEngine::Configure(const ActiveAE::CActiveAEMediaSettings& config)
{
  std::unique_lock lock(lock_);
  config_ = config;
  requestedFormat_ = ActiveAE::CActiveAESettings::BuildFormatForMediaSource(config);
  passthrough_ = requestedFormat_.m_dataFormat == AE_FMT_RAW &&
                 requestedFormat_.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD;
  configured_ = passthrough_;
  playRequested_ = false;
  outputStarted_ = false;
  ended_ = false;
  volume_ = config.volume;
  hostClockUs_ = CURRENT_POSITION_NOT_SET;
  hostClockSpeed_ = 1.0;
  packedQueue_.clear();
  totalWrittenFrames_ = 0;
  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  iecPipeline_.Configure(requestedFormat_, config.iecVerboseLogging);
  output_.Release();
  output_.SetVerboseLogging(config.iecVerboseLogging);
  MarkReleasePendingLocked();
  CLog::Log(LOGINFO,
            "KodiTrueHdAEEngine::Configure mimeKind={} sampleRate={} channelCount={} passthrough={}",
            static_cast<int>(config.mimeKind),
            config.sampleRate,
            config.channelCount,
            passthrough_);
  return configured_;
}

int KodiTrueHdAEEngine::Write(const uint8_t* data,
                              int size,
                              int64_t presentation_time_us,
                              int encoded_access_unit_count)
{
  std::unique_lock lock(lock_);
  if (!configured_ || data == nullptr || size <= 0)
    return 0;

  (void)encoded_access_unit_count;
  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  ended_ = false;

  const int consumed = iecPipeline_.Feed(data, size, presentation_time_us, packedQueue_);
  if (config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAEEngine::Write consumed={} queuedPackets={} ptsUs={} "
              "releasePending={}",
              consumed,
              packedQueue_.size(),
              presentation_time_us,
              IsReleasePendingLocked(NowUs()));
  }
  if (!packedQueue_.empty())
    EnsureOutputConfiguredLocked();
  if (playRequested_)
    FlushPackedQueueToHardwareLocked();
  return consumed;
}

void KodiTrueHdAEEngine::Play()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return;
  playRequested_ = true;
  if (!packedQueue_.empty())
  {
    EnsureOutputConfiguredLocked();
    FlushPackedQueueToHardwareLocked();
  }
}

void KodiTrueHdAEEngine::Pause()
{
  std::unique_lock lock(lock_);
  playRequested_ = false;
  outputStarted_ = false;
  output_.Pause();
}

void KodiTrueHdAEEngine::Flush()
{
  std::unique_lock lock(lock_);
  playRequested_ = false;
  outputStarted_ = false;
  packedQueue_.clear();
  totalWrittenFrames_ = 0;
  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  ended_ = false;
  iecPipeline_.Reset();
  output_.Release();
  MarkReleasePendingLocked();
}

void KodiTrueHdAEEngine::Drain()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return;
  EnsureOutputConfiguredLocked();
  FlushPackedQueueToHardwareLocked();
  ended_ = packedQueue_.empty() && !iecPipeline_.HasParserBacklog();
}

void KodiTrueHdAEEngine::HandleDiscontinuity()
{
  std::unique_lock lock(lock_);
  packedQueue_.clear();
  iecPipeline_.Reset();
  output_.Release();
  outputStarted_ = false;
  totalWrittenFrames_ = 0;
}

void KodiTrueHdAEEngine::SetVolume(float volume)
{
  std::unique_lock lock(lock_);
  volume_ = volume;
}

void KodiTrueHdAEEngine::SetHostClockUs(int64_t host_clock_us)
{
  std::unique_lock lock(lock_);
  hostClockUs_ = host_clock_us;
}

void KodiTrueHdAEEngine::SetHostClockSpeed(double speed)
{
  std::unique_lock lock(lock_);
  hostClockSpeed_ = speed > 0.0 ? speed : 1.0;
}

int64_t KodiTrueHdAEEngine::GetCurrentPositionUs()
{
  std::unique_lock lock(lock_);
  if (!output_.IsConfigured() || output_.SampleRate() == 0)
    return CURRENT_POSITION_NOT_SET;
  return static_cast<int64_t>(output_.GetPlaybackFrames64() * 1000000ULL / output_.SampleRate());
}

bool KodiTrueHdAEEngine::HasPendingData()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return false;
  if (!packedQueue_.empty() || iecPipeline_.HasParserBacklog())
    return true;
  if (!output_.IsConfigured())
    return false;
  return totalWrittenFrames_ > output_.GetPlaybackFrames64();
}

bool KodiTrueHdAEEngine::IsEnded()
{
  std::unique_lock lock(lock_);
  if (!ended_)
    return false;
  if (!packedQueue_.empty() || iecPipeline_.HasParserBacklog())
    return false;
  if (!output_.IsConfigured())
    return true;
  return totalWrittenFrames_ <= output_.GetPlaybackFrames64();
}

int64_t KodiTrueHdAEEngine::GetBufferSizeUs() const
{
  std::unique_lock lock(lock_);
  if (!output_.IsConfigured() || output_.SampleRate() == 0)
    return 0;
  return static_cast<int64_t>(output_.GetBufferSizeInFrames()) * 1000000LL / output_.SampleRate();
}

int KodiTrueHdAEEngine::ConsumeLastWriteOutputBytes()
{
  std::unique_lock lock(lock_);
  const int value = lastWriteOutputBytes_;
  lastWriteOutputBytes_ = 0;
  return value;
}

int KodiTrueHdAEEngine::ConsumeLastWriteErrorCode()
{
  std::unique_lock lock(lock_);
  const int value = lastWriteErrorCode_;
  lastWriteErrorCode_ = 0;
  return value;
}

bool KodiTrueHdAEEngine::IsReleasePending()
{
  std::unique_lock lock(lock_);
  return IsReleasePendingLocked(NowUs());
}

void KodiTrueHdAEEngine::Reset()
{
  std::unique_lock lock(lock_);
  configured_ = false;
  playRequested_ = false;
  outputStarted_ = false;
  ended_ = false;
  passthrough_ = false;
  packedQueue_.clear();
  totalWrittenFrames_ = 0;
  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  iecPipeline_.Reset();
  output_.Release();
  releasePendingUntilUs_ = CURRENT_POSITION_NOT_SET;
}

bool KodiTrueHdAEEngine::EnsureOutputConfiguredLocked()
{
  if (output_.IsConfigured())
    return true;
  if (packedQueue_.empty())
    return false;

  const KodiTrueHdPackedUnit& packet = packedQueue_.front();
  const int encoding = CJNIAudioFormat::ENCODING_IEC61937 > 0
                           ? CJNIAudioFormat::ENCODING_IEC61937
                           : CJNIAudioFormat::ENCODING_PCM_16BIT;
  if (config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAEEngine::EnsureOutputConfigured outputRate={} outputChannels={} "
              "encoding={} burstBytes={} pc=0x{:04x} pd={}",
              packet.outputRate,
              packet.outputChannels,
              encoding,
              packet.burstSizeBytes,
              packet.burstInfo,
              packet.payloadLengthCode);
  }
  return output_.Configure(packet.outputRate, packet.outputChannels, encoding, true);
}

int KodiTrueHdAEEngine::FlushPackedQueueToHardwareLocked()
{
  if (!EnsureOutputConfiguredLocked())
    return 0;

  if (playRequested_ && !outputStarted_)
    outputStarted_ = output_.Play();

  int writtenTotal = 0;
  while (!packedQueue_.empty())
  {
    KodiTrueHdPackedUnit& packet = packedQueue_.front();
    const int remaining = static_cast<int>(packet.bytes.size() - packet.writeOffset);
    if (remaining <= 0)
    {
      iecPipeline_.AcknowledgeConsumedInputBytes(packet.inputBytesConsumed);
      packedQueue_.pop_front();
      continue;
    }

    const int written =
        output_.WriteNonBlocking(packet.bytes.data() + packet.writeOffset, remaining);
    if (written <= 0)
    {
      lastWriteErrorCode_ = written < 0 ? written : lastWriteErrorCode_;
      break;
    }

    packet.writeOffset += static_cast<size_t>(written);
    lastWriteOutputBytes_ += written;
    writtenTotal += written;

    if (config_.iecVerboseLogging)
    {
      CLog::Log(LOGINFO,
                "KodiTrueHdAEEngine::FlushPackedQueueToHardware writeRemaining={} written={} "
                "writeOffset={} burstBytes={} inputBytes={} ptsUs={} auCount={}",
                remaining,
                written,
                packet.writeOffset,
                packet.burstSizeBytes,
                packet.inputBytesConsumed,
                packet.ptsUs,
                packet.sourceAccessUnitCount);
    }

    const unsigned int frameSizeBytes = output_.FrameSizeBytes();
    if (frameSizeBytes > 0)
      totalWrittenFrames_ += static_cast<uint64_t>(written / frameSizeBytes);

    if (packet.writeOffset >= packet.bytes.size())
    {
      iecPipeline_.AcknowledgeConsumedInputBytes(packet.inputBytesConsumed);
      packedQueue_.pop_front();
    }
    else
    {
      break;
    }
  }

  return writtenTotal;
}

}  // namespace androidx_media3
