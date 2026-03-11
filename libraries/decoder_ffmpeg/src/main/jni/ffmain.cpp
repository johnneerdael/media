#include <jni.h>
extern "C" {
#include "libavcodec/version.h"
#include "libavcodec/defs.h"
#include "libavutil/hwcontext.h"
}
#include "config.h"
#include "config_components.h"
#include "ffcommon.h"
#include <cstddef>
#include <cstdint>

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
