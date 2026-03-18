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

#include "utils/log.h"

namespace androidx_media3
{

bool KodiTrueHdAEEngine::Configure(const ActiveAE::CActiveAEMediaSettings& config)
{
  CLog::Log(LOGINFO,
            "KodiTrueHdAEEngine::Configure mimeKind={} sampleRate={} channelCount={} volume={}",
            static_cast<int>(config.mimeKind),
            config.sampleRate,
            config.channelCount,
            config.volume);
  output_.Release();
  return engine_.Configure(config);
}

int KodiTrueHdAEEngine::Write(const uint8_t* data,
                              int size,
                              int64_t presentation_time_us,
                              int encoded_access_unit_count)
{
  return engine_.Write(data, size, presentation_time_us, encoded_access_unit_count);
}

void KodiTrueHdAEEngine::Play()
{
  engine_.Play();
}

void KodiTrueHdAEEngine::Pause()
{
  engine_.Pause();
}

void KodiTrueHdAEEngine::Flush()
{
  engine_.Flush();
}

void KodiTrueHdAEEngine::Drain()
{
  engine_.Drain();
}

void KodiTrueHdAEEngine::HandleDiscontinuity()
{
  engine_.HandleDiscontinuity();
}

void KodiTrueHdAEEngine::SetVolume(float volume)
{
  engine_.SetVolume(volume);
}

void KodiTrueHdAEEngine::SetHostClockUs(int64_t host_clock_us)
{
  engine_.SetHostClockUs(host_clock_us);
}

void KodiTrueHdAEEngine::SetHostClockSpeed(double speed)
{
  engine_.SetHostClockSpeed(speed);
}

int64_t KodiTrueHdAEEngine::GetCurrentPositionUs()
{
  return engine_.GetCurrentPositionUs();
}

bool KodiTrueHdAEEngine::HasPendingData()
{
  return engine_.HasPendingData();
}

bool KodiTrueHdAEEngine::IsEnded()
{
  return engine_.IsEnded();
}

int64_t KodiTrueHdAEEngine::GetBufferSizeUs() const
{
  return engine_.GetBufferSizeUs();
}

int KodiTrueHdAEEngine::ConsumeLastWriteOutputBytes()
{
  return engine_.ConsumeLastWriteOutputBytes();
}

int KodiTrueHdAEEngine::ConsumeLastWriteErrorCode()
{
  return engine_.ConsumeLastWriteErrorCode();
}

bool KodiTrueHdAEEngine::IsReleasePending()
{
  return engine_.IsReleasePending();
}

void KodiTrueHdAEEngine::Reset()
{
  engine_.Reset();
  output_.Release();
}

}  // namespace androidx_media3
