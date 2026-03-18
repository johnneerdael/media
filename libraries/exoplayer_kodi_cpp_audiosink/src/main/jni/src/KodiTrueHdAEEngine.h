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

#include "KodiTrueHdAudioTrackOutput.h"
#include "KodiTrueHdIecPipeline.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAESettings.h"
#include "threads/CriticalSection.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>

namespace androidx_media3
{

class KodiTrueHdAEEngine
{
public:
  KodiTrueHdAEEngine() = default;

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
  int ConsumeLastWriteOutputBytes();
  int ConsumeLastWriteErrorCode();
  bool IsReleasePending();
  void Reset();

private:
  static constexpr int64_t NO_PTS = std::numeric_limits<int64_t>::min();
  static constexpr int64_t CURRENT_POSITION_NOT_SET = std::numeric_limits<int64_t>::min();
  static constexpr int64_t RELEASE_PENDING_HOLD_US = 200000;

  bool EnsureOutputConfiguredLocked();
  int FlushPackedQueueToHardwareLocked();
  void MarkReleasePendingLocked();
  bool IsReleasePendingLocked(int64_t nowUs) const;

  mutable CCriticalSection lock_;
  ActiveAE::CActiveAEMediaSettings config_{};
  AEAudioFormat requestedFormat_{};
  bool configured_{false};
  bool playRequested_{false};
  bool outputStarted_{false};
  bool ended_{false};
  bool passthrough_{false};
  float volume_{1.0f};
  int64_t hostClockUs_{CURRENT_POSITION_NOT_SET};
  double hostClockSpeed_{1.0};
  int64_t releasePendingUntilUs_{CURRENT_POSITION_NOT_SET};

  KodiTrueHdAudioTrackOutput output_;
  KodiTrueHdIecPipeline iecPipeline_;
  std::deque<KodiTrueHdPackedUnit> packedQueue_;
  uint64_t totalWrittenFrames_{0};
  int lastWriteOutputBytes_{0};
  int lastWriteErrorCode_{0};
};

}  // namespace androidx_media3
