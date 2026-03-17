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

  pendingPassthroughInput_.reset();
  pendingPackedOutput_.reset();
  pendingPcmOutput_.reset();
  queuedDurationUs_ = 0;
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
            "pendingInput={} pendingPacked={} pendingPcm={} totalWrittenFrames={} safePlayedFrames={}",
            prePlayAcceptGapUs,
            prePlayWriteGapUs,
            pendingPassthroughInput_.has_value() ? 1 : 0,
            pendingPackedOutput_.has_value() ? 1 : 0,
            pendingPcmOutput_.has_value() ? 1 : 0,
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
  pendingPassthroughInput_.reset();
  pendingPackedOutput_.reset();
  pendingPcmOutput_.reset();
  queuedDurationUs_ = 0;
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
  StartOutputIfPrimedLocked();
}

void KodiActiveAEEngine::HandleDiscontinuity()
{
  std::unique_lock lock(lock_);
  startMediaTimeUsNeedsSync_ = true;
  pendingSyncPtsUs_ = NO_PTS;
  nextExpectedPtsValid_ = false;
  nextExpectedPtsUs_ = 0;
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

  if (pendingPassthroughInput_.has_value() || pendingPackedOutput_.has_value() ||
      pendingPcmOutput_.has_value() || iecPipeline_.HasParserBacklog())
    return true;

  if (!output_.IsConfigured() || output_.FrameSizeBytes() == 0)
    return false;

  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  return GetSubmittedOutputFramesLocked() > playedFrames;
}

bool KodiActiveAEEngine::IsEnded()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return true;
  ended_ = !HasPendingData();
  return ended_;
}

bool KodiActiveAEEngine::IsPassthroughStartupReady()
{
  std::unique_lock lock(lock_);
  if (!configured_ || !passthrough_ || !output_.IsConfigured())
    return false;

  if (!playRequested_)
    return false;

  const uint64_t submittedFrames = GetSubmittedOutputFramesLocked();
  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  const uint64_t queuedFrames = submittedFrames > playedFrames ? (submittedFrames - playedFrames) : 0;
  const uint64_t startupTargetFrames =
      static_cast<uint64_t>(std::max(1, output_.GetBufferSizeInFrames()));
  const bool hardwareAdvanced = outputStarted_ && playedFrames > framesAtPlay_;
  if (config_.iecVerboseLogging &&
      requestedFormat_.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::IsPassthroughStartupReady truehd outputStarted={} "
              "hardwareAdvanced={} queuedFrames={} targetFrames={} submittedFrames={} "
              "playedFrames={} framesAtPlay={}",
              outputStarted_ ? 1 : 0,
              hardwareAdvanced ? 1 : 0,
              queuedFrames,
              startupTargetFrames,
              submittedFrames,
              playedFrames,
              framesAtPlay_);
  }
  return hardwareAdvanced || queuedFrames >= startupTargetFrames;
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

int64_t KodiActiveAEEngine::GetBufferSizeBytes() const
{
  std::unique_lock lock(lock_);
  if (!output_.IsConfigured())
    return 0;
  const int frames = output_.GetBufferSizeInFrames();
  const unsigned int frameSizeBytes = output_.FrameSizeBytes();
  if (frames <= 0 || frameSizeBytes == 0)
    return 0;
  return static_cast<int64_t>(frames) * static_cast<int64_t>(frameSizeBytes);
}

void KodiActiveAEEngine::ProbePassthroughStartupBuffer(const uint8_t* data,
                                                       int size,
                                                       int64_t presentation_time_us,
                                                       int encoded_access_unit_count)
{
  std::unique_lock lock(lock_);
  if (!configured_ || !passthrough_ || playRequested_ || data == nullptr || size <= 0)
    return;
  if (output_.IsConfigured())
    return;

  KodiIecPipeline probePipeline;
  probePipeline.Configure(requestedFormat_);

  KodiPackedAccessUnit packet;
  bool emittedPacket = false;
  probePipeline.Feed(data,
                     size,
                     presentation_time_us,
                     &packet,
                     &emittedPacket);
  if (!emittedPacket)
    return;

  EnsurePassthroughOutputConfiguredLocked(packet);
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
  pendingPassthroughInput_.reset();
  pendingPackedOutput_.reset();
  pendingPcmOutput_.reset();
  queuedDurationUs_ = 0;
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

  int acknowledgedThisCall = 0;
  int totalAcknowledgedThisCall = 0;
  const bool truehdPassthrough =
      requestedFormat_.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD;
  constexpr int kMaxTrueHdBurstsPerWrite = 4;
  int burstIterations = 0;

  while (burstIterations < (truehdPassthrough ? kMaxTrueHdBurstsPerWrite : 1))
  {
    acknowledgedThisCall = 0;

    if (pendingPackedOutput_.has_value())
    {
      acknowledgedThisCall = FlushPackedQueueToHardwareLocked();
      if (acknowledgedThisCall > 0 && pendingPassthroughInput_.has_value())
      {
        pendingPassthroughInput_->acknowledgedBytes += acknowledgedThisCall;
        iecPipeline_.AcknowledgeConsumedInputBytes(acknowledgedThisCall);
        if (pendingPassthroughInput_->acknowledgedBytes >=
            static_cast<int>(pendingPassthroughInput_->bytes.size()))
        {
          pendingPassthroughInput_.reset();
        }
        totalAcknowledgedThisCall += acknowledgedThisCall;
        if (!truehdPassthrough)
          return totalAcknowledgedThisCall;
      }
      if (pendingPackedOutput_.has_value() || (!truehdPassthrough && pendingPassthroughInput_.has_value()))
        return totalAcknowledgedThisCall;
      if (!truehdPassthrough && totalAcknowledgedThisCall > 0)
        return totalAcknowledgedThisCall;
    }

    if (startMediaTimeUsNeedsSync_ && !TryResolvePendingDiscontinuityLocked())
      return totalAcknowledgedThisCall;

    if (!pendingPassthroughInput_.has_value())
    {
      PendingPassthroughInput input;
      input.bytes.assign(data, data + size);
      input.feedOffset = 0;
      input.acknowledgedBytes = 0;
      input.ptsUs = ptsUs;
      input.encodedAccessUnitCount = std::max(1, encodedAccessUnitCount);
      pendingPassthroughInput_ = std::move(input);
    }

    if (!pendingPackedOutput_.has_value() && pendingPassthroughInput_.has_value())
    {
      auto& input = *pendingPassthroughInput_;
      KodiPackedAccessUnit packet;
      bool emittedPacket = false;
      const int remaining =
          static_cast<int>(input.bytes.size() - std::min(input.feedOffset, input.bytes.size()));
      const uint8_t* feedData = input.bytes.data() + input.feedOffset;
      const int64_t feedPtsUs = input.feedOffset == 0 ? input.ptsUs : NO_PTS;
      const int consumed =
          iecPipeline_.Feed(feedData, remaining, feedPtsUs, &packet, &emittedPacket);
      if (consumed > 0)
        input.feedOffset += static_cast<size_t>(consumed);
      if (emittedPacket)
      {
        if (config_.iecVerboseLogging && truehdPassthrough)
        {
          CLog::Log(LOGINFO,
                    "KodiActiveAEEngine::WritePassthroughLocked truehd emitted packet "
                    "inputBytesConsumed={} packetBytes={} durationUs={} remainingInput={} "
                    "feedOffset={} acknowledgedBytes={} encodedAccessUnits={} sourceAccessUnits={} "
                    "ptsUs={}",
                    packet.inputBytesConsumed,
                    packet.bytes.size(),
                    packet.durationUs,
                    std::max(0, remaining - consumed),
                    input.feedOffset,
                    input.acknowledgedBytes,
                    input.encodedAccessUnitCount,
                    packet.sourceAccessUnitCount,
                    packet.ptsUs);
        }
        pendingPackedOutput_ = std::move(packet);
      }
    }

    if (pendingPassthroughInput_.has_value() && !pendingPackedOutput_.has_value() &&
        pendingPassthroughInput_->feedOffset >= pendingPassthroughInput_->bytes.size())
    {
      const int absorbedBytes = static_cast<int>(pendingPassthroughInput_->feedOffset) -
                                pendingPassthroughInput_->acknowledgedBytes;
      if (absorbedBytes > 0)
      {
        pendingPassthroughInput_->acknowledgedBytes += absorbedBytes;
        iecPipeline_.AcknowledgeConsumedInputBytes(absorbedBytes);
        pendingPassthroughInput_.reset();
        totalAcknowledgedThisCall += absorbedBytes;
        return totalAcknowledgedThisCall;
      }
    }

    if (pendingPackedOutput_.has_value() && !output_.IsConfigured())
      EnsurePassthroughOutputConfiguredLocked(*pendingPackedOutput_);
    acknowledgedThisCall = FlushPackedQueueToHardwareLocked();
    StartOutputIfPrimedLocked();

    if (acknowledgedThisCall > 0 && pendingPassthroughInput_.has_value())
    {
      pendingPassthroughInput_->acknowledgedBytes += acknowledgedThisCall;
      iecPipeline_.AcknowledgeConsumedInputBytes(acknowledgedThisCall);
      if (pendingPassthroughInput_->acknowledgedBytes >=
          static_cast<int>(pendingPassthroughInput_->bytes.size()))
      {
        pendingPassthroughInput_.reset();
      }
      totalAcknowledgedThisCall += acknowledgedThisCall;
      if (!truehdPassthrough)
        return totalAcknowledgedThisCall;
      ++burstIterations;
      continue;
    }

    break;
  }

  if (totalAcknowledgedThisCall > 0)
    return totalAcknowledgedThisCall;

  if (config_.iecVerboseLogging && !playRequested_ && pendingPackedOutput_.has_value())
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::WritePassthroughLocked paused backpressure pendingInput={} "
              "pendingPacked={} queuedDurationUs={} queuedBytes={}",
              pendingPassthroughInput_.has_value() ? 1 : 0,
              pendingPackedOutput_.has_value() ? 1 : 0,
              QueueDurationUsLocked(),
              QueueBytesLocked());
  }
  else if (config_.iecVerboseLogging && truehdPassthrough &&
           (pendingPackedOutput_.has_value() || pendingPassthroughInput_.has_value()))
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::WritePassthroughLocked truehd awaiting downstream "
              "playRequested={} outputStarted={} pendingInput={} pendingPacked={} "
              "queuedDurationUs={} queuedBytes={}",
              playRequested_ ? 1 : 0,
              outputStarted_ ? 1 : 0,
              pendingPassthroughInput_.has_value() ? 1 : 0,
              pendingPackedOutput_.has_value() ? 1 : 0,
              QueueDurationUsLocked(),
              QueueBytesLocked());
  }
  return 0;
}

int KodiActiveAEEngine::WritePcmLocked(const uint8_t* data, int size, int64_t ptsUs)
{
  const int frameSize = std::max(1, static_cast<int>(requestedFormat_.m_frameSize));
  int bytesToWrite = size - (size % frameSize);
  if (bytesToWrite <= 0)
    return 0;

  if (!EnsurePcmOutputConfiguredLocked())
    return 0;
  if (pendingPcmOutput_.has_value())
  {
    FlushPcmQueueToHardwareLocked();
    if (pendingPcmOutput_.has_value())
      return 0;
  }
  if (startMediaTimeUsNeedsSync_ && !TryResolvePendingDiscontinuityLocked())
    return 0;
  PendingPcmChunk chunk;
  chunk.bytes.assign(data, data + bytesToWrite);
  chunk.writeOffset = 0;
  chunk.inputBytesConsumed = bytesToWrite;
  chunk.ptsUs = ptsUs;
  pendingPcmOutput_ = std::move(chunk);
  FlushPcmQueueToHardwareLocked();
  if (!pendingPcmOutput_.has_value())
    return bytesToWrite;

  const size_t writtenBytes =
      std::min(pendingPcmOutput_->writeOffset, pendingPcmOutput_->bytes.size());
  return static_cast<int>(std::min<size_t>(writtenBytes, static_cast<size_t>(bytesToWrite)));
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
                                   true,
                                   packet.streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD);
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
  const bool configured =
      output_.Configure(requestedFormat_.m_sampleRate, channels, encoding, false, false);
  if (configured)
    releasePendingUntilUs_ = CURRENT_POSITION_NOT_SET;
  return configured;
}

int KodiActiveAEEngine::FlushPackedQueueToHardwareLocked()
{
  if (!pendingPackedOutput_.has_value())
    return 0;

  if (!output_.IsConfigured())
    EnsurePassthroughOutputConfiguredLocked(*pendingPackedOutput_);
  if (!output_.IsConfigured())
    return 0;

  int totalWriteCalls = 0;
  int totalWriteAttempts = 0;
  int totalBytesWritten = 0;
  int lastWriteResult = 0;
  int zeroWriteRetries = 0;
  while (pendingPackedOutput_.has_value() && totalWriteCalls < MAX_WRITE_CALLS_PER_FLUSH)
  {
    const bool truehdPacket =
        pendingPackedOutput_->streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD;
    const int remaining =
        static_cast<int>(pendingPackedOutput_->bytes.size() - pendingPackedOutput_->writeOffset);
    if (remaining <= 0)
    {
      pendingPackedOutput_.reset();
      break;
    }

    ++totalWriteAttempts;
    const int written =
        truehdPacket
            ? output_.WriteBlocking(
                  pendingPackedOutput_->bytes.data() + pendingPackedOutput_->writeOffset, remaining)
            : output_.WriteNonBlocking(
                  pendingPackedOutput_->bytes.data() + pendingPackedOutput_->writeOffset, remaining);
    lastWriteResult = written;
    if (written <= 0)
    {
      if (written < 0)
      {
        lastWriteErrorCode_ = written;
        InvalidateCurrentOutputLocked();
        output_.Release();
        MarkReleasePendingLocked();
        zeroWriteRetries = 0;
      }
      else
      {
        const int maxZeroWriteRetries = truehdPacket ? 1 : 1;
        if (zeroWriteRetries < maxZeroWriteRetries)
        {
          ++zeroWriteRetries;
          int64_t sleepTimeUs = pendingPackedOutput_->durationUs;
          if (sleepTimeUs <= 0 && output_.SampleRate() > 0)
          {
            const unsigned int totalPacketBytes =
                static_cast<unsigned int>(pendingPackedOutput_->bytes.size());
            const unsigned int frameSizeBytes = output_.FrameSizeBytes();
            if (totalPacketBytes > 0 && frameSizeBytes > 0)
            {
              const int64_t packetFrames = totalPacketBytes / frameSizeBytes;
              if (packetFrames > 0)
              {
                sleepTimeUs = (packetFrames * 1000000LL) / output_.SampleRate();
              }
            }
          }
          if (sleepTimeUs <= 0)
            sleepTimeUs = 1000;
          lock_.unlock();
          std::this_thread::sleep_for(std::chrono::microseconds(sleepTimeUs));
          lock_.lock();
          if (!pendingPackedOutput_.has_value() || !output_.IsConfigured())
            break;
          continue;
        }
      }
      break;
    }

    zeroWriteRetries = 0;
    ++totalWriteCalls;
    totalBytesWritten += written;
    lastWriteOutputBytes_ += written;
    pendingPackedOutput_->writeOffset += static_cast<size_t>(written);
    if (config_.iecVerboseLogging && truehdPacket)
    {
      CLog::Log(LOGINFO,
                "KodiActiveAEEngine::FlushPackedQueueToHardwareLocked truehd write "
                "written={} remaining={} packetBytes={} inputBytesConsumed={} durationUs={} "
                "writeOffset={} outputRate={} outputChannels={} sourceAccessUnits={}",
                written,
                std::max(
                    0,
                    static_cast<int>(pendingPackedOutput_->bytes.size() -
                                     pendingPackedOutput_->writeOffset)),
                pendingPackedOutput_->bytes.size(),
                pendingPackedOutput_->inputBytesConsumed,
                pendingPackedOutput_->durationUs,
                pendingPackedOutput_->writeOffset,
                pendingPackedOutput_->outputRate,
                pendingPackedOutput_->outputChannels,
                pendingPackedOutput_->sourceAccessUnitCount);
    }
    if (pendingPackedOutput_->writeOffset >= pendingPackedOutput_->bytes.size())
    {
      const KodiPackedAccessUnit completedPacket = *pendingPackedOutput_;
      OnBytesWrittenLocked(completedPacket.ptsUs,
                           static_cast<int>(completedPacket.bytes.size()),
                           output_.SampleRate(),
                           output_.FrameSizeBytes());
      pendingPackedOutput_.reset();
      queuedDurationUs_ = QueueDurationUsLocked();
      return std::max(0, completedPacket.inputBytesConsumed);
    }
  }
  queuedDurationUs_ = QueueDurationUsLocked();
  if (totalWriteAttempts > 0 && config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::FlushPackedQueueToHardwareLocked phase={} attempts={} "
              "writes={} bytesWritten={} lastWriteResult={} pendingPacked={} queuedDurationUs={}",
              StartupPhaseToString(startupPhase_),
              totalWriteAttempts,
              totalWriteCalls,
              totalBytesWritten,
              lastWriteResult,
              pendingPackedOutput_.has_value() ? 1 : 0,
              queuedDurationUs_);
  }
  return 0;
}

int KodiActiveAEEngine::FlushPcmQueueToHardwareLocked()
{
  if (!pendingPcmOutput_.has_value())
    return 0;
  if (!EnsurePcmOutputConfiguredLocked())
    return 0;
  int totalWriteCalls = 0;
  int totalWriteAttempts = 0;
  int totalBytesWritten = 0;
  int lastWriteResult = 0;
  while (pendingPcmOutput_.has_value() && totalWriteCalls < MAX_WRITE_CALLS_PER_FLUSH)
  {
    const int remaining =
        static_cast<int>(pendingPcmOutput_->bytes.size() - pendingPcmOutput_->writeOffset);
    if (remaining <= 0)
    {
      pendingPcmOutput_.reset();
      break;
    }

    ++totalWriteAttempts;
    const int written = output_.WriteNonBlocking(
        pendingPcmOutput_->bytes.data() + pendingPcmOutput_->writeOffset, remaining);
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
    totalBytesWritten += written;
    OnBytesWrittenLocked(pendingPcmOutput_->ptsUs, written, output_.SampleRate(), output_.FrameSizeBytes());
    pendingPcmOutput_->writeOffset += static_cast<size_t>(written);
    if (pendingPcmOutput_->writeOffset >= pendingPcmOutput_->bytes.size())
    {
      const int consumedBytes = pendingPcmOutput_->inputBytesConsumed;
      pendingPcmOutput_.reset();
      queuedDurationUs_ = QueueDurationUsLocked();
      return consumedBytes;
    }
    break;
  }
  queuedDurationUs_ = QueueDurationUsLocked();
  if (totalWriteAttempts > 0 && config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::FlushPcmQueueToHardwareLocked phase={} attempts={} "
              "writes={} bytesWritten={} lastWriteResult={} pendingPcm={} queuedDurationUs={}",
              StartupPhaseToString(startupPhase_),
              totalWriteAttempts,
              totalWriteCalls,
              totalBytesWritten,
              lastWriteResult,
              pendingPcmOutput_.has_value() ? 1 : 0,
              queuedDurationUs_);
  }
  if (!pendingPcmOutput_.has_value())
    return 0;
  return static_cast<int>(
      std::min(pendingPcmOutput_->writeOffset, pendingPcmOutput_->bytes.size()));
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
  const uint64_t submittedFrames = GetSubmittedOutputFramesLocked();
  if (anchorSinkSampleRate_ == 0 || submittedFrames <= anchorPlaybackFrames_)
    return 0;
  const uint64_t writtenFrames = submittedFrames - anchorPlaybackFrames_;
  return static_cast<int64_t>((writtenFrames * 1000000ULL) / anchorSinkSampleRate_);
}

uint64_t KodiActiveAEEngine::GetSafePlayedFramesLocked()
{
  if (!output_.IsConfigured())
    return 0;
  const uint64_t submittedFrames = GetSubmittedOutputFramesLocked();
  uint64_t rawPlayedFrames = output_.GetPlaybackFrames64();
  if (!playRequested_ || !outputStarted_)
    return std::min(lastStablePlayedFrames_, submittedFrames);

  uint64_t boundedPlayedFrames = std::min(rawPlayedFrames, submittedFrames);
  if (boundedPlayedFrames < lastStablePlayedFrames_)
    return lastStablePlayedFrames_;

  lastStablePlayedFrames_ = boundedPlayedFrames;
  return lastStablePlayedFrames_;
}

int64_t KodiActiveAEEngine::QueueDurationUsLocked() const
{
  int64_t total = 0;
  if (pendingPackedOutput_.has_value())
  {
    const auto& packet = *pendingPackedOutput_;
    const size_t totalBytes = packet.bytes.size();
    const size_t writtenBytes = std::min(packet.writeOffset, totalBytes);
    const size_t remainingBytes = totalBytes - writtenBytes;
    if (remainingBytes > 0 && packet.durationUs > 0 && totalBytes > 0)
    {
      total += static_cast<int64_t>(
          (static_cast<long double>(packet.durationUs) * static_cast<long double>(remainingBytes)) /
          static_cast<long double>(totalBytes));
    }
  }
  if (pendingPcmOutput_.has_value() && requestedFormat_.m_sampleRate > 0 && requestedFormat_.m_frameSize > 0)
  {
    const size_t remainingBytes =
        pendingPcmOutput_->bytes.size() - std::min(pendingPcmOutput_->writeOffset, pendingPcmOutput_->bytes.size());
    const int64_t frames =
        static_cast<int64_t>(remainingBytes / static_cast<size_t>(requestedFormat_.m_frameSize));
    total += (frames * 1000000LL) / requestedFormat_.m_sampleRate;
  }
  return total;
}

uint64_t KodiActiveAEEngine::QueueBytesLocked() const
{
  uint64_t total = 0;
  if (pendingPackedOutput_.has_value())
  {
    const auto& packet = *pendingPackedOutput_;
    const size_t totalBytes = packet.bytes.size();
    const size_t writtenBytes = std::min(packet.writeOffset, totalBytes);
    total += static_cast<uint64_t>(totalBytes - writtenBytes);
  }
  if (pendingPcmOutput_.has_value())
  {
    total += static_cast<uint64_t>(
        pendingPcmOutput_->bytes.size() -
        std::min(pendingPcmOutput_->writeOffset, pendingPcmOutput_->bytes.size()));
  }
  return total;
}

uint64_t KodiActiveAEEngine::GetSubmittedOutputFramesLocked() const
{
  uint64_t submittedFrames = totalWrittenFrames_;
  if (!pendingPackedOutput_.has_value() || output_.FrameSizeBytes() == 0)
    return submittedFrames;

  submittedFrames += static_cast<uint64_t>(
      std::min(pendingPackedOutput_->writeOffset, pendingPackedOutput_->bytes.size()) /
      static_cast<size_t>(output_.FrameSizeBytes()));
  return submittedFrames;
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
  const bool drained = !pendingPackedOutput_.has_value() && !pendingPcmOutput_.has_value() &&
                       totalWrittenFrames_ <= playedFrames;
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
  const uint64_t submittedFrames = GetSubmittedOutputFramesLocked();
  if (submittedFrames <= playedFrames)
    return false;

  UpdateTimestampStateLocked(TimestampState::INITIALIZING,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
  if (!output_.Play())
  {
    CLog::Log(LOGWARNING,
              "KodiActiveAEEngine::StartOutputIfPrimedLocked failed to enter PLAYING state "
              "submittedFrames={} safePlayedFrames={}",
              submittedFrames,
              playedFrames);
    return false;
  }
  systemTimeAtPlayUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  framesAtPlay_ = std::min(output_.GetPlaybackFrames64(), submittedFrames);

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
              "KodiActiveAEEngine::Startup phase={} pendingInput={} pendingPacked={} pendingPcm={} totalWrittenFrames={} "
              "submittedFrames={} safePlayedFrames={}",
              StartupPhaseToString(phase),
              pendingPassthroughInput_.has_value() ? 1 : 0,
              pendingPackedOutput_.has_value() ? 1 : 0,
              pendingPcmOutput_.has_value() ? 1 : 0,
              totalWrittenFrames_,
              GetSubmittedOutputFramesLocked(),
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
  if (pendingPackedOutput_.has_value())
    pendingPackedOutput_->writeOffset = 0;
  if (pendingPcmOutput_.has_value())
    pendingPcmOutput_->writeOffset = 0;
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
