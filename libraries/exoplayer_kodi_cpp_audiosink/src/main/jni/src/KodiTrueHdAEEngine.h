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

#include "KodiActiveAEEngine.h"
#include "KodiTrueHdAudioTrackOutput.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAESettings.h"

#include <cstdint>

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
  KodiTrueHdAudioTrackOutput output_;
  KodiActiveAEEngine engine_;
};

}  // namespace androidx_media3
