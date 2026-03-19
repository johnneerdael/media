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
#include "KodiIecPipeline.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAESettings.h"
#include "threads/CriticalSection.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace androidx_media3
{

class KodiActiveAEEngine
{
public:
  KodiActiveAEEngine() = default;
  ~KodiActiveAEEngine();

  bool Configure(const ActiveAE::CActiveAEMediaSettings& config);
  int Write(const uint8_t* data, int size, int64_t presentation_time_us, int encoded_access_unit_count);
  void Play();
  void Pause();
  void Flush();
  void Drain();
  void HandleDiscontinuity();
  void SetVolume(float volume);
  void SetHostClockUs(int64_t host_clock_us);
  void SetHostClockSpeed(double speed);
  int64_t GetCurrentPositionUs();
  bool HasPendingData();
  bool IsEnded();
  int64_t GetBufferSizeUs() const;
  int GetOutputSampleRate() const;
  int GetOutputChannelCount() const;
  int GetOutputEncoding() const;
  int GetOutputAudioTrackState() const;
  int GetDirectPlaybackSupportState() const;
  int ConsumeLastWriteOutputBytes();
  int ConsumeLastWriteErrorCode();
  bool ConsumeNextCapturedPackedBurst(std::vector<uint8_t>& bytes, int64_t& ptsUs);
  bool ConsumeNextCapturedAudioTrackWriteBurst(std::vector<uint8_t>& bytes, int64_t& ptsUs);
  bool IsReleasePending();
  void Reset();

private:
  static constexpr int64_t NO_PTS = std::numeric_limits<int64_t>::min();
  static constexpr int64_t CURRENT_POSITION_NOT_SET = std::numeric_limits<int64_t>::min();
  static constexpr int64_t MAX_POSITION_DRIFT_FOR_SMOOTHING_US = 1000000;
  static constexpr int64_t MIN_PLAYHEAD_OFFSET_SAMPLE_INTERVAL_US = 30000;
  static constexpr int64_t FAST_TIMESTAMP_POLL_INTERVAL_US = 10000;
  static constexpr int64_t SLOW_TIMESTAMP_POLL_INTERVAL_US = 10000000;
  static constexpr int64_t ERROR_TIMESTAMP_POLL_INTERVAL_US = 500000;
  static constexpr int64_t INITIALIZING_DURATION_US = 500000;
  static constexpr int64_t WAIT_FOR_TIMESTAMP_ADVANCE_US = 2000000;
  static constexpr int64_t MAX_ADVANCING_TIMESTAMP_DRIFT_US = 1000;
  static constexpr int64_t MAX_AUDIO_TIMESTAMP_OFFSET_US = 5000000;
  static constexpr int64_t MAX_RESUME_TIMESTAMP_DRIFT_US = 200000;
  static constexpr int MAX_POSITION_SMOOTHING_SPEED_CHANGE_PERCENT = 10;
  static constexpr int64_t DISCONTINUITY_THRESHOLD_US = 200000;
  static constexpr int MAX_WRITE_CALLS_PER_FLUSH = 256;
  static constexpr int PASSTHROUGH_CONFIG_RETRY_ATTEMPTS = 6;
  static constexpr int PASSTHROUGH_CONFIG_RETRY_DELAY_MS = 20;
  static constexpr int64_t RELEASE_PENDING_HOLD_US = 200000;

  enum class StartupPhase
  {
    IDLE,
    PREPARED,
    PRIME_ATTEMPTED,
    STARTED,
    POST_START_REFILL,
    RUNNING,
    RECOVERY_RECREATE
  };
  enum class TimestampState
  {
    INITIALIZING,
    TIMESTAMP,
    TIMESTAMP_ADVANCING,
    NO_TIMESTAMP,
    ERROR
  };

  struct PendingPcmChunk
  {
    std::vector<uint8_t> bytes;
    int64_t ptsUs{NO_PTS};
  };

  struct MediaPositionParameters
  {
    double playbackSpeed{1.0};
    int64_t mediaTimeUs{0};
    int64_t audioOutputPositionUs{0};
    int64_t mediaPositionDriftUs{0};
  };

  struct CapturedValidationBurst
  {
    std::vector<uint8_t> bytes;
    int64_t ptsUs{NO_PTS};
  };

  int WritePassthroughLocked(const uint8_t* data,
                             int size,
                             int64_t ptsUs,
                             int encodedAccessUnitCount);
  int WritePcmLocked(const uint8_t* data, int size, int64_t ptsUs);
  void EnsurePassthroughOutputConfiguredLocked(const KodiPackedAccessUnit& packet);
  bool EnsurePcmOutputConfiguredLocked();
  int FlushPackedQueueToHardwareLocked();
  int FlushPcmQueueToHardwareLocked();
  bool StartOutputIfPrimedLocked();

  void OnBytesWrittenLocked(int64_t packetPtsUs, int bytesWritten, unsigned int sampleRate, unsigned int frameSizeBytes);
  int64_t ComputePositionFromHardwareLocked();
  int64_t GetAudioOutputPositionUsLocked();
  int64_t ApplyMediaPositionParametersLocked(int64_t outputPositionUs);
  int64_t ApplySkippingLocked(int64_t mediaPositionUs) const;
  int64_t GetWrittenAudioOutputPositionUsLocked() const;
  uint64_t GetSafePlayedFramesLocked();
  void UpdateTimestampStateLocked(TimestampState state, int64_t systemTimeUs);
  void SetStartupPhaseLocked(StartupPhase phase);
  const char* StartupPhaseToString(StartupPhase phase) const;
  bool IsTimestampAdvancingFromInitialLocked(uint64_t tsFrames,
                                             int64_t tsSystemTimeUs,
                                             int64_t systemTimeUs,
                                             int64_t playbackHeadEstimateUs) const;
  int64_t QueueDurationUsLocked() const;
  uint64_t QueueBytesLocked() const;
  void UpdateExpectedPtsLocked(int64_t packetPtsUs, int64_t packetDurationUs);
  bool TryResolvePendingDiscontinuityLocked();
  void ReanchorForDiscontinuityLocked(int64_t packetPtsUs);
  void ResetOutputPositionEstimatorLocked();
  void ResetPositionLocked();
  void InvalidateCurrentOutputLocked();
  void MarkReleasePendingLocked();
  bool IsReleasePendingLocked(int64_t nowUs) const;
  void RecordPackedBurstLocked(const KodiPackedAccessUnit& packet);
  void RecordAudioTrackWriteChunkLocked(const KodiPackedAccessUnit& packet, int bytesWritten);
  void FinalizeAudioTrackWriteBurstLocked(const KodiPackedAccessUnit& packet);
  void ClearCapturedValidationBurstsLocked();

  mutable CCriticalSection lock_;
  ActiveAE::CActiveAEMediaSettings config_{};
  AEAudioFormat requestedFormat_{};
  bool configured_{false};
  bool playRequested_{false};
  bool outputStarted_{false};
  bool passthrough_{false};
  bool ended_{false};

  KodiIecPipeline iecPipeline_;
  KodiAudioTrackOutput output_;
  std::deque<KodiPackedAccessUnit> packedQueue_;
  std::deque<PendingPcmChunk> pcmQueue_;
  int64_t queuedDurationUs_{0};
  int64_t firstQueuedPtsUs_{NO_PTS};
  int pendingPassthroughAckBytes_{0};

  uint64_t totalWrittenFrames_{0};
  bool anchorValid_{false};
  int64_t anchorPtsUs_{CURRENT_POSITION_NOT_SET};
  uint64_t anchorPlaybackFrames_{0};
  unsigned int anchorMediaSampleRate_{0};
  unsigned int anchorSinkSampleRate_{0};
  int64_t systemTimeAtAnchorUs_{0};
  int64_t lastPositionUs_{CURRENT_POSITION_NOT_SET};
  int64_t hostClockUs_{CURRENT_POSITION_NOT_SET};
  double hostClockSpeed_{1.0};
  float volume_{1.0f};
  bool hasPendingData_{false};
  int directPlaybackSupportState_{-1};
  int lastWriteOutputBytes_{0};
  int lastWriteErrorCode_{0};
  int64_t releasePendingUntilUs_{CURRENT_POSITION_NOT_SET};
  std::deque<CapturedValidationBurst> capturedPackedBursts_;
  std::deque<CapturedValidationBurst> capturedAudioTrackWriteBursts_;
  std::vector<uint8_t> pendingAudioTrackWriteCapture_;
  int64_t pendingAudioTrackWriteCapturePtsUs_{NO_PTS};

  std::array<int64_t, 10> playheadOffsetsUs_{};
  int playheadOffsetCount_{0};
  int nextPlayheadOffsetIndex_{0};
  int64_t smoothedPlayheadOffsetUs_{0};
  int64_t lastPlayheadSampleTimeUs_{0};
  int64_t lastOutputPositionUs_{CURRENT_POSITION_NOT_SET};
  int64_t lastOutputPositionSystemTimeUs_{CURRENT_POSITION_NOT_SET};
  int64_t systemTimeAtPlayUs_{CURRENT_POSITION_NOT_SET};
  uint64_t framesAtPlay_{0};
  int64_t lastPrePlayAcceptSystemTimeUs_{CURRENT_POSITION_NOT_SET};
  int64_t lastPrePlayWriteSystemTimeUs_{CURRENT_POSITION_NOT_SET};
  uint64_t lastStablePlayedFrames_{0};
  TimestampState timestampState_{TimestampState::INITIALIZING};
  int64_t timestampInitializeSystemTimeUs_{0};
  int64_t timestampSampleIntervalUs_{FAST_TIMESTAMP_POLL_INTERVAL_US};
  int64_t timestampLastSampleTimeUs_{0};
  uint64_t timestampInitialFrames_{0};
  int64_t timestampInitialSystemTimeUs_{CURRENT_POSITION_NOT_SET};
  uint64_t lastTimestampFrames_{0};
  int64_t lastTimestampSystemTimeUs_{CURRENT_POSITION_NOT_SET};

  MediaPositionParameters mediaPositionParameters_{};
  std::deque<MediaPositionParameters> mediaPositionParametersCheckpoints_;
  int64_t skippedOutputFrameCount_{0};
  int64_t skippedOutputFrameCountAtLastPosition_{0};
  bool nextExpectedPtsValid_{false};
  int64_t nextExpectedPtsUs_{0};
  bool startMediaTimeUsNeedsSync_{false};
  int64_t pendingSyncPtsUs_{NO_PTS};
  StartupPhase startupPhase_{StartupPhase::IDLE};
};

}  // namespace androidx_media3
