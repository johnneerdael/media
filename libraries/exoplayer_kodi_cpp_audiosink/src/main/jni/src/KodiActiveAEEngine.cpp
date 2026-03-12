#include "KodiActiveAEEngine.h"

#include "ServiceBroker.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAESettings.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace androidx_media3
{

KodiActiveAEEngine::~KodiActiveAEEngine()
{
  Reset();
}

bool KodiActiveAEEngine::Configure(const ActiveAE::CActiveAEMediaSettings& config)
{
  std::unique_lock lock(lock_);

  stream_adapter_.Reset();
  abort_add_packets_.store(false);
  play_requested_ = false;
  has_pending_data_ = false;
  playing_pts_us_ = POSITION_NOT_SET;
  clock_pts_us_ = POSITION_NOT_SET;
  host_clock_us_ = POSITION_NOT_SET;
  host_clock_speed_ = 1.0;

  ActiveAE::CActiveAESettings::EnsureMediaSinkRegistered();
  AEAudioFormat requested_format = ActiveAE::CActiveAESettings::BuildFormatForMediaSource(config);
  ActiveAE::CActiveAESettings::ApplyForMediaSource(config);
  stream_adapter_.Configure(requested_format);

  CLog::Log(LOGINFO,
            ActiveAE::CActiveAESettings::DescribeMediaSourceConfiguration(config, requested_format));

  if (!RecreateEngineLocked())
  {
    CLog::Log(LOGERROR, "KodiActiveAEEngine::Configure failed to create ActiveAE");
    return false;
  }

  // For PCM, create stream immediately. For passthrough, defer until parser resolves.
  if (!stream_adapter_.IsPassthrough() && !CreateStreamLocked(requested_format))
  {
    DestroyEngineLocked();
    return false;
  }

  engine_->SetVolume(config.volume);

  const bool passthrough = stream_adapter_.IsPassthrough();
  CLog::Log(LOGDEBUG,
            LOGAUDIO,
            "KodiActiveAEEngine::Configure passthrough={} mimeKind={} sampleRate={} channels={}",
            passthrough,
            config.mimeKind,
            requested_format.m_sampleRate,
            passthrough ? requested_format.m_streamInfo.m_channels
                        : requested_format.m_channelLayout.Count());
  return true;
}

int KodiActiveAEEngine::Write(
    const uint8_t* data, int size, int64_t presentation_time_us, int /* encoded_access_unit_count */)
{
  std::unique_lock lock(lock_);
  if (data == nullptr || size <= 0)
    return 0;

  abort_add_packets_.store(false);
  int written;
  if (stream_adapter_.IsPassthrough())
    written = WritePassthroughLocked(data, size, presentation_time_us);
  else
    written = WritePcmLocked(data, size, presentation_time_us);

  if (written > 0)
    has_pending_data_ = true;

  return written;
}

void KodiActiveAEEngine::Play()
{
  std::unique_lock lock(lock_);
  play_requested_ = true;
  CLog::Log(LOGINFO, "KodiActiveAEEngine::Play stream={}", stream_ != nullptr);
  if (stream_)
    stream_->Resume();
}

void KodiActiveAEEngine::Pause()
{
  std::unique_lock lock(lock_);
  abort_add_packets_.store(true);
  play_requested_ = false;
  if (stream_)
    stream_->Pause();
  playing_pts_us_ = POSITION_NOT_SET;
  clock_pts_us_ = POSITION_NOT_SET;
  host_clock_us_ = POSITION_NOT_SET;
}

void KodiActiveAEEngine::Flush()
{
  std::unique_lock lock(lock_);
  abort_add_packets_.store(true);
  stream_adapter_.Reset();
  playing_pts_us_ = POSITION_NOT_SET;
  clock_pts_us_ = POSITION_NOT_SET;
  host_clock_us_ = POSITION_NOT_SET;
  has_pending_data_ = false;
  if (stream_)
    stream_->Flush();
}

void KodiActiveAEEngine::Drain()
{
  std::unique_lock lock(lock_);
  abort_add_packets_.store(true);
  if (!stream_)
    return;

  stream_adapter_.FinalizeParser();
  stream_->Drain(true);
}

void KodiActiveAEEngine::SetVolume(float volume)
{
  std::unique_lock lock(lock_);
  if (engine_)
    engine_->SetVolume(volume);
}

void KodiActiveAEEngine::SetHostClockUs(int64_t host_clock_us)
{
  std::unique_lock lock(lock_);
  if (host_clock_us == NO_PTS || host_clock_us == POSITION_NOT_SET)
    return;
  host_clock_us_ = host_clock_us;
  time_of_host_clock_ = std::chrono::steady_clock::now();
}

void KodiActiveAEEngine::SetHostClockSpeed(double speed)
{
  std::unique_lock lock(lock_);
  host_clock_speed_ = speed > 0.0 ? speed : 1.0;
}

// Based on CAudioSinkAE::GetPlayingPts() — wall-clock interpolation clamped by cache time.
// Note: upstream Kodi has a unit mismatch (microseconds vs seconds) that makes its
// interpolation a near no-op. We fix the units here because Media3 uses this as its
// primary position signal and needs it to actually advance.
int64_t KodiActiveAEEngine::GetCurrentPositionUs()
{
  std::unique_lock lock(lock_);
  return ComputeCurrentPositionUsLocked();
}

bool KodiActiveAEEngine::HasPendingData()
{
  std::unique_lock lock(lock_);
  // Media3 uses hasPendingData to decide renderer readiness (isReady).
  // If this returns false, play() is never called and the paused stream
  // never resumes — deadlock. Return true once Write has accepted data.
  if (has_pending_data_)
    return true;
  if (!stream_)
    return stream_adapter_.HasBacklog();
  return stream_->GetDelay() > 0.0 || stream_->GetCacheTime() > 0.0 || stream_->IsDraining();
}

bool KodiActiveAEEngine::IsEnded()
{
  std::unique_lock lock(lock_);
  if (!stream_)
    return !has_pending_data_ && !stream_adapter_.HasBacklog();
  if (!stream_->IsDraining())
    return false;
  if (stream_->IsDrained())
  {
    has_pending_data_ = false;
    return true;
  }
  return false;
}

int64_t KodiActiveAEEngine::GetBufferSizeUs() const
{
  std::unique_lock lock(lock_);
  if (!stream_)
    return 0;
  return static_cast<int64_t>(stream_->GetCacheTotal() * 1000000.0);
}

void KodiActiveAEEngine::Reset()
{
  std::unique_lock lock(lock_);
  abort_add_packets_.store(true);
  stream_adapter_.Reset();
  play_requested_ = false;
  has_pending_data_ = false;
  playing_pts_us_ = POSITION_NOT_SET;
  clock_pts_us_ = POSITION_NOT_SET;
  host_clock_us_ = POSITION_NOT_SET;
  host_clock_speed_ = 1.0;
  DestroyEngineLocked();
}

// ---- Write paths ----

int KodiActiveAEEngine::WritePassthroughLocked(
    const uint8_t* data, int size, int64_t presentation_time_us)
{
  int totalConsumed = 0;

  // Drain any backlog access units first.
  while (stream_adapter_.HasBacklog() && !abort_add_packets_.load())
  {
    // Don't drain if stream is full and not playing.
    if (stream_ && !play_requested_ && stream_->GetSpace() == 0)
      break;

    const uint8_t* auData = nullptr;
    unsigned int auSize = 0;
    int64_t auPtsUs = 0, auDurationUs = 0;
    if (!stream_adapter_.DrainBacklog(&auData, &auSize, &auPtsUs, &auDurationUs))
      break;
    if (auSize == 0)
      continue;

    // Ensure stream exists after parser resolves.
    if (!stream_ && stream_adapter_.HasResolvedPassthroughFormat())
    {
      const AEAudioFormat fmt = stream_adapter_.GetResolvedFormat();
      CLog::Log(LOGINFO,
                "KodiActiveAEEngine::Write creating passthrough stream sampleRate={} streamType={}",
                fmt.m_sampleRate, fmt.m_streamInfo.m_type);
      if (!CreateStreamLocked(fmt))
      {
        CLog::Log(LOGERROR, "KodiActiveAEEngine::Write failed to create passthrough stream");
        return 0;
      }
    }
    if (!stream_)
      break;

    if (!AddDataToStreamLocked(auData, auSize, auPtsUs, auDurationUs))
      break;
  }

  // Parse and write new input data.
  const uint8_t* current = data;
  int remaining = size;

  while (remaining > 0 && !abort_add_packets_.load())
  {
    // Don't parse more data if the stream is full and not playing — parsing is
    // destructive (bytes can't be re-consumed) and the resulting AU would be lost
    // if AddData can't accept it.
    if (stream_ && !play_requested_ && stream_->GetSpace() == 0)
      break;

    const uint8_t* auData = nullptr;
    unsigned int auSize = 0;
    int64_t auPtsUs = 0, auDurationUs = 0;

    const int consumed = stream_adapter_.FeedPassthrough(
        current, remaining, presentation_time_us,
        &auData, &auSize, &auPtsUs, &auDurationUs);

    if (consumed > 0)
    {
      current += consumed;
      remaining -= consumed;
      totalConsumed += consumed;
      // Only use the original PTS for the first chunk.
      presentation_time_us = NO_PTS;
    }

    if (auSize > 0)
    {
      // Ensure stream exists after parser resolves.
      if (!stream_ && stream_adapter_.HasResolvedPassthroughFormat())
      {
        const AEAudioFormat fmt = stream_adapter_.GetResolvedFormat();
        CLog::Log(LOGINFO,
                  "KodiActiveAEEngine::Write creating passthrough stream sampleRate={} streamType={}",
                  fmt.m_sampleRate, fmt.m_streamInfo.m_type);
        if (!CreateStreamLocked(fmt))
        {
          CLog::Log(LOGERROR, "KodiActiveAEEngine::Write failed to create passthrough stream");
          return totalConsumed > 0 ? totalConsumed : 0;
        }
      }

      if (stream_)
      {
        if (!AddDataToStreamLocked(auData, auSize, auPtsUs, auDurationUs))
          break; // backpressure — return what we consumed, Media3 retries
      }
    }

    if (consumed <= 0)
      break;
  }

  return totalConsumed;
}

int KodiActiveAEEngine::WritePcmLocked(
    const uint8_t* data, int size, int64_t presentation_time_us)
{
  if (!stream_)
    return 0;

  const AEAudioFormat& fmt = stream_adapter_.GetRequestedFormat();
  const int frameSize = static_cast<int>(fmt.m_frameSize);
  if (frameSize <= 0)
    return 0;

  // Align to frame boundary.
  int bytesToWrite = size - (size % frameSize);
  if (bytesToWrite <= 0)
    return 0;

  const unsigned int frames = static_cast<unsigned int>(bytesToWrite / frameSize);
  const int64_t durationUs = stream_adapter_.DurationUsForWrite(bytesToWrite);

  // Avoid AddData's 200ms internal wait when not playing and buffers are full.
  if (!play_requested_ && stream_->GetSpace() == 0)
    return 0;

  const uint8_t* planes[] = {data};
  IAEStream::ExtData extData;
  extData.pts = presentation_time_us != NO_PTS ? presentation_time_us / 1000.0 : 0.0;

  // Dynamic deadline matching AudioSinkAE::AddPackets():124-128.
  constexpr double SAFETY_MARGIN_SECS = 2.0;
  const double deadlineSecs = play_requested_
      ? GetDelaySecsLocked() + durationUs / 1000000.0 + SAFETY_MARGIN_SECS
      : 0.0;
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::duration<double>(deadlineSecs));
  unsigned int offset = 0;
  unsigned int remaining = frames;

  while (remaining > 0 && !abort_add_packets_.load())
  {
    // Avoid entering AddData's 200ms internal wait when not playing.
    if (!play_requested_ && offset > 0 && stream_->GetSpace() == 0)
      break;

    const unsigned int copied = stream_->AddData(planes, offset, remaining, &extData);
    offset += copied;
    remaining -= copied;

    if (remaining <= 0)
      break;
    if (copied == 0 && std::chrono::steady_clock::now() >= deadline)
    {
      CLog::Log(LOGERROR, "KodiActiveAEEngine::WritePcm - timeout adding data to stream");
      break;
    }
    if (copied == 0)
    {
      lock_.unlock();
      std::this_thread::sleep_for(1ms);
      lock_.lock();
      if (abort_add_packets_.load())
        break;
    }
  }

  const int bytesWritten = static_cast<int>(offset) * frameSize;
  if (bytesWritten > 0)
  {
    const int64_t writtenDurationUs =
        stream_adapter_.DurationUsForWrite(bytesWritten);
    UpdatePlayingPtsLocked(presentation_time_us, writtenDurationUs);
  }

  return bytesWritten;
}

// Direct write of one passthrough access unit to the stream.
bool KodiActiveAEEngine::AddDataToStreamLocked(
    const uint8_t* data, unsigned int frames,
    int64_t ptsUs, int64_t durationUs)
{
  // CActiveAEStream::AddData has an internal 200ms wait for STREAMBUFFER.
  // When the stream is paused, buffers fill up and never drain (ProcessBuffers
  // is skipped for paused streams). If we enter AddData with no space, it blocks
  // for 200ms and returns 0 — blocking the Media3 thread so play() can never
  // be called. Check GetSpace() first to avoid this deadlock.
  if (!play_requested_ && stream_->GetSpace() == 0)
    return false;

  const uint8_t* planes[] = {data};
  IAEStream::ExtData extData;
  extData.pts = ptsUs != NO_PTS ? ptsUs / 1000.0 : 0.0;

  // Dynamic deadline matching AudioSinkAE::AddPackets():124-128.
  constexpr double SAFETY_MARGIN_SECS = 2.0;
  const double deadlineSecs = play_requested_
      ? GetDelaySecsLocked() + durationUs / 1000000.0 + SAFETY_MARGIN_SECS
      : 0.0;
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::duration<double>(deadlineSecs));
  unsigned int offset = 0;
  unsigned int remaining = frames;

  while (remaining > 0 && !abort_add_packets_.load())
  {
    // Avoid entering AddData's 200ms internal wait when not playing.
    if (!play_requested_ && offset > 0 && stream_->GetSpace() == 0)
      break;

    const unsigned int copied = stream_->AddData(planes, offset, remaining, &extData);
    offset += copied;
    remaining -= copied;

    if (remaining <= 0)
      break;
    if (copied == 0 && std::chrono::steady_clock::now() >= deadline)
      break;
    if (copied == 0)
    {
      lock_.unlock();
      std::this_thread::sleep_for(1ms);
      lock_.lock();
      if (abort_add_packets_.load())
        break;
    }
  }

  if (offset > 0)
    UpdatePlayingPtsLocked(ptsUs, durationUs);

  return offset > 0;
}

// ---- Position tracking (mirrors AudioSinkAE::AddPackets line 162) ----

void KodiActiveAEEngine::UpdatePlayingPtsLocked(int64_t packetPtsUs, int64_t packetDurationUs)
{
  if (packetPtsUs == NO_PTS)
    return;

  // Keep explicit host timeline aligned with packet timeline as a fallback.
  host_clock_us_ = packetPtsUs + packetDurationUs;
  time_of_host_clock_ = std::chrono::steady_clock::now();

  // Feed ActiveAE's clock callback with a player-style timeline anchor sourced directly
  // from incoming packet PTS (not from sink delay/position).
  clock_pts_us_ = packetPtsUs + packetDurationUs;
  time_of_clock_pts_ = std::chrono::steady_clock::now();

  const double delaySecs = GetDelaySecsLocked();
  const int64_t delayUs = static_cast<int64_t>(delaySecs * 1000000.0);

  playing_pts_us_ = packetPtsUs + packetDurationUs - delayUs;
  time_of_pts_ = std::chrono::steady_clock::now();
}

int64_t KodiActiveAEEngine::ComputeCurrentPositionUsLocked() const
{
  if (playing_pts_us_ == POSITION_NOT_SET)
    return POSITION_NOT_SET;

  const auto now = std::chrono::steady_clock::now();
  const double elapsedSecs = std::chrono::duration<double>(now - time_of_pts_).count();
  const double cacheSecs = GetCacheTimeSecsLocked();

  const double playedSecs = std::min(elapsedSecs, std::max(cacheSecs, 0.0));
  const int64_t playedUs = static_cast<int64_t>(playedSecs * 1000000.0);
  return playing_pts_us_ + playedUs;
}

double KodiActiveAEEngine::GetDelaySecsLocked() const
{
  if (!stream_)
    return 0.3; // default like AudioSinkAE::GetDelay
  return stream_->GetDelay();
}

double KodiActiveAEEngine::GetCacheTimeSecsLocked() const
{
  if (!stream_)
    return 0.0;
  return stream_->GetCacheTime();
}

double KodiActiveAEEngine::GetClock()
{
  std::unique_lock lock(lock_);
  const auto now = std::chrono::steady_clock::now();
  if (host_clock_us_ != POSITION_NOT_SET)
  {
    const double elapsedSecs = std::chrono::duration<double>(now - time_of_host_clock_).count();
    const int64_t elapsedUs = static_cast<int64_t>(elapsedSecs * 1000000.0 * host_clock_speed_);
    return static_cast<double>(host_clock_us_ + elapsedUs) / 1000.0;
  }
  if (clock_pts_us_ != POSITION_NOT_SET)
  {
    const double elapsedSecs = std::chrono::duration<double>(now - time_of_clock_pts_).count();
    const int64_t elapsedUs = static_cast<int64_t>(elapsedSecs * 1000000.0);
    return static_cast<double>(clock_pts_us_ + elapsedUs) / 1000.0;
  }
  return 0.0;
}

double KodiActiveAEEngine::GetClockSpeed()
{
  std::unique_lock lock(lock_);
  return host_clock_speed_;
}

// ---- Engine lifecycle ----

bool KodiActiveAEEngine::RecreateEngineLocked()
{
  DestroyEngineLocked();

  engine_ = std::make_unique<ActiveAE::CActiveAE>();
  CServiceBroker::RegisterAE(engine_.get());
  engine_->Start();
  return engine_ != nullptr;
}

bool KodiActiveAEEngine::CreateStreamLocked(const AEAudioFormat& stream_format)
{
  if (!engine_)
    return false;

  if (stream_)
    return true;

  AEAudioFormat format = stream_format;
  stream_ = engine_->MakeStream(format, AESTREAM_PAUSED, this);
  if (!stream_)
  {
    CLog::Log(LOGERROR, "KodiActiveAEEngine::CreateStreamLocked failed to create ActiveAE stream");
    return false;
  }

  // If play was already requested before stream existed, resume immediately.
  if (play_requested_)
    stream_->Resume();

  return true;
}

void KodiActiveAEEngine::DestroyEngineLocked()
{
  stream_.reset();
  if (engine_)
  {
    engine_->Shutdown();
    engine_.reset();
  }
  CServiceBroker::UnregisterAE();
}

}  // namespace androidx_media3
