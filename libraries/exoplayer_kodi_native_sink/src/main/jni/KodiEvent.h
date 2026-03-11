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

#ifndef ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_EVENT_H_
#define ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_EVENT_H_

#include <condition_variable>
#include <mutex>

namespace androidx_media3 {

class CEvent {
 public:
  explicit CEvent(bool manual_reset = false, bool signaled = false)
      : manual_reset_(manual_reset), signaled_(signaled) {}

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = false;
  }

  void Set() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
    if (manual_reset_) {
      cv_.notify_all();
    } else {
      cv_.notify_one();
    }
  }

  bool Signaled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return signaled_;
  }

  template <typename Rep, typename Period>
  bool Wait(std::chrono::duration<Rep, Period> duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool notified = cv_.wait_for(lock, duration, [this] { return signaled_; });
    if (!notified) {
      return false;
    }
    if (!manual_reset_) {
      signaled_ = false;
    }
    return true;
  }

  bool Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return signaled_; });
    if (!manual_reset_) {
      signaled_ = false;
    }
    return true;
  }

 private:
  bool manual_reset_;
  bool signaled_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace androidx_media3

#endif  // ANDROIDX_MEDIA3_EXOPLAYER_AUDIO_KODI_EVENT_H_
