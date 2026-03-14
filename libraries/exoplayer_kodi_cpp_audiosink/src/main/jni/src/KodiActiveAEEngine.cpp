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

#include "KodiActiveAEEngine.h"

#include "ServiceBroker.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace androidx_media3
{

KodiActiveAEEngine::~KodiActiveAEEngine()
{
  Reset();
}

void KodiActiveAEEngine::MarkReleasePendingLocked()
{
  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  releasePendingUntilUs_ = nowUs + RELEASE_PENDING_HOLD_US;
}

bool KodiActiveAEEngine::IsReleasePendingLocked(int64_t nowUs) const
{
  return releasePendingUntilUs_ != CURRENT_POSITION_NOT_SET && nowUs < releasePendingUntilUs_;
}

bool KodiActiveAEEngine::Configure(const ActiveAE::CActiveAEMediaSettings& config)
{
  std::unique_lock lock(lock_);
  config_ = config;
  requestedFormat_ = ActiveAE::CActiveAESettings::BuildFormatForMediaSource(config);
  passthrough_ = requestedFormat_.m_dataFormat == AE_FMT_RAW;
  configured_ = true;
  playRequested_ = false;
  outputStarted_ = false;
  ended_ = false;
  volume_ = config.volume;
  hasPendingData_ = false;

  packedQueue_.clear();
  pcmQueue_.clear();
  queuedDurationUs_ = 0;
  firstQueuedPtsUs_ = NO_PTS;
  pendingPassthroughAckBytes_ = 0;
  totalWrittenFrames_ = 0;
  iecPipeline_.Configure(requestedFormat_);
  output_.Release();
  MarkReleasePendingLocked();
  ResetPositionLocked();
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  startupPhase_ = StartupPhase::IDLE;

  CLog::Log(LOGINFO,
            ActiveAE::CActiveAESettings::DescribeMediaSourceConfiguration(config, requestedFormat_));
  return true;
}

int KodiActiveAEEngine::Write(const uint8_t* data,
                              int size,
                              int64_t presentation_time_us,
                              int encoded_access_unit_count)
{
  std::unique_lock lock(lock_);
  if (!configured_ || data == nullptr || size <= 0)
    return 0;

  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  ended_ = false;
  const int consumed =
      passthrough_
          ? WritePassthroughLocked(data, size, presentation_time_us, encoded_access_unit_count)
          : WritePcmLocked(data, size, presentation_time_us);
  if (consumed > 0 && !playRequested_)
  {
    lastPrePlayAcceptSystemTimeUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
  }
  hasPendingData_ = HasPendingData();
  return consumed;
}

void KodiActiveAEEngine::Play()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return;

  playRequested_ = true;
  if (!passthrough_)
    EnsurePcmOutputConfiguredLocked();
  // Resume/start from a clean estimator state to avoid stale timestamp/playhead
  // artifacts after long pause intervals on direct outputs.
  ResetOutputPositionEstimatorLocked();
  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  const int64_t prePlayAcceptGapUs =
      lastPrePlayAcceptSystemTimeUs_ != CURRENT_POSITION_NOT_SET && nowUs >= lastPrePlayAcceptSystemTimeUs_
          ? (nowUs - lastPrePlayAcceptSystemTimeUs_)
          : -1;
  const int64_t prePlayWriteGapUs =
      lastPrePlayWriteSystemTimeUs_ != CURRENT_POSITION_NOT_SET && nowUs >= lastPrePlayWriteSystemTimeUs_
          ? (nowUs - lastPrePlayWriteSystemTimeUs_)
          : -1;
  CLog::Log(LOGINFO,
            "KodiActiveAEEngine::Play startup prePlayAcceptGapUs={} prePlayWriteGapUs={} "
            "packedQueue={} pcmQueue={} totalWrittenFrames={} safePlayedFrames={}",
            prePlayAcceptGapUs,
            prePlayWriteGapUs,
            packedQueue_.size(),
            pcmQueue_.size(),
            totalWrittenFrames_,
            GetSafePlayedFramesLocked());
  StartOutputIfPrimedLocked();
}

void KodiActiveAEEngine::Pause()
{
  std::unique_lock lock(lock_);
  playRequested_ = false;
  outputStarted_ = false;
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  output_.Pause();
  // Pause/resume on direct tracks can return stale timestamps; clear estimator
  // state so next position samples rebuild from fresh data.
  ResetOutputPositionEstimatorLocked();
}

void KodiActiveAEEngine::Flush()
{
  std::unique_lock lock(lock_);
  playRequested_ = false;
  outputStarted_ = false;
  packedQueue_.clear();
  pcmQueue_.clear();
  queuedDurationUs_ = 0;
  firstQueuedPtsUs_ = NO_PTS;
  pendingPassthroughAckBytes_ = 0;
  hasPendingData_ = false;
  ended_ = false;
  iecPipeline_.Reset();
  // Align stock DefaultAudioSink compatibility behavior: release on every flush.
  output_.Release();
  MarkReleasePendingLocked();
  ResetPositionLocked();
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  startupPhase_ = StartupPhase::IDLE;
}

void KodiActiveAEEngine::Drain()
{
  std::unique_lock lock(lock_);
  if (passthrough_)
    FlushPackedQueueToHardwareLocked();
  else
    FlushPcmQueueToHardwareLocked();
  if (StartOutputIfPrimedLocked())
  {
    if (passthrough_)
      FlushPackedQueueToHardwareLocked();
    else
      FlushPcmQueueToHardwareLocked();
  }
}

void KodiActiveAEEngine::HandleDiscontinuity()
{
  std::unique_lock lock(lock_);
  startMediaTimeUsNeedsSync_ = true;
  pendingSyncPtsUs_ = NO_PTS;
  nextExpectedPtsValid_ = false;
  nextExpectedPtsUs_ = 0;
  pendingPassthroughAckBytes_ = 0;
}

void KodiActiveAEEngine::SetVolume(float volume)
{
  std::unique_lock lock(lock_);
  volume_ = volume;
}

void KodiActiveAEEngine::SetHostClockUs(int64_t host_clock_us)
{
  std::unique_lock lock(lock_);
  hostClockUs_ = host_clock_us;
}

void KodiActiveAEEngine::SetHostClockSpeed(double speed)
{
  std::unique_lock lock(lock_);
  const double newSpeed = speed > 0.0 ? speed : 1.0;
  if (std::abs(newSpeed - hostClockSpeed_) < 1e-6)
    return;

  if (anchorValid_ && output_.IsConfigured())
  {
    const int64_t outputPositionUs = GetAudioOutputPositionUsLocked();
    const int64_t mediaPositionUs = ApplyMediaPositionParametersLocked(outputPositionUs);
    MediaPositionParameters checkpoint;
    checkpoint.playbackSpeed = newSpeed;
    checkpoint.mediaTimeUs = mediaPositionUs;
    checkpoint.audioOutputPositionUs = outputPositionUs;
    checkpoint.mediaPositionDriftUs = mediaPositionParameters_.mediaPositionDriftUs;
    mediaPositionParametersCheckpoints_.push_back(checkpoint);
  }

  hostClockSpeed_ = newSpeed;
}

int64_t KodiActiveAEEngine::GetCurrentPositionUs()
{
  std::unique_lock lock(lock_);
  return ComputePositionFromHardwareLocked();
}

bool KodiActiveAEEngine::HasPendingData()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return false;

  if (!packedQueue_.empty() || !pcmQueue_.empty() || iecPipeline_.HasParserBacklog())
    return true;

  if (!output_.IsConfigured() || output_.FrameSizeBytes() == 0)
    return false;

  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  return totalWrittenFrames_ > playedFrames;
}

bool KodiActiveAEEngine::IsEnded()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return true;
  ended_ = !HasPendingData();
  return ended_;
}

int64_t KodiActiveAEEngine::GetBufferSizeUs() const
{
  std::unique_lock lock(lock_);
  if (!output_.IsConfigured() || output_.SampleRate() == 0)
    return 0;
  const int frames = output_.GetBufferSizeInFrames();
  if (frames <= 0)
    return 0;
  return static_cast<int64_t>(frames) * 1000000LL / output_.SampleRate();
}

int KodiActiveAEEngine::ConsumeLastWriteOutputBytes()
{
  std::unique_lock lock(lock_);
  const int value = lastWriteOutputBytes_;
  lastWriteOutputBytes_ = 0;
  return value;
}

int KodiActiveAEEngine::ConsumeLastWriteErrorCode()
{
  std::unique_lock lock(lock_);
  const int value = lastWriteErrorCode_;
  lastWriteErrorCode_ = 0;
  return value;
}

bool KodiActiveAEEngine::IsReleasePending()
{
  std::unique_lock lock(lock_);
  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  return IsReleasePendingLocked(nowUs);
}

void KodiActiveAEEngine::Reset()
{
  std::unique_lock lock(lock_);
  configured_ = false;
  playRequested_ = false;
  outputStarted_ = false;
  passthrough_ = false;
  ended_ = false;
  hasPendingData_ = false;
  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  packedQueue_.clear();
  pcmQueue_.clear();
  queuedDurationUs_ = 0;
  firstQueuedPtsUs_ = NO_PTS;
  pendingPassthroughAckBytes_ = 0;
  iecPipeline_.Reset();
  output_.Release();
  MarkReleasePendingLocked();
  ResetPositionLocked();
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  startupPhase_ = StartupPhase::IDLE;
}

int KodiActiveAEEngine::WritePassthroughLocked(const uint8_t* data,
                                               int size,
                                               int64_t ptsUs,
                                               int encodedAccessUnitCount)
{
  if (data == nullptr || size <= 0)
    return 0;

  int consumedTotal = 0;
  int remaining = size;
  const uint8_t* cursor = data;
  int64_t currentPtsUs = ptsUs;
  int remainingAccessUnits = std::max(1, encodedAccessUnitCount);
  while (remaining > 0)
  {
    if (startMediaTimeUsNeedsSync_ && !TryResolvePendingDiscontinuityLocked())
      break;

    FlushPackedQueueToHardwareLocked();
    if (pendingPassthroughAckBytes_ > 0)
    {
      const int acknowledgedBytes = std::min(remaining, pendingPassthroughAckBytes_);
      pendingPassthroughAckBytes_ -= acknowledgedBytes;
      consumedTotal += acknowledgedBytes;
      cursor += acknowledgedBytes;
      remaining -= acknowledgedBytes;
      if (remainingAccessUnits > 1)
        --remainingAccessUnits;
      currentPtsUs = NO_PTS;
      continue;
    }

    // Stock-like write-progress backpressure: if previously packed bytes are still
    // pending (i.e. no write progress), do not consume additional upstream input.
    if (!packedQueue_.empty())
    {
      if (config_.iecVerboseLogging && !playRequested_)
      {
        CLog::Log(LOGINFO,
                  "KodiActiveAEEngine::WritePassthroughLocked paused backpressure packedQueue={} "
                  "queuedDurationUs={} queuedBytes={}",
                  packedQueue_.size(),
                  QueueDurationUsLocked(),
                  QueueBytesLocked());
      }
      break;
    }

    const int chunkBytes = std::max(1, remaining / remainingAccessUnits);
    const int feedSize = std::min(remaining, chunkBytes);
    const size_t queueSizeBeforeFeed = packedQueue_.size();
    const int consumed =
        iecPipeline_.Feed(cursor, feedSize, currentPtsUs, packedQueue_, /*maxPackets=*/1);
    if (consumed <= 0)
      break;

    if (packedQueue_.size() > queueSizeBeforeFeed)
    {
      if (!output_.IsConfigured())
        EnsurePassthroughOutputConfiguredLocked(packedQueue_.front());
      FlushPackedQueueToHardwareLocked();
      StartOutputIfPrimedLocked();
      if (pendingPassthroughAckBytes_ > 0)
      {
        const int acknowledgedBytes = std::min(consumed, pendingPassthroughAckBytes_);
        pendingPassthroughAckBytes_ -= acknowledgedBytes;
        consumedTotal += acknowledgedBytes;
        cursor += acknowledgedBytes;
        remaining -= acknowledgedBytes;
        if (remainingAccessUnits > 1)
          --remainingAccessUnits;
        currentPtsUs = NO_PTS;
      }
      if (!packedQueue_.empty())
        break;
    }
    else if (iecPipeline_.HasParserBacklog())
    {
      // Parser accepted bytes into internal backlog but did not emit a packet yet.
      // Stop here to force upstream re-entry rather than over-consuming while no
      // output write progress has occurred.
      iecPipeline_.AcknowledgeConsumedInputBytes(consumed);
      consumedTotal += consumed;
      cursor += consumed;
      remaining -= consumed;
      if (remainingAccessUnits > 1)
        --remainingAccessUnits;
      currentPtsUs = NO_PTS;
      break;
    }
    else
    {
      iecPipeline_.AcknowledgeConsumedInputBytes(consumed);
      consumedTotal += consumed;
      cursor += consumed;
      remaining -= consumed;
      if (remainingAccessUnits > 1)
        --remainingAccessUnits;
      currentPtsUs = NO_PTS;
    }

    if (firstQueuedPtsUs_ == NO_PTS && ptsUs != NO_PTS)
      firstQueuedPtsUs_ = ptsUs;
    queuedDurationUs_ = QueueDurationUsLocked();
  }

  return consumedTotal;
}

int KodiActiveAEEngine::WritePcmLocked(const uint8_t* data, int size, int64_t ptsUs)
{
  const int frameSize = std::max(1, static_cast<int>(requestedFormat_.m_frameSize));
  int bytesToWrite = size - (size % frameSize);
  if (bytesToWrite <= 0)
    return 0;

  if (!EnsurePcmOutputConfiguredLocked())
    return 0;
  if (!pcmQueue_.empty())
  {
    FlushPcmQueueToHardwareLocked();
    if (!pcmQueue_.empty())
      return 0;
  }
  if (startMediaTimeUsNeedsSync_ && !TryResolvePendingDiscontinuityLocked())
    return 0;

  // Unified stock-like behavior: let AudioTrack.write(WRITE_NON_BLOCKING)
  // dictate write progress instead of pre-computed pending-frame clamps.
  int written = output_.WriteNonBlocking(data, bytesToWrite);
  if (written > 0)
  {
    lastWriteOutputBytes_ += written;
    OnBytesWrittenLocked(ptsUs, written, output_.SampleRate(), output_.FrameSizeBytes());
    StartOutputIfPrimedLocked();
  }
  else if (written < 0)
  {
    lastWriteErrorCode_ = written;
    InvalidateCurrentOutputLocked();
    output_.Release();
    MarkReleasePendingLocked();
  }
  return std::max(0, written);
}

void KodiActiveAEEngine::EnsurePassthroughOutputConfiguredLocked(const KodiPackedAccessUnit& packet)
{
  if (output_.IsConfigured() && output_.SampleRate() == packet.outputRate &&
      output_.ChannelCount() == packet.outputChannels)
    return;

  InvalidateCurrentOutputLocked();
  output_.Release();
  MarkReleasePendingLocked();
  bool configured = false;
  for (int attempt = 1; attempt <= PASSTHROUGH_CONFIG_RETRY_ATTEMPTS && !configured; ++attempt)
  {
    configured = output_.Configure(packet.outputRate,
                                   packet.outputChannels,
                                   CJNIAudioFormat::ENCODING_IEC61937,
                                   true);
    if (configured)
      break;

    if (config_.iecVerboseLogging)
    {
      CLog::Log(LOGWARNING,
                "KodiActiveAEEngine::EnsurePassthroughOutputConfiguredLocked configure retry "
                "attempt={} sampleRate={} channels={}",
                attempt,
                packet.outputRate,
                packet.outputChannels);
    }
    if (attempt < PASSTHROUGH_CONFIG_RETRY_ATTEMPTS)
    {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(PASSTHROUGH_CONFIG_RETRY_DELAY_MS));
    }
  }
  if (!configured)
    return;

  releasePendingUntilUs_ = CURRENT_POSITION_NOT_SET;
  if (playRequested_ && outputStarted_ && !output_.Play())
  {
    outputStarted_ = false;
    CLog::Log(LOGWARNING,
              "KodiActiveAEEngine::EnsurePassthroughOutputConfiguredLocked failed to enter "
              "PLAYING state after reconfigure");
  }
  lastStablePlayedFrames_ = 0;
  UpdateTimestampStateLocked(TimestampState::INITIALIZING,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}

bool KodiActiveAEEngine::EnsurePcmOutputConfiguredLocked()
{
  if (output_.IsConfigured())
    return true;

  const unsigned int channels =
      std::max(1u, static_cast<unsigned int>(requestedFormat_.m_channelLayout.Count()));
  int encoding = config_.pcmEncoding;
  if (encoding <= 0)
    encoding = requestedFormat_.m_dataFormat == AE_FMT_FLOAT ? CJNIAudioFormat::ENCODING_PCM_FLOAT
                                                             : CJNIAudioFormat::ENCODING_PCM_16BIT;
  const bool configured = output_.Configure(requestedFormat_.m_sampleRate, channels, encoding, false);
  if (configured)
    releasePendingUntilUs_ = CURRENT_POSITION_NOT_SET;
  return configured;
}

int KodiActiveAEEngine::FlushPackedQueueToHardwareLocked()
{
  if (packedQueue_.empty())
    return 0;

  if (!output_.IsConfigured())
    EnsurePassthroughOutputConfiguredLocked(packedQueue_.front());
  if (!output_.IsConfigured())
    return 0;
  int totalConsumedPackets = 0;
  int totalWriteCalls = 0;
  int totalWriteAttempts = 0;
  int totalBytesWritten = 0;
  int lastWriteResult = 0;
  while (!packedQueue_.empty() && totalWriteCalls < MAX_WRITE_CALLS_PER_FLUSH)
  {
    KodiPackedAccessUnit& packet = packedQueue_.front();

    const int remaining = static_cast<int>(packet.bytes.size() - packet.writeOffset);
    if (remaining <= 0)
    {
      packedQueue_.pop_front();
      ++totalConsumedPackets;
      continue;
    }

    ++totalWriteAttempts;
    const int written = output_.WriteNonBlocking(packet.bytes.data() + packet.writeOffset, remaining);
    lastWriteResult = written;
    if (written <= 0)
    {
      if (written < 0)
      {
        lastWriteErrorCode_ = written;
        InvalidateCurrentOutputLocked();
        output_.Release();
        MarkReleasePendingLocked();
      }
      break;
    }

    ++totalWriteCalls;
    totalBytesWritten += written;
    lastWriteOutputBytes_ += written;
    packet.writeOffset += static_cast<size_t>(written);
    if (packet.writeOffset >= packet.bytes.size())
    {
      // Stock AudioTrackAudioOutput only advances non-PCM written-frame accounting once the
      // whole encoded access unit has been submitted.
      pendingPassthroughAckBytes_ += std::max(0, packet.inputBytesConsumed);
      OnBytesWrittenLocked(packet.ptsUs,
                           static_cast<int>(packet.bytes.size()),
                           output_.SampleRate(),
                           output_.FrameSizeBytes());
      packedQueue_.pop_front();
      ++totalConsumedPackets;
    }
  }
  queuedDurationUs_ = QueueDurationUsLocked();
  if (totalWriteAttempts > 0 && config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::FlushPackedQueueToHardwareLocked phase={} attempts={} "
              "writes={} bytesWritten={} lastWriteResult={} packedQueue={} queuedDurationUs={}",
              StartupPhaseToString(startupPhase_),
              totalWriteAttempts,
              totalWriteCalls,
              totalBytesWritten,
              lastWriteResult,
              packedQueue_.size(),
              queuedDurationUs_);
  }
  if (packedQueue_.empty() && pcmQueue_.empty())
    firstQueuedPtsUs_ = NO_PTS;
  return totalConsumedPackets;
}

int KodiActiveAEEngine::FlushPcmQueueToHardwareLocked()
{
  if (pcmQueue_.empty())
    return 0;
  if (!EnsurePcmOutputConfiguredLocked())
    return 0;
  int totalConsumedBytes = 0;
  int totalWriteCalls = 0;
  int totalWriteAttempts = 0;
  int lastWriteResult = 0;
  while (!pcmQueue_.empty() && totalWriteCalls < MAX_WRITE_CALLS_PER_FLUSH)
  {
    PendingPcmChunk& chunk = pcmQueue_.front();
    const int remaining = static_cast<int>(chunk.bytes.size());
    if (remaining <= 0)
    {
      pcmQueue_.pop_front();
      continue;
    }

    ++totalWriteAttempts;
    const int written = output_.WriteNonBlocking(chunk.bytes.data(), remaining);
    lastWriteResult = written;
    if (written <= 0)
    {
      if (written < 0)
      {
        lastWriteErrorCode_ = written;
        InvalidateCurrentOutputLocked();
        output_.Release();
        MarkReleasePendingLocked();
      }
      break;
    }

    ++totalWriteCalls;
    lastWriteOutputBytes_ += written;
    OnBytesWrittenLocked(chunk.ptsUs, written, output_.SampleRate(), output_.FrameSizeBytes());
    totalConsumedBytes += written;
    if (written >= remaining)
    {
      pcmQueue_.pop_front();
    }
    else
    {
      chunk.bytes.erase(chunk.bytes.begin(), chunk.bytes.begin() + written);
      break;
    }
  }
  queuedDurationUs_ = QueueDurationUsLocked();
  if (totalWriteAttempts > 0 && config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::FlushPcmQueueToHardwareLocked phase={} attempts={} "
              "writes={} bytesWritten={} lastWriteResult={} pcmQueue={} queuedDurationUs={}",
              StartupPhaseToString(startupPhase_),
              totalWriteAttempts,
              totalWriteCalls,
              totalConsumedBytes,
              lastWriteResult,
              pcmQueue_.size(),
              queuedDurationUs_);
  }
  if (packedQueue_.empty() && pcmQueue_.empty())
    firstQueuedPtsUs_ = NO_PTS;
  return totalConsumedBytes;
}

void KodiActiveAEEngine::OnBytesWrittenLocked(int64_t packetPtsUs,
                                              int bytesWritten,
                                              unsigned int sampleRate,
                                              unsigned int frameSizeBytes)
{
  if (bytesWritten <= 0 || sampleRate == 0 || frameSizeBytes == 0)
    return;

  const uint64_t frames = static_cast<uint64_t>(bytesWritten / static_cast<int>(frameSizeBytes));
  totalWrittenFrames_ += frames;
  if (!playRequested_)
  {
    lastPrePlayWriteSystemTimeUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
  }
  const int64_t packetDurationUs =
      static_cast<int64_t>(frames * 1000000ULL / std::max(1u, sampleRate));

  if (packetPtsUs != NO_PTS)
    UpdateExpectedPtsLocked(packetPtsUs, packetDurationUs);

  if (!anchorValid_ && packetPtsUs != NO_PTS)
  {
    anchorValid_ = true;
    anchorPtsUs_ = packetPtsUs;
    anchorPlaybackFrames_ = GetSafePlayedFramesLocked();
    anchorSinkSampleRate_ = sampleRate;
    anchorMediaSampleRate_ = passthrough_
                                 ? std::max(1u, requestedFormat_.m_streamInfo.m_sampleRate)
                                 : sampleRate;
    systemTimeAtAnchorUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    ResetOutputPositionEstimatorLocked();
    mediaPositionParameters_ = {hostClockSpeed_, anchorPtsUs_, 0, 0};
    mediaPositionParametersCheckpoints_.clear();
    lastPositionUs_ = anchorPtsUs_;
  }
}

int64_t KodiActiveAEEngine::ComputePositionFromHardwareLocked()
{
  if (!anchorValid_ || !output_.IsConfigured() || output_.SampleRate() == 0)
    return CURRENT_POSITION_NOT_SET;

  int64_t outputPositionUs = GetAudioOutputPositionUsLocked();
  outputPositionUs = std::min(outputPositionUs, GetWrittenAudioOutputPositionUsLocked());
  int64_t posUs = ApplyMediaPositionParametersLocked(outputPositionUs);
  posUs = ApplySkippingLocked(posUs);
  if (lastPositionUs_ != CURRENT_POSITION_NOT_SET && posUs < lastPositionUs_)
    posUs = lastPositionUs_;
  lastPositionUs_ = posUs;
  return posUs;
}

int64_t KodiActiveAEEngine::GetAudioOutputPositionUsLocked()
{
  const unsigned int sinkRate = std::max(1u, anchorSinkSampleRate_);
  const int64_t systemTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  const uint64_t elapsedFrames =
      playedFrames >= anchorPlaybackFrames_ ? (playedFrames - anchorPlaybackFrames_) : 0;
  int64_t playbackHeadEstimateUs = static_cast<int64_t>((elapsedFrames * 1000000ULL) / sinkRate);
  if (systemTimeAtPlayUs_ != CURRENT_POSITION_NOT_SET && systemTimeUs >= systemTimeAtPlayUs_ &&
      framesAtPlay_ >= anchorPlaybackFrames_)
  {
    const uint64_t framesSincePlay = playedFrames > framesAtPlay_ ? (playedFrames - framesAtPlay_) : 0;
    const int64_t expectedElapsedSincePlayUs = systemTimeUs - systemTimeAtPlayUs_;
    const int64_t framesSincePlayUs = static_cast<int64_t>((framesSincePlay * 1000000ULL) / sinkRate);
    const bool isTimestampStale =
        std::llabs(framesSincePlayUs - expectedElapsedSincePlayUs) > MAX_RESUME_TIMESTAMP_DRIFT_US;
    if (playedFrames <= framesAtPlay_ || isTimestampStale)
    {
      const int64_t baseFramesUs =
          static_cast<int64_t>(((framesAtPlay_ - anchorPlaybackFrames_) * 1000000ULL) / sinkRate);
      playbackHeadEstimateUs = baseFramesUs + std::max<int64_t>(0, expectedElapsedSincePlayUs);
    }
  }

  bool timestampUpdated = false;
  uint64_t tsFrames = 0;
  int64_t tsSystemTimeUs = 0;
  if (systemTimeUs - timestampLastSampleTimeUs_ >= timestampSampleIntervalUs_)
  {
    timestampLastSampleTimeUs_ = systemTimeUs;
    timestampUpdated = output_.GetTimestamp(&tsFrames, &tsSystemTimeUs) &&
                       tsFrames >= anchorPlaybackFrames_;
    if (timestampUpdated)
    {
      const uint64_t tsElapsedFrames = tsFrames - anchorPlaybackFrames_;
      const int64_t tsPositionUs = static_cast<int64_t>((tsElapsedFrames * 1000000ULL) / sinkRate);
      const int64_t elapsedSinceTimestampUs = std::max<int64_t>(0, systemTimeUs - tsSystemTimeUs);
      const int64_t timestampPositionUs = tsPositionUs + elapsedSinceTimestampUs;
      const bool invalidSystemOffset =
          std::llabs(tsSystemTimeUs - systemTimeUs) > MAX_AUDIO_TIMESTAMP_OFFSET_US;
      const bool invalidFrameOffset =
          std::llabs(timestampPositionUs - playbackHeadEstimateUs) > MAX_AUDIO_TIMESTAMP_OFFSET_US;
      if (invalidSystemOffset || invalidFrameOffset)
      {
        UpdateTimestampStateLocked(TimestampState::ERROR, systemTimeUs);
      }
      else
      {
        switch (timestampState_)
        {
          case TimestampState::INITIALIZING:
            if (tsSystemTimeUs >= timestampInitializeSystemTimeUs_)
            {
              timestampInitialFrames_ = tsFrames;
              timestampInitialSystemTimeUs_ = tsSystemTimeUs;
              UpdateTimestampStateLocked(TimestampState::TIMESTAMP, systemTimeUs);
            }
            break;
          case TimestampState::TIMESTAMP:
            if (IsTimestampAdvancingFromInitialLocked(
                    tsFrames, tsSystemTimeUs, systemTimeUs, playbackHeadEstimateUs))
            {
              UpdateTimestampStateLocked(TimestampState::TIMESTAMP_ADVANCING, systemTimeUs);
            }
            else if (systemTimeUs - timestampInitializeSystemTimeUs_ > WAIT_FOR_TIMESTAMP_ADVANCE_US)
            {
              UpdateTimestampStateLocked(TimestampState::NO_TIMESTAMP, systemTimeUs);
            }
            else
            {
              timestampInitialFrames_ = tsFrames;
              timestampInitialSystemTimeUs_ = tsSystemTimeUs;
            }
            break;
          case TimestampState::TIMESTAMP_ADVANCING:
            break;
          case TimestampState::NO_TIMESTAMP:
            UpdateTimestampStateLocked(TimestampState::INITIALIZING, systemTimeUs);
            break;
          case TimestampState::ERROR:
            UpdateTimestampStateLocked(TimestampState::INITIALIZING, systemTimeUs);
            break;
        }
        lastTimestampFrames_ = tsFrames;
        lastTimestampSystemTimeUs_ = tsSystemTimeUs;
      }
    }
    else
    {
      if (timestampState_ == TimestampState::INITIALIZING &&
          systemTimeUs - timestampInitializeSystemTimeUs_ > INITIALIZING_DURATION_US)
      {
        UpdateTimestampStateLocked(TimestampState::NO_TIMESTAMP, systemTimeUs);
      }
      else if (timestampState_ == TimestampState::TIMESTAMP ||
               timestampState_ == TimestampState::TIMESTAMP_ADVANCING)
      {
        UpdateTimestampStateLocked(TimestampState::INITIALIZING, systemTimeUs);
      }
    }
  }

  if (timestampState_ == TimestampState::TIMESTAMP_ADVANCING &&
      lastTimestampFrames_ >= anchorPlaybackFrames_ &&
      lastTimestampSystemTimeUs_ != CURRENT_POSITION_NOT_SET)
  {
    const uint64_t tsElapsedFrames = lastTimestampFrames_ - anchorPlaybackFrames_;
    const int64_t tsPositionUs = static_cast<int64_t>((tsElapsedFrames * 1000000ULL) / sinkRate);
    const int64_t elapsedSinceTimestampUs =
        std::max<int64_t>(0, systemTimeUs - lastTimestampSystemTimeUs_);
    const int64_t positionUs = tsPositionUs + elapsedSinceTimestampUs;
    lastOutputPositionUs_ = positionUs;
    lastOutputPositionSystemTimeUs_ = systemTimeUs;
    return std::max<int64_t>(0, positionUs);
  }

  if (systemTimeUs - lastPlayheadSampleTimeUs_ >= MIN_PLAYHEAD_OFFSET_SAMPLE_INTERVAL_US &&
      playbackHeadEstimateUs > 0)
  {
    playheadOffsetsUs_[nextPlayheadOffsetIndex_] = playbackHeadEstimateUs - systemTimeUs;
    nextPlayheadOffsetIndex_ = (nextPlayheadOffsetIndex_ + 1) % static_cast<int>(playheadOffsetsUs_.size());
    if (playheadOffsetCount_ < static_cast<int>(playheadOffsetsUs_.size()))
      ++playheadOffsetCount_;
    lastPlayheadSampleTimeUs_ = systemTimeUs;

    smoothedPlayheadOffsetUs_ = 0;
    for (int i = 0; i < playheadOffsetCount_; ++i)
      smoothedPlayheadOffsetUs_ += playheadOffsetsUs_[i] / playheadOffsetCount_;
  }

  int64_t positionUs = playbackHeadEstimateUs;
  if (playheadOffsetCount_ > 0)
    positionUs = systemTimeUs + smoothedPlayheadOffsetUs_;

  if (lastOutputPositionSystemTimeUs_ != CURRENT_POSITION_NOT_SET &&
      lastOutputPositionUs_ != CURRENT_POSITION_NOT_SET)
  {
    const int64_t elapsedSystemTimeUs = std::max<int64_t>(0, systemTimeUs - lastOutputPositionSystemTimeUs_);
    const int64_t expectedPositionDiffUs =
        static_cast<int64_t>(elapsedSystemTimeUs * std::max(0.1, hostClockSpeed_));
    const int64_t expectedPositionUs = lastOutputPositionUs_ + expectedPositionDiffUs;
    const int64_t positionDiffUs = positionUs - lastOutputPositionUs_;
    const int64_t driftUs = std::llabs(expectedPositionUs - positionUs);
    if (positionDiffUs != 0 && driftUs < MAX_POSITION_DRIFT_FOR_SMOOTHING_US)
    {
      const int64_t maxAllowedDriftUs =
          expectedPositionDiffUs * MAX_POSITION_SMOOTHING_SPEED_CHANGE_PERCENT / 100;
      positionUs = std::clamp(positionUs,
                              expectedPositionUs - maxAllowedDriftUs,
                              expectedPositionUs + maxAllowedDriftUs);
    }
  }

  positionUs = std::max<int64_t>(0, positionUs);
  lastOutputPositionUs_ = positionUs;
  lastOutputPositionSystemTimeUs_ = systemTimeUs;
  return positionUs;
}

int64_t KodiActiveAEEngine::ApplyMediaPositionParametersLocked(int64_t outputPositionUs)
{
  while (!mediaPositionParametersCheckpoints_.empty() &&
         outputPositionUs >= mediaPositionParametersCheckpoints_.front().audioOutputPositionUs)
  {
    mediaPositionParameters_ = mediaPositionParametersCheckpoints_.front();
    mediaPositionParametersCheckpoints_.pop_front();
  }

  const int64_t playoutDurationUs =
      outputPositionUs - mediaPositionParameters_.audioOutputPositionUs;
  const int64_t estimatedMediaDurationUs = static_cast<int64_t>(
      playoutDurationUs * std::max(0.1, mediaPositionParameters_.playbackSpeed));

  if (mediaPositionParametersCheckpoints_.empty())
  {
    const int64_t actualMediaDurationUs = estimatedMediaDurationUs;
    mediaPositionParameters_.mediaPositionDriftUs =
        actualMediaDurationUs - estimatedMediaDurationUs;
    return mediaPositionParameters_.mediaTimeUs + actualMediaDurationUs;
  }

  return mediaPositionParameters_.mediaTimeUs + estimatedMediaDurationUs +
         mediaPositionParameters_.mediaPositionDriftUs;
}

int64_t KodiActiveAEEngine::ApplySkippingLocked(int64_t mediaPositionUs) const
{
  if (anchorMediaSampleRate_ == 0)
    return mediaPositionUs;
  const int64_t skippedDurationUs =
      static_cast<int64_t>((skippedOutputFrameCount_ * 1000000ULL) / anchorMediaSampleRate_);
  return mediaPositionUs + skippedDurationUs;
}

int64_t KodiActiveAEEngine::GetWrittenAudioOutputPositionUsLocked() const
{
  if (anchorSinkSampleRate_ == 0 || totalWrittenFrames_ <= anchorPlaybackFrames_)
    return 0;
  const uint64_t writtenFrames = totalWrittenFrames_ - anchorPlaybackFrames_;
  return static_cast<int64_t>((writtenFrames * 1000000ULL) / anchorSinkSampleRate_);
}

uint64_t KodiActiveAEEngine::GetSafePlayedFramesLocked()
{
  if (!output_.IsConfigured())
    return 0;
  uint64_t rawPlayedFrames = output_.GetPlaybackFrames64();
  if (!playRequested_ || !outputStarted_)
    return std::min(lastStablePlayedFrames_, totalWrittenFrames_);

  uint64_t boundedPlayedFrames = std::min(rawPlayedFrames, totalWrittenFrames_);
  if (boundedPlayedFrames < lastStablePlayedFrames_)
    return lastStablePlayedFrames_;

  lastStablePlayedFrames_ = boundedPlayedFrames;
  return lastStablePlayedFrames_;
}

int64_t KodiActiveAEEngine::QueueDurationUsLocked() const
{
  int64_t total = 0;
  for (const auto& packet : packedQueue_)
  {
    const size_t totalBytes = packet.bytes.size();
    const size_t writtenBytes = std::min(packet.writeOffset, totalBytes);
    const size_t remainingBytes = totalBytes - writtenBytes;
    if (remainingBytes == 0)
      continue;
    if (packet.durationUs <= 0 || totalBytes == 0)
      continue;
    // Count only the not-yet-written fraction of packet duration.
    total += static_cast<int64_t>(
        (static_cast<long double>(packet.durationUs) * static_cast<long double>(remainingBytes)) /
        static_cast<long double>(totalBytes));
  }

  if (!pcmQueue_.empty() && requestedFormat_.m_sampleRate > 0 && requestedFormat_.m_frameSize > 0)
  {
    for (const auto& chunk : pcmQueue_)
    {
      const int64_t frames =
          static_cast<int64_t>(chunk.bytes.size() / static_cast<size_t>(requestedFormat_.m_frameSize));
      total += (frames * 1000000LL) / requestedFormat_.m_sampleRate;
    }
  }
  return total;
}

uint64_t KodiActiveAEEngine::QueueBytesLocked() const
{
  uint64_t total = 0;
  for (const auto& packet : packedQueue_)
  {
    const size_t totalBytes = packet.bytes.size();
    const size_t writtenBytes = std::min(packet.writeOffset, totalBytes);
    total += static_cast<uint64_t>(totalBytes - writtenBytes);
  }
  return total;
}

void KodiActiveAEEngine::UpdateExpectedPtsLocked(int64_t packetPtsUs, int64_t packetDurationUs)
{
  if (packetPtsUs == NO_PTS)
    return;

  const bool isDiscontinuity =
      nextExpectedPtsValid_ &&
      std::llabs(packetPtsUs - nextExpectedPtsUs_) > DISCONTINUITY_THRESHOLD_US;
  if (isDiscontinuity)
  {
    // Stock-like handling: request retime after drain; don't immediately jump
    // timing while there is still pending output.
    startMediaTimeUsNeedsSync_ = true;
    pendingSyncPtsUs_ = packetPtsUs;
    return;
  }

  nextExpectedPtsUs_ = packetPtsUs + std::max<int64_t>(0, packetDurationUs);
  nextExpectedPtsValid_ = true;
}

bool KodiActiveAEEngine::TryResolvePendingDiscontinuityLocked()
{
  if (!startMediaTimeUsNeedsSync_)
    return true;

  if (passthrough_)
    FlushPackedQueueToHardwareLocked();
  else
    FlushPcmQueueToHardwareLocked();

  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  const bool drained = packedQueue_.empty() && pcmQueue_.empty() && totalWrittenFrames_ <= playedFrames;
  if (!drained)
    return false;

  if (pendingSyncPtsUs_ != NO_PTS)
    ReanchorForDiscontinuityLocked(pendingSyncPtsUs_);
  startMediaTimeUsNeedsSync_ = false;
  pendingSyncPtsUs_ = NO_PTS;
  nextExpectedPtsValid_ = false;
  nextExpectedPtsUs_ = 0;
  return true;
}

void KodiActiveAEEngine::ReanchorForDiscontinuityLocked(int64_t packetPtsUs)
{
  if (packetPtsUs == NO_PTS || !output_.IsConfigured())
    return;

  anchorValid_ = true;
  anchorPtsUs_ = packetPtsUs;
  anchorPlaybackFrames_ = GetSafePlayedFramesLocked();
  anchorSinkSampleRate_ = std::max(1u, output_.SampleRate());
  anchorMediaSampleRate_ =
      passthrough_ ? std::max(1u, requestedFormat_.m_streamInfo.m_sampleRate) : anchorSinkSampleRate_;
  ResetOutputPositionEstimatorLocked();
  mediaPositionParameters_ = {hostClockSpeed_, anchorPtsUs_, 0, 0};
  mediaPositionParametersCheckpoints_.clear();
  lastPositionUs_ = anchorPtsUs_;
  startMediaTimeUsNeedsSync_ = false;
  pendingSyncPtsUs_ = NO_PTS;
}

bool KodiActiveAEEngine::StartOutputIfPrimedLocked()
{
  if (!playRequested_ || outputStarted_ || !output_.IsConfigured())
    return false;

  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  if (totalWrittenFrames_ <= playedFrames)
    return false;

  UpdateTimestampStateLocked(TimestampState::INITIALIZING,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
  if (!output_.Play())
  {
    CLog::Log(LOGWARNING,
              "KodiActiveAEEngine::StartOutputIfPrimedLocked failed to enter PLAYING state "
              "totalWrittenFrames={} safePlayedFrames={}",
              totalWrittenFrames_,
              playedFrames);
    return false;
  }
  systemTimeAtPlayUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  framesAtPlay_ = std::min(output_.GetPlaybackFrames64(), totalWrittenFrames_);
  outputStarted_ = true;
  SetStartupPhaseLocked(StartupPhase::STARTED);
  return true;
}

void KodiActiveAEEngine::SetStartupPhaseLocked(StartupPhase phase)
{
  if (startupPhase_ == phase)
    return;
  startupPhase_ = phase;
  if (config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::Startup phase={} packedQueue={} pcmQueue={} totalWrittenFrames={} "
              "safePlayedFrames={}",
              StartupPhaseToString(phase),
              packedQueue_.size(),
              pcmQueue_.size(),
              totalWrittenFrames_,
              GetSafePlayedFramesLocked());
  }
}

const char* KodiActiveAEEngine::StartupPhaseToString(StartupPhase phase) const
{
  switch (phase)
  {
    case StartupPhase::IDLE:
      return "IDLE";
    case StartupPhase::PREPARED:
      return "PREPARED";
    case StartupPhase::PRIME_ATTEMPTED:
      return "PRIME_ATTEMPTED";
    case StartupPhase::STARTED:
      return "STARTED";
    case StartupPhase::POST_START_REFILL:
      return "POST_START_REFILL";
    case StartupPhase::RUNNING:
      return "RUNNING";
    case StartupPhase::RECOVERY_RECREATE:
      return "RECOVERY_RECREATE";
  }
  return "UNKNOWN";
}

void KodiActiveAEEngine::UpdateTimestampStateLocked(TimestampState state, int64_t systemTimeUs)
{
  timestampState_ = state;
  switch (state)
  {
    case TimestampState::INITIALIZING:
      timestampLastSampleTimeUs_ = 0;
      timestampInitializeSystemTimeUs_ = systemTimeUs;
      timestampInitialFrames_ = 0;
      timestampInitialSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
      timestampSampleIntervalUs_ = FAST_TIMESTAMP_POLL_INTERVAL_US;
      break;
    case TimestampState::TIMESTAMP:
      timestampSampleIntervalUs_ = FAST_TIMESTAMP_POLL_INTERVAL_US;
      break;
    case TimestampState::TIMESTAMP_ADVANCING:
    case TimestampState::NO_TIMESTAMP:
      timestampSampleIntervalUs_ = SLOW_TIMESTAMP_POLL_INTERVAL_US;
      break;
    case TimestampState::ERROR:
      timestampSampleIntervalUs_ = ERROR_TIMESTAMP_POLL_INTERVAL_US;
      break;
  }
}

bool KodiActiveAEEngine::IsTimestampAdvancingFromInitialLocked(uint64_t tsFrames,
                                                               int64_t tsSystemTimeUs,
                                                               int64_t systemTimeUs,
                                                               int64_t playbackHeadEstimateUs) const
{
  if (tsFrames <= timestampInitialFrames_ || timestampInitialSystemTimeUs_ == CURRENT_POSITION_NOT_SET ||
      anchorSinkSampleRate_ == 0)
    return false;

  const unsigned int sinkRate = std::max(1u, anchorSinkSampleRate_);
  const uint64_t initialElapsedFrames = timestampInitialFrames_ - anchorPlaybackFrames_;
  const int64_t initialPositionUs =
      static_cast<int64_t>((initialElapsedFrames * 1000000ULL) / sinkRate);
  const int64_t initialElapsedSinceTimestampUs =
      std::max<int64_t>(0, systemTimeUs - timestampInitialSystemTimeUs_);
  const int64_t positionEstimateUsingInitialTimestampUs =
      initialPositionUs + initialElapsedSinceTimestampUs;

  const uint64_t currentElapsedFrames = tsFrames - anchorPlaybackFrames_;
  const int64_t currentPositionUs =
      static_cast<int64_t>((currentElapsedFrames * 1000000ULL) / sinkRate);
  const int64_t currentElapsedSinceTimestampUs = std::max<int64_t>(0, systemTimeUs - tsSystemTimeUs);
  const int64_t positionEstimateUsingCurrentTimestampUs =
      currentPositionUs + currentElapsedSinceTimestampUs;
  const int64_t advancingDriftUs =
      std::llabs(positionEstimateUsingCurrentTimestampUs - positionEstimateUsingInitialTimestampUs);
  const int64_t estimateDriftUs =
      std::llabs(positionEstimateUsingCurrentTimestampUs - playbackHeadEstimateUs);
  return advancingDriftUs < MAX_ADVANCING_TIMESTAMP_DRIFT_US &&
         estimateDriftUs <= MAX_AUDIO_TIMESTAMP_OFFSET_US;
}

void KodiActiveAEEngine::ResetPositionLocked()
{
  anchorValid_ = false;
  anchorPtsUs_ = CURRENT_POSITION_NOT_SET;
  anchorPlaybackFrames_ = 0;
  anchorMediaSampleRate_ = 0;
  anchorSinkSampleRate_ = 0;
  systemTimeAtAnchorUs_ = 0;
  totalWrittenFrames_ = 0;
  lastPositionUs_ = CURRENT_POSITION_NOT_SET;
  nextExpectedPtsValid_ = false;
  nextExpectedPtsUs_ = 0;
  startMediaTimeUsNeedsSync_ = false;
  pendingSyncPtsUs_ = NO_PTS;
  skippedOutputFrameCount_ = 0;
  skippedOutputFrameCountAtLastPosition_ = 0;
  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  mediaPositionParameters_ = {hostClockSpeed_, 0, 0, 0};
  mediaPositionParametersCheckpoints_.clear();
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastStablePlayedFrames_ = 0;
  ResetOutputPositionEstimatorLocked();
}

void KodiActiveAEEngine::InvalidateCurrentOutputLocked()
{
  outputStarted_ = false;
  pendingPassthroughAckBytes_ = 0;
  for (auto& packet : packedQueue_)
    packet.writeOffset = 0;
  ResetPositionLocked();
}

void KodiActiveAEEngine::ResetOutputPositionEstimatorLocked()
{
  playheadOffsetsUs_.fill(0);
  playheadOffsetCount_ = 0;
  nextPlayheadOffsetIndex_ = 0;
  smoothedPlayheadOffsetUs_ = 0;
  lastPlayheadSampleTimeUs_ = 0;
  lastOutputPositionUs_ = CURRENT_POSITION_NOT_SET;
  lastOutputPositionSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  systemTimeAtPlayUs_ = CURRENT_POSITION_NOT_SET;
  framesAtPlay_ = 0;
  lastTimestampFrames_ = 0;
  lastTimestampSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  UpdateTimestampStateLocked(
      TimestampState::INITIALIZING,
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace androidx_media3
