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

#include "ServiceBroker.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace androidx_media3
{

KodiTrueHdAEEngine::~KodiTrueHdAEEngine()
{
  Reset();
}

void KodiTrueHdAEEngine::MarkReleasePendingLocked()
{
  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  releasePendingUntilUs_ = nowUs + RELEASE_PENDING_HOLD_US;
}

bool KodiTrueHdAEEngine::IsReleasePendingLocked(int64_t nowUs) const
{
  return releasePendingUntilUs_ != CURRENT_POSITION_NOT_SET && nowUs < releasePendingUntilUs_;
}







bool KodiTrueHdAEEngine::HasReachedSteadyStatePendingPackedHandoffLocked()
{
  if (!playRequested_ || !outputStarted_ || !output_.IsConfigured())
    return false;

  const uint64_t submittedFrames = GetSubmittedOutputFramesLocked();
  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  const uint64_t framesSincePlay =
      submittedFrames > framesAtPlay_ ? (submittedFrames - framesAtPlay_) : 0;
  const uint64_t playedSincePlay =
      playedFrames > framesAtPlay_ ? (playedFrames - framesAtPlay_) : 0;
  const uint64_t handoffFrames =
      static_cast<uint64_t>(std::max(1, output_.GetBufferSizeInFrames() / 4));
  return playedSincePlay > 0 || framesSincePlay >= handoffFrames;
}

std::optional<KodiTrueHdAEEngine::PendingPassthroughInput>&
KodiTrueHdAEEngine::GetPendingPassthroughInputSlotLocked(PendingPassthroughOwner owner)
{
  return owner == PendingPassthroughOwner::STARTUP ? startupPendingPassthroughInput_
                                                   : steadyStatePendingPassthroughInput_;
}

const std::optional<KodiTrueHdAEEngine::PendingPassthroughInput>&
KodiTrueHdAEEngine::GetPendingPassthroughInputSlotLocked(PendingPassthroughOwner owner) const
{
  return owner == PendingPassthroughOwner::STARTUP ? startupPendingPassthroughInput_
                                                   : steadyStatePendingPassthroughInput_;
}

KodiTrueHdAEEngine::PendingPassthroughOwner
KodiTrueHdAEEngine::GetWritableTrueHdPendingPassthroughOwnerLocked()
{
  if (startupPendingPassthroughInput_.has_value())
    return PendingPassthroughOwner::STARTUP;
  if (steadyStatePendingPassthroughInput_.has_value())
    return PendingPassthroughOwner::STEADY_STATE;

  const bool steadyStateReady = HasReachedSteadyStatePendingPackedHandoffLocked() &&
                                !startupPendingPackedOutput_.has_value();
  return steadyStateReady ? PendingPassthroughOwner::STEADY_STATE
                          : PendingPassthroughOwner::STARTUP;
}

KodiTrueHdAEEngine::PendingPassthroughOwner
KodiTrueHdAEEngine::GetActiveTrueHdPendingPassthroughOwnerLocked()
{
  if (startupPendingPassthroughInput_.has_value())
    return PendingPassthroughOwner::STARTUP;
  if (steadyStatePendingPassthroughInput_.has_value())
    return PendingPassthroughOwner::STEADY_STATE;
  return GetWritableTrueHdPendingPassthroughOwnerLocked();
}

KodiTrueHdAEEngine::PendingPassthroughOwner
KodiTrueHdAEEngine::GetActiveTrueHdPendingPackedOutputOwnerLocked()
{
  if (startupPendingPackedOutput_.has_value())
    return PendingPassthroughOwner::STARTUP;
  if (steadyStatePendingPackedOutput_.has_value())
    return PendingPassthroughOwner::STEADY_STATE;
  return GetWritableTrueHdPendingPassthroughOwnerLocked();
}

std::optional<KodiTrueHdAEEngine::PendingPassthroughInput>*
KodiTrueHdAEEngine::GetCurrentTrueHdPendingPassthroughInputSlotLocked()
{
  if (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value())
  {
    auto owner = GetActiveTrueHdPendingPackedOutputOwnerLocked();
    return &GetPendingPassthroughInputSlotLocked(owner);
  }

  auto owner = GetActiveTrueHdPendingPassthroughOwnerLocked();
  return &GetPendingPassthroughInputSlotLocked(owner);
}

bool KodiTrueHdAEEngine::ShouldRetryStartupPendingPackedRemainderLocked(int64_t nowUs,
                                                                 
                                                                 int remainingBytes,
                                                                 uint64_t playedFrames,
                                                                 int bufferFitFrames,
                                                                 int* playbackHeadDeltaFrames,
                                                                 int* bufferFitDeltaFrames,
                                                                 const char** retryReason)
{
  constexpr int kPendingRetryCooldownUs = 20000;
  constexpr int kSmallRemainderRetryCooldownUs = 5000;
  constexpr int kSmallSteadyStateRemainderBytes = 16384;
  constexpr int kMeaningfulBufferFitFramesDelta = 256;
  constexpr int kMeaningfulPreviousProgressBytes = 8192;

  if (playbackHeadDeltaFrames != nullptr)
    *playbackHeadDeltaFrames = 0;
  if (bufferFitDeltaFrames != nullptr)
    *bufferFitDeltaFrames = 0;
  if (retryReason != nullptr)
    *retryReason = nullptr;

  if (!startupPendingPackedOutput_.has_value() || startupPendingPackedOutput_->writeOffset == 0)
    return true;

  const int currentPacketId = startupPendingPackedOutput_->packetId;
  const int currentOffset = static_cast<int>(startupPendingPackedOutput_->writeOffset);
  if (startupRetryState_.packetId_ != currentPacketId || currentOffset <= 0 ||
      (startupRetryState_.lastOffsetBytes_ > 0 && currentOffset < startupRetryState_.lastOffsetBytes_))
  {
    startupRetryState_.Reset();
    startupRetryState_.packetId_ = currentPacketId;
    startupRetryState_.firstOffsetBytes_ = currentOffset;
    startupRetryState_.lastOffsetBytes_ = currentOffset;
    startupRetryState_.lastPlayedFrames_ = playedFrames;
    startupRetryState_.lastBufferFitFrames_ = bufferFitFrames;
    return true;
  }

  const uint64_t playbackHeadDelta =
      playedFrames > startupRetryState_.lastPlayedFrames_
          ? (playedFrames - startupRetryState_.lastPlayedFrames_)
          : 0;
  const int bufferFitDelta = bufferFitFrames - startupRetryState_.lastBufferFitFrames_;
  const bool playbackHeadAdvanced = playbackHeadDelta > 0;
  const bool bufferFitImproved = bufferFitDelta >= kMeaningfulBufferFitFramesDelta;
  const bool hasSuccessfulWriteForCurrentRemainder =
      startupRetryState_.lastSuccessfulWriteTimeUs_ != CURRENT_POSITION_NOT_SET &&
      startupRetryState_.lastSuccessfulWriteBytes_ > 0;
  const bool meaningfulPreviousProgress =
      startupRetryState_.lastSuccessfulWriteBytes_ >= kMeaningfulPreviousProgressBytes;
  const bool smallSteadyStateRemainder =
      false &&
      remainingBytes > 0 &&
      remainingBytes <= kSmallSteadyStateRemainderBytes &&
      hasSuccessfulWriteForCurrentRemainder;
  const int retryCooldownUs =
      smallSteadyStateRemainder ? kSmallRemainderRetryCooldownUs : kPendingRetryCooldownUs;
  const bool cooldownElapsed =
      startupRetryState_.lastAttemptTimeUs_ != CURRENT_POSITION_NOT_SET &&
      (nowUs - startupRetryState_.lastAttemptTimeUs_) >= retryCooldownUs;
  const bool playbackHeadDrivenRetryAllowed =
      playbackHeadAdvanced &&
      (true || hasSuccessfulWriteForCurrentRemainder ||
       meaningfulPreviousProgress);
  const bool bufferFitDrivenRetryAllowed =
      bufferFitImproved &&
      (true || hasSuccessfulWriteForCurrentRemainder ||
       meaningfulPreviousProgress);

  const bool retryEligible =
      playbackHeadDrivenRetryAllowed || bufferFitDrivenRetryAllowed || cooldownElapsed ||
      (false && meaningfulPreviousProgress) ||
      (true &&
       meaningfulPreviousProgress &&
       startupRetryState_.lastAttemptTimeUs_ == CURRENT_POSITION_NOT_SET);

  if (!retryEligible)
  {
    if (retryReason != nullptr)
      *retryReason = "not_eligible";
    return false;
  }

  if (playbackHeadDeltaFrames != nullptr)
    *playbackHeadDeltaFrames = static_cast<int>(std::min<uint64_t>(
        playbackHeadDelta, static_cast<uint64_t>(std::numeric_limits<int>::max())));
  if (bufferFitDeltaFrames != nullptr)
    *bufferFitDeltaFrames = std::max(0, bufferFitDelta);
  if (retryReason != nullptr) {
    if (playbackHeadDrivenRetryAllowed)
      *retryReason = "playback_head_advanced";
    else if (bufferFitDrivenRetryAllowed)
      *retryReason = "buffer_fit_improved";
    else if (smallSteadyStateRemainder && cooldownElapsed)
      *retryReason = "small_remainder_cooldown_elapsed";
    else if (meaningfulPreviousProgress)
      *retryReason = "meaningful_previous_progress";
    else
      *retryReason = "cooldown_elapsed";
  }
  return true;
}


void KodiTrueHdAEEngine::CompactPendingPassthroughInputLocked()
{
  CompactPendingPassthroughInputLocked(startupPendingPassthroughInput_);
  CompactPendingPassthroughInputLocked(steadyStatePendingPassthroughInput_);
}

void KodiTrueHdAEEngine::CompactPendingPassthroughInputLocked(
    std::optional<PendingPassthroughInput>& pendingInput)
{
  if (!pendingInput.has_value())
    return;

  auto& input = *pendingInput;
  if (input.acknowledgedBytes <= 0)
    return;

  const size_t acknowledgedPrefix =
      std::min<size_t>(static_cast<size_t>(input.acknowledgedBytes), input.bytes.size());
  if (acknowledgedPrefix == 0)
    return;

  input.bytes.erase(input.bytes.begin(), input.bytes.begin() + acknowledgedPrefix);
  input.feedOffset = input.feedOffset >= acknowledgedPrefix ? (input.feedOffset - acknowledgedPrefix) : 0;
  input.acknowledgedBytes -= static_cast<int>(acknowledgedPrefix);
  input.ptsUs = NO_PTS;

  if (input.bytes.empty())
    pendingInput.reset();
}

bool KodiTrueHdAEEngine::HasPendingPassthroughInputLocked() const
{
  return HasPendingPassthroughInputLocked(startupPendingPassthroughInput_) ||
         HasPendingPassthroughInputLocked(steadyStatePendingPassthroughInput_);
}

bool KodiTrueHdAEEngine::HasPendingPassthroughInputLocked(
    const std::optional<PendingPassthroughInput>& pendingInput) const
{
  if (!pendingInput.has_value())
    return false;

  const auto& input = *pendingInput;
  const size_t totalBytes = input.bytes.size();
  const size_t acknowledgedBytes =
      std::min<size_t>(static_cast<size_t>(std::max(0, input.acknowledgedBytes)), totalBytes);
  const size_t fedBytes = std::min(input.feedOffset, totalBytes);
  return acknowledgedBytes < totalBytes || fedBytes < totalBytes;
}

bool KodiTrueHdAEEngine::Configure(const ActiveAE::CActiveAEMediaSettings& config)
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
  directPlaybackSupportState_ = -1;

  startupPendingPassthroughInput_.reset();
  steadyStatePendingPassthroughInput_.reset();
  startupPendingPackedOutput_.reset();
  steadyStatePendingPackedOutput_.reset();
  startupRetryState_.Reset();
  pendingPcmOutput_.reset();
  queuedDurationUs_ = 0;
  totalWrittenFrames_ = 0;
  ClearCapturedValidationBurstsLocked();
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

int KodiTrueHdAEEngine::Write(const uint8_t* data,
                              int size,
                              int64_t presentation_time_us,
                              int encoded_access_unit_count)
{
  std::unique_lock lock(lock_);
  if (!configured_ || data == nullptr || size <= 0)
    return 0;

  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  lastWriteDiagnosticDetail_.clear();
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

void KodiTrueHdAEEngine::Play()
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
            "KodiTrueHdAEEngine::Play startup prePlayAcceptGapUs={} prePlayWriteGapUs={} "
            "pendingInput={} pendingPacked={} pendingPcm={} totalWrittenFrames={} safePlayedFrames={}",
            prePlayAcceptGapUs,
            prePlayWriteGapUs,
            HasPendingPassthroughInputLocked() ? 1 : 0,
            (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
            pendingPcmOutput_.has_value() ? 1 : 0,
            totalWrittenFrames_,
            GetSafePlayedFramesLocked());
  StartOutputIfPrimedLocked();
}

void KodiTrueHdAEEngine::Pause()
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

void KodiTrueHdAEEngine::Flush()
{
  std::unique_lock lock(lock_);
  playRequested_ = false;
  outputStarted_ = false;
  startupPendingPassthroughInput_.reset();
  steadyStatePendingPassthroughInput_.reset();
  startupPendingPackedOutput_.reset();
  steadyStatePendingPackedOutput_.reset();
  startupRetryState_.Reset();
  pendingPcmOutput_.reset();
  queuedDurationUs_ = 0;
  hasPendingData_ = false;
  ended_ = false;
  ClearCapturedValidationBurstsLocked();
  iecPipeline_.Reset();
  // Align stock DefaultAudioSink compatibility behavior: release on every flush.
  output_.Release();
  MarkReleasePendingLocked();
  ResetPositionLocked();
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  startupPhase_ = StartupPhase::IDLE;
}

void KodiTrueHdAEEngine::Drain()
{
  std::unique_lock lock(lock_);
  if (passthrough_)
    FlushPackedQueueToHardwareLocked();
  else
    FlushPcmQueueToHardwareLocked();
  StartOutputIfPrimedLocked();
}

void KodiTrueHdAEEngine::HandleDiscontinuity()
{
  std::unique_lock lock(lock_);
  if (config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAEEngine::HandleDiscontinuity phase={} pendingInput={} pendingPacked={} "
              "pendingPcm={} totalWrittenFrames={} safePlayedFrames={}",
              StartupPhaseToString(startupPhase_),
              HasPendingPassthroughInputLocked() ? 1 : 0,
              (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
              pendingPcmOutput_.has_value() ? 1 : 0,
              totalWrittenFrames_,
              GetSafePlayedFramesLocked());
  }
  startMediaTimeUsNeedsSync_ = true;
  pendingSyncPtsUs_ = NO_PTS;
  nextExpectedPtsValid_ = false;
  nextExpectedPtsUs_ = 0;
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

int64_t KodiTrueHdAEEngine::GetCurrentPositionUs()
{
  std::unique_lock lock(lock_);
  return ComputePositionFromHardwareLocked();
}

bool KodiTrueHdAEEngine::HasPendingData()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return false;

  if (HasPendingPassthroughInputLocked() || (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ||
      pendingPcmOutput_.has_value() || iecPipeline_.HasParserBacklog())
    return true;

  if (!output_.IsConfigured() || output_.FrameSizeBytes() == 0)
    return false;

  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  return GetSubmittedOutputFramesLocked() > playedFrames;
}

bool KodiTrueHdAEEngine::IsEnded()
{
  std::unique_lock lock(lock_);
  if (!configured_)
    return true;
  ended_ = !HasPendingData();
  return ended_;
}

bool KodiTrueHdAEEngine::IsPassthroughStartupReady()
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
  const bool outputPrimed = outputStarted_ && submittedFrames > framesAtPlay_;
  const bool hardwareAdvanced = outputStarted_ && playedFrames > framesAtPlay_;
  return outputPrimed || hardwareAdvanced || queuedFrames >= startupTargetFrames;
}

bool KodiTrueHdAEEngine::IsTrueHdSteadyStateHandoffReady()
{
  std::unique_lock lock(lock_);
  if (!configured_ || !passthrough_ || !output_.IsConfigured())
    return false;

  if (requestedFormat_.m_streamInfo.m_type != CAEStreamInfo::STREAM_TYPE_TRUEHD)
    return false;

  return HasReachedSteadyStatePendingPackedHandoffLocked() &&
         !startupPendingPackedOutput_.has_value();
}

int64_t KodiTrueHdAEEngine::GetBufferSizeUs() const
{
  std::unique_lock lock(lock_);
  if (!output_.IsConfigured() || output_.SampleRate() == 0)
    return 0;
  const int frames = output_.GetBufferSizeInFrames();
  if (frames <= 0)
    return 0;
  return static_cast<int64_t>(frames) * 1000000LL / output_.SampleRate();
}

int64_t KodiTrueHdAEEngine::GetBufferSizeBytes() const
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

int KodiTrueHdAEEngine::GetOutputSampleRate() const
{
  std::unique_lock lock(lock_);
  return static_cast<int>(output_.SampleRate());
}

int KodiTrueHdAEEngine::GetOutputChannelCount() const
{
  std::unique_lock lock(lock_);
  return static_cast<int>(output_.ChannelCount());
}

int KodiTrueHdAEEngine::GetOutputEncoding() const
{
  std::unique_lock lock(lock_);
  return output_.Encoding();
}

int KodiTrueHdAEEngine::GetOutputAudioTrackState() const
{
  std::unique_lock lock(lock_);
  return output_.AudioTrackState();
}

int KodiTrueHdAEEngine::GetOutputUnderrunCount() const
{
  std::unique_lock lock(lock_);
  return output_.GetUnderrunCount();
}

int KodiTrueHdAEEngine::GetOutputRestartCount() const
{
  std::unique_lock lock(lock_);
  return output_.GetRestartCount();
}

int KodiTrueHdAEEngine::GetDirectPlaybackSupportState() const
{
  std::unique_lock lock(lock_);
  return directPlaybackSupportState_;
}

bool KodiTrueHdAEEngine::IsOutputStarted() const
{
  std::unique_lock lock(lock_);
  return outputStarted_ || output_.IsPlaying();
}

void KodiTrueHdAEEngine::ProbePassthroughStartupBuffer(const uint8_t* data,
                                                       int size,
                                                       int64_t presentation_time_us,
                                                       int encoded_access_unit_count)
{
  std::unique_lock lock(lock_);
  if (!configured_ || !passthrough_ || playRequested_ || data == nullptr || size <= 0)
    return;
  if (output_.IsConfigured())
    return;

  KodiTrueHdIecPipeline probePipeline;
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

std::string KodiTrueHdAEEngine::ConsumeLastWriteDiagnosticDetail()
{
  std::unique_lock lock(lock_);
  std::string value = std::move(lastWriteDiagnosticDetail_);
  lastWriteDiagnosticDetail_.clear();
  return value;
}

bool KodiTrueHdAEEngine::ConsumeNextCapturedPackedBurst(std::vector<uint8_t>& bytes, int64_t& ptsUs)
{
  std::unique_lock lock(lock_);
  if (capturedPackedBursts_.empty())
    return false;
  auto burst = std::move(capturedPackedBursts_.front());
  capturedPackedBursts_.pop_front();
  bytes = std::move(burst.bytes);
  ptsUs = burst.ptsUs;
  return true;
}

bool KodiTrueHdAEEngine::ConsumeNextCapturedAudioTrackWriteBurst(std::vector<uint8_t>& bytes,
                                                                 int64_t& ptsUs)
{
  std::unique_lock lock(lock_);
  if (capturedAudioTrackWriteBursts_.empty())
    return false;
  auto burst = std::move(capturedAudioTrackWriteBursts_.front());
  capturedAudioTrackWriteBursts_.pop_front();
  bytes = std::move(burst.bytes);
  ptsUs = burst.ptsUs;
  return true;
}

bool KodiTrueHdAEEngine::IsReleasePending()
{
  std::unique_lock lock(lock_);
  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  return IsReleasePendingLocked(nowUs);
}

void KodiTrueHdAEEngine::Reset()
{
  std::unique_lock lock(lock_);
  configured_ = false;
  playRequested_ = false;
  outputStarted_ = false;
  passthrough_ = false;
  ended_ = false;
  hasPendingData_ = false;
  directPlaybackSupportState_ = -1;
  lastWriteOutputBytes_ = 0;
  lastWriteErrorCode_ = 0;
  lastWriteDiagnosticDetail_.clear();
  startupPendingPassthroughInput_.reset();
  steadyStatePendingPassthroughInput_.reset();
  startupPendingPackedOutput_.reset();
  steadyStatePendingPackedOutput_.reset();
  startupRetryState_.Reset();
  pendingPcmOutput_.reset();
  queuedDurationUs_ = 0;
  ClearCapturedValidationBurstsLocked();
  iecPipeline_.Reset();
  output_.Release();
  MarkReleasePendingLocked();
  ResetPositionLocked();
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  startupPhase_ = StartupPhase::IDLE;
}

int KodiTrueHdAEEngine::WritePassthroughLocked(const uint8_t* data,
                                               int size,
                                               int64_t ptsUs,
                                               int encodedAccessUnitCount)
{
  if (requestedFormat_.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
    return WriteTrueHdPassthroughLocked(data, size, ptsUs, encodedAccessUnitCount);

  return WritePassthroughLockedBaseline(data, size, ptsUs, encodedAccessUnitCount);
}

int KodiTrueHdAEEngine::WritePassthroughLockedBaseline(const uint8_t* data,
                                                       int size,
                                                       int64_t ptsUs,
                                                       int encodedAccessUnitCount)
{
  auto& pendingInput = startupPendingPassthroughInput_;

  (void)encodedAccessUnitCount;
  if (data == nullptr || size <= 0)
    return 0;

  int acknowledgedThisCall = 0;

  if ((startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()))
  {
    acknowledgedThisCall = FlushPackedQueueToHardwareLocked();
    if (acknowledgedThisCall > 0 && pendingInput.has_value())
    {
      pendingInput->acknowledgedBytes += acknowledgedThisCall;
      iecPipeline_.AcknowledgeConsumedInputBytes(acknowledgedThisCall);
      if (pendingInput->acknowledgedBytes >= static_cast<int>(pendingInput->bytes.size()))
      {
        pendingInput.reset();
      }
      return acknowledgedThisCall;
    }
    if ((startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) || pendingInput.has_value())
      return 0;
  }

  if (startMediaTimeUsNeedsSync_ && !TryResolvePendingDiscontinuityLocked())
    return 0;

  if (!pendingInput.has_value())
  {
    PendingPassthroughInput input;
    input.bytes.assign(data, data + size);
    input.feedOffset = 0;
    input.acknowledgedBytes = 0;
    input.ptsUs = ptsUs;
    input.encodedAccessUnitCount = std::max(1, encodedAccessUnitCount);
    pendingInput = std::move(input);
  }

  if (!(startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) && pendingInput.has_value())
  {
    auto& input = *pendingInput;
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
      RecordPackedBurstLocked(packet);
      startupPendingPackedOutput_ = std::move(packet);
    }
  }

  if (pendingInput.has_value() && !(startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) &&
      pendingInput->feedOffset >= pendingInput->bytes.size())
  {
    const int absorbedBytes =
        static_cast<int>(pendingInput->feedOffset) - pendingInput->acknowledgedBytes;
    if (absorbedBytes > 0)
    {
      pendingInput->acknowledgedBytes += absorbedBytes;
      iecPipeline_.AcknowledgeConsumedInputBytes(absorbedBytes);
      pendingInput.reset();
      return absorbedBytes;
    }
  }

  if ((startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) && !output_.IsConfigured()) {
    if (startupPendingPackedOutput_.has_value()) {
        EnsurePassthroughOutputConfiguredLocked(*startupPendingPackedOutput_);
    } else if (steadyStatePendingPackedOutput_.has_value()) {
        EnsurePassthroughOutputConfiguredLocked(steadyStatePendingPackedOutput_->packet);
    }
  }
  acknowledgedThisCall = FlushPackedQueueToHardwareLocked();
  StartOutputIfPrimedLocked();

  if (acknowledgedThisCall > 0 && pendingInput.has_value())
  {
    pendingInput->acknowledgedBytes += acknowledgedThisCall;
    iecPipeline_.AcknowledgeConsumedInputBytes(acknowledgedThisCall);
    if (pendingInput->acknowledgedBytes >= static_cast<int>(pendingInput->bytes.size()))
    {
      pendingInput.reset();
    }
    return acknowledgedThisCall;
  }

  if (config_.iecVerboseLogging && !playRequested_ && (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()))
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAEEngine::WritePassthroughLocked paused backpressure pendingInput={} "
              "pendingPacked={} queuedDurationUs={} queuedBytes={}",
              pendingInput.has_value() ? 1 : 0,
              (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
              QueueDurationUsLocked(),
              QueueBytesLocked());
  }
  return 0;
}

int KodiTrueHdAEEngine::WriteTrueHdPassthroughLocked(const uint8_t* data,
                                                     int size,
                                                     int64_t ptsUs,
                                                     int encodedAccessUnitCount)
{
  constexpr int kMaxTrueHdBurstsPerWrite = 4;
  if (data == nullptr || size <= 0)
    return 0;

  int acknowledgedThisCall = 0;
  int totalAcknowledgedThisCall = 0;
  int burstIterations = 0;

  while (burstIterations < kMaxTrueHdBurstsPerWrite)
  {
    acknowledgedThisCall = 0;

    if ((startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()))
    {
      acknowledgedThisCall = FlushPackedQueueToHardwareLocked();
      if (acknowledgedThisCall > 0)
      {
        auto* pendingInputSlot = GetCurrentTrueHdPendingPassthroughInputSlotLocked();
        if (pendingInputSlot != nullptr && pendingInputSlot->has_value())
        {
          pendingInputSlot->value().acknowledgedBytes += acknowledgedThisCall;
          iecPipeline_.AcknowledgeConsumedInputBytes(acknowledgedThisCall);
          CompactPendingPassthroughInputLocked(*pendingInputSlot);
          totalAcknowledgedThisCall += acknowledgedThisCall;
        }
      }
      if ((startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()))
        return totalAcknowledgedThisCall;
    }

    if (startMediaTimeUsNeedsSync_ && !TryResolvePendingDiscontinuityLocked())
    {
      if (config_.iecVerboseLogging)
      {
        CLog::Log(LOGINFO,
                  "KodiTrueHdAEEngine::WriteTrueHdPassthroughLocked blockedByDiscontinuity "
                  "phase={} totalAck={} pendingInput={} pendingPacked={} totalWrittenFrames={} "
                  "safePlayedFrames={}",
                  StartupPhaseToString(startupPhase_),
                  totalAcknowledgedThisCall,
                  HasPendingPassthroughInputLocked() ? 1 : 0,
                  (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
                  totalWrittenFrames_,
                  GetSafePlayedFramesLocked());
      }
      return totalAcknowledgedThisCall;
    }

    auto* pendingInputSlot = GetCurrentTrueHdPendingPassthroughInputSlotLocked();
    if (pendingInputSlot == nullptr || !pendingInputSlot->has_value())
    {
      PendingPassthroughInput input;
      input.bytes.assign(data, data + size);
      input.feedOffset = 0;
      input.acknowledgedBytes = 0;
      input.ptsUs = ptsUs;
      input.encodedAccessUnitCount = std::max(1, encodedAccessUnitCount);
      auto owner = GetWritableTrueHdPendingPassthroughOwnerLocked();
      auto& pendingInput = GetPendingPassthroughInputSlotLocked(owner);
      pendingInput = std::move(input);
      pendingInputSlot = &pendingInput;
    }
    else
    {
      CompactPendingPassthroughInputLocked(*pendingInputSlot);
      if (!pendingInputSlot->has_value())
      {
        continue;
      }
      auto& input = pendingInputSlot->value();
      const int currentBufferedBytes = static_cast<int>(input.bytes.size());
      if (size > currentBufferedBytes && config_.iecVerboseLogging)
      {
        CLog::Log(LOGINFO,
                  "KodiTrueHdAEEngine::WriteTrueHdPassthroughLocked holdingAdditionalInput "
                  "incomingBytes={} currentBufferedBytes={} acknowledgedBytes={} feedOffset={}",
                  size,
                  currentBufferedBytes,
                  input.acknowledgedBytes,
                  input.feedOffset);
      }
    }

    if (!(startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) &&
        pendingInputSlot != nullptr && pendingInputSlot->has_value())
    {
      auto& input = pendingInputSlot->value();
      KodiPackedAccessUnit packet;
      bool emittedPacket = false;
      const int remaining =
          static_cast<int>(input.bytes.size() - std::min(input.feedOffset, input.bytes.size()));
      const uint8_t* feedData = input.bytes.data() + input.feedOffset;
      const int64_t feedPtsUs = input.feedOffset == 0 ? input.ptsUs : NO_PTS;
      const bool backlogBeforeFeed = iecPipeline_.HasParserBacklog();
      const int consumed =
          iecPipeline_.Feed(feedData, remaining, feedPtsUs, &packet, &emittedPacket);
      const bool backlogAfterFeed = iecPipeline_.HasParserBacklog();
      if (config_.iecVerboseLogging)
      {
        CLog::Log(LOGINFO,
                  "KodiTrueHdAEEngine::WriteTrueHdPassthroughLocked feed "
                  "bytesSize={} feedOffset={} remaining={} consumed={} emittedPacket={} "
                  "backlogBefore={} backlogAfter={} pendingPacked={} inputBytesConsumed={}",
                  input.bytes.size(),
                  input.feedOffset,
                  remaining,
                  consumed,
                  emittedPacket ? 1 : 0,
                  backlogBeforeFeed ? 1 : 0,
                  backlogAfterFeed ? 1 : 0,
                  (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
                  emittedPacket ? packet.inputBytesConsumed : 0);
      }
      if (consumed > 0)
        input.feedOffset += static_cast<size_t>(consumed);
      if (emittedPacket)
      {
        packet.packetId = nextPackedPacketId_++;
        RecordPackedBurstLocked(packet);
        if (GetActiveTrueHdPendingPassthroughOwnerLocked() == PendingPassthroughOwner::STEADY_STATE)
        {
          PendingSteadyStatePackedOutput steadyStatePendingOutput;
          steadyStatePendingOutput.packet = std::move(packet);
          steadyStatePendingOutput.controlState.Reset();
          steadyStatePendingPackedOutput_ = std::move(steadyStatePendingOutput);
        }
        else
        {
          startupPendingPackedOutput_ = std::move(packet);
          startupRetryState_.Reset();
        }
      }
    }

    if (pendingInputSlot != nullptr && pendingInputSlot->has_value() &&
        !(startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) &&
        pendingInputSlot->value().feedOffset >= pendingInputSlot->value().bytes.size())
    {
      auto& input = pendingInputSlot->value();
      const int absorbedBytes = static_cast<int>(input.feedOffset) - input.acknowledgedBytes;
      if (absorbedBytes > 0)
      {
        input.acknowledgedBytes += absorbedBytes;
        iecPipeline_.AcknowledgeConsumedInputBytes(absorbedBytes);
        CompactPendingPassthroughInputLocked(*pendingInputSlot);
        totalAcknowledgedThisCall += absorbedBytes;
        return totalAcknowledgedThisCall;
      }
    }

    if ((startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) &&
        !output_.IsConfigured())
    {
      if (startupPendingPackedOutput_.has_value())
      {
        EnsurePassthroughOutputConfiguredLocked(*startupPendingPackedOutput_);
      }
      else if (steadyStatePendingPackedOutput_.has_value())
      {
        EnsurePassthroughOutputConfiguredLocked(steadyStatePendingPackedOutput_->packet);
      }
    }
    acknowledgedThisCall = FlushPackedQueueToHardwareLocked();
    StartOutputIfPrimedLocked();

    if (acknowledgedThisCall > 0)
    {
      pendingInputSlot = GetCurrentTrueHdPendingPassthroughInputSlotLocked();
      if (pendingInputSlot != nullptr && pendingInputSlot->has_value())
      {
        pendingInputSlot->value().acknowledgedBytes += acknowledgedThisCall;
        iecPipeline_.AcknowledgeConsumedInputBytes(acknowledgedThisCall);
        CompactPendingPassthroughInputLocked(*pendingInputSlot);
        totalAcknowledgedThisCall += acknowledgedThisCall;
        ++burstIterations;
        continue;
      }
    }

    break;
  }

  if (totalAcknowledgedThisCall > 0)
    return totalAcknowledgedThisCall;

  if (config_.iecVerboseLogging && !playRequested_ && (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()))
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAEEngine::WritePassthroughLocked paused backpressure pendingInput={} "
              "pendingPacked={} queuedDurationUs={} queuedBytes={}",
              HasPendingPassthroughInputLocked() ? 1 : 0,
              (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
              QueueDurationUsLocked(),
              QueueBytesLocked());
  }
  return 0;
}

int KodiTrueHdAEEngine::WritePcmLocked(const uint8_t* data, int size, int64_t ptsUs)
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

void KodiTrueHdAEEngine::EnsurePassthroughOutputConfiguredLocked(const KodiPackedAccessUnit& packet)
{
  if (packet.streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    EnsureTrueHdPassthroughOutputConfiguredLocked(packet);
    return;
  }

  EnsurePassthroughOutputConfiguredLockedBaseline(packet);
}

void KodiTrueHdAEEngine::EnsurePassthroughOutputConfiguredLockedBaseline(
    const KodiPackedAccessUnit& packet)
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
                "KodiTrueHdAEEngine::EnsurePassthroughOutputConfiguredLocked configure retry "
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
  {
    directPlaybackSupportState_ = 0;
    return;
  }

  directPlaybackSupportState_ = 1;
  releasePendingUntilUs_ = CURRENT_POSITION_NOT_SET;
  if (playRequested_ && outputStarted_ && !output_.Play())
  {
    outputStarted_ = false;
    CLog::Log(LOGWARNING,
              "KodiTrueHdAEEngine::EnsurePassthroughOutputConfiguredLocked failed to enter "
              "PLAYING state after reconfigure");
  }
  lastStablePlayedFrames_ = 0;
  UpdateTimestampStateLocked(TimestampState::INITIALIZING,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}

void KodiTrueHdAEEngine::EnsureTrueHdPassthroughOutputConfiguredLocked(const KodiPackedAccessUnit& packet)
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
    configured = output_.ConfigureTrueHd(packet.outputRate,
                                         packet.outputChannels,
                                         CJNIAudioFormat::ENCODING_IEC61937,
                                         true);
    if (configured)
      break;

    if (config_.iecVerboseLogging)
    {
      CLog::Log(LOGWARNING,
                "KodiTrueHdAEEngine::EnsurePassthroughOutputConfiguredLocked configure retry "
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
  {
    directPlaybackSupportState_ = 0;
    return;
  }

  directPlaybackSupportState_ = 1;
  releasePendingUntilUs_ = CURRENT_POSITION_NOT_SET;
  if (playRequested_ && outputStarted_ && !output_.Play())
  {
    outputStarted_ = false;
    CLog::Log(LOGWARNING,
              "KodiTrueHdAEEngine::EnsurePassthroughOutputConfiguredLocked failed to enter "
              "PLAYING state after reconfigure");
  }
  lastStablePlayedFrames_ = 0;
  UpdateTimestampStateLocked(TimestampState::INITIALIZING,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}

bool KodiTrueHdAEEngine::EnsurePcmOutputConfiguredLocked()
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
  directPlaybackSupportState_ = configured ? 1 : 0;
  if (configured)
    releasePendingUntilUs_ = CURRENT_POSITION_NOT_SET;
  return configured;
}

int KodiTrueHdAEEngine::FlushPackedQueueToHardwareLocked()
{
  if (!startupPendingPackedOutput_.has_value() && !steadyStatePendingPackedOutput_.has_value())
    return 0;

  if (startupPendingPackedOutput_.has_value() && startupPendingPackedOutput_->streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
    return FlushTrueHdPackedQueueToHardwareLocked();
  if (steadyStatePendingPackedOutput_.has_value() &&
      steadyStatePendingPackedOutput_->packet.streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
    return FlushTrueHdPackedQueueToHardwareLocked();

  return FlushPackedQueueToHardwareLockedBaseline();
}

int KodiTrueHdAEEngine::FlushPackedQueueToHardwareLockedBaseline()
{
  if (!(startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()))
    return 0;

  if (!output_.IsConfigured())
    EnsurePassthroughOutputConfiguredLocked(*startupPendingPackedOutput_);
  if (!output_.IsConfigured())
    return 0;

  int totalWriteCalls = 0;
  int totalWriteAttempts = 0;
  int totalBytesWritten = 0;
  int lastWriteResult = 0;
  bool retriedZeroWrite = false;
  while ((startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) && totalWriteCalls < MAX_WRITE_CALLS_PER_FLUSH)
  {
    const int remaining =
        static_cast<int>(startupPendingPackedOutput_->bytes.size() - startupPendingPackedOutput_->writeOffset);
    if (remaining <= 0)
    {
      startupPendingPackedOutput_.reset();
      break;
    }

    ++totalWriteAttempts;
    const int written =
        output_.WriteNonBlocking(startupPendingPackedOutput_->bytes.data() + startupPendingPackedOutput_->writeOffset,
                                 remaining);
    lastWriteResult = written;
    if (written <= 0)
    {
      if (written < 0)
      {
        lastWriteErrorCode_ = written;
        InvalidateCurrentOutputLocked();
        output_.Release();
        MarkReleasePendingLocked();
        retriedZeroWrite = false;
      }
      else if (!retriedZeroWrite)
      {
        retriedZeroWrite = true;
        int64_t sleepTimeUs = startupPendingPackedOutput_->durationUs;
        if (sleepTimeUs <= 0 && output_.SampleRate() > 0)
        {
          const unsigned int totalPacketBytes =
              static_cast<unsigned int>(startupPendingPackedOutput_->bytes.size());
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
        if (!(startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) || !output_.IsConfigured())
          break;
        continue;
      }
      break;
    }

    retriedZeroWrite = false;
    ++totalWriteCalls;
    totalBytesWritten += written;
    lastWriteOutputBytes_ += written;
    RecordAudioTrackWriteChunkLocked(*startupPendingPackedOutput_, written);
    startupPendingPackedOutput_->writeOffset += static_cast<size_t>(written);
    if (written < remaining)
    {
      // Treat a partial non-blocking write as "made progress, but the track is full now".
      // Leave the packet remainder pending and let the next flush retry after downstream time passes.
      break;
    }
    if (startupPendingPackedOutput_->writeOffset >= startupPendingPackedOutput_->bytes.size())
    {
      const KodiPackedAccessUnit completedPacket = *startupPendingPackedOutput_;
      OnBytesWrittenLocked(completedPacket.ptsUs,
                           static_cast<int>(completedPacket.bytes.size()),
                           output_.SampleRate(),
                           output_.FrameSizeBytes());
      FinalizeAudioTrackWriteBurstLocked(completedPacket);
      startupPendingPackedOutput_.reset();
      queuedDurationUs_ = QueueDurationUsLocked();
      return std::max(0, completedPacket.inputBytesConsumed);
    }
  }
  queuedDurationUs_ = QueueDurationUsLocked();
  if (totalWriteAttempts > 0 && config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAEEngine::FlushPackedQueueToHardwareLocked phase={} attempts={} "
              "writes={} bytesWritten={} lastWriteResult={} pendingPacked={} queuedDurationUs={}",
              StartupPhaseToString(startupPhase_),
              totalWriteAttempts,
              totalWriteCalls,
              totalBytesWritten,
              lastWriteResult,
              (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
              queuedDurationUs_);
  }
  return 0;
}

int KodiTrueHdAEEngine::FlushTrueHdPackedQueueToHardwareLocked()
{
  if (!(startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()))
  {
    return 0;
  }

  auto flushPendingOutput = [&](KodiPackedAccessUnit*& pendingPackedOutput,
                                auto& retryState,
                                bool isSteadyState) -> int {
    auto& pendingPackedRetryPacketId_ = retryState.packetId_;
    auto& pendingPackedRetryFirstOffsetBytes_ = retryState.firstOffsetBytes_;
    auto& pendingPackedRetryLastOffsetBytes_ = retryState.lastOffsetBytes_;
    auto& pendingPackedRetryCount_ = retryState.count_;
    auto& pendingPackedRetryZeroWriteStreak_ = retryState.zeroWriteStreak_;
    auto& pendingPackedRetryLastSuccessfulWriteBytes_ = retryState.lastSuccessfulWriteBytes_;
    auto& pendingPackedRetryLastSuccessfulWriteTimeUs_ = retryState.lastSuccessfulWriteTimeUs_;
    auto& pendingPackedRetryLastAttemptTimeUs_ = retryState.lastAttemptTimeUs_;
    auto& pendingPackedRetryLastProgressTimeUs_ = retryState.lastProgressTimeUs_;
    auto& pendingPackedRetryLastPlayedFrames_ = retryState.lastPlayedFrames_;
    auto& pendingPackedRetryLastBufferFitFrames_ = retryState.lastBufferFitFrames_;

    if (!output_.IsConfigured())
      EnsureTrueHdPassthroughOutputConfiguredLocked(*pendingPackedOutput);
    if (!output_.IsConfigured())
      return 0;

    int totalWriteCalls = 0;
    int totalWriteAttempts = 0;
    int totalBytesWritten = 0;
    int lastWriteResult = 0;
    while (pendingPackedOutput != nullptr && totalWriteCalls < MAX_WRITE_CALLS_PER_FLUSH)
    {
      lastWriteDiagnosticDetail_.clear();
      const int remaining =
          static_cast<int>(pendingPackedOutput->bytes.size() - pendingPackedOutput->writeOffset);
      if (remaining <= 0)
      {
        if (isSteadyState)
          steadyStatePendingPackedOutput_.reset();
        else
          startupPendingPackedOutput_.reset();
        pendingPackedOutput = nullptr;
        retryState.Reset();
        break;
      }

      const bool retryingPendingRemainder = pendingPackedOutput->writeOffset > 0;
      int playbackHeadDeltaFrames = 0;
      int bufferFitDeltaFrames = 0;
      const char* retryReason = nullptr;

      if (retryingPendingRemainder)
      {
        const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        const uint64_t playedFrames = GetSafePlayedFramesLocked();
        const int frameSizeBytes = output_.FrameSizeBytes();
        const int bufferSizeFrames = output_.GetBufferSizeInFrames();
        int bufferFitFrames = 0;
        if (frameSizeBytes > 0 && bufferSizeFrames > 0)
        {
          const uint64_t submittedFrames = GetSubmittedOutputFramesLocked();
          const uint64_t queuedFrames =
              submittedFrames > playedFrames ? (submittedFrames - playedFrames) : 0;
          bufferFitFrames =
              queuedFrames >= static_cast<uint64_t>(bufferSizeFrames)
                  ? 0
                  : static_cast<int>(static_cast<uint64_t>(bufferSizeFrames) - queuedFrames);
        }

        if (isSteadyState)
        {
          if (outputStarted_ && !output_.IsPlaying())
          {
            outputStarted_ = false;
            StartOutputIfPrimedLocked();
          }

          const uint64_t playbackHeadDelta =
              playedFrames > pendingPackedRetryLastPlayedFrames_
                  ? (playedFrames - pendingPackedRetryLastPlayedFrames_)
                  : 0;
          playbackHeadDeltaFrames = static_cast<int>(std::min<uint64_t>(
              playbackHeadDelta, static_cast<uint64_t>(std::numeric_limits<int>::max())));
          bufferFitDeltaFrames =
              std::max(0, bufferFitFrames - pendingPackedRetryLastBufferFitFrames_);

          if (retryReason == nullptr)
            retryReason = "steady_state_output_driven";
        }
        else if (output_.IsPlaying())
        {
          const bool canRetryStartupRemainder = ShouldRetryStartupPendingPackedRemainderLocked(
              nowUs,
              remaining,
              playedFrames,
              bufferFitFrames,
              &playbackHeadDeltaFrames,
              &bufferFitDeltaFrames,
              &retryReason);
          if (!canRetryStartupRemainder)
          {
            const int64_t sinceLastSuccessfulWriteMs =
                pendingPackedRetryLastSuccessfulWriteTimeUs_ == CURRENT_POSITION_NOT_SET
                    ? 0
                    : std::max<int64_t>(
                          0,
                          (nowUs - pendingPackedRetryLastSuccessfulWriteTimeUs_) / 1000);
            lastWriteDiagnosticDetail_ =
                "requestedBytes=" + std::to_string(remaining) +
                " pendingRemainderId=" + std::to_string(pendingPackedOutput->packetId) +
                " packetId=" + std::to_string(pendingPackedOutput->packetId) +
                " ownership=" + std::string(isSteadyState ? "steady_state" : "startup") +
                " firstOffsetBytes=" + std::to_string(pendingPackedRetryFirstOffsetBytes_) +
                " offsetBytes=" + std::to_string(pendingPackedOutput->writeOffset) +
                " bytesRemaining=" + std::to_string(remaining) +
                " lastWriteBytes=" + std::to_string(pendingPackedRetryLastSuccessfulWriteBytes_) +
                " pendingRemainderRetryCount=" + std::to_string(pendingPackedRetryCount_) +
                " retryCount=" + std::to_string(pendingPackedRetryCount_) +
                " pendingRemainderLastProgressUs=" +
                std::to_string(pendingPackedRetryLastProgressTimeUs_) +
                " sinceLastSuccessfulWriteMs=" + std::to_string(sinceLastSuccessfulWriteMs) +
                " playbackHeadDeltaFrames=" + std::to_string(playbackHeadDeltaFrames) +
                " bufferFitDeltaFrames=" + std::to_string(bufferFitDeltaFrames) +
                " pendingRemainderRetryEligibleReason=" +
                std::string(retryReason != nullptr ? retryReason : "not_eligible") +
                " retryEligibleReason=" +
                std::string(retryReason != nullptr ? retryReason : "not_eligible");
            break;
          }
        }
      }

      ++totalWriteAttempts;
      const int written = output_.WriteNonBlocking(
          pendingPackedOutput->bytes.data() + pendingPackedOutput->writeOffset, remaining);
      lastWriteResult = written;
      if (retryingPendingRemainder)
      {
        const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        pendingPackedRetryCount_ += 1;
        pendingPackedRetryPacketId_ = pendingPackedOutput->packetId;
        if (pendingPackedRetryFirstOffsetBytes_ <= 0)
        {
          pendingPackedRetryFirstOffsetBytes_ =
              static_cast<int>(pendingPackedOutput->writeOffset);
        }
        pendingPackedRetryLastOffsetBytes_ = static_cast<int>(pendingPackedOutput->writeOffset);
        pendingPackedRetryLastAttemptTimeUs_ = nowUs;
        pendingPackedRetryLastPlayedFrames_ = GetSafePlayedFramesLocked();
        const int frameSizeBytes = output_.FrameSizeBytes();
        const int bufferSizeFrames = output_.GetBufferSizeInFrames();
        if (frameSizeBytes > 0 && bufferSizeFrames > 0)
        {
          const uint64_t submittedFrames = GetSubmittedOutputFramesLocked();
          const uint64_t queuedFrames =
              submittedFrames > pendingPackedRetryLastPlayedFrames_
                  ? (submittedFrames - pendingPackedRetryLastPlayedFrames_)
                  : 0;
          pendingPackedRetryLastBufferFitFrames_ =
              queuedFrames >= static_cast<uint64_t>(bufferSizeFrames)
                  ? 0
                  : static_cast<int>(static_cast<uint64_t>(bufferSizeFrames) - queuedFrames);
        }
        const int64_t sinceLastSuccessfulWriteMs =
            pendingPackedRetryLastSuccessfulWriteTimeUs_ == CURRENT_POSITION_NOT_SET
                ? 0
                : std::max<int64_t>(
                      0,
                      (nowUs - pendingPackedRetryLastSuccessfulWriteTimeUs_) / 1000);
        const char* fallbackRetryReason =
            retryReason != nullptr
                ? retryReason
                : (isSteadyState ? "steady_state_output_driven"
                                 : "startup_retry_reason_unset");
        lastWriteDiagnosticDetail_ =
            "requestedBytes=" + std::to_string(remaining) +
            " pendingRemainderId=" + std::to_string(pendingPackedOutput->packetId) +
            " packetId=" + std::to_string(pendingPackedOutput->packetId) +
            " ownership=" + std::string(isSteadyState ? "steady_state" : "startup") +
            " firstOffsetBytes=" + std::to_string(pendingPackedRetryFirstOffsetBytes_) +
            " offsetBytes=" + std::to_string(pendingPackedOutput->writeOffset) +
            " bytesRemaining=" + std::to_string(remaining) +
            " lastWriteBytes=" + std::to_string(pendingPackedRetryLastSuccessfulWriteBytes_) +
            " pendingRemainderRetryCount=" + std::to_string(pendingPackedRetryCount_) +
            " pendingRemainderLastProgressUs=" +
            std::to_string(pendingPackedRetryLastProgressTimeUs_) +
            " sinceLastSuccessfulWriteMs=" + std::to_string(sinceLastSuccessfulWriteMs) +
            " playbackHeadDeltaFrames=" + std::to_string(playbackHeadDeltaFrames) +
            " bufferFitDeltaFrames=" + std::to_string(bufferFitDeltaFrames) +
            " retryCount=" + std::to_string(pendingPackedRetryCount_) +
            " retryReason=" + std::string(fallbackRetryReason) +
            " pendingRemainderRetryEligibleReason=" +
            std::string(fallbackRetryReason) +
            " retryEligibleReason=" + std::string(fallbackRetryReason);
      }
      if (written <= 0)
      {
        if (written == 0 && retryingPendingRemainder && isSteadyState)
        {
          const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
          pendingPackedRetryZeroWriteStreak_ += 1;
          retryReason = "steady_state_packet_duration_backoff";
          const int64_t sinceLastSuccessfulWriteMs =
              pendingPackedRetryLastSuccessfulWriteTimeUs_ == CURRENT_POSITION_NOT_SET
                  ? 0
                  : std::max<int64_t>(
                        0,
                        (nowUs - pendingPackedRetryLastSuccessfulWriteTimeUs_) / 1000);
          lastWriteDiagnosticDetail_ =
              "requestedBytes=" + std::to_string(remaining) +
              " pendingRemainderId=" + std::to_string(pendingPackedOutput->packetId) +
              " packetId=" + std::to_string(pendingPackedOutput->packetId) +
              " ownership=steady_state" +
              " firstOffsetBytes=" + std::to_string(pendingPackedRetryFirstOffsetBytes_) +
              " offsetBytes=" + std::to_string(pendingPackedOutput->writeOffset) +
              " bytesRemaining=" + std::to_string(remaining) +
              " lastWriteBytes=" + std::to_string(pendingPackedRetryLastSuccessfulWriteBytes_) +
              " pendingRemainderRetryCount=" + std::to_string(pendingPackedRetryCount_) +
              " retryCount=" + std::to_string(pendingPackedRetryCount_) +
              " pendingRemainderLastProgressUs=" +
              std::to_string(pendingPackedRetryLastProgressTimeUs_) +
              " sinceLastSuccessfulWriteMs=" + std::to_string(sinceLastSuccessfulWriteMs) +
              " playbackHeadDeltaFrames=" + std::to_string(playbackHeadDeltaFrames) +
              " bufferFitDeltaFrames=" + std::to_string(bufferFitDeltaFrames) +
              " retryReason=steady_state_packet_duration_backoff" +
              " pendingRemainderRetryEligibleReason=steady_state_packet_duration_backoff" +
              " retryEligibleReason=steady_state_packet_duration_backoff";
        }
        if (written < 0)
        {
          lastWriteErrorCode_ = written;
          InvalidateCurrentOutputLocked();
          output_.Release();
          MarkReleasePendingLocked();
          retryState.Reset();
        }
        break;
      }

      ++totalWriteCalls;
      totalBytesWritten += written;
      lastWriteOutputBytes_ += written;
      RecordAudioTrackWriteChunkLocked(*pendingPackedOutput, written);
      pendingPackedOutput->writeOffset += static_cast<size_t>(written);
      pendingPackedRetryLastSuccessfulWriteBytes_ = written;
      pendingPackedRetryLastSuccessfulWriteTimeUs_ =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      pendingPackedRetryLastProgressTimeUs_ = pendingPackedRetryLastSuccessfulWriteTimeUs_;
      pendingPackedRetryLastOffsetBytes_ = static_cast<int>(pendingPackedOutput->writeOffset);
      if (isSteadyState)
      {
        pendingPackedRetryZeroWriteStreak_ = 0;
      }

      if (written < remaining)
      {
        break;
      }
      if (pendingPackedOutput->writeOffset >= pendingPackedOutput->bytes.size())
      {
        const KodiPackedAccessUnit completedPacket = *pendingPackedOutput;
        OnBytesWrittenLocked(completedPacket.ptsUs,
                             static_cast<int>(completedPacket.bytes.size()),
                             output_.SampleRate(),
                             output_.FrameSizeBytes());
        FinalizeAudioTrackWriteBurstLocked(completedPacket);
        if (isSteadyState)
          steadyStatePendingPackedOutput_.reset();
        else
          startupPendingPackedOutput_.reset();
        pendingPackedOutput = nullptr;
        retryState.Reset();
        queuedDurationUs_ = QueueDurationUsLocked();
        return std::max(0, completedPacket.inputBytesConsumed);
      }
    }
    queuedDurationUs_ = QueueDurationUsLocked();
    return 0;
  };

  if (startupPendingPackedOutput_.has_value())
  {
    KodiPackedAccessUnit* pendingPackedOutput = &*startupPendingPackedOutput_;
    return flushPendingOutput(pendingPackedOutput, startupRetryState_, false);
  }

  if (steadyStatePendingPackedOutput_.has_value())
  {
    KodiPackedAccessUnit* pendingPackedOutput = &steadyStatePendingPackedOutput_->packet;
    return flushPendingOutput(
        pendingPackedOutput, steadyStatePendingPackedOutput_->controlState, true);
  }

  queuedDurationUs_ = QueueDurationUsLocked();
  return 0;
}

int KodiTrueHdAEEngine::FlushPcmQueueToHardwareLocked()
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
              "KodiTrueHdAEEngine::FlushPcmQueueToHardwareLocked phase={} attempts={} "
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

void KodiTrueHdAEEngine::OnBytesWrittenLocked(int64_t packetPtsUs,
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

void KodiTrueHdAEEngine::RecordPackedBurstLocked(const KodiPackedAccessUnit& packet)
{
  if (packet.bytes.empty())
    return;
  CapturedValidationBurst burst;
  burst.bytes = packet.bytes;
  burst.ptsUs = packet.ptsUs;
  capturedPackedBursts_.push_back(std::move(burst));
}

void KodiTrueHdAEEngine::RecordAudioTrackWriteChunkLocked(const KodiPackedAccessUnit& packet,
                                                          int bytesWritten)
{
  if (bytesWritten <= 0 || packet.bytes.empty())
    return;
  const size_t startOffset = packet.writeOffset;
  const size_t endOffset =
      std::min(packet.bytes.size(), startOffset + static_cast<size_t>(bytesWritten));
  if (endOffset <= startOffset)
    return;
  if (pendingAudioTrackWriteCapture_.empty())
    pendingAudioTrackWriteCapturePtsUs_ = packet.ptsUs;
  pendingAudioTrackWriteCapture_.insert(pendingAudioTrackWriteCapture_.end(),
                                        packet.bytes.begin() + static_cast<ptrdiff_t>(startOffset),
                                        packet.bytes.begin() + static_cast<ptrdiff_t>(endOffset));
}

void KodiTrueHdAEEngine::FinalizeAudioTrackWriteBurstLocked(const KodiPackedAccessUnit& packet)
{
  if (pendingAudioTrackWriteCapture_.empty())
    return;
  CapturedValidationBurst burst;
  burst.bytes = std::move(pendingAudioTrackWriteCapture_);
  burst.ptsUs = pendingAudioTrackWriteCapturePtsUs_ != NO_PTS ? pendingAudioTrackWriteCapturePtsUs_
                                                              : packet.ptsUs;
  capturedAudioTrackWriteBursts_.push_back(std::move(burst));
  pendingAudioTrackWriteCapture_.clear();
  pendingAudioTrackWriteCapturePtsUs_ = NO_PTS;
}

void KodiTrueHdAEEngine::ClearCapturedValidationBurstsLocked()
{
  capturedPackedBursts_.clear();
  capturedAudioTrackWriteBursts_.clear();
  pendingAudioTrackWriteCapture_.clear();
  pendingAudioTrackWriteCapturePtsUs_ = NO_PTS;
}

int64_t KodiTrueHdAEEngine::ComputePositionFromHardwareLocked()
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

int64_t KodiTrueHdAEEngine::GetAudioOutputPositionUsLocked()
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

int64_t KodiTrueHdAEEngine::ApplyMediaPositionParametersLocked(int64_t outputPositionUs)
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

int64_t KodiTrueHdAEEngine::ApplySkippingLocked(int64_t mediaPositionUs) const
{
  if (anchorMediaSampleRate_ == 0)
    return mediaPositionUs;
  const int64_t skippedDurationUs =
      static_cast<int64_t>((skippedOutputFrameCount_ * 1000000ULL) / anchorMediaSampleRate_);
  return mediaPositionUs + skippedDurationUs;
}

int64_t KodiTrueHdAEEngine::GetWrittenAudioOutputPositionUsLocked() const
{
  if (anchorSinkSampleRate_ == 0 || totalWrittenFrames_ <= anchorPlaybackFrames_)
    return 0;
  const uint64_t writtenFrames = totalWrittenFrames_ - anchorPlaybackFrames_;
  return static_cast<int64_t>((writtenFrames * 1000000ULL) / anchorSinkSampleRate_);
}

uint64_t KodiTrueHdAEEngine::GetSafePlayedFramesLocked()
{
  return GetSafePlayedFramesLockedBaseline();
}

uint64_t KodiTrueHdAEEngine::GetSafePlayedFramesLockedBaseline()
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

int64_t KodiTrueHdAEEngine::QueueDurationUsLocked() const
{
  int64_t total = 0;
  if (startupPendingPackedOutput_.has_value())
  {
    const auto& packet = *startupPendingPackedOutput_;
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
  if (steadyStatePendingPackedOutput_.has_value())
  {
    const auto& packet = steadyStatePendingPackedOutput_->packet;
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

uint64_t KodiTrueHdAEEngine::QueueBytesLocked() const
{
  uint64_t total = 0;
  if (startupPendingPackedOutput_.has_value())
  {
    const auto& packet = *startupPendingPackedOutput_;
    const size_t totalBytes = packet.bytes.size();
    const size_t writtenBytes = std::min(packet.writeOffset, totalBytes);
    total += static_cast<uint64_t>(totalBytes - writtenBytes);
  }
  if (steadyStatePendingPackedOutput_.has_value())
  {
    const auto& packet = steadyStatePendingPackedOutput_->packet;
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

uint64_t KodiTrueHdAEEngine::GetSubmittedOutputFramesLocked() const
{
  uint64_t submittedFrames = totalWrittenFrames_;
  if (output_.FrameSizeBytes() == 0) return submittedFrames;
  if (startupPendingPackedOutput_.has_value()) {
      submittedFrames += static_cast<uint64_t>(
          std::min(startupPendingPackedOutput_->writeOffset, startupPendingPackedOutput_->bytes.size()) /
          static_cast<size_t>(output_.FrameSizeBytes()));
  }
  if (steadyStatePendingPackedOutput_.has_value()) {
      submittedFrames += static_cast<uint64_t>(
          std::min(steadyStatePendingPackedOutput_->packet.writeOffset, steadyStatePendingPackedOutput_->packet.bytes.size()) /
          static_cast<size_t>(output_.FrameSizeBytes()));
  }
  return submittedFrames;
}

void KodiTrueHdAEEngine::UpdateExpectedPtsLocked(int64_t packetPtsUs, int64_t packetDurationUs)
{
  if (packetPtsUs == NO_PTS)
    return;

  const bool suppressTrueHdPacketPtsDiscontinuity =
      passthrough_ && requestedFormat_.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD &&
      startupPhase_ != StartupPhase::RUNNING;
  if (suppressTrueHdPacketPtsDiscontinuity)
  {
    nextExpectedPtsUs_ = packetPtsUs + std::max<int64_t>(0, packetDurationUs);
    nextExpectedPtsValid_ = true;
    return;
  }

  const bool isDiscontinuity =
      nextExpectedPtsValid_ &&
      std::llabs(packetPtsUs - nextExpectedPtsUs_) > DISCONTINUITY_THRESHOLD_US;
  if (isDiscontinuity)
  {
    // Stock-like handling: request retime after drain; don't immediately jump
    // timing while there is still pending output.
    if (config_.iecVerboseLogging)
    {
      CLog::Log(LOGINFO,
                "KodiTrueHdAEEngine::UpdateExpectedPtsLocked discontinuity packetPtsUs={} "
                "nextExpectedPtsUs={} packetDurationUs={} phase={} totalWrittenFrames={} "
                "safePlayedFrames={}",
                packetPtsUs,
                nextExpectedPtsUs_,
                packetDurationUs,
                StartupPhaseToString(startupPhase_),
                totalWrittenFrames_,
                GetSafePlayedFramesLocked());
    }
    startMediaTimeUsNeedsSync_ = true;
    pendingSyncPtsUs_ = packetPtsUs;
    return;
  }

  nextExpectedPtsUs_ = packetPtsUs + std::max<int64_t>(0, packetDurationUs);
  nextExpectedPtsValid_ = true;
}

bool KodiTrueHdAEEngine::TryResolvePendingDiscontinuityLocked()
{
  if (!startMediaTimeUsNeedsSync_)
    return true;

  if (passthrough_)
    FlushPackedQueueToHardwareLocked();
  else
    FlushPcmQueueToHardwareLocked();

  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  const bool drained = !HasPendingPassthroughInputLocked() && !(startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) &&
                       !pendingPcmOutput_.has_value() && totalWrittenFrames_ <= playedFrames;
  if (config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAEEngine::TryResolvePendingDiscontinuityLocked drained={} phase={} "
              "pendingPacked={} pendingPcm={} totalWrittenFrames={} safePlayedFrames={} "
              "pendingSyncPtsUs={}",
              drained ? 1 : 0,
              StartupPhaseToString(startupPhase_),
              (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
              pendingPcmOutput_.has_value() ? 1 : 0,
              totalWrittenFrames_,
              playedFrames,
              pendingSyncPtsUs_);
  }
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

void KodiTrueHdAEEngine::ReanchorForDiscontinuityLocked(int64_t packetPtsUs)
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

bool KodiTrueHdAEEngine::StartOutputIfPrimedLocked()
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
              "KodiTrueHdAEEngine::StartOutputIfPrimedLocked failed to enter PLAYING state "
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

void KodiTrueHdAEEngine::SetStartupPhaseLocked(StartupPhase phase)
{
  if (startupPhase_ == phase)
    return;
  startupPhase_ = phase;
  if (config_.iecVerboseLogging)
  {
    CLog::Log(LOGINFO,
              "KodiTrueHdAEEngine::Startup phase={} pendingInput={} pendingPacked={} pendingPcm={} totalWrittenFrames={} "
              "submittedFrames={} safePlayedFrames={}",
              StartupPhaseToString(phase),
              HasPendingPassthroughInputLocked() ? 1 : 0,
              (startupPendingPackedOutput_.has_value() || steadyStatePendingPackedOutput_.has_value()) ? 1 : 0,
              pendingPcmOutput_.has_value() ? 1 : 0,
              totalWrittenFrames_,
              totalWrittenFrames_,
              GetSafePlayedFramesLocked());
  }
}

const char* KodiTrueHdAEEngine::StartupPhaseToString(StartupPhase phase) const
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

void KodiTrueHdAEEngine::UpdateTimestampStateLocked(TimestampState state, int64_t systemTimeUs)
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

bool KodiTrueHdAEEngine::IsTimestampAdvancingFromInitialLocked(uint64_t tsFrames,
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

void KodiTrueHdAEEngine::ResetPositionLocked()
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
  lastWriteDiagnosticDetail_.clear();
  mediaPositionParameters_ = {hostClockSpeed_, 0, 0, 0};
  mediaPositionParametersCheckpoints_.clear();
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastStablePlayedFrames_ = 0;
  ResetOutputPositionEstimatorLocked();
}

void KodiTrueHdAEEngine::InvalidateCurrentOutputLocked()
{
  outputStarted_ = false;
  if (startupPendingPackedOutput_.has_value())
    startupPendingPackedOutput_->writeOffset = 0;
  if (steadyStatePendingPackedOutput_.has_value())
    steadyStatePendingPackedOutput_->packet.writeOffset = 0;
  if (pendingPcmOutput_.has_value())
    pendingPcmOutput_->writeOffset = 0;
  startupRetryState_.Reset();
  if (steadyStatePendingPackedOutput_.has_value())
    steadyStatePendingPackedOutput_->controlState.Reset();
  ResetPositionLocked();
}

void KodiTrueHdAEEngine::ResetOutputPositionEstimatorLocked()
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
