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
#include "libavutil/time.h"
}
#include "config.h"
#include "config_components.h"
#include "ffcommon.h"
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <atomic>
#include <string>

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

std::string probeUrlForLog(const std::string &url) {
    std::string safe = url;
    const auto q = safe.find('?');
    if (q != std::string::npos) {
        safe = safe.substr(0, q) + "?...";
    }
    if (safe.size() > 160) {
        safe = safe.substr(0, 160) + "...";
    }
    return safe;
}

int openInputForProbeTimed(
        AVFormatContext **formatContext,
        const char *urlChars,
        const char *headersChars,
        const char *probeName,
        int attemptIndex) {
    const int64_t start_us = av_gettime_relative();
    const int result = openInputForProbe(formatContext, urlChars, headersChars);
    const int64_t elapsed_ms = (av_gettime_relative() - start_us) / 1000;
    const std::string safe = urlChars != nullptr ? probeUrlForLog(urlChars) : std::string();
    __android_log_print(
            ANDROID_LOG_INFO, LOG_TAG,
            "PROBE_OPEN: name=%s attempt=%d result=%d elapsedMs=%lld url=%s",
            probeName != nullptr ? probeName : "unknown",
            attemptIndex,
            result,
            static_cast<long long>(elapsed_ms),
            safe.c_str());
    return result;
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

std::string rationalToJsonString(AVRational rational) {
    if (rational.num <= 0 || rational.den <= 0) {
        return "";
    }
    return std::to_string(rational.num) + "/" + std::to_string(rational.den);
}

}  // namespace

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

    // Proxy-URL filtering lives in Kotlin (FfmpegStreamMetadataProbe.resolveDirectProbeUrl).
    // The native side never rewrites the URL; a single probe attempt keeps the native
    // surface strict and makes misrouted calls visible via PROBE_OPEN logs.
    const int open_result = openInputForProbeTimed(
            &format_context,
            url_chars,
            headers_chars,
            "StreamMetadataJson",
            /* attemptIndex */ 0);
    if (open_result < 0 || format_context == nullptr) {
        format_context = nullptr;
    }

    std::string json = "{\"streams\":[";
    bool first_stream = true;

    int find_info_result = -1;
    if (open_result >= 0 && format_context != nullptr) {
        const int64_t find_info_start_us = av_gettime_relative();
        find_info_result = avformat_find_stream_info(format_context, nullptr);
        const int64_t find_info_elapsed_ms =
                (av_gettime_relative() - find_info_start_us) / 1000;
        __android_log_print(
                ANDROID_LOG_INFO, LOG_TAG,
                "PROBE_FIND_INFO: name=StreamMetadataJson result=%d elapsedMs=%lld",
                find_info_result,
                static_cast<long long>(find_info_elapsed_ms));
    }
    if (open_result >= 0 && format_context != nullptr && find_info_result >= 0) {
        for (unsigned int i = 0; i < format_context->nb_streams; ++i) {
            AVStream *stream = format_context->streams[i];
            if (stream == nullptr || stream->codecpar == nullptr) {
                continue;
            }

            const AVCodecParameters *codecpar = stream->codecpar;
            if (codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
                continue;
            }
            if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                static_cast<int>(i) != av_find_best_stream(
                        format_context,
                        AVMEDIA_TYPE_VIDEO,
                        -1,
                        -1,
                        nullptr,
                        0)) {
                continue;
            }
            const char *codec_type = av_get_media_type_string(codecpar->codec_type);
            const char *codec_name = avcodec_get_name(codecpar->codec_id);
            if (codec_type == nullptr || codec_name == nullptr || codec_name[0] == '\0') {
                continue;
            }

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
            json += "\"index\":" + std::to_string(stream->index);
            json += ",\"codec_type\":\"" + escapeJsonString(codec_type) + "\"";
            json += ",\"codec_name\":\"" + escapeJsonString(codec_name) + "\"";
            if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (codecpar->width > 0) {
                    json += ",\"width\":" + std::to_string(codecpar->width);
                }
                if (codecpar->height > 0) {
                    json += ",\"height\":" + std::to_string(codecpar->height);
                }

                const std::string avg_frame_rate = rationalToJsonString(stream->avg_frame_rate);
                if (!avg_frame_rate.empty()) {
                    json += ",\"avg_frame_rate\":\"" + escapeJsonString(avg_frame_rate) + "\"";
                }

                const std::string r_frame_rate = rationalToJsonString(stream->r_frame_rate);
                if (!r_frame_rate.empty()) {
                    json += ",\"r_frame_rate\":\"" + escapeJsonString(r_frame_rate) + "\"";
                }
            }
            if (dv_profile >= 0) {
                json += ",\"side_data_list\":[{\"side_data_type\":\"DOVI configuration record\",\"dv_profile\":" +
                        std::to_string(dv_profile) + "}]";
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
