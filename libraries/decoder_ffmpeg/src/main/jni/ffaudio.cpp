
#include <android/log.h>
#include <jni.h>
#include <cstdlib>
#include <android/native_window_jni.h>
#include <algorithm>
#include "ffcommon.h"

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <cstdint>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

# define LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
# define LOGI(...)  __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Output format corresponding to AudioFormat.ENCODING_PCM_16BIT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_16BIT = AV_SAMPLE_FMT_S16;
// Output format corresponding to AudioFormat.ENCODING_PCM_FLOAT.
static const AVSampleFormat OUTPUT_FORMAT_PCM_FLOAT = AV_SAMPLE_FMT_FLT;

static const int AUDIO_DECODER_ERROR_INVALID_DATA = -1;
static const int AUDIO_DECODER_ERROR_OTHER = -2;

static jmethodID growOutputBufferMethod;
struct GrowOutputBufferCallback;

struct AudioDecoderContext {
    AVCodecContext *codecContext;
    AVCodecParserContext *parserContext;
    SwrContext *resampleContext;
    AVChannelLayout resampleInChLayout;
    int resampleInSampleRate;
    AVSampleFormat resampleInSampleFormat;
};


/**
 * Allocates and opens a new AVCodecContext for the specified codec, passing the
 * provided extraData as initialization data for the decoder if it is non-NULL.
 * Returns the created context.
 */
AudioDecoderContext *createContext(JNIEnv *env, AVCodec *codec, jbyteArray extraData,
                                   jboolean outputFloat, jint rawSampleRate,
                                   jint rawChannelCount);

/**
 * Decodes the packet into the output buffer, returning the number of bytes
 * written, or a negative AUDIO_DECODER_ERROR constant value in the case of an
 * error.
 */
int decodePacket(AudioDecoderContext *decoderContext, uint8_t *inputBuffer, int inputSize,
                 uint8_t *outputBuffer, int outputSize, GrowOutputBufferCallback growBuffer);

/**
 * Transforms ffmpeg AVERROR into a negative AUDIO_DECODER_ERROR constant value.
 */
int transformError(int errorNumber);

struct GrowOutputBufferCallback {
    uint8_t *operator()(int requiredSize) const;

    JNIEnv *env;
    jobject thiz;
    jobject decoderOutputBuffer;
};

uint8_t *GrowOutputBufferCallback::operator()(int requiredSize) const {
    jobject newOutputData = env->CallObjectMethod(thiz, growOutputBufferMethod, decoderOutputBuffer, requiredSize);
    if (env->ExceptionCheck()) {
        LOGE("growOutputBuffer() failed");
        env->ExceptionDescribe();
        return nullptr;
    }
    return static_cast<uint8_t *>(env->GetDirectBufferAddress(newOutputData));
}

AudioDecoderContext *createContext(JNIEnv *env, AVCodec *codec, jbyteArray extraData,
                                   jboolean outputFloat, jint rawSampleRate,
                                   jint rawChannelCount) {
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context) {
        LOGE("Failed to allocate context.");
        return nullptr;
    }
    context->request_sample_fmt =
            outputFloat ? OUTPUT_FORMAT_PCM_FLOAT : OUTPUT_FORMAT_PCM_16BIT;
    if (extraData) {
        jsize size = env->GetArrayLength(extraData);
        context->extradata_size = size;
        context->extradata =
                (uint8_t *) av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!context->extradata) {
            LOGE("Failed to allocate extra data.");
            releaseContext(context);
            return nullptr;
        }
        env->GetByteArrayRegion(extraData, 0, size, (jbyte *) context->extradata);
    }
    if (context->codec_id == AV_CODEC_ID_PCM_MULAW ||
        context->codec_id == AV_CODEC_ID_PCM_ALAW) {
        context->sample_rate = rawSampleRate;
        context->ch_layout.nb_channels = rawChannelCount;
        av_channel_layout_default(&context->ch_layout, rawChannelCount);
    }
    context->err_recognition = AV_EF_IGNORE_ERR;
    int result = avcodec_open2(context, codec, nullptr);
    if (result < 0) {
        logError("avcodec_open2", result);
        releaseContext(context);
        return nullptr;
    }

    AVCodecParserContext *parserContext = nullptr;
    if (context->codec_id == AV_CODEC_ID_DTS) {
        parserContext = av_parser_init(context->codec_id);
        if (!parserContext) {
            LOGE("Failed to allocate DTS parser.");
            releaseContext(context);
            return nullptr;
        }
    }

    AVChannelLayout emptyLayout;
    memset(&emptyLayout, 0, sizeof(emptyLayout));
    auto *decoderContext =
            new AudioDecoderContext{
                    context,
                    parserContext,
                    nullptr,
                    emptyLayout,
                    0,
                    AV_SAMPLE_FMT_NONE};
    return decoderContext;
}

int receiveFrames(AudioDecoderContext *decoderContext, uint8_t *&outputBuffer, int &outputSize, int &outSize,
                  GrowOutputBufferCallback growBuffer) {
    AVCodecContext *context = decoderContext->codecContext;
    int result = 0;
    while (true) {
        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            LOGE("Failed to allocate output frame.");
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        result = avcodec_receive_frame(context, frame);
        if (result) {
            av_frame_free(&frame);
            if (result == AVERROR(EAGAIN)) {
                break;
            }
            logError("avcodec_receive_frame", result);
            return transformError(result);
        }

        // Resample output.
        AVSampleFormat sampleFormat = static_cast<AVSampleFormat>(frame->format);
        int channelCount = frame->ch_layout.nb_channels;
        int sampleRate = frame->sample_rate;
        int sampleCount = frame->nb_samples;
        SwrContext *resampleContext = decoderContext->resampleContext;
        bool needsResampleReconfigure =
                resampleContext == nullptr ||
                decoderContext->resampleInSampleFormat != sampleFormat ||
                decoderContext->resampleInSampleRate != sampleRate ||
                av_channel_layout_compare(&decoderContext->resampleInChLayout, &frame->ch_layout) != 0;
        if (needsResampleReconfigure) {
            if (resampleContext) {
                swr_free(&resampleContext);
                decoderContext->resampleContext = nullptr;
            }
            av_channel_layout_uninit(&decoderContext->resampleInChLayout);
            resampleContext = swr_alloc();
            if (!resampleContext) {
                LOGE("Failed to allocate resample context.");
                av_frame_free(&frame);
                return AUDIO_DECODER_ERROR_OTHER;
            }
            result = av_opt_set_chlayout(resampleContext, "in_chlayout", &frame->ch_layout, 0);
            if (result < 0) {
                logError("av_opt_set_chlayout(in_chlayout)", result);
                swr_free(&resampleContext);
                av_frame_free(&frame);
                return transformError(result);
            }
            result = av_opt_set_chlayout(resampleContext, "out_chlayout", &frame->ch_layout, 0);
            if (result < 0) {
                logError("av_opt_set_chlayout(out_chlayout)", result);
                swr_free(&resampleContext);
                av_frame_free(&frame);
                return transformError(result);
            }
            av_opt_set_int(resampleContext, "in_sample_rate", sampleRate, 0);
            av_opt_set_int(resampleContext, "out_sample_rate", sampleRate, 0);
            av_opt_set_int(resampleContext, "in_sample_fmt", sampleFormat, 0);
            // The output format is always the requested format.
            av_opt_set_int(resampleContext, "out_sample_fmt",
                           context->request_sample_fmt, 0);
            result = swr_init(resampleContext);
            if (result < 0) {
                logError("swr_init", result);
                swr_free(&resampleContext);
                av_frame_free(&frame);
                return transformError(result);
            }
            result = av_channel_layout_copy(&decoderContext->resampleInChLayout, &frame->ch_layout);
            if (result < 0) {
                logError("av_channel_layout_copy", result);
                swr_free(&resampleContext);
                av_frame_free(&frame);
                return transformError(result);
            }
            decoderContext->resampleContext = resampleContext;
            decoderContext->resampleInSampleRate = sampleRate;
            decoderContext->resampleInSampleFormat = sampleFormat;
            if (ffmpegIsExperimentalIecDebugLoggingEnabled()) {
                char layoutDescription[128];
                av_channel_layout_describe(&frame->ch_layout, layoutDescription, sizeof(layoutDescription));
                LOGI(
                        "IEC_FFMPEG: resample-config codec=%d sampleRate=%d channels=%d frameFmt=%d reqFmt=%d layout=%s",
                        context->codec_id,
                        sampleRate,
                        channelCount,
                        sampleFormat,
                        context->request_sample_fmt,
                        layoutDescription);
            }
        }
        int outSampleSize = av_get_bytes_per_sample(context->request_sample_fmt);
        int outSamples = swr_get_out_samples(resampleContext, sampleCount);
        int bufferOutSize = outSampleSize * channelCount * outSamples;
        if (outSize + bufferOutSize > outputSize) {
            LOGD(
                    "Output buffer size (%d) too small for output data (%d), "
                    "reallocating buffer.",
                    outputSize, outSize + bufferOutSize);
            outputSize = outSize + bufferOutSize;
            outputBuffer = growBuffer(outputSize);
            if (!outputBuffer) {
                LOGE("Failed to reallocate output buffer.");
                av_frame_free(&frame);
                return AUDIO_DECODER_ERROR_OTHER;
            }
        }
        uint8_t *writePtr = outputBuffer;
        result = swr_convert(resampleContext, &writePtr, outSamples,
                             (const uint8_t **) frame->data, frame->nb_samples);
        av_frame_free(&frame);
        if (result < 0) {
            logError("swr_convert", result);
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        int convertedOutSize = outSampleSize * channelCount * result;
        if (ffmpegIsExperimentalIecDebugLoggingEnabled()) {
            LOGI(
                    "IEC_FFMPEG: frame codec=%d inSamples=%d outSamples=%d outBytes=%d sampleRate=%d channels=%d",
                    context->codec_id,
                    sampleCount,
                    result,
                    convertedOutSize,
                    sampleRate,
                    channelCount);
        }
        int available = swr_get_out_samples(resampleContext, 0);
        if (available != 0) {
            LOGE("Expected no samples remaining after resampling, but found %d.",
                 available);
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        outputBuffer += convertedOutSize;
        outSize += convertedOutSize;
    }
    return outSize;
}

int sendPacket(AudioDecoderContext *decoderContext, AVPacket *packet, uint8_t *&outputBuffer, int &outputSize,
               int &outSize, GrowOutputBufferCallback growBuffer) {
    AVCodecContext *context = decoderContext->codecContext;
    int result = avcodec_send_packet(context, packet);
    if (result) {
        logError("avcodec_send_packet", result);
        return transformError(result);
    }
    return receiveFrames(decoderContext, outputBuffer, outputSize, outSize, growBuffer);
}

int decodePacket(AudioDecoderContext *decoderContext, uint8_t *inputBuffer, int inputSize,
                 uint8_t *outputBuffer, int outputSize, GrowOutputBufferCallback growBuffer) {
    AVCodecContext *context = decoderContext->codecContext;
    int outSize = 0;

    if (!decoderContext->parserContext) {
        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            LOGE("audio_decoder_decode_frame: av_packet_alloc failed");
            return AUDIO_DECODER_ERROR_OTHER;
        }
        packet->data = inputBuffer;
        packet->size = inputSize;
        int result = sendPacket(decoderContext, packet, outputBuffer, outputSize, outSize, growBuffer);
        av_packet_free(&packet);
        return result;
    }

    while (inputSize > 0) {
        uint8_t *parsedData = nullptr;
        int parsedSize = 0;
        int consumed =
                av_parser_parse2(
                        decoderContext->parserContext,
                        context,
                        &parsedData,
                        &parsedSize,
                        inputBuffer,
                        inputSize,
                        AV_NOPTS_VALUE,
                        AV_NOPTS_VALUE,
                        0);
        if (consumed < 0) {
            logError("av_parser_parse2", consumed);
            return transformError(consumed);
        }
        if (consumed == 0 && parsedSize == 0) {
            LOGE("DTS parser made no progress.");
            return AUDIO_DECODER_ERROR_INVALID_DATA;
        }
        if (ffmpegIsExperimentalIecDebugLoggingEnabled()) {
            LOGI(
                    "IEC_FFMPEG: dts-parser consumed=%d parsedSize=%d remaining=%d",
                    consumed,
                    parsedSize,
                    inputSize - consumed);
        }

        inputBuffer += consumed;
        inputSize -= consumed;

        if (parsedSize == 0) {
            continue;
        }

        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            LOGE("audio_decoder_decode_frame: av_packet_alloc failed");
            return AUDIO_DECODER_ERROR_OTHER;
        }
        packet->data = parsedData;
        packet->size = parsedSize;
        int result = sendPacket(decoderContext, packet, outputBuffer, outputSize, outSize, growBuffer);
        av_packet_free(&packet);
        if (result < 0) {
            return result;
        }
    }

    return outSize;
}

int transformError(int errorNumber) {
    return errorNumber == AVERROR_INVALIDDATA ? AUDIO_DECODER_ERROR_INVALID_DATA
                                              : AUDIO_DECODER_ERROR_OTHER;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_ffmpegInitialize(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jstring codec_name,
                                                                        jbyteArray extra_data,
                                                                        jboolean output_float,
                                                                        jint raw_sample_rate,
                                                                        jint raw_channel_count) {
    AVCodec *codec = getCodecByName(env, codec_name);
    if (!codec) {
        LOGE("Codec not found.");
        return 0L;
    }
    jclass clazz = env->FindClass("androidx/media3/decoder/ffmpeg/FfmpegAudioDecoder");
    growOutputBufferMethod = env->GetMethodID(clazz, "growOutputBuffer","(Landroidx/media3/decoder/SimpleDecoderOutputBuffer;I)Ljava/nio/ByteBuffer;");
    return (jlong) createContext(env, codec, extra_data, output_float, raw_sample_rate,
                                 raw_channel_count);
}

extern "C"
JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_ffmpegDecode(JNIEnv *env,
                                                                    jobject thiz,
                                                                    jlong context,
                                                                    jobject input_data,
                                                                    jint input_size,
                                                                    jobject decoderOutputBuffer,
                                                                    jobject output_data,
                                                                    jint output_size) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    if (!input_data || !decoderOutputBuffer || !output_data) {
        LOGE("Input and output buffers must be non-NULL.");
        return -1;
    }
    if (input_size < 0) {
        LOGE("Invalid input buffer size: %d.", input_size);
        return -1;
    }
    if (output_size < 0) {
        LOGE("Invalid output buffer length: %d", output_size);
        return -1;
    }
    auto *inputBuffer = (uint8_t *) env->GetDirectBufferAddress(input_data);
    auto *outputBuffer = (uint8_t *) env->GetDirectBufferAddress(output_data);
    return decodePacket((AudioDecoderContext *) context, inputBuffer, input_size, outputBuffer,
                        output_size, GrowOutputBufferCallback{env, thiz, decoderOutputBuffer});
}

extern "C"
JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_ffmpegGetChannelCount(
        JNIEnv *env, jobject thiz, jlong context) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    return ((AudioDecoderContext *) context)->codecContext->ch_layout.nb_channels;
}

extern "C"
JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_ffmpegGetSampleRate(JNIEnv *env,
                                                                           jobject thiz,
                                                                           jlong context) {
    if (!context) {
        LOGE("Context must be non-NULL.");
        return -1;
    }
    return ((AudioDecoderContext *) context)->codecContext->sample_rate;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_ffmpegReset(JNIEnv *env,
                                                                   jobject thiz,
                                                                   jlong jContext,
                                                                   jbyteArray extra_data) {
    auto *decoderContext = (AudioDecoderContext *) jContext;
    if (!decoderContext || !decoderContext->codecContext) {
        LOGE("Tried to reset without a context.");
        return 0L;
    }

    AVCodecContext *context = decoderContext->codecContext;
    AVCodecID codecId = context->codec_id;
    if (codecId == AV_CODEC_ID_TRUEHD) {
        // Release and recreate the context if the codec is TrueHD.
        // TODO: Figure out why flushing doesn't work for this codec.
        auto outputFloat =
                (jboolean) (context->request_sample_fmt == OUTPUT_FORMAT_PCM_FLOAT);
        if (decoderContext->parserContext) {
            av_parser_close(decoderContext->parserContext);
        }
        if (decoderContext->resampleContext) {
            swr_free(&decoderContext->resampleContext);
        }
        av_channel_layout_uninit(&decoderContext->resampleInChLayout);
        releaseContext(context);
        delete decoderContext;
        auto *codec = const_cast<AVCodec *>(avcodec_find_decoder(codecId));
        if (!codec) {
            LOGE("Unexpected error finding codec %d.", codecId);
            return 0L;
        }
        return (jlong) createContext(env, codec, extra_data, outputFloat,
                /* rawSampleRate= */ -1,
                /* rawChannelCount= */ -1);
    }

    avcodec_flush_buffers(context);
    if (decoderContext->parserContext) {
        av_parser_close(decoderContext->parserContext);
        decoderContext->parserContext = av_parser_init(codecId);
        if (!decoderContext->parserContext) {
            LOGE("Failed to reset DTS parser.");
            releaseContext(context);
            delete decoderContext;
            return 0L;
        }
    }
    if (decoderContext->resampleContext) {
        swr_free(&decoderContext->resampleContext);
    }
    av_channel_layout_uninit(&decoderContext->resampleInChLayout);
    decoderContext->resampleInSampleRate = 0;
    decoderContext->resampleInSampleFormat = AV_SAMPLE_FMT_NONE;
    return (jlong) decoderContext;
}

extern "C"
JNIEXPORT void JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegAudioDecoder_ffmpegRelease(JNIEnv *env,
                                                                     jobject thiz,
                                                                     jlong context) {
    if (context) {
        auto *decoderContext = (AudioDecoderContext *) context;
        if (decoderContext->parserContext) {
            av_parser_close(decoderContext->parserContext);
        }
        if (decoderContext->resampleContext) {
            swr_free(&decoderContext->resampleContext);
        }
        av_channel_layout_uninit(&decoderContext->resampleInChLayout);
        releaseContext(decoderContext->codecContext);
        delete decoderContext;
    }
}
