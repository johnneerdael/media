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

#include "KodiCapabilitySelector.h"

#include "AEAudioFormat.h"
#include "ActiveAEPolicy.h"
#include "ActiveAESettings.h"
#include "ActiveAESink.h"

namespace androidx_media3 {
namespace {

constexpr int kModeUnsupported = 0;
constexpr int kModePcm = 1;
constexpr int kModePassthroughDirect = 2;
constexpr int kModePassthroughIecStereo = 3;
constexpr int kModePassthroughIecMultichannel = 4;

constexpr int kChannelOutStereo = 12;
constexpr int kChannelOut7Point1 = 6396;

PlaybackDecision MakeUnsupportedDecision() {
  return PlaybackDecision{/*mode=*/kModeUnsupported,
                          /*output_encoding=*/0,
                          /*channel_config=*/0,
                          /*stream_type=*/CAEStreamInfo::STREAM_TYPE_NULL,
                          /*flags=*/0};
}

PlaybackDecision MakePcmDecision() {
  return PlaybackDecision{/*mode=*/kModePcm,
                          /*output_encoding=*/0,
                          /*channel_config=*/0,
                          /*stream_type=*/CAEStreamInfo::STREAM_TYPE_NULL,
                          /*flags=*/0};
}

PlaybackDecision MakeDecisionFromProbe(const ProbeResult& probe, int stream_type) {
  if (!probe.supported) {
    return MakeUnsupportedDecision();
  }
  const bool multichannel = probe.channel_config != kChannelOutStereo;
  return PlaybackDecision{
      /*mode=*/multichannel ? kModePassthroughIecMultichannel
                            : kModePassthroughIecStereo,
      /*output_encoding=*/probe.encoding,
      /*channel_config=*/probe.channel_config,
      /*stream_type=*/stream_type,
      /*flags=*/0};
}

CAEStreamInfo::DataType MimeKindToStreamType(int mime_kind) {
  switch (mime_kind) {
    case kMimeKindAc3:
      return CAEStreamInfo::STREAM_TYPE_AC3;
    case kMimeKindEAc3:
      return CAEStreamInfo::STREAM_TYPE_EAC3;
    case kMimeKindDts:
      return CAEStreamInfo::STREAM_TYPE_DTSHD_CORE;
    case kMimeKindDtsHd:
    case kMimeKindDtsUhd:
      return CAEStreamInfo::STREAM_TYPE_DTSHD;
    case kMimeKindTrueHd:
      return CAEStreamInfo::STREAM_TYPE_TRUEHD;
    case kMimeKindPcm:
    case kMimeKindUnknown:
    default:
      return CAEStreamInfo::STREAM_TYPE_NULL;
  }
}

ProbeResult ProbeForStreamType(const CapabilitySnapshot& snapshot,
                               CAEStreamInfo::DataType stream_type) {
  switch (stream_type) {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      return snapshot.ac3;
    case CAEStreamInfo::STREAM_TYPE_EAC3:
      return snapshot.eac3;
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      return snapshot.dts;
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      return snapshot.dtshd;
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      return snapshot.truehd;
    case CAEStreamInfo::STREAM_TYPE_NULL:
    case CAEStreamInfo::STREAM_TYPE_MLP:
    default:
      return ProbeResult{/*supported=*/false, /*encoding=*/0, /*channel_config=*/0};
  }
}

AEAudioFormat MakePassthroughFormat(CAEStreamInfo::DataType stream_type,
                                    int sample_rate,
                                    int channel_count) {
  AEAudioFormat format;
  format.m_dataFormat = AE_FMT_RAW;
  format.m_sampleRate = static_cast<unsigned int>(sample_rate);
  format.m_streamInfo.m_type = stream_type;
  format.m_streamInfo.m_sampleRate = static_cast<unsigned int>(sample_rate);
  format.m_streamInfo.m_channels = static_cast<unsigned int>(channel_count);
  format.m_frameSize = 1;
  return format;
}

PlaybackDecision MakeDecisionForDevice(const ProbeResult& probe,
                                       int stream_type,
                                       const CAEDeviceInfo* device_info) {
  if (device_info == nullptr) {
    return MakeUnsupportedDecision();
  }
  if (!device_info->m_wantsIECPassthrough) {
    return PlaybackDecision{/*mode=*/kModePassthroughDirect,
                            /*output_encoding=*/probe.encoding,
                            /*channel_config=*/probe.channel_config,
                            /*stream_type=*/stream_type,
                            /*flags=*/0};
  }
  return MakeDecisionFromProbe(probe, stream_type);
}

}  // namespace

PlaybackDecision EvaluatePlaybackDecision(
    const CapabilitySnapshot& snapshot,
    const UserAudioSettings& user_settings,
    int mime_kind,
    int channel_count,
    int sample_rate) {
  if (!user_settings.passthrough_enabled) {
    return MakePcmDecision();
  }

  const CAEStreamInfo::DataType requested_stream_type = MimeKindToStreamType(mime_kind);
  if (mime_kind == kMimeKindPcm) {
    return MakePcmDecision();
  }
  if (requested_stream_type == CAEStreamInfo::STREAM_TYPE_NULL) {
    return MakeUnsupportedDecision();
  }

  CActiveAESink sink;
  sink.EnumerateOutputDevices(snapshot);
  const CActiveAESettings::AudioSettings settings =
      CActiveAESettings::Load(user_settings, sink.HasPassthroughDevice());
  CActiveAEPolicy policy(settings, sink);

  AEAudioFormat format = MakePassthroughFormat(requested_stream_type, sample_rate, channel_count);
  const CAEDeviceInfo* selected_device = nullptr;
  if (policy.SupportsRaw(format, &selected_device)) {
    return MakeDecisionForDevice(ProbeForStreamType(snapshot, format.m_streamInfo.m_type),
                                 format.m_streamInfo.m_type,
                                 selected_device);
  }

  if ((mime_kind == kMimeKindDtsHd || mime_kind == kMimeKindDtsUhd) &&
      policy.UsesDtsCoreFallback()) {
    AEAudioFormat fallback_format =
        MakePassthroughFormat(CAEStreamInfo::STREAM_TYPE_DTSHD_CORE, sample_rate, channel_count);
    if (policy.SupportsRaw(fallback_format, &selected_device)) {
      return MakeDecisionForDevice(snapshot.dts, fallback_format.m_streamInfo.m_type,
                                   selected_device);
    }
  }

  return MakePcmDecision();
}

}  // namespace androidx_media3
