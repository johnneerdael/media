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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_END_TIME_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_END_TIME_H_

#include <chrono>

namespace androidx_media3 {

template <typename T = std::chrono::milliseconds>
class EndTime {
 public:
  EndTime() = default;

  static constexpr T Max() {
    return std::chrono::duration_cast<T>(std::chrono::steady_clock::duration::max());
  }

  explicit EndTime(T duration) { Set(duration); }

  void Set(T duration) {
    start_time_ = std::chrono::steady_clock::now();
    total_wait_time_ = duration > Max() ? Max() : duration;
  }

  bool IsTimePast() const {
    return (std::chrono::steady_clock::now() - start_time_) >= total_wait_time_;
  }

  T GetTimeLeft() const {
    const auto now = std::chrono::steady_clock::now();
    const auto left = (start_time_ + total_wait_time_) - now;
    if (left < T::zero()) {
      return T::zero();
    }
    return std::chrono::duration_cast<T>(left);
  }

  void SetExpired() { total_wait_time_ = T::zero(); }
  void SetInfinite() { total_wait_time_ = Max(); }

 private:
  std::chrono::steady_clock::time_point start_time_{};
  T total_wait_time_ = T::zero();
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_END_TIME_H_
