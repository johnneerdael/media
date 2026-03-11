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

#include "KodiActiveAEBufferCompat.h"

#include <cstring>

namespace androidx_media3 {

CSoundPacketCompat::CSoundPacketCompat() {
  plane_data_[0] = nullptr;
  data = plane_data_;
}

void CSoundPacketCompat::SetData(const uint8_t* input,
                                 int size_bytes,
                                 int max_samples,
                                 int used_samples) {
  storage_.assign(input, input + size_bytes);
  plane_data_[0] = storage_.empty() ? nullptr : storage_.data();
  linesize = size_bytes;
  max_nb_samples = max_samples;
  nb_samples = used_samples;
  pause_burst_ms = 0;
}

void CSoundPacketCompat::SetPauseBurst(int pause_millis, int max_samples) {
  storage_.clear();
  plane_data_[0] = nullptr;
  linesize = 0;
  max_nb_samples = max_samples;
  nb_samples = 0;
  pause_burst_ms = pause_millis;
}

CSampleBufferCompat::CSampleBufferCompat() : pkt(std::make_unique<CSoundPacketCompat>()) {}

CSampleBufferCompat* CSampleBufferCompat::Acquire() {
  refCount++;
  return this;
}

void CSampleBufferCompat::Return() {
  refCount--;
  if (pool != nullptr && refCount <= 0) {
    pool->ReturnBuffer(this);
  }
}

void CActiveAEBufferPoolCompat::ReturnBuffer(CSampleBufferCompat* buffer) {
  if (buffer == nullptr || buffer->pkt == nullptr) {
    return;
  }
  buffer->pkt->nb_samples = 0;
  buffer->pkt->pause_burst_ms = 0;
}

}  // namespace androidx_media3
