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

namespace androidx_media3
{

KodiActiveAEEngine::~KodiActiveAEEngine()
{
  Reset();
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
  totalWrittenFrames_ = 0;
  iecPipeline_.Configure(requestedFormat_);
  output_.Release();
  ResetPositionLocked();

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

  // Deferred prime: write queued data only after play() is requested.
  if (passthrough_)
    FlushPackedQueueToHardwareLocked();
  else
    FlushPcmQueueToHardwareLocked();

  bool started = StartOutputIfPrimedLocked();

  // One-shot startup recovery for passthrough invalidation:
  // if the initial prime/start made no progress and queue data remains, rebuild
  // output and re-prime before attempting to start again.
  if (passthrough_ && !started && !packedQueue_.empty() && totalWrittenFrames_ == 0)
  {
    CLog::Log(LOGWARNING,
              "KodiActiveAEEngine::Play startup recovery trigger packedQueue={} totalWrittenFrames={}",
              packedQueue_.size(),
              totalWrittenFrames_);

    output_.Release();
    outputStarted_ = false;
    EnsurePassthroughOutputConfiguredLocked(packedQueue_.front());
    ResetPositionLocked();

    const uint64_t recoveryWrittenFramesBefore = totalWrittenFrames_;
    FlushPackedQueueToHardwareLocked();
    const uint64_t recoveryWrittenFramesAfter = totalWrittenFrames_;
    started = StartOutputIfPrimedLocked();
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::Play startup recovery refill wroteFramesDelta={} packedQueue={} started={}",
              recoveryWrittenFramesAfter >= recoveryWrittenFramesBefore
                  ? (recoveryWrittenFramesAfter - recoveryWrittenFramesBefore)
                  : 0,
              packedQueue_.size(),
              started ? 1 : 0);
  }

  // Post-start refill guard: top off once playback has started.
  if (started)
  {
    const uint64_t writtenFramesBeforeRefill = totalWrittenFrames_;
    const uint64_t playedFramesBeforeRefill = GetSafePlayedFramesLocked();
    if (passthrough_)
      FlushPackedQueueToHardwareLocked();
    else
      FlushPcmQueueToHardwareLocked();
    const uint64_t writtenFramesAfterRefill = totalWrittenFrames_;
    const uint64_t playedFramesAfterRefill = GetSafePlayedFramesLocked();
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::Play startup refill started=true wroteFramesDelta={} "
              "playedFramesDelta={} packedQueue={} pcmQueue={}",
              writtenFramesAfterRefill >= writtenFramesBeforeRefill
                  ? (writtenFramesAfterRefill - writtenFramesBeforeRefill)
                  : 0,
              playedFramesAfterRefill >= playedFramesBeforeRefill
                  ? (playedFramesAfterRefill - playedFramesBeforeRefill)
                  : 0,
              packedQueue_.size(),
              pcmQueue_.size());
  }
  else
  {
    CLog::Log(LOGINFO,
              "KodiActiveAEEngine::Play startup refill started=false packedQueue={} pcmQueue={}",
              packedQueue_.size(),
              pcmQueue_.size());
  }
}

void KodiActiveAEEngine::Pause()
{
  std::unique_lock lock(lock_);
  playRequested_ = false;
  outputStarted_ = false;
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
  hasPendingData_ = false;
  ended_ = false;
  iecPipeline_.Reset();
  // Align stock DefaultAudioSink compatibility behavior: release on every flush.
  output_.Release();
  ResetPositionLocked();
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

void KodiActiveAEEngine::Reset()
{
  std::unique_lock lock(lock_);
  configured_ = false;
  playRequested_ = false;
  outputStarted_ = false;
  passthrough_ = false;
  ended_ = false;
  hasPendingData_ = false;
  packedQueue_.clear();
  pcmQueue_.clear();
  queuedDurationUs_ = 0;
  firstQueuedPtsUs_ = NO_PTS;
  iecPipeline_.Reset();
  output_.Release();
  ResetPositionLocked();
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
    // Backpressure is strictly output-progress driven: while previously packed bytes
    // are not fully accepted by AudioTrack, don't consume additional upstream input.
    if (!packedQueue_.empty())
    {
      if (playRequested_)
      {
        break;
      }
      // While paused, cap pre-roll to physical output capacity so Media3 receives
      // truthful backpressure and keeps waking frequently enough to feed startup.
      int64_t hardwareCapacityUs = 21000;
      if (output_.IsConfigured() && output_.SampleRate() > 0)
      {
        const int bufferFrames = std::max(0, output_.GetBufferSizeInFrames());
        if (bufferFrames > 0)
        {
          hardwareCapacityUs =
              static_cast<int64_t>((static_cast<uint64_t>(bufferFrames) * 1000000ULL) /
                                   std::max(1u, output_.SampleRate()));
        }
      }
      queuedDurationUs_ = QueueDurationUsLocked();
      if (queuedDurationUs_ >= hardwareCapacityUs)
        break;
    }

    const int chunkBytes = std::max(1, remaining / remainingAccessUnits);
    const int feedSize = std::min(remaining, chunkBytes);
    const size_t queueSizeBeforeFeed = packedQueue_.size();
    const int consumed =
        iecPipeline_.Feed(cursor, feedSize, currentPtsUs, packedQueue_, /*maxPackets=*/1);
    if (consumed <= 0)
      break;

    consumedTotal += consumed;
    cursor += consumed;
    remaining -= consumed;
    if (remainingAccessUnits > 1)
      --remainingAccessUnits;
    currentPtsUs = NO_PTS;

    if (packedQueue_.size() > queueSizeBeforeFeed)
    {
      if (!output_.IsConfigured())
        EnsurePassthroughOutputConfiguredLocked(packedQueue_.front());
      FlushPackedQueueToHardwareLocked();
      StartOutputIfPrimedLocked();
      if (!packedQueue_.empty())
        break;
    }
    else if (iecPipeline_.HasParserBacklog())
    {
      // Parser accepted bytes into internal backlog but did not emit a packet yet.
      // Stop here to force upstream re-entry rather than over-consuming while no
      // output write progress has occurred.
      break;
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

  // Headroom-bounded direct write, even while paused.
  const int frameSizeBytes = std::max(1u, output_.FrameSizeBytes());
  const int bufferFrames = std::max(0, output_.GetBufferSizeInFrames());
  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  const uint64_t pendingFrames = totalWrittenFrames_ > playedFrames ? (totalWrittenFrames_ - playedFrames) : 0;
  if (static_cast<uint64_t>(bufferFrames) <= pendingFrames)
    return 0;

  const int64_t writableBytes =
      static_cast<int64_t>((static_cast<uint64_t>(bufferFrames) - pendingFrames) * frameSizeBytes);
  const int chunkBytes = static_cast<int>(std::min<int64_t>(bytesToWrite, writableBytes));
  if (chunkBytes <= 0)
    return 0;

  int written = output_.WriteNonBlocking(data, chunkBytes);
  if (written > 0)
  {
    OnBytesWrittenLocked(ptsUs, written, output_.SampleRate(), output_.FrameSizeBytes());
    StartOutputIfPrimedLocked();
  }
  return std::max(0, written);
}

void KodiActiveAEEngine::EnsurePassthroughOutputConfiguredLocked(const KodiPackedAccessUnit& packet)
{
  if (output_.IsConfigured() && output_.SampleRate() == packet.outputRate &&
      output_.ChannelCount() == packet.outputChannels)
    return;

  output_.Release();
  output_.Configure(packet.outputRate,
                    packet.outputChannels,
                    CJNIAudioFormat::ENCODING_IEC61937,
                    true);
  if (playRequested_ && outputStarted_)
    output_.Play();
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
  return output_.Configure(requestedFormat_.m_sampleRate, channels, encoding, false);
}

int KodiActiveAEEngine::FlushPackedQueueToHardwareLocked()
{
  if (packedQueue_.empty())
    return 0;

  if (!output_.IsConfigured())
    EnsurePassthroughOutputConfiguredLocked(packedQueue_.front());
  if (!output_.IsConfigured())
    return 0;
  // Deferred priming for passthrough startup: keep queued data in C++ until play().
  if (passthrough_ && !playRequested_)
    return 0;

  const int frameSizeBytes = std::max(1u, output_.FrameSizeBytes());
  const int bufferFrames = std::max(0, output_.GetBufferSizeInFrames());
  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  const uint64_t pendingFrames = totalWrittenFrames_ > playedFrames ? (totalWrittenFrames_ - playedFrames) : 0;
  if (static_cast<uint64_t>(bufferFrames) <= pendingFrames)
    return 0;

  int64_t writableBytes =
      static_cast<int64_t>((static_cast<uint64_t>(bufferFrames) - pendingFrames) * frameSizeBytes);
  int totalConsumedPackets = 0;
  while (writableBytes > 0 && !packedQueue_.empty())
  {
    KodiPackedAccessUnit& packet = packedQueue_.front();

    const int remaining = static_cast<int>(packet.bytes.size() - packet.writeOffset);
    if (remaining <= 0)
    {
      packedQueue_.pop_front();
      ++totalConsumedPackets;
      continue;
    }

    const int chunkBytes = static_cast<int>(std::min<int64_t>(remaining, writableBytes));
    if (chunkBytes <= 0)
      break;

    const int written =
        output_.WriteNonBlocking(packet.bytes.data() + packet.writeOffset, chunkBytes);
    if (written <= 0)
      break;

    OnBytesWrittenLocked(packet.ptsUs, written, output_.SampleRate(), output_.FrameSizeBytes());
    packet.writeOffset += static_cast<size_t>(written);
    writableBytes -= written;
    if (packet.writeOffset >= packet.bytes.size())
    {
      packedQueue_.pop_front();
      ++totalConsumedPackets;
    }
  }
  queuedDurationUs_ = QueueDurationUsLocked();
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
  const int frameSizeBytes = std::max(1u, output_.FrameSizeBytes());
  const int bufferFrames = std::max(0, output_.GetBufferSizeInFrames());
  const uint64_t playedFrames = GetSafePlayedFramesLocked();
  const uint64_t pendingFrames = totalWrittenFrames_ > playedFrames ? (totalWrittenFrames_ - playedFrames) : 0;
  if (static_cast<uint64_t>(bufferFrames) <= pendingFrames)
    return 0;

  int64_t writableBytes =
      static_cast<int64_t>((static_cast<uint64_t>(bufferFrames) - pendingFrames) * frameSizeBytes);
  int totalConsumedBytes = 0;
  while (writableBytes > 0 && !pcmQueue_.empty())
  {
    PendingPcmChunk& chunk = pcmQueue_.front();
    const int remaining = static_cast<int>(chunk.bytes.size());
    if (remaining <= 0)
    {
      pcmQueue_.pop_front();
      continue;
    }

    const int chunkBytes = static_cast<int>(std::min<int64_t>(remaining, writableBytes));
    if (chunkBytes <= 0)
      break;

    const int written = output_.WriteNonBlocking(chunk.bytes.data(), chunkBytes);
    if (written <= 0)
      break;

    OnBytesWrittenLocked(chunk.ptsUs, written, output_.SampleRate(), output_.FrameSizeBytes());
    totalConsumedBytes += written;
    writableBytes -= written;
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
  systemTimeAtPlayUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  framesAtPlay_ = std::min(output_.GetPlaybackFrames64(), totalWrittenFrames_);
  output_.Play();
  outputStarted_ = true;
  return true;
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
  mediaPositionParameters_ = {hostClockSpeed_, 0, 0, 0};
  mediaPositionParametersCheckpoints_.clear();
  lastPrePlayAcceptSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastPrePlayWriteSystemTimeUs_ = CURRENT_POSITION_NOT_SET;
  lastStablePlayedFrames_ = 0;
  ResetOutputPositionEstimatorLocked();
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
