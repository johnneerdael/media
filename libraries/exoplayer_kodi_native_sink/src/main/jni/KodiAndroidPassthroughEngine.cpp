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

#include "KodiAndroidPassthroughEngine.h"

#include <android/log.h>

#include <cstdarg>
#include <cstring>
#include <cmath>

namespace androidx_media3 {

namespace {

constexpr int kPacketKindPassthroughDirect = 1;
constexpr int kPacketKindIec61937 = 2;
constexpr int kPacketKindPcm = 3;
constexpr int kModeUnsupported = 0;
constexpr int kModePcm = 1;
constexpr int kModePassthroughDirect = 2;
constexpr int kModePassthroughIecStereo = 3;
constexpr int kModePassthroughIecMultichannel = 4;
constexpr char kLogTag[] = "KodiNativeSink";

bool PlaybackDecisionUsesIecCarrier(const PlaybackDecision& playback_decision)
{
  return playback_decision.mode == kModePassthroughIecStereo ||
         playback_decision.mode == kModePassthroughIecMultichannel;
}

int GetTransportSampleRate(const PlaybackDecision& playback_decision, int sample_rate)
{
  if (!PlaybackDecisionUsesIecCarrier(playback_decision))
  {
    if (playback_decision.stream_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
      return 192000;
    return sample_rate;
  }

  switch (playback_decision.stream_type)
  {
    case CAEStreamInfo::STREAM_TYPE_EAC3:
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      return 192000;
    default:
      return sample_rate;
  }
}

int64_t DurationUsToFrames(double duration_us, int sample_rate)
{
  if (duration_us <= 0 || sample_rate <= 0)
    return 0;
  return static_cast<int64_t>(std::llround(duration_us * sample_rate / 1000000.0));
}

int BytesPerSampleForPcmEncoding(int pcm_encoding)
{
  switch (pcm_encoding)
  {
    case 2:
      return 2;
    case 4:
      return 4;
    case 536870912:
      return 3;
    case 805306368:
      return 4;
    default:
      return 2;
  }
}

}  // namespace

void KodiAndroidPassthroughEngine::Configure(int mime_kind,
                                             int sample_rate,
                                             int channel_count,
                                             int pcm_encoding,
                                             int specified_buffer_size,
                                             int output_channel_count,
                                             const CapabilitySnapshot& capability_snapshot,
                                             const PlaybackDecision& playback_decision)
{
  (void)mime_kind;
  (void)capability_snapshot;
  sample_rate_ = sample_rate;
  channel_count_ = channel_count;
  pcm_encoding_ = pcm_encoding;
  specified_buffer_size_ = specified_buffer_size;
  output_channel_count_ = output_channel_count;
  playback_decision_ = playback_decision;
  pending_packets_.clear();
  last_dequeued_packet_data_.clear();
  bitstream_packer_.Reset();
  passthrough_codec_.reset();
  audio_format_ = {};
  LogVerbose("engine configure mime=%d rate=%d channels=%d pcm=%d mode=%d stream=%d outChannels=%d buffer=%d",
             mime_kind,
             sample_rate,
             channel_count,
             pcm_encoding,
             playback_decision.mode,
             playback_decision.stream_type,
             output_channel_count,
             specified_buffer_size);
  ConfigureStreamInfo();
}

bool KodiAndroidPassthroughEngine::ConfigureStreamInfo()
{
  stream_info_ = {};
  stream_info_.m_sampleRate = static_cast<unsigned int>(sample_rate_);
  stream_info_.m_channels = static_cast<unsigned int>(channel_count_);
  stream_info_.m_dataIsLE = false;
  const auto codec_stream_type =
      static_cast<CAEStreamInfo::DataType>(playback_decision_.stream_type);
  if (codec_stream_type == CAEStreamInfo::STREAM_TYPE_NULL)
  {
    stream_info_.m_type = CAEStreamInfo::STREAM_TYPE_NULL;
    return false;
  }

  passthrough_codec_ = std::make_unique<CDVDAudioCodecPassthrough>(
      playback_decision_.mode == kModePassthroughDirect, codec_stream_type);
  if (!passthrough_codec_->Open())
  {
    passthrough_codec_.reset();
    stream_info_.m_type = CAEStreamInfo::STREAM_TYPE_NULL;
    return false;
  }
  return true;
}

bool KodiAndroidPassthroughEngine::UsesKodiPassthroughCodec() const
{
  return passthrough_codec_ != nullptr;
}

void KodiAndroidPassthroughEngine::QueueCopiedOutput(int kind,
                                                     const uint8_t* data,
                                                     int size,
                                                     int64_t presentation_time_us,
                                                     int encoded_access_unit_count,
                                                     int64_t total_frames)
{
  if (data == nullptr || size <= 0)
    return;

  PendingPacket packet;
  packet.metadata = PacketMetadata{
      /*kind=*/kind,
      /*size_bytes=*/size,
      /*total_frames=*/total_frames,
      /*normalized_access_units=*/encoded_access_unit_count,
      /*effective_presentation_time_us=*/presentation_time_us,
  };
  packet.data.resize(size);
  std::memcpy(packet.data.data(), data, size);
  pending_packets_.push_back(std::move(packet));
  LogVerbose("engine emit copied kind=%d size=%d frames=%lld accessUnits=%d ptsUs=%lld pending=%zu",
             kind,
             size,
             static_cast<long long>(total_frames),
             encoded_access_unit_count,
             static_cast<long long>(presentation_time_us),
             pending_packets_.size());
}

void KodiAndroidPassthroughEngine::QueueCodecFrames(const uint8_t* data,
                                                    int size,
                                                    int64_t presentation_time_us,
                                                    int encoded_access_unit_count)
{
  if (passthrough_codec_ == nullptr || data == nullptr || size <= 0)
    return;

  DemuxPacket packet{data, size, static_cast<double>(presentation_time_us)};
  passthrough_codec_->AddData(packet);

  while (true)
  {
    DVDAudioFrame frame;
    if (!passthrough_codec_->GetData(frame))
      break;

    audio_format_ = frame.format;
    stream_info_ = frame.format.m_streamInfo;
    const int64_t effective_pts =
        frame.hasTimestamp && frame.pts != DVD_NOPTS_VALUE
            ? static_cast<int64_t>(frame.pts)
            : presentation_time_us;

    if (PlaybackDecisionUsesIecCarrier(playback_decision_))
    {
      bitstream_packer_.Pack(stream_info_, frame.data, static_cast<int>(frame.nb_frames));
      QueuePackedOutput(effective_pts, encoded_access_unit_count);
    }
    else
    {
      const int64_t total_frames =
          DurationUsToFrames(frame.duration, GetTransportSampleRate(playback_decision_, sample_rate_));
      QueueCopiedOutput(kPacketKindPassthroughDirect,
                        frame.data,
                        static_cast<int>(frame.nb_frames),
                        effective_pts,
                        encoded_access_unit_count,
                        total_frames);
    }
  }
}

void KodiAndroidPassthroughEngine::QueuePackedOutput(int64_t presentation_time_us,
                                                     int encoded_access_unit_count)
{
  const unsigned int packed_size = bitstream_packer_.GetSize();
  if (packed_size == 0)
    return;

  PendingPacket packet;
  packet.metadata = PacketMetadata{
      /*kind=*/kPacketKindIec61937,
      /*size_bytes=*/static_cast<int>(packed_size),
      /*total_frames=*/static_cast<int64_t>(CAEBitstreamPacker::GetOutputRate(stream_info_)),
      /*normalized_access_units=*/encoded_access_unit_count,
      /*effective_presentation_time_us=*/presentation_time_us,
  };
  packet.data.resize(packed_size);
  std::memcpy(packet.data.data(), bitstream_packer_.GetBuffer(), packed_size);
  pending_packets_.push_back(std::move(packet));
  LogVerbose("engine emit packed size=%u frames=%lld accessUnits=%d ptsUs=%lld pending=%zu stream=%d",
             packed_size,
             static_cast<long long>(CAEBitstreamPacker::GetOutputRate(stream_info_)),
             encoded_access_unit_count,
             static_cast<long long>(presentation_time_us),
             pending_packets_.size(),
             stream_info_.m_type);
}

void KodiAndroidPassthroughEngine::QueueInput(const uint8_t* data,
                                              int size,
                                              int64_t presentation_time_us,
                                              int encoded_access_unit_count)
{
  if (size <= 0)
    return;

  queued_input_bytes_ += size;
  LogVerbose("engine queue size=%d ptsUs=%lld accessUnits=%d mode=%d stream=%d queuedBytes=%lld",
             size,
             static_cast<long long>(presentation_time_us),
             encoded_access_unit_count,
             playback_decision_.mode,
             playback_decision_.stream_type,
             static_cast<long long>(queued_input_bytes_));
  if (playback_decision_.mode == kModeUnsupported)
    return;

  if (playback_decision_.mode == kModePcm)
  {
    const int output_channel_count = output_channel_count_ > 0 ? output_channel_count_ : channel_count_;
    const int bytes_per_frame = output_channel_count * BytesPerSampleForPcmEncoding(pcm_encoding_);
    const int64_t total_frames = bytes_per_frame > 0 ? size / bytes_per_frame : 0;
    if (data != nullptr)
    {
      QueueCopiedOutput(kPacketKindPcm,
                        data,
                        size,
                        presentation_time_us,
                        encoded_access_unit_count,
                        total_frames);
    }
    return;
  }

  if (data != nullptr)
  {
    if (PlaybackDecisionUsesIecCarrier(playback_decision_))
    {
      if (UsesKodiPassthroughCodec())
      {
        QueueCodecFrames(data, size, presentation_time_us, encoded_access_unit_count);
        return;
      }

      bitstream_packer_.Pack(stream_info_, const_cast<uint8_t*>(data), size);
      QueuePackedOutput(presentation_time_us, encoded_access_unit_count);
      return;
    }
    else if (playback_decision_.mode == kModePassthroughDirect)
    {
      if (UsesKodiPassthroughCodec())
      {
        QueueCodecFrames(data, size, presentation_time_us, encoded_access_unit_count);
        return;
      }
      QueueCopiedOutput(kPacketKindPassthroughDirect,
                        data,
                        size,
                        presentation_time_us,
                        encoded_access_unit_count,
                        /*total_frames=*/0);
      return;
    }
  }
  else
  {
    return;
  }
}

void KodiAndroidPassthroughEngine::QueuePause(unsigned int millis, bool iec_bursts)
{
  if (playback_decision_.mode == kModeUnsupported || playback_decision_.mode == kModePcm)
    return;

  if (stream_info_.m_type == CAEStreamInfo::STREAM_TYPE_NULL)
    return;

  if (!bitstream_packer_.PackPause(stream_info_, millis, iec_bursts))
    return;

  LogVerbose("engine queuePause millis=%u iec=%d stream=%d", millis, iec_bursts ? 1 : 0, stream_info_.m_type);
  QueuePackedOutput(/* presentation_time_us= */ 0, /* encoded_access_unit_count= */ 0);
}

bool KodiAndroidPassthroughEngine::DequeuePacket(PacketMetadata* packet)
{
  if (packet == nullptr || pending_packets_.empty())
    return false;

  PendingPacket pending_packet = std::move(pending_packets_.front());
  pending_packets_.pop_front();
  *packet = pending_packet.metadata;
  last_dequeued_packet_data_ = std::move(pending_packet.data);
  LogVerbose("engine dequeue kind=%d size=%d frames=%lld accessUnits=%d ptsUs=%lld pending=%zu",
             packet->kind,
             packet->size_bytes,
             static_cast<long long>(packet->total_frames),
             packet->normalized_access_units,
             static_cast<long long>(packet->effective_presentation_time_us),
             pending_packets_.size());
  return true;
}

void KodiAndroidPassthroughEngine::LogVerbose(const char* format, ...) const
{
  if (!verbose_logging_enabled_)
    return;
  va_list args;
  va_start(args, format);
  __android_log_vprint(ANDROID_LOG_INFO, kLogTag, format, args);
  va_end(args);
}

bool KodiAndroidPassthroughEngine::TakeLastDequeuedPacketData(std::vector<uint8_t>* data)
{
  if (data == nullptr || last_dequeued_packet_data_.empty())
    return false;

  *data = std::move(last_dequeued_packet_data_);
  last_dequeued_packet_data_.clear();
  return true;
}

void KodiAndroidPassthroughEngine::Play() { playing_ = true; }

void KodiAndroidPassthroughEngine::Pause() { playing_ = false; }

void KodiAndroidPassthroughEngine::Flush()
{
  pending_packets_.clear();
  last_dequeued_packet_data_.clear();
  bitstream_packer_.Reset();
  if (passthrough_codec_ != nullptr)
    passthrough_codec_->Reset();
}

void KodiAndroidPassthroughEngine::Stop()
{
  playing_ = false;
  pending_packets_.clear();
  last_dequeued_packet_data_.clear();
  bitstream_packer_.Reset();
  if (passthrough_codec_ != nullptr)
    passthrough_codec_->Reset();
}

void KodiAndroidPassthroughEngine::Reset()
{
  Stop();
  queued_input_bytes_ = 0;
  sample_rate_ = 0;
  channel_count_ = 0;
  pcm_encoding_ = 0;
  specified_buffer_size_ = 0;
  output_channel_count_ = 0;
  playback_decision_ = {};
  passthrough_codec_.reset();
  audio_format_ = {};
  stream_info_ = {};
}

int KodiAndroidPassthroughEngine::pending_packet_count() const
{
  return static_cast<int>(pending_packets_.size());
}

int64_t KodiAndroidPassthroughEngine::queued_input_bytes() const
{
  return queued_input_bytes_;
}

}  // namespace androidx_media3
