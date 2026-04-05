#include <jni.h>
extern "C" {
#include "libavcodec/version.h"
#include "libavcodec/defs.h"
#include "libavcodec/packet.h"
#include "libavformat/avformat.h"
#include "libavutil/hwcontext.h"
#include "libavutil/dict.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
}
#include "config.h"
#include "config_components.h"
#include "ffcommon.h"
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <atomic>
#include <string>
#include <algorithm>
#include <vector>

#ifndef FFMPEG_TONEMAP_FILTERS
#define FFMPEG_TONEMAP_FILTERS 0
#endif

#if FFMPEG_TONEMAP_FILTERS && defined(CONFIG_LIBPLACEBO_FILTER) && CONFIG_LIBPLACEBO_FILTER && \
    defined(CONFIG_LIBPLACEBO) && CONFIG_LIBPLACEBO && defined(CONFIG_VULKAN) && CONFIG_VULKAN
#define FFMPEG_DV5_TONEMAP_AVAILABLE 1
#else
#define FFMPEG_DV5_TONEMAP_AVAILABLE 0
#endif

void ffmpegSetExperimentalDv5HardwareToneMapRpuBridgeEnabled(bool enabled);
void ffmpegPushExperimentalDv5HardwareRpuSample(
        int64_t sampleTimeUs, const uint8_t *payload, size_t payloadSize);
void ffmpegNotifyExperimentalDv5HardwareFramePresented(int64_t presentationTimeUs);
bool ffmpegRenderExperimentalDv5HardwareFrame(
        JNIEnv *env,
        int64_t presentationTimeUs,
        jobject hardwareBuffer,
        int32_t displayedWidth,
        int32_t displayedHeight,
        jobject outputSurface);
bool ffmpegRenderExperimentalDv5HardwareFramePure(
        JNIEnv *env,
        int64_t presentationTimeUs,
        jobject hardwareBuffer,
        int32_t displayedWidth,
        int32_t displayedHeight,
        jobject outputSurface);

static std::atomic_bool gExperimentalIecDebugLoggingEnabled(false);

bool ffmpegIsExperimentalIecDebugLoggingEnabled() {
    return gExperimentalIecDebugLoggingEnabled.load();
}

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return -1;
    }
    return JNI_VERSION_1_6;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegGetVersion(JNIEnv *env,
                                                                   jclass clazz) {
    return env->NewStringUTF(LIBAVCODEC_IDENT);
}

extern "C"
JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegGetInputBufferPaddingSize(
        JNIEnv *env, jclass clazz) {
    return (jint) AV_INPUT_BUFFER_PADDING_SIZE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegHasDecoder(JNIEnv *env,
                                                                   jclass clazz,
                                                                   jstring codec_name) {
    return getCodecByName(env, codec_name) != nullptr;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegSupportsDv5ToneMapToSdr(
        JNIEnv *env,
        jclass clazz) {
    (void) env;
    (void) clazz;
    return FFMPEG_DV5_TONEMAP_AVAILABLE ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegSupportsDv5ToneMapToSdrRuntime(
        JNIEnv *env,
        jclass clazz) {
    (void) env;
    (void) clazz;
#if !FFMPEG_DV5_TONEMAP_AVAILABLE
    return JNI_FALSE;
#else
    AVBufferRef *deviceRef = nullptr;
    int result = av_hwdevice_ctx_create(
            &deviceRef,
            AV_HWDEVICE_TYPE_VULKAN,
            nullptr,
            nullptr,
            0);
    if (result < 0) {
        logError("av_hwdevice_ctx_create(vulkan)[runtime_probe]", result);
        av_buffer_unref(&deviceRef);
        return JNI_FALSE;
    }
    av_buffer_unref(&deviceRef);
    return JNI_TRUE;
#endif
}

extern "C"
JNIEXPORT void JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegSetExperimentalDv5HardwareToneMapRpuBridgeEnabled(
        JNIEnv *env,
        jclass clazz,
        jboolean enabled) {
    (void) env;
    (void) clazz;
    ffmpegSetExperimentalDv5HardwareToneMapRpuBridgeEnabled(enabled == JNI_TRUE);
}

extern "C"
JNIEXPORT void JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegSetExperimentalIecDebugLoggingEnabled(
        JNIEnv *env,
        jclass clazz,
        jboolean enabled) {
    (void) env;
    (void) clazz;
    gExperimentalIecDebugLoggingEnabled.store(enabled == JNI_TRUE);
}

extern "C"
JNIEXPORT void JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegPushExperimentalDv5HardwareRpuSample(
        JNIEnv *env,
        jclass clazz,
        jlong sample_time_us,
        jbyteArray rpu_nal_payload) {
    (void) clazz;
    if (rpu_nal_payload == nullptr) {
        return;
    }
    jsize payloadLength = env->GetArrayLength(rpu_nal_payload);
    if (payloadLength <= 0) {
        return;
    }
    jbyte *payload = env->GetByteArrayElements(rpu_nal_payload, nullptr);
    if (!payload) {
        return;
    }
    ffmpegPushExperimentalDv5HardwareRpuSample(
            static_cast<int64_t>(sample_time_us),
            reinterpret_cast<const uint8_t *>(payload),
            static_cast<size_t>(payloadLength));
    env->ReleaseByteArrayElements(rpu_nal_payload, payload, JNI_ABORT);
}

extern "C"
JNIEXPORT void JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegNotifyExperimentalDv5HardwareFramePresented(
        JNIEnv *env,
        jclass clazz,
        jlong presentation_time_us) {
    (void) env;
    (void) clazz;
    ffmpegNotifyExperimentalDv5HardwareFramePresented(static_cast<int64_t>(presentation_time_us));
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegRenderExperimentalDv5HardwareFrame(
        JNIEnv *env,
        jclass clazz,
        jlong presentation_time_us,
        jobject hardware_buffer,
        jint displayed_width,
        jint displayed_height,
        jobject output_surface) {
    (void) clazz;
    if (hardware_buffer == nullptr || output_surface == nullptr) {
        return JNI_FALSE;
    }
    return ffmpegRenderExperimentalDv5HardwareFrame(
                   env,
                   static_cast<int64_t>(presentation_time_us),
                   hardware_buffer,
                   static_cast<int32_t>(displayed_width),
                   static_cast<int32_t>(displayed_height),
                   output_surface)
           ? JNI_TRUE
           : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegRenderExperimentalDv5HardwareFramePure(
        JNIEnv *env,
        jclass clazz,
        jlong presentation_time_us,
        jobject hardware_buffer,
        jint displayed_width,
        jint displayed_height,
        jobject output_surface) {
    (void) clazz;
    if (hardware_buffer == nullptr || output_surface == nullptr) {
        return JNI_FALSE;
    }
    return ffmpegRenderExperimentalDv5HardwareFramePure(
                   env,
                   static_cast<int64_t>(presentation_time_us),
                   hardware_buffer,
                   static_cast<int32_t>(displayed_width),
                   static_cast<int32_t>(displayed_height),
                   output_surface)
           ? JNI_TRUE
           : JNI_FALSE;
}

namespace {

int openInputForProbe(
        AVFormatContext **formatContext,
        const char *urlChars,
        const char *headersChars) {
    if (urlChars == nullptr || urlChars[0] == '\0') {
        return -1;
    }
    AVDictionary *options = nullptr;
    // Low-budget fast path for autoplay scoring probes.
    av_dict_set(&options, "probesize", "10000", 0);
    av_dict_set(&options, "analyzeduration", "10000", 0);
    av_dict_set(&options, "rw_timeout", "5000000", 0);
    if (headersChars != nullptr && headersChars[0] != '\0') {
        av_dict_set(&options, "headers", headersChars, 0);
    }
    int openResult = avformat_open_input(formatContext, urlChars, nullptr, &options);
    av_dict_free(&options);
    return openResult;
}

std::string urlDecode(const std::string &value) {
    auto hex_value = [](unsigned char input) -> int {
        if (input >= '0' && input <= '9') return input - '0';
        if (input >= 'a' && input <= 'f') return input - 'a' + 10;
        if (input >= 'A' && input <= 'F') return input - 'A' + 10;
        return -1;
    };

    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '%' && i + 2 < value.size()) {
            const unsigned char hi = static_cast<unsigned char>(value[i + 1]);
            const unsigned char lo = static_cast<unsigned char>(value[i + 2]);
            const int hi_value = hex_value(hi);
            const int lo_value = hex_value(lo);
            if (hi_value >= 0 && lo_value >= 0) {
                decoded.push_back(static_cast<char>((hi_value << 4) | lo_value));
                i += 2;
                continue;
            }
        }
        if (c == '+') {
            decoded.push_back(' ');
            continue;
        }
        decoded.push_back(static_cast<char>(c));
    }
    return decoded;
}

std::string extractEmbeddedResolveUrl(const std::string &sourceUrl) {
    const std::string marker = "/resolve/";
    const auto markerIndex = sourceUrl.find(marker);
    if (markerIndex == std::string::npos) {
        return "";
    }

    const auto afterResolve = sourceUrl.substr(markerIndex + marker.size());
    const auto firstSlash = afterResolve.find('/');
    if (firstSlash == std::string::npos) {
        return "";
    }
    const auto secondSlash = afterResolve.find('/', firstSlash + 1);
    if (secondSlash == std::string::npos) {
        return "";
    }
    const auto nestedEncoded = afterResolve.substr(secondSlash + 1);
    if (nestedEncoded.empty()) {
        return "";
    }
    return urlDecode(nestedEncoded);
}

std::string toHttpFallbackUrl(const std::string &url) {
    const std::string httpsPrefix = "https://";
    if (url.size() >= httpsPrefix.size() &&
        std::equal(httpsPrefix.begin(), httpsPrefix.end(), url.begin())) {
        return std::string("http://") + url.substr(httpsPrefix.size());
    }
    return "";
}

std::string sanitizeProbeValue(const std::string &value) {
    std::string sanitized = value;
    std::replace(sanitized.begin(), sanitized.end(), ';', '_');
    return sanitized;
}

std::string escapeJsonString(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(c);
                break;
        }
    }
    return escaped;
}

}  // namespace

extern "C"
JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegProbeDolbyVisionProfile(
        JNIEnv *env,
        jclass clazz,
        jstring url,
        jstring request_headers_blob) {
    (void) clazz;
    if (url == nullptr) {
        return -3;
    }

    const char *url_chars = env->GetStringUTFChars(url, nullptr);
    const char *headers_chars =
            request_headers_blob != nullptr
            ? env->GetStringUTFChars(request_headers_blob, nullptr)
            : nullptr;

    AVFormatContext *format_context = nullptr;
    avformat_network_init();

    std::vector<std::string> probe_urls;
    probe_urls.reserve(3);
    auto add_unique_probe_url = [&probe_urls](const std::string &candidate) {
        if (candidate.empty()) {
            return;
        }
        if (std::find(probe_urls.begin(), probe_urls.end(), candidate) == probe_urls.end()) {
            probe_urls.push_back(candidate);
        }
    };

    add_unique_probe_url(url_chars);
    add_unique_probe_url(extractEmbeddedResolveUrl(url_chars));
    for (size_t i = 0; i < probe_urls.size(); ++i) {
        add_unique_probe_url(toHttpFallbackUrl(probe_urls[i]));
    }

    int open_result = -1;
    for (const auto &probe_url : probe_urls) {
        open_result = openInputForProbe(&format_context, probe_url.c_str(), headers_chars);
        if (open_result >= 0 && format_context != nullptr) {
            break;
        }
        format_context = nullptr;
    }

    if (open_result < 0 || format_context == nullptr) {
        logError("avformat_open_input[ffmpegProbeDolbyVisionProfile]", open_result);
        if (headers_chars != nullptr) {
            env->ReleaseStringUTFChars(request_headers_blob, headers_chars);
        }
        env->ReleaseStringUTFChars(url, url_chars);
        return -3;
    }

    int info_result = avformat_find_stream_info(format_context, nullptr);
    if (info_result < 0) {
        logError("avformat_find_stream_info[ffmpegProbeDolbyVisionProfile]", info_result);
        avformat_close_input(&format_context);
        if (headers_chars != nullptr) {
            env->ReleaseStringUTFChars(request_headers_blob, headers_chars);
        }
        env->ReleaseStringUTFChars(url, url_chars);
        return -3;
    }

    int video_stream_index = av_find_best_stream(
            format_context,
            AVMEDIA_TYPE_VIDEO,
            -1,
            -1,
            nullptr,
            0);
    if (video_stream_index < 0) {
        avformat_close_input(&format_context);
        if (headers_chars != nullptr) {
            env->ReleaseStringUTFChars(request_headers_blob, headers_chars);
        }
        env->ReleaseStringUTFChars(url, url_chars);
        return -3;
    }

    AVStream *stream = format_context->streams[video_stream_index];
    auto extract_profile_from_dovi_side_data =
            [](const AVPacketSideData *side_data) -> jint {
        if (side_data == nullptr ||
            side_data->size < static_cast<int>(sizeof(AVDOVIDecoderConfigurationRecord))) {
            return -1;
        }
        const auto *dovi =
                reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(side_data->data);
        return static_cast<jint>(dovi->dv_profile);
    };

    auto extract_profile_from_dovi_box = [](const uint8_t *data, size_t size) -> jint {
        if (data == nullptr || size < 8) return -1;
        for (size_t i = 0; i + 8 <= size; ++i) {
            const bool has_dovi_tag =
                    (data[i] == 'd' && data[i + 1] == 'v' &&
                     (data[i + 2] == 'c' || data[i + 2] == 'v' || data[i + 2] == 'w') &&
                     data[i + 3] == 'C');
            if (!has_dovi_tag) continue;
            if (i + 8 > size) break;
            const uint8_t *payload = data + i + 4;
            const uint32_t buf = (static_cast<uint32_t>(payload[2]) << 8) |
                                 static_cast<uint32_t>(payload[3]);
            const jint profile = static_cast<jint>((buf >> 9) & 0x7f);
            if (profile > 0 && profile < 32) {
                return profile;
            }
        }
        return -1;
    };

    jint result = -2;
    result = extract_profile_from_dovi_side_data(
        av_packet_side_data_get(
                stream->codecpar->coded_side_data,
                stream->codecpar->nb_coded_side_data,
                AV_PKT_DATA_DOVI_CONF));

    if (result < 0) {
        result = extract_profile_from_dovi_box(
                stream->codecpar->extradata,
                static_cast<size_t>(std::max(stream->codecpar->extradata_size, 0)));
    }

    if (result < 0) {
        AVPacket packet;
        av_init_packet(&packet);
        int packets_scanned = 0;
        while (packets_scanned < 12 && av_read_frame(format_context, &packet) >= 0) {
            if (packet.stream_index == video_stream_index) {
                packets_scanned += 1;
                result = extract_profile_from_dovi_side_data(
                        av_packet_side_data_get(
                                packet.side_data,
                                packet.side_data_elems,
                                AV_PKT_DATA_DOVI_CONF));
                if (result < 0) {
                    result = extract_profile_from_dovi_box(
                            packet.data,
                            static_cast<size_t>(std::max(packet.size, 0)));
                }
                av_packet_unref(&packet);
                if (result >= 0) {
                    break;
                }
            } else {
                av_packet_unref(&packet);
            }
        }
    }

    avformat_close_input(&format_context);
    if (headers_chars != nullptr) {
        env->ReleaseStringUTFChars(request_headers_blob, headers_chars);
    }
    env->ReleaseStringUTFChars(url, url_chars);
    return result;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegProbeDolbyVisionMetadataBlob(
        JNIEnv *env,
        jclass clazz,
        jstring url,
        jstring request_headers_blob) {
    (void) clazz;
    if (url == nullptr) {
        return nullptr;
    }

    const char *url_chars = env->GetStringUTFChars(url, nullptr);
    const char *headers_chars =
            request_headers_blob != nullptr
            ? env->GetStringUTFChars(request_headers_blob, nullptr)
            : nullptr;

    AVFormatContext *format_context = nullptr;
    avformat_network_init();

    std::vector<std::string> probe_urls;
    probe_urls.reserve(3);
    auto add_unique_probe_url = [&probe_urls](const std::string &candidate) {
        if (candidate.empty()) {
            return;
        }
        if (std::find(probe_urls.begin(), probe_urls.end(), candidate) == probe_urls.end()) {
            probe_urls.push_back(candidate);
        }
    };

    add_unique_probe_url(url_chars);
    add_unique_probe_url(extractEmbeddedResolveUrl(url_chars));
    for (size_t i = 0; i < probe_urls.size(); ++i) {
        add_unique_probe_url(toHttpFallbackUrl(probe_urls[i]));
    }

    int open_result = -1;
    for (const auto &probe_url : probe_urls) {
        open_result = openInputForProbe(&format_context, probe_url.c_str(), headers_chars);
        if (open_result >= 0 && format_context != nullptr) {
            break;
        }
        format_context = nullptr;
    }

    std::string video_codec = "unknown";
    std::string audio_codec = "unknown";
    std::string hdr_type = "unknown";

    if (open_result >= 0 && format_context != nullptr &&
        avformat_find_stream_info(format_context, nullptr) >= 0) {
        const int video_stream_index = av_find_best_stream(
                format_context,
                AVMEDIA_TYPE_VIDEO,
                -1,
                -1,
                nullptr,
                0);
        if (video_stream_index >= 0) {
            AVStream *video_stream = format_context->streams[video_stream_index];
            if (video_stream != nullptr && video_stream->codecpar != nullptr) {
                const char *codec_name = avcodec_get_name(video_stream->codecpar->codec_id);
                if (codec_name != nullptr && codec_name[0] != '\0') {
                    video_codec = codec_name;
                }

                const AVPacketSideData *dovi_side_data = av_packet_side_data_get(
                        video_stream->codecpar->coded_side_data,
                        video_stream->codecpar->nb_coded_side_data,
                        AV_PKT_DATA_DOVI_CONF);
                if (dovi_side_data != nullptr &&
                    dovi_side_data->size >= static_cast<int>(sizeof(AVDOVIDecoderConfigurationRecord))) {
                    hdr_type = "DolbyVision";
                } else {
                    const uint8_t *data = video_stream->codecpar->extradata;
                    const size_t size = static_cast<size_t>(
                            std::max(video_stream->codecpar->extradata_size, 0));
                    if (data != nullptr && size >= 8) {
                        for (size_t i = 0; i + 8 <= size; ++i) {
                            const bool has_dovi_tag =
                                    (data[i] == 'd' && data[i + 1] == 'v' &&
                                     (data[i + 2] == 'c' || data[i + 2] == 'v' || data[i + 2] == 'w') &&
                                     data[i + 3] == 'C');
                            if (has_dovi_tag) {
                                hdr_type = "DolbyVision";
                                break;
                            }
                        }
                    }
                }

                if (hdr_type == "unknown") {
                    if (video_stream->codecpar->color_trc == AVCOL_TRC_SMPTE2084) {
                        hdr_type = "HDR10";
                    } else if (video_stream->codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67) {
                        hdr_type = "HLG";
                    }
                }
            }
        }

        const int audio_stream_index = av_find_best_stream(
                format_context,
                AVMEDIA_TYPE_AUDIO,
                -1,
                -1,
                nullptr,
                0);
        if (audio_stream_index >= 0) {
            AVStream *audio_stream = format_context->streams[audio_stream_index];
            if (audio_stream != nullptr && audio_stream->codecpar != nullptr) {
                const char *codec_name = avcodec_get_name(audio_stream->codecpar->codec_id);
                if (codec_name != nullptr && codec_name[0] != '\0') {
                    audio_codec = codec_name;
                }
            }
        }
    }

    if (format_context != nullptr) {
        avformat_close_input(&format_context);
    }
    if (headers_chars != nullptr) {
        env->ReleaseStringUTFChars(request_headers_blob, headers_chars);
    }
    env->ReleaseStringUTFChars(url, url_chars);

    std::string blob = "video=" + video_codec + ";audio=" + audio_codec + ";hdr=" + hdr_type;
    return env->NewStringUTF(blob.c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegProbeDolbyVisionProbeBlob(
        JNIEnv *env,
        jclass clazz,
        jstring url,
        jstring request_headers_blob) {
    (void) clazz;
    if (url == nullptr) {
        return nullptr;
    }

    const char *url_chars = env->GetStringUTFChars(url, nullptr);
    const char *headers_chars =
            request_headers_blob != nullptr
            ? env->GetStringUTFChars(request_headers_blob, nullptr)
            : nullptr;

    AVFormatContext *format_context = nullptr;
    avformat_network_init();

    std::vector<std::string> probe_urls;
    probe_urls.reserve(3);
    auto add_unique_probe_url = [&probe_urls](const std::string &candidate) {
        if (candidate.empty()) {
            return;
        }
        if (std::find(probe_urls.begin(), probe_urls.end(), candidate) == probe_urls.end()) {
            probe_urls.push_back(candidate);
        }
    };

    add_unique_probe_url(url_chars);
    add_unique_probe_url(extractEmbeddedResolveUrl(url_chars));
    for (size_t i = 0; i < probe_urls.size(); ++i) {
        add_unique_probe_url(toHttpFallbackUrl(probe_urls[i]));
    }

    int open_result = -1;
    for (const auto &probe_url : probe_urls) {
        open_result = openInputForProbe(&format_context, probe_url.c_str(), headers_chars);
        if (open_result >= 0 && format_context != nullptr) {
            break;
        }
        format_context = nullptr;
    }

    std::string status = "failed";
    std::string error = "ffmpeg_probe_failed";
    std::string video_codec = "unknown";
    std::string audio_codec = "unknown";
    std::string hdr_type = "unknown";
    int profile = -2;
    int video_stream_index = -1;

    if (open_result >= 0 && format_context != nullptr) {
        const int info_result = avformat_find_stream_info(format_context, nullptr);
        if (info_result >= 0) {
            video_stream_index = av_find_best_stream(
                    format_context,
                    AVMEDIA_TYPE_VIDEO,
                    -1,
                    -1,
                    nullptr,
                    0);
            if (video_stream_index >= 0) {
                AVStream *video_stream = format_context->streams[video_stream_index];
                if (video_stream != nullptr && video_stream->codecpar != nullptr) {
                    const char *codec_name = avcodec_get_name(video_stream->codecpar->codec_id);
                    if (codec_name != nullptr && codec_name[0] != '\0') {
                        video_codec = codec_name;
                    }

                    const AVPacketSideData *dovi_side_data = av_packet_side_data_get(
                            video_stream->codecpar->coded_side_data,
                            video_stream->codecpar->nb_coded_side_data,
                            AV_PKT_DATA_DOVI_CONF);
                    if (dovi_side_data != nullptr &&
                        dovi_side_data->size >= static_cast<int>(sizeof(AVDOVIDecoderConfigurationRecord))) {
                        const auto *dovi =
                                reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(dovi_side_data->data);
                        profile = static_cast<int>(dovi->dv_profile);
                        hdr_type = "dolbyvision";
                    } else if (video_stream->codecpar->color_trc == AVCOL_TRC_SMPTE2084) {
                        hdr_type = "hdr10";
                    } else if (video_stream->codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67) {
                        hdr_type = "hlg";
                    }
                }
            }

            const int audio_stream_index = av_find_best_stream(
                    format_context,
                    AVMEDIA_TYPE_AUDIO,
                    -1,
                    -1,
                    nullptr,
                    0);
            if (audio_stream_index >= 0) {
                AVStream *audio_stream = format_context->streams[audio_stream_index];
                if (audio_stream != nullptr && audio_stream->codecpar != nullptr) {
                    const char *codec_name = avcodec_get_name(audio_stream->codecpar->codec_id);
                    if (codec_name != nullptr && codec_name[0] != '\0') {
                        audio_codec = codec_name;
                    }
                }
            }

            if (profile >= 0) {
                status = "detected";
                error.clear();
            } else if (video_stream_index >= 0) {
                status = "not_dolby_vision";
                error.clear();
            }
        }
    }

    if (format_context != nullptr) {
        avformat_close_input(&format_context);
    }
    if (headers_chars != nullptr) {
        env->ReleaseStringUTFChars(request_headers_blob, headers_chars);
    }
    env->ReleaseStringUTFChars(url, url_chars);

    std::string blob = "status=" + sanitizeProbeValue(status) +
                       ";video=" + sanitizeProbeValue(video_codec) +
                       ";audio=" + sanitizeProbeValue(audio_codec) +
                       ";hdr=" + sanitizeProbeValue(hdr_type);
    if (profile >= 0) {
        blob += ";profile=" + std::to_string(profile);
    }
    if (!error.empty()) {
        blob += ";error=" + sanitizeProbeValue(error);
    }
    return env->NewStringUTF(blob.c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegLibrary_ffmpegProbeDolbyVisionStreamMetadataJson(
        JNIEnv *env,
        jclass clazz,
        jstring url,
        jstring request_headers_blob) {
    (void) clazz;
    if (url == nullptr) {
        return nullptr;
    }

    const char *url_chars = env->GetStringUTFChars(url, nullptr);
    const char *headers_chars =
            request_headers_blob != nullptr
            ? env->GetStringUTFChars(request_headers_blob, nullptr)
            : nullptr;

    AVFormatContext *format_context = nullptr;
    avformat_network_init();

    std::vector<std::string> probe_urls;
    probe_urls.reserve(3);
    auto add_unique_probe_url = [&probe_urls](const std::string &candidate) {
        if (candidate.empty()) {
            return;
        }
        if (std::find(probe_urls.begin(), probe_urls.end(), candidate) == probe_urls.end()) {
            probe_urls.push_back(candidate);
        }
    };

    add_unique_probe_url(url_chars);
    add_unique_probe_url(extractEmbeddedResolveUrl(url_chars));
    for (size_t i = 0; i < probe_urls.size(); ++i) {
        add_unique_probe_url(toHttpFallbackUrl(probe_urls[i]));
    }

    int open_result = -1;
    for (const auto &probe_url : probe_urls) {
        open_result = openInputForProbe(&format_context, probe_url.c_str(), headers_chars);
        if (open_result >= 0 && format_context != nullptr) {
            break;
        }
        format_context = nullptr;
    }

    std::string json = "{\"streams\":[";
    bool first_stream = true;

    if (open_result >= 0 && format_context != nullptr &&
        avformat_find_stream_info(format_context, nullptr) >= 0) {
        for (unsigned int i = 0; i < format_context->nb_streams; ++i) {
            AVStream *stream = format_context->streams[i];
            if (stream == nullptr || stream->codecpar == nullptr) {
                continue;
            }

            const AVCodecParameters *codecpar = stream->codecpar;
            const char *codec_type = av_get_media_type_string(codecpar->codec_type);
            const char *codec_name = avcodec_get_name(codecpar->codec_id);
            if (codec_type == nullptr || codec_name == nullptr || codec_name[0] == '\0') {
                continue;
            }

            bool has_hdr10_plus = av_packet_side_data_get(
                    codecpar->coded_side_data,
                    codecpar->nb_coded_side_data,
                    AV_PKT_DATA_DYNAMIC_HDR10_PLUS) != nullptr;

            int dv_profile = -1;
            const AVPacketSideData *dovi_side_data = av_packet_side_data_get(
                    codecpar->coded_side_data,
                    codecpar->nb_coded_side_data,
                    AV_PKT_DATA_DOVI_CONF);
            if (dovi_side_data != nullptr &&
                dovi_side_data->size >= static_cast<int>(sizeof(AVDOVIDecoderConfigurationRecord))) {
                const auto *dovi =
                        reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(dovi_side_data->data);
                dv_profile = static_cast<int>(dovi->dv_profile);
            }

            if (!first_stream) {
                json += ",";
            }
            first_stream = false;
            json += "{";
            json += "\"codec_type\":\"" + escapeJsonString(codec_type) + "\"";
            json += ",\"codec_name\":\"" + escapeJsonString(codec_name) + "\"";
            if (codecpar->color_trc != AVCOL_TRC_UNSPECIFIED) {
                const char *color_transfer = av_color_transfer_name(codecpar->color_trc);
                if (color_transfer != nullptr && color_transfer[0] != '\0') {
                    json += ",\"color_transfer\":\"" + escapeJsonString(color_transfer) + "\"";
                }
            }
            if (codecpar->color_primaries != AVCOL_PRI_UNSPECIFIED) {
                const char *color_primaries = av_color_primaries_name(codecpar->color_primaries);
                if (color_primaries != nullptr && color_primaries[0] != '\0') {
                    json += ",\"color_primaries\":\"" + escapeJsonString(color_primaries) + "\"";
                }
            }
            if (dv_profile >= 0) {
                json += ",\"dv_profile\":" + std::to_string(dv_profile);
            }
            if (has_hdr10_plus) {
                json += ",\"hdr10_plus\":true";
            }
            json += "}";
        }
    }

    json += "]}";

    if (format_context != nullptr) {
        avformat_close_input(&format_context);
    }
    if (headers_chars != nullptr) {
        env->ReleaseStringUTFChars(request_headers_blob, headers_chars);
    }
    env->ReleaseStringUTFChars(url, url_chars);
    return env->NewStringUTF(json.c_str());
}
