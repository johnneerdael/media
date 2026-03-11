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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ACTIVE_AE_BUFFER_COMPAT_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ACTIVE_AE_BUFFER_COMPAT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "AEStreamInfo.h"

namespace androidx_media3 {

class CActiveAEBufferPoolCompat;

class CSoundPacketCompat {
 public:
  CSoundPacketCompat();

  void SetData(const uint8_t* input, int size_bytes, int max_samples, int used_samples);
  void SetPauseBurst(int pause_millis, int max_samples);

  uint8_t** data;
  int bytes_per_sample = 1;
  int linesize = 0;
  int planes = 1;
  int nb_samples = 0;
  int max_nb_samples = 0;
  int pause_burst_ms = 0;

 private:
  uint8_t* plane_data_[1];
  std::vector<uint8_t> storage_;
};

class CSampleBufferCompat {
 public:
  CSampleBufferCompat();
  CSampleBufferCompat* Acquire();
  void Return();

  std::unique_ptr<CSoundPacketCompat> pkt;
  CActiveAEBufferPoolCompat* pool = nullptr;
  int64_t timestamp = 0;
  int pkt_start_offset = 0;
  int refCount = 0;
  double centerMixLevel = 0.7071067811865476;
  CAEStreamInfo stream_info = {};
};

class CActiveAEBufferPoolCompat {
 public:
  void ReturnBuffer(CSampleBufferCompat* buffer);
};

using CSoundPacket = CSoundPacketCompat;
using CSampleBuffer = CSampleBufferCompat;
using CActiveAEBufferPool = CActiveAEBufferPoolCompat;

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_ACTIVE_AE_BUFFER_COMPAT_H_
