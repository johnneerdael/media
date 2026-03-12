#pragma once

#include "cores/AudioEngine/Engines/ActiveAE/ActiveAE.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAESettings.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAEStream.h"
#include "cores/AudioEngine/Interfaces/AEStream.h"
#include "cores/AudioEngine/Utils/AEStreamData.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>

namespace androidx_media3
{

class KodiActiveAEEngine : public IAEClockCallback
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
  void SetVolume(float volume);
  void SetHostClockUs(int64_t host_clock_us);
  void SetHostClockSpeed(double speed);
  int64_t GetCurrentPositionUs();
  bool HasPendingData();
  bool IsEnded();
  int64_t GetBufferSizeUs() const;
  void Reset();

  double GetClock() override;
  double GetClockSpeed() override;

private:
  static constexpr int64_t NO_PTS = std::numeric_limits<int64_t>::min();
  static constexpr int64_t POSITION_NOT_SET = std::numeric_limits<int64_t>::min();

  bool RecreateEngineLocked();
  bool CreateStreamLocked(const AEAudioFormat& stream_format);
  void DestroyEngineLocked();

  int WritePassthroughLocked(const uint8_t* data, int size, int64_t presentation_time_us);
  int WritePcmLocked(const uint8_t* data, int size, int64_t presentation_time_us);

  // Direct write to stream with short deadline retry (like AudioSinkAE::AddPackets).
  // Returns true if at least some data was accepted.
  bool AddDataToStreamLocked(const uint8_t* data, unsigned int frames,
                             int64_t ptsUs, int64_t durationUs);

  // Kodi-style position helpers.
  void UpdatePlayingPtsLocked(int64_t packetPtsUs, int64_t packetDurationUs);
  int64_t ComputeCurrentPositionUsLocked() const;
  double GetDelaySecsLocked() const;
  double GetCacheTimeSecsLocked() const;

  mutable CCriticalSection lock_;
  std::unique_ptr<ActiveAE::CActiveAE> engine_;
  IAE::StreamPtr stream_;
  ActiveAE::CActiveAEMediaStreamAdapter stream_adapter_;

  bool play_requested_ = false;
  bool has_pending_data_ = false;  // set when Write accepts data; cleared on flush/reset/drain-complete
  std::atomic_bool abort_add_packets_{false};

  // Kodi-style played position tracking (mirrors AudioSinkAE).
  int64_t playing_pts_us_ = POSITION_NOT_SET;
  std::chrono::steady_clock::time_point time_of_pts_;
  // IAEClockCallback timeline anchor (player-style clock domain from incoming packet PTS).
  int64_t clock_pts_us_ = POSITION_NOT_SET;
  std::chrono::steady_clock::time_point time_of_clock_pts_;
  // Explicit host media clock feed (closest equivalent to AudioSinkAE's CDVDClock contract).
  int64_t host_clock_us_ = POSITION_NOT_SET;
  std::chrono::steady_clock::time_point time_of_host_clock_;
  double host_clock_speed_ = 1.0;
};

}  // namespace androidx_media3
