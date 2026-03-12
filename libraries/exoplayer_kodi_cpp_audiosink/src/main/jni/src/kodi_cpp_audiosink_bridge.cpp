#include <jni.h>

#include <algorithm>
#include <string>
#include <vector>

#include "androidjni/AudioFormat.h"
#include "cores/AudioEngine/Sinks/AESinkAUDIOTRACK.h"
#include "cores/AudioEngine/Utils/AEAudioFormat.h"
#include "cores/AudioEngine/Utils/AEDeviceInfo.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"

namespace {

constexpr char kSinkName[] = "AUDIOTRACK";

constexpr int kModeUnsupported = 0;
constexpr int kModePcm = 1;
constexpr int kModePassthroughDirect = 2;
constexpr int kModePassthroughIecStereo = 3;
constexpr int kModePassthroughIecMultichannel = 4;

constexpr int kMimeKindUnknown = 0;
constexpr int kMimeKindAc3 = 1;
constexpr int kMimeKindEAc3 = 2;
constexpr int kMimeKindDts = 3;
constexpr int kMimeKindDtsHd = 4;
constexpr int kMimeKindDtsUhd = 5;
constexpr int kMimeKindTrueHd = 6;
constexpr int kMimeKindPcm = 7;

struct ParsedDevice {
  std::string driver;
  std::string name;
  std::string friendly_name;
};

jobjectArray BuildStringArray(JNIEnv* env, const std::vector<std::string>& values) {
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) {
    return nullptr;
  }
  jobjectArray array = env->NewObjectArray(static_cast<jsize>(values.size()), string_class, nullptr);
  env->DeleteLocalRef(string_class);
  if (array == nullptr) {
    return nullptr;
  }
  for (jsize i = 0; i < static_cast<jsize>(values.size()); ++i) {
    jstring value = env->NewStringUTF(values[i].c_str());
    if (value == nullptr) {
      return array;
    }
    env->SetObjectArrayElement(array, i, value);
    env->DeleteLocalRef(value);
  }
  return array;
}

AEDeviceInfoList EnumerateDeviceInfos(bool force) {
  CAESinkAUDIOTRACK::Register();
  AEDeviceInfoList devices;
  CAESinkAUDIOTRACK::EnumerateDevicesEx(devices, force);
  return devices;
}

bool DeviceAllowedForListing(const CAEDeviceInfo& info, bool passthrough) {
  if (passthrough && (info.m_deviceType == AE_DEVTYPE_PCM || info.m_onlyPCM)) {
    return false;
  }
  if (!passthrough && info.m_onlyPassthrough) {
    return false;
  }
  return true;
}

std::vector<std::string> EnumerateDevices(bool force, bool passthrough_only) {
  AEDeviceInfoList devices = EnumerateDeviceInfos(force);
  std::vector<std::string> results;
  for (const auto& device : devices) {
    if (!DeviceAllowedForListing(device, passthrough_only)) {
      continue;
    }
    results.push_back(device.m_deviceName);
  }
  return results;
}

ParsedDevice ParseDevice(const std::string& device) {
  ParsedDevice parsed;
  parsed.name = device;

  size_t pos = parsed.name.find_first_of(':');
  bool found = false;
  if (pos != std::string::npos) {
    parsed.driver = device.substr(0, pos);
    if (parsed.driver == kSinkName) {
      parsed.name = parsed.name.substr(pos + 1, parsed.name.length() - pos - 1);
      found = true;
    }
  }

  if (!found) {
    parsed.driver.clear();
  }

  pos = parsed.name.find_last_of('|');
  if (pos != std::string::npos) {
    if (found) {
      parsed.friendly_name = parsed.name.substr(pos + 1);
    }
    parsed.name = parsed.name.substr(0, pos);
  }

  return parsed;
}

std::string ValidateOutputDevice(
    const std::string& device, bool passthrough, const AEDeviceInfoList& devices) {
  const ParsedDevice parsed = ParseDevice(device);

  if (!parsed.driver.empty() && !parsed.name.empty()) {
    for (const auto& info : devices) {
      if (!DeviceAllowedForListing(info, passthrough)) {
        continue;
      }
      if (info.m_deviceName == parsed.name) {
        return info.ToDeviceString(kSinkName);
      }
    }
  }

  if (!parsed.driver.empty() && !parsed.friendly_name.empty()) {
    for (const auto& info : devices) {
      if (!DeviceAllowedForListing(info, passthrough)) {
        continue;
      }
      if (info.GetFriendlyName() == parsed.friendly_name) {
        return info.ToDeviceString(kSinkName);
      }
    }
  }

  std::string first_device;
  for (const auto& info : devices) {
    if (!DeviceAllowedForListing(info, passthrough)) {
      continue;
    }

    if (first_device.empty()) {
      first_device = info.ToDeviceString(kSinkName);
    }

    if (info.m_deviceName.find("default") != std::string::npos) {
      return info.ToDeviceString(kSinkName);
    }
  }

  return first_device;
}

const CAEDeviceInfo* FindDevice(
    const std::string& device, const AEDeviceInfoList& devices, bool passthrough) {
  const ParsedDevice parsed = ParseDevice(device);
  for (const auto& info : devices) {
    if (!DeviceAllowedForListing(info, passthrough)) {
      continue;
    }
    if (info.m_deviceName == parsed.name) {
      return &info;
    }
  }
  return nullptr;
}

bool HasPassthroughDevice(const AEDeviceInfoList& devices) {
  for (const auto& info : devices) {
    if (info.m_deviceType != AE_DEVTYPE_PCM && !info.m_streamTypes.empty()) {
      return true;
    }
  }
  return false;
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
    default:
      return CAEStreamInfo::STREAM_TYPE_NULL;
  }
}

AEAudioFormat MakePassthroughFormat(
    CAEStreamInfo::DataType stream_type, int sample_rate, int channel_count) {
  AEAudioFormat format;
  format.m_dataFormat = AE_FMT_RAW;
  format.m_sampleRate = static_cast<unsigned int>(sample_rate);
  format.m_streamInfo.m_type = stream_type;
  format.m_streamInfo.m_sampleRate = static_cast<unsigned int>(sample_rate);
  format.m_streamInfo.m_channels = static_cast<unsigned int>(channel_count);
  format.m_frameSize = 1;
  return format;
}

bool SupportsFormat(const CAEDeviceInfo& info, AEAudioFormat& format) {
  const bool is_raw = format.m_dataFormat == AE_FMT_RAW;
  bool format_exists = false;
  unsigned int sample_rate = format.m_sampleRate;

  if (is_raw && info.m_wantsIECPassthrough) {
    switch (format.m_streamInfo.m_type) {
      case CAEStreamInfo::STREAM_TYPE_EAC3:
        sample_rate = 192000;
        break;
      case CAEStreamInfo::STREAM_TYPE_TRUEHD:
        if (format.m_streamInfo.m_sampleRate == 48000 ||
            format.m_streamInfo.m_sampleRate == 96000 ||
            format.m_streamInfo.m_sampleRate == 192000) {
          sample_rate = 192000;
        } else {
          sample_rate = 176400;
        }
        break;
      case CAEStreamInfo::STREAM_TYPE_DTSHD:
      case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
        sample_rate = 192000;
        break;
      default:
        break;
    }
    format_exists =
        std::find(info.m_streamTypes.begin(), info.m_streamTypes.end(), format.m_streamInfo.m_type) !=
        info.m_streamTypes.end();
  } else if (is_raw && !info.m_wantsIECPassthrough) {
    sample_rate = 48000;
    format_exists =
        std::find(info.m_streamTypes.begin(), info.m_streamTypes.end(), format.m_streamInfo.m_type) !=
        info.m_streamTypes.end();
  } else {
    format_exists =
        std::find(info.m_dataFormats.begin(), info.m_dataFormats.end(), format.m_dataFormat) !=
        info.m_dataFormats.end();
  }

  if (!format_exists) {
    return false;
  }

  return std::find(info.m_sampleRates.begin(), info.m_sampleRates.end(), sample_rate) !=
         info.m_sampleRates.end();
}

int EncodingForStreamType(CAEStreamInfo::DataType stream_type, bool wants_iec) {
  if (wants_iec) {
    return CJNIAudioFormat::ENCODING_IEC61937;
  }
  switch (stream_type) {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      return CJNIAudioFormat::ENCODING_AC3;
    case CAEStreamInfo::STREAM_TYPE_EAC3:
      return CJNIAudioFormat::ENCODING_E_AC3;
    case CAEStreamInfo::STREAM_TYPE_DTS_512:
    case CAEStreamInfo::STREAM_TYPE_DTS_1024:
    case CAEStreamInfo::STREAM_TYPE_DTS_2048:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      return CJNIAudioFormat::ENCODING_DTS;
    case CAEStreamInfo::STREAM_TYPE_DTSHD:
    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      return CJNIAudioFormat::ENCODING_DTS_HD;
    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      return CJNIAudioFormat::ENCODING_DOLBY_TRUEHD;
    default:
      return 0;
  }
}

int ChannelConfigForStreamType(CAEStreamInfo::DataType stream_type, bool wants_iec) {
  if (wants_iec) {
    if (stream_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA ||
        stream_type == CAEStreamInfo::STREAM_TYPE_TRUEHD) {
      return CJNIAudioFormat::CHANNEL_OUT_7POINT1_SURROUND;
    }
    return CJNIAudioFormat::CHANNEL_OUT_STEREO;
  }
  if (stream_type == CAEStreamInfo::STREAM_TYPE_DTSHD ||
      stream_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA ||
      stream_type == CAEStreamInfo::STREAM_TYPE_TRUEHD) {
    return CJNIAudioFormat::CHANNEL_OUT_7POINT1_SURROUND;
  }
  return CJNIAudioFormat::CHANNEL_OUT_STEREO;
}

std::array<jint, 5> MakeDecision(
    int mode, int output_encoding, int channel_config, int stream_type, int flags) {
  return {mode, output_encoding, channel_config, stream_type, flags};
}

std::array<jint, 5> EvaluatePlaybackDecision(
    int config,
    bool passthrough_enabled,
    bool ac3_passthrough_enabled,
    bool eac3_passthrough_enabled,
    bool dts_passthrough_enabled,
    bool truehd_passthrough_enabled,
    bool dtshd_passthrough_enabled,
    bool dtshd_core_fallback_enabled,
    const std::string& device,
    const std::string& passthrough_device,
    int mime_kind,
    int channel_count,
    int sample_rate) {
  if (!passthrough_enabled || config == 1) {
    return MakeDecision(kModePcm, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
  }
  if (mime_kind == kMimeKindPcm) {
    return MakeDecision(kModePcm, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
  }

  const CAEStreamInfo::DataType requested_stream_type = MimeKindToStreamType(mime_kind);
  if (requested_stream_type == CAEStreamInfo::STREAM_TYPE_NULL) {
    return MakeDecision(kModeUnsupported, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
  }

  const AEDeviceInfoList devices = EnumerateDeviceInfos(false);
  if (!HasPassthroughDevice(devices)) {
    return MakeDecision(kModePcm, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
  }

  const std::string validated_pcm_device = ValidateOutputDevice(device, false, devices);
  const std::string validated_passthrough_device =
      ValidateOutputDevice(passthrough_device, true, devices);
  (void)validated_pcm_device;
  if (validated_passthrough_device.empty()) {
    return MakeDecision(kModePcm, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
  }

  const CAEDeviceInfo* selected_device =
      FindDevice(validated_passthrough_device, devices, true);
  if (selected_device == nullptr) {
    return MakeDecision(kModePcm, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
  }

  auto streamEnabled = [&](CAEStreamInfo::DataType stream_type) {
    switch (stream_type) {
      case CAEStreamInfo::STREAM_TYPE_AC3:
        return ac3_passthrough_enabled;
      case CAEStreamInfo::STREAM_TYPE_EAC3:
        return eac3_passthrough_enabled;
      case CAEStreamInfo::STREAM_TYPE_DTS_512:
      case CAEStreamInfo::STREAM_TYPE_DTS_1024:
      case CAEStreamInfo::STREAM_TYPE_DTS_2048:
      case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
        return dts_passthrough_enabled;
      case CAEStreamInfo::STREAM_TYPE_DTSHD:
      case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
        return dtshd_passthrough_enabled;
      case CAEStreamInfo::STREAM_TYPE_TRUEHD:
        return truehd_passthrough_enabled;
      default:
        return false;
    }
  };

  auto tryDecision = [&](CAEStreamInfo::DataType stream_type) -> std::array<jint, 5> {
    if (!streamEnabled(stream_type)) {
      return MakeDecision(kModeUnsupported, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
    }

    AEAudioFormat format = MakePassthroughFormat(stream_type, sample_rate, channel_count);
    if (!SupportsFormat(*selected_device, format)) {
      return MakeDecision(kModeUnsupported, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
    }

    const bool wants_iec = selected_device->m_wantsIECPassthrough;
    const int mode =
        wants_iec
            ? ((stream_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA ||
                stream_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
                   ? kModePassthroughIecMultichannel
                   : kModePassthroughIecStereo)
            : kModePassthroughDirect;
    return MakeDecision(
        mode,
        EncodingForStreamType(stream_type, wants_iec),
        ChannelConfigForStreamType(stream_type, wants_iec),
        stream_type,
        0);
  };

  std::array<jint, 5> decision = tryDecision(requested_stream_type);
  if (decision[0] != kModeUnsupported) {
    return decision;
  }

  if ((mime_kind == kMimeKindDtsHd || mime_kind == kMimeKindDtsUhd) &&
      dtshd_core_fallback_enabled) {
    decision = tryDecision(CAEStreamInfo::STREAM_TYPE_DTSHD_CORE);
    if (decision[0] != kModeUnsupported) {
      return decision;
    }
  }

  return MakeDecision(kModePcm, 0, 0, CAEStreamInfo::STREAM_TYPE_NULL, 0);
}

static jintArray BuildIntArray(JNIEnv* env, const std::array<jint, 5>& values) {
  jintArray array = env->NewIntArray(static_cast<jsize>(values.size()));
  if (array == nullptr) {
    return nullptr;
  }
  env->SetIntArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
  return array;
}

}  // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_androidx_media3_exoplayer_audio_kodi_cpp_KodiCppAudioSinkLibrary_nEnumerateAudioTrackDevices(
    JNIEnv* env, jclass /* clazz */, jboolean force) {
  return BuildStringArray(env, EnumerateDevices(force == JNI_TRUE, false));
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_androidx_media3_exoplayer_audio_kodi_cpp_KodiCppAudioSinkLibrary_nEnumerateAudioTrackPassthroughDevices(
    JNIEnv* env, jclass /* clazz */, jboolean force) {
  return BuildStringArray(env, EnumerateDevices(force == JNI_TRUE, true));
}

extern "C" JNIEXPORT jintArray JNICALL
Java_androidx_media3_exoplayer_audio_kodi_cpp_KodiCppCapabilitySelector_nEvaluatePlaybackDecision(
    JNIEnv* env,
    jclass /* clazz */,
    jint config,
    jboolean passthrough_enabled,
    jboolean ac3_passthrough_enabled,
    jboolean eac3_passthrough_enabled,
    jboolean dts_passthrough_enabled,
    jboolean truehd_passthrough_enabled,
    jboolean dtshd_passthrough_enabled,
    jboolean dtshd_core_fallback_enabled,
    jstring device,
    jstring passthrough_device,
    jint mime_kind,
    jint channel_count,
    jint sample_rate) {
  const char* device_chars = device != nullptr ? env->GetStringUTFChars(device, nullptr) : nullptr;
  const char* passthrough_device_chars =
      passthrough_device != nullptr ? env->GetStringUTFChars(passthrough_device, nullptr) : nullptr;
  const std::string device_string = device_chars != nullptr ? device_chars : "";
  const std::string passthrough_device_string =
      passthrough_device_chars != nullptr ? passthrough_device_chars : "";
  if (device_chars != nullptr) {
    env->ReleaseStringUTFChars(device, device_chars);
  }
  if (passthrough_device_chars != nullptr) {
    env->ReleaseStringUTFChars(passthrough_device, passthrough_device_chars);
  }

  return BuildIntArray(
      env,
      EvaluatePlaybackDecision(
          config,
          passthrough_enabled == JNI_TRUE,
          ac3_passthrough_enabled == JNI_TRUE,
          eac3_passthrough_enabled == JNI_TRUE,
          dts_passthrough_enabled == JNI_TRUE,
          truehd_passthrough_enabled == JNI_TRUE,
          dtshd_passthrough_enabled == JNI_TRUE,
          dtshd_core_fallback_enabled == JNI_TRUE,
          device_string,
          passthrough_device_string,
          mime_kind,
          channel_count,
          sample_rate));
}
