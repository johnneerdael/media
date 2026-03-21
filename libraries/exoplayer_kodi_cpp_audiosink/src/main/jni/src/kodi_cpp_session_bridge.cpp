#include <jni.h>

#include <cstdint>
#include <string>
#include <vector>

#include "KodiActiveAEEngine.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAESettings.h"

namespace {

using androidx_media3::KodiActiveAEEngine;

KodiActiveAEEngine* AsSession(jlong native_handle)
{
  return reinterpret_cast<KodiActiveAEEngine*>(native_handle);
}

jint GetIntField(JNIEnv* env, jobject obj, jclass clazz, const char* name)
{
  return env->GetIntField(obj, env->GetFieldID(clazz, name, "I"));
}

jfloat GetFloatField(JNIEnv* env, jobject obj, jclass clazz, const char* name)
{
  return env->GetFloatField(obj, env->GetFieldID(clazz, name, "F"));
}

jboolean GetBooleanField(JNIEnv* env, jobject obj, jclass clazz, const char* name)
{
  return env->GetBooleanField(obj, env->GetFieldID(clazz, name, "Z"));
}

std::string GetStringField(JNIEnv* env, jobject obj, jclass clazz, const char* name)
{
  jstring value =
      static_cast<jstring>(env->GetObjectField(obj, env->GetFieldID(clazz, name, "Ljava/lang/String;")));
  if (value == nullptr)
    return std::string();
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string result = chars != nullptr ? chars : "";
  if (chars != nullptr)
    env->ReleaseStringUTFChars(value, chars);
  env->DeleteLocalRef(value);
  return result;
}

jobject BuildNativeBurst(JNIEnv* env, const std::vector<uint8_t>& bytes, int64_t ptsUs)
{
  jclass clazz =
      env->FindClass("androidx/media3/exoplayer/audio/kodi/validation/TransportValidationNativeBurst");
  if (clazz == nullptr)
    return nullptr;
  jmethodID ctor = env->GetMethodID(clazz, "<init>", "(J[B)V");
  if (ctor == nullptr)
  {
    env->DeleteLocalRef(clazz);
    return nullptr;
  }
  jbyteArray byteArray = env->NewByteArray(static_cast<jsize>(bytes.size()));
  if (byteArray == nullptr)
  {
    env->DeleteLocalRef(clazz);
    return nullptr;
  }
  if (!bytes.empty())
  {
    env->SetByteArrayRegion(byteArray,
                            0,
                            static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte*>(bytes.data()));
  }
  jobject result = env->NewObject(clazz, ctor, static_cast<jlong>(ptsUs), byteArray);
  env->DeleteLocalRef(byteArray);
  env->DeleteLocalRef(clazz);
  return result;
}

ActiveAE::CActiveAEMediaSettings ParseConfig(JNIEnv* env, jobject config_obj)
{
  jclass clazz = env->GetObjectClass(config_obj);
  ActiveAE::CActiveAEMediaSettings config;
  config.mimeKind = ActiveAE::CActiveAESettings::MimeKindFromMediaMimeType(
      GetStringField(env, config_obj, clazz, "sampleMimeType"));
  config.sampleRate = GetIntField(env, config_obj, clazz, "sampleRate");
  config.channelCount = GetIntField(env, config_obj, clazz, "channelCount");
  config.pcmEncoding = GetIntField(env, config_obj, clazz, "pcmEncoding");
  config.preferredDevice = GetStringField(env, config_obj, clazz, "preferredDevice");
  config.volume = GetFloatField(env, config_obj, clazz, "volume");
  config.superviseAudioDelay =
      GetBooleanField(env, config_obj, clazz, "superviseAudioDelay") == JNI_TRUE;
  config.iecVerboseLogging =
      GetBooleanField(env, config_obj, clazz, "iecVerboseLogging") == JNI_TRUE;
  env->DeleteLocalRef(clazz);
  return config;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nCreate(JNIEnv* env, jclass clazz)
{
  (void)env;
  (void)clazz;
  auto* engine = new KodiActiveAEEngine();
  return reinterpret_cast<jlong>(engine);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nConfigure(
    JNIEnv* env, jclass clazz, jlong native_handle, jobject config_obj)
{
  (void)clazz;
  return AsSession(native_handle)->Configure(ParseConfig(env, config_obj)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nWrite(
    JNIEnv* env,
    jclass clazz,
    jlong native_handle,
    jobject buffer,
    jint offset,
    jint size,
    jlong presentation_time_us,
    jint encoded_access_unit_count)
{
  (void)clazz;
  auto* data = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
  if (data == nullptr)
    return 0;
  return AsSession(native_handle)
      ->Write(data + offset, size, static_cast<int64_t>(presentation_time_us), encoded_access_unit_count);
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nConsumeLastWriteOutputBytes(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->ConsumeLastWriteOutputBytes());
}

extern "C" JNIEXPORT jobject JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nConsumeNextCapturedPackedBurst(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)clazz;
  std::vector<uint8_t> bytes;
  int64_t ptsUs = 0;
  if (!AsSession(native_handle)->ConsumeNextCapturedPackedBurst(bytes, ptsUs))
    return nullptr;
  return BuildNativeBurst(env, bytes, ptsUs);
}

extern "C" JNIEXPORT jobject JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nConsumeNextCapturedAudioTrackWriteBurst(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)clazz;
  std::vector<uint8_t> bytes;
  int64_t ptsUs = 0;
  if (!AsSession(native_handle)->ConsumeNextCapturedAudioTrackWriteBurst(bytes, ptsUs))
    return nullptr;
  return BuildNativeBurst(env, bytes, ptsUs);
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nConsumeLastWriteErrorCode(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->ConsumeLastWriteErrorCode());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nIsReleasePending(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return AsSession(native_handle)->IsReleasePending() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nPlay(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Play();
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nPause(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Pause();
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nFlush(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Flush();
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nDrain(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  AsSession(native_handle)->Drain();
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nHandleDiscontinuity(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  AsSession(native_handle)->HandleDiscontinuity();
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nSetVolume(
    JNIEnv* env, jclass clazz, jlong native_handle, jfloat volume)
{
  (void)env;
  (void)clazz;
  AsSession(native_handle)->SetVolume(volume);
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nSetHostClockUs(
    JNIEnv* env, jclass clazz, jlong native_handle, jlong host_clock_us)
{
  (void)env;
  (void)clazz;
  AsSession(native_handle)->SetHostClockUs(static_cast<int64_t>(host_clock_us));
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nSetHostClockSpeed(
    JNIEnv* env, jclass clazz, jlong native_handle, jdouble speed)
{
  (void)env;
  (void)clazz;
  AsSession(native_handle)->SetHostClockSpeed(static_cast<double>(speed));
}

extern "C" JNIEXPORT jlong JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetCurrentPositionUs(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jlong>(AsSession(native_handle)->GetCurrentPositionUs());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nHasPendingData(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return AsSession(native_handle)->HasPendingData() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nIsEnded(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return AsSession(native_handle)->IsEnded() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetBufferSizeUs(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jlong>(AsSession(native_handle)->GetBufferSizeUs());
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetOutputSampleRate(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->GetOutputSampleRate());
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetOutputChannelCount(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->GetOutputChannelCount());
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetOutputEncoding(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->GetOutputEncoding());
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetOutputAudioTrackState(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->GetOutputAudioTrackState());
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetOutputUnderrunCount(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->GetOutputUnderrunCount());
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetOutputRestartCount(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->GetOutputRestartCount());
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nGetDirectPlaybackSupportState(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  return static_cast<jint>(AsSession(native_handle)->GetDirectPlaybackSupportState());
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_exoplayer_audio_kodi_KodiNativeAudioSink_nRelease(
    JNIEnv* env, jclass clazz, jlong native_handle)
{
  (void)env;
  (void)clazz;
  delete AsSession(native_handle);
}
