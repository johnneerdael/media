#include "kodi_cpp_jni_support.h"

#include "androidjni/AudioAttributes.h"
#include "androidjni/AudioFormat.h"
#include "androidjni/AudioManager.h"
#include "androidjni/AudioTimestamp.h"
#include "androidjni/AudioTrack.h"
#include "androidjni/ByteBuffer.h"
#include "utils/log.h"

#include <jni.h>

#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace {

JavaVM* g_vm = nullptr;
std::mutex g_audio_constants_mutex;
bool g_audio_constants_loaded = false;
std::string g_audio_constants_signature;

JNIEnv* GetEnvOrNull() {
  if (g_vm == nullptr) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
    return env;
  }
  if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
    return nullptr;
  }
  return env;
}

template <typename T>
int LoadStaticIntField(const char* className, const char* fieldName, T defaultValue = static_cast<T>(-1)) {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr) {
    return defaultValue;
  }
  jclass clazz = env->FindClass(className);
  if (clazz == nullptr) {
    env->ExceptionClear();
    return defaultValue;
  }
  jfieldID field = env->GetStaticFieldID(clazz, fieldName, "I");
  if (field == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(clazz);
    return defaultValue;
  }
  const int value = env->GetStaticIntField(clazz, field);
  env->DeleteLocalRef(clazz);
  return value;
}

template <typename T>
void AppendConstant(std::ostringstream& stream, const char* name, const T value)
{
  if (stream.tellp() > 0)
    stream << ", ";
  stream << name << "=" << value;
}

struct AudioIds
{
  jclass audioAttributesBuilderClass = nullptr;
  jmethodID audioAttributesBuilderCtor = nullptr;
  jmethodID audioAttributesSetUsage = nullptr;
  jmethodID audioAttributesSetContentType = nullptr;
  jmethodID audioAttributesBuild = nullptr;

  jclass audioFormatBuilderClass = nullptr;
  jmethodID audioFormatBuilderCtor = nullptr;
  jmethodID audioFormatSetChannelMask = nullptr;
  jmethodID audioFormatSetEncoding = nullptr;
  jmethodID audioFormatSetSampleRate = nullptr;
  jmethodID audioFormatBuild = nullptr;

  jclass audioTrackClass = nullptr;
  jmethodID audioTrackCtor = nullptr;
  jmethodID audioTrackGetState = nullptr;
  jmethodID audioTrackGetPlayState = nullptr;
  jmethodID audioTrackPlay = nullptr;
  jmethodID audioTrackPause = nullptr;
  jmethodID audioTrackStop = nullptr;
  jmethodID audioTrackFlush = nullptr;
  jmethodID audioTrackRelease = nullptr;
  jmethodID audioTrackSetVolume = nullptr;
  jmethodID audioTrackWriteFloat = nullptr;
  jmethodID audioTrackWriteShort = nullptr;
  jmethodID audioTrackWriteByte = nullptr;
  jmethodID audioTrackWriteBuffer = nullptr;
  jmethodID audioTrackGetPlaybackHeadPosition = nullptr;
  jmethodID audioTrackGetLatency = nullptr;
  jmethodID audioTrackGetBufferSizeInFrames = nullptr;
  jmethodID audioTrackGetUnderrunCount = nullptr;
  jmethodID audioTrackGetTimestamp = nullptr;
  jmethodID audioTrackGetMinBufferSize = nullptr;
  jmethodID audioTrackGetNativeOutputSampleRate = nullptr;

  jclass audioTimestampClass = nullptr;
  jmethodID audioTimestampCtor = nullptr;
  jfieldID audioTimestampFramePosition = nullptr;
  jfieldID audioTimestampNanoTime = nullptr;

  jclass byteBufferClass = nullptr;
  jmethodID byteBufferWrap = nullptr;

  bool loaded = false;
};

AudioIds& GetIds() {
  static AudioIds ids;
  return ids;
}

bool EnsureIds() {
  AudioIds& ids = GetIds();
  if (ids.loaded) {
    return true;
  }
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr) {
    return false;
  }

  auto loadClass = [env](const char* name) -> jclass {
    jclass local = env->FindClass(name);
    if (local == nullptr) {
      env->ExceptionClear();
      return nullptr;
    }
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
  };

  ids.audioAttributesBuilderClass = loadClass("android/media/AudioAttributes$Builder");
  ids.audioFormatBuilderClass = loadClass("android/media/AudioFormat$Builder");
  ids.audioTrackClass = loadClass("android/media/AudioTrack");
  ids.audioTimestampClass = loadClass("android/media/AudioTimestamp");
  ids.byteBufferClass = loadClass("java/nio/ByteBuffer");
  if (ids.audioAttributesBuilderClass == nullptr || ids.audioFormatBuilderClass == nullptr ||
      ids.audioTrackClass == nullptr || ids.audioTimestampClass == nullptr ||
      ids.byteBufferClass == nullptr) {
    return false;
  }

  ids.audioAttributesBuilderCtor = env->GetMethodID(ids.audioAttributesBuilderClass, "<init>", "()V");
  ids.audioAttributesSetUsage =
      env->GetMethodID(ids.audioAttributesBuilderClass, "setUsage",
                       "(I)Landroid/media/AudioAttributes$Builder;");
  ids.audioAttributesSetContentType =
      env->GetMethodID(ids.audioAttributesBuilderClass, "setContentType",
                       "(I)Landroid/media/AudioAttributes$Builder;");
  ids.audioAttributesBuild =
      env->GetMethodID(ids.audioAttributesBuilderClass, "build", "()Landroid/media/AudioAttributes;");

  ids.audioFormatBuilderCtor = env->GetMethodID(ids.audioFormatBuilderClass, "<init>", "()V");
  ids.audioFormatSetChannelMask =
      env->GetMethodID(ids.audioFormatBuilderClass, "setChannelMask",
                       "(I)Landroid/media/AudioFormat$Builder;");
  ids.audioFormatSetEncoding =
      env->GetMethodID(ids.audioFormatBuilderClass, "setEncoding",
                       "(I)Landroid/media/AudioFormat$Builder;");
  ids.audioFormatSetSampleRate =
      env->GetMethodID(ids.audioFormatBuilderClass, "setSampleRate",
                       "(I)Landroid/media/AudioFormat$Builder;");
  ids.audioFormatBuild =
      env->GetMethodID(ids.audioFormatBuilderClass, "build", "()Landroid/media/AudioFormat;");

  ids.audioTrackCtor =
      env->GetMethodID(ids.audioTrackClass, "<init>",
                       "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V");
  ids.audioTrackGetState = env->GetMethodID(ids.audioTrackClass, "getState", "()I");
  ids.audioTrackGetPlayState = env->GetMethodID(ids.audioTrackClass, "getPlayState", "()I");
  ids.audioTrackPlay = env->GetMethodID(ids.audioTrackClass, "play", "()V");
  ids.audioTrackPause = env->GetMethodID(ids.audioTrackClass, "pause", "()V");
  ids.audioTrackStop = env->GetMethodID(ids.audioTrackClass, "stop", "()V");
  ids.audioTrackFlush = env->GetMethodID(ids.audioTrackClass, "flush", "()V");
  ids.audioTrackRelease = env->GetMethodID(ids.audioTrackClass, "release", "()V");
  ids.audioTrackSetVolume = env->GetMethodID(ids.audioTrackClass, "setVolume", "(F)I");
  ids.audioTrackWriteFloat = env->GetMethodID(ids.audioTrackClass, "write", "([FIII)I");
  ids.audioTrackWriteShort = env->GetMethodID(ids.audioTrackClass, "write", "([SIII)I");
  ids.audioTrackWriteByte = env->GetMethodID(ids.audioTrackClass, "write", "([BIII)I");
  ids.audioTrackWriteBuffer =
      env->GetMethodID(ids.audioTrackClass, "write", "(Ljava/nio/ByteBuffer;IIJ)I");
  ids.audioTrackGetPlaybackHeadPosition =
      env->GetMethodID(ids.audioTrackClass, "getPlaybackHeadPosition", "()I");
  ids.audioTrackGetLatency = env->GetMethodID(ids.audioTrackClass, "getLatency", "()I");
  ids.audioTrackGetBufferSizeInFrames =
      env->GetMethodID(ids.audioTrackClass, "getBufferSizeInFrames", "()I");
  ids.audioTrackGetUnderrunCount =
      env->GetMethodID(ids.audioTrackClass, "getUnderrunCount", "()I");
  if (ids.audioTrackGetUnderrunCount == nullptr)
    env->ExceptionClear();
  ids.audioTrackGetTimestamp =
      env->GetMethodID(ids.audioTrackClass, "getTimestamp", "(Landroid/media/AudioTimestamp;)Z");
  ids.audioTrackGetMinBufferSize =
      env->GetStaticMethodID(ids.audioTrackClass, "getMinBufferSize", "(III)I");
  ids.audioTrackGetNativeOutputSampleRate =
      env->GetStaticMethodID(ids.audioTrackClass, "getNativeOutputSampleRate", "(I)I");

  ids.audioTimestampCtor = env->GetMethodID(ids.audioTimestampClass, "<init>", "()V");
  ids.audioTimestampFramePosition = env->GetFieldID(ids.audioTimestampClass, "framePosition", "J");
  ids.audioTimestampNanoTime = env->GetFieldID(ids.audioTimestampClass, "nanoTime", "J");

  ids.byteBufferWrap =
      env->GetStaticMethodID(ids.byteBufferClass, "wrap", "([B)Ljava/nio/ByteBuffer;");

  ids.loaded = ids.audioTrackCtor != nullptr && ids.audioTrackGetMinBufferSize != nullptr &&
               ids.byteBufferWrap != nullptr;
  return ids.loaded;
}

template <typename ArrayType, typename ElementType>
int WritePrimitiveArray(jobject audioTrack,
                        jmethodID method,
                        const std::vector<ElementType>& data,
                        int offset,
                        int size,
                        int writeMode,
                        ArrayType (*create)(JNIEnv*, jsize),
                        void (*setRegion)(JNIEnv*, ArrayType, jsize, jsize, const ElementType*)) {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr || audioTrack == nullptr || size <= 0) {
    return 0;
  }
  ArrayType array = create(env, static_cast<jsize>(size));
  if (array == nullptr) {
    return -1;
  }
  setRegion(env, array, 0, static_cast<jsize>(size), data.data() + offset);
  int written = env->CallIntMethod(audioTrack, method, array, 0, size, writeMode);
  env->DeleteLocalRef(array);
  return written;
}

}  // namespace

JavaVM* kodi_cpp_get_java_vm() {
  return g_vm;
}

void kodi_cpp_set_java_vm(JavaVM* vm) {
  g_vm = vm;
}

JNIEnv* kodi_cpp_get_env() {
  return GetEnvOrNull();
}

namespace jni
{
static bool EnsureAudioConstants();
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void* /* reserved */) {
  kodi_cpp_set_java_vm(vm);
  if (!EnsureIds()) {
    CLog::Log(LOGERROR, "JNI_OnLoad: failed to initialize Android audio JNI method ids");
  }
  if (!jni::EnsureAudioConstants()) {
    CLog::Log(LOGWARNING, "JNI_OnLoad: Android audio constants are only partially initialized");
  }
  return JNI_VERSION_1_6;
}

namespace jni
{

int CJNIAudioAttributes::USAGE_MEDIA = 1;
int CJNIAudioAttributes::CONTENT_TYPE_MUSIC = 2;

int CJNIAudioManager::AUDIO_SESSION_ID_GENERATE = 0;
int CJNIAudioManager::STREAM_MUSIC = 3;

int CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT = 0;
int CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT = 0;
int CJNIAudioFormat::CHANNEL_OUT_FRONT_CENTER = 0;
int CJNIAudioFormat::CHANNEL_OUT_LOW_FREQUENCY = 0;
int CJNIAudioFormat::CHANNEL_OUT_BACK_LEFT = 0;
int CJNIAudioFormat::CHANNEL_OUT_BACK_RIGHT = 0;
int CJNIAudioFormat::CHANNEL_OUT_SIDE_LEFT = 0;
int CJNIAudioFormat::CHANNEL_OUT_SIDE_RIGHT = 0;
int CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT_OF_CENTER = 0;
int CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT_OF_CENTER = 0;
int CJNIAudioFormat::CHANNEL_OUT_BACK_CENTER = 0;
int CJNIAudioFormat::CHANNEL_OUT_STEREO = 0;
int CJNIAudioFormat::CHANNEL_OUT_5POINT1 = 0;
int CJNIAudioFormat::CHANNEL_OUT_7POINT1_SURROUND = 0;
int CJNIAudioFormat::ENCODING_PCM_16BIT = 0;
int CJNIAudioFormat::ENCODING_PCM_FLOAT = -1;
int CJNIAudioFormat::ENCODING_AC3 = -1;
int CJNIAudioFormat::ENCODING_E_AC3 = -1;
int CJNIAudioFormat::ENCODING_DTS = -1;
int CJNIAudioFormat::ENCODING_DTS_HD = -1;
int CJNIAudioFormat::ENCODING_DOLBY_TRUEHD = -1;
int CJNIAudioFormat::ENCODING_IEC61937 = -1;

int CJNIAudioTrack::MODE_STREAM = 1;
int CJNIAudioTrack::WRITE_BLOCKING = 0;
int CJNIAudioTrack::WRITE_NON_BLOCKING = 1;
int CJNIAudioTrack::PLAYSTATE_STOPPED = 1;
int CJNIAudioTrack::PLAYSTATE_PAUSED = 2;
int CJNIAudioTrack::PLAYSTATE_PLAYING = 3;
int CJNIAudioTrack::STATE_UNINITIALIZED = 0;
int CJNIAudioTrack::STATE_INITIALIZED = 1;

static bool EnsureAudioConstants() {
  std::lock_guard<std::mutex> lock(g_audio_constants_mutex);
  if (g_audio_constants_loaded && CJNIAudioFormat::ENCODING_IEC61937 >= 0) {
    return true;
  }

  CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_FRONT_LEFT", 0x4);
  CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_FRONT_RIGHT", 0x8);
  CJNIAudioFormat::CHANNEL_OUT_FRONT_CENTER =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_FRONT_CENTER", 0x10);
  CJNIAudioFormat::CHANNEL_OUT_LOW_FREQUENCY =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_LOW_FREQUENCY", 0x20);
  CJNIAudioFormat::CHANNEL_OUT_BACK_LEFT =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_BACK_LEFT", 0x40);
  CJNIAudioFormat::CHANNEL_OUT_BACK_RIGHT =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_BACK_RIGHT", 0x80);
  CJNIAudioFormat::CHANNEL_OUT_SIDE_LEFT =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_SIDE_LEFT", 0x800);
  CJNIAudioFormat::CHANNEL_OUT_SIDE_RIGHT =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_SIDE_RIGHT", 0x1000);
  CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT_OF_CENTER =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_FRONT_LEFT_OF_CENTER", 0x100);
  CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT_OF_CENTER =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_FRONT_RIGHT_OF_CENTER", 0x200);
  CJNIAudioFormat::CHANNEL_OUT_BACK_CENTER =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_BACK_CENTER", 0x400);
  CJNIAudioFormat::CHANNEL_OUT_STEREO =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_STEREO", 0xC);
  CJNIAudioFormat::CHANNEL_OUT_5POINT1 =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_5POINT1", 0xFC);
  CJNIAudioFormat::CHANNEL_OUT_7POINT1_SURROUND =
      LoadStaticIntField<int>("android/media/AudioFormat", "CHANNEL_OUT_7POINT1_SURROUND", 0x18FC);

  CJNIAudioFormat::ENCODING_PCM_16BIT =
      LoadStaticIntField<int>("android/media/AudioFormat", "ENCODING_PCM_16BIT", 2);
  CJNIAudioFormat::ENCODING_PCM_FLOAT =
      LoadStaticIntField<int>("android/media/AudioFormat", "ENCODING_PCM_FLOAT", 4);
  CJNIAudioFormat::ENCODING_AC3 =
      LoadStaticIntField<int>("android/media/AudioFormat", "ENCODING_AC3");
  CJNIAudioFormat::ENCODING_E_AC3 =
      LoadStaticIntField<int>("android/media/AudioFormat", "ENCODING_E_AC3");
  CJNIAudioFormat::ENCODING_DTS =
      LoadStaticIntField<int>("android/media/AudioFormat", "ENCODING_DTS");
  CJNIAudioFormat::ENCODING_DTS_HD =
      LoadStaticIntField<int>("android/media/AudioFormat", "ENCODING_DTS_HD");
  CJNIAudioFormat::ENCODING_DOLBY_TRUEHD =
      LoadStaticIntField<int>("android/media/AudioFormat", "ENCODING_DOLBY_TRUEHD");
  CJNIAudioFormat::ENCODING_IEC61937 =
      LoadStaticIntField<int>("android/media/AudioFormat", "ENCODING_IEC61937");

  CJNIAudioTrack::WRITE_BLOCKING =
      LoadStaticIntField<int>("android/media/AudioTrack", "WRITE_BLOCKING", 0);
  CJNIAudioTrack::WRITE_NON_BLOCKING =
      LoadStaticIntField<int>("android/media/AudioTrack", "WRITE_NON_BLOCKING", 1);

  g_audio_constants_loaded = CJNIAudioFormat::CHANNEL_OUT_STEREO != 0 &&
                             CJNIAudioFormat::ENCODING_PCM_16BIT > 0;

  std::ostringstream constants;
  AppendConstant(constants, "IEC61937", CJNIAudioFormat::ENCODING_IEC61937);
  AppendConstant(constants, "AC3", CJNIAudioFormat::ENCODING_AC3);
  AppendConstant(constants, "E_AC3", CJNIAudioFormat::ENCODING_E_AC3);
  AppendConstant(constants, "DTS", CJNIAudioFormat::ENCODING_DTS);
  AppendConstant(constants, "DTS_HD", CJNIAudioFormat::ENCODING_DTS_HD);
  AppendConstant(constants, "TRUEHD", CJNIAudioFormat::ENCODING_DOLBY_TRUEHD);
  AppendConstant(constants, "PCM_16", CJNIAudioFormat::ENCODING_PCM_16BIT);
  AppendConstant(constants, "PCM_FLOAT", CJNIAudioFormat::ENCODING_PCM_FLOAT);
  const std::string signature = constants.str();
  if (signature != g_audio_constants_signature)
  {
    g_audio_constants_signature = signature;
    CLog::Log(LOGINFO, "Loaded AudioFormat constants: {}", signature);
  }

  return g_audio_constants_loaded;
}

CJNIAudioAttributesBuilder::CJNIAudioAttributesBuilder() {
  EnsureIds();
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr) {
    m_builder = env->NewGlobalRef(env->NewObject(GetIds().audioAttributesBuilderClass,
                                                 GetIds().audioAttributesBuilderCtor));
  }
}

CJNIAudioAttributesBuilder::~CJNIAudioAttributesBuilder() {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_builder != nullptr) {
    env->DeleteGlobalRef(m_builder);
  }
}

void CJNIAudioAttributesBuilder::setUsage(int usage) const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_builder != nullptr) {
    env->CallObjectMethod(m_builder, GetIds().audioAttributesSetUsage, usage);
  }
}

void CJNIAudioAttributesBuilder::setContentType(int contentType) const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_builder != nullptr) {
    env->CallObjectMethod(m_builder, GetIds().audioAttributesSetContentType, contentType);
  }
}

jobject CJNIAudioAttributesBuilder::build() const {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr || m_builder == nullptr) {
    return nullptr;
  }
  return env->CallObjectMethod(m_builder, GetIds().audioAttributesBuild);
}

CJNIAudioFormatBuilder::CJNIAudioFormatBuilder() {
  EnsureIds();
  EnsureAudioConstants();
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr) {
    m_builder = env->NewGlobalRef(
        env->NewObject(GetIds().audioFormatBuilderClass, GetIds().audioFormatBuilderCtor));
  }
}

CJNIAudioFormatBuilder::~CJNIAudioFormatBuilder() {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_builder != nullptr) {
    env->DeleteGlobalRef(m_builder);
  }
}

void CJNIAudioFormatBuilder::setChannelMask(int channelMask) const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_builder != nullptr) {
    env->CallObjectMethod(m_builder, GetIds().audioFormatSetChannelMask, channelMask);
  }
}

void CJNIAudioFormatBuilder::setEncoding(int encoding) const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_builder != nullptr) {
    env->CallObjectMethod(m_builder, GetIds().audioFormatSetEncoding, encoding);
  }
}

void CJNIAudioFormatBuilder::setSampleRate(int sampleRate) const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_builder != nullptr) {
    env->CallObjectMethod(m_builder, GetIds().audioFormatSetSampleRate, sampleRate);
  }
}

jobject CJNIAudioFormatBuilder::build() const {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr || m_builder == nullptr) {
    return nullptr;
  }
  return env->CallObjectMethod(m_builder, GetIds().audioFormatBuild);
}

CJNIAudioTimestamp::CJNIAudioTimestamp() {
  EnsureIds();
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr) {
    jobject local = env->NewObject(GetIds().audioTimestampClass, GetIds().audioTimestampCtor);
    if (local != nullptr) {
      m_object = env->NewGlobalRef(local);
      env->DeleteLocalRef(local);
    }
  }
}

CJNIAudioTimestamp::~CJNIAudioTimestamp() {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_object != nullptr) {
    env->DeleteGlobalRef(m_object);
  }
}

jobject CJNIAudioTimestamp::get_raw() const {
  return m_object;
}

uint64_t CJNIAudioTimestamp::get_framePosition() const {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr || m_object == nullptr) {
    return 0;
  }
  return static_cast<uint64_t>(
      env->GetLongField(m_object, GetIds().audioTimestampFramePosition));
}

int64_t CJNIAudioTimestamp::get_nanoTime() const {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr || m_object == nullptr) {
    return 0;
  }
  return static_cast<int64_t>(env->GetLongField(m_object, GetIds().audioTimestampNanoTime));
}

CJNIByteBuffer::CJNIByteBuffer(jobject object) : m_object(object) {}

CJNIByteBuffer::~CJNIByteBuffer() {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_object != nullptr) {
    env->DeleteGlobalRef(m_object);
  }
}

CJNIByteBuffer::CJNIByteBuffer(CJNIByteBuffer&& other) noexcept : m_object(other.m_object) {
  other.m_object = nullptr;
}

CJNIByteBuffer& CJNIByteBuffer::operator=(CJNIByteBuffer&& other) noexcept {
  if (this != &other) {
    JNIEnv* env = GetEnvOrNull();
    if (env != nullptr && m_object != nullptr) {
      env->DeleteGlobalRef(m_object);
    }
    m_object = other.m_object;
    other.m_object = nullptr;
  }
  return *this;
}

CJNIByteBuffer CJNIByteBuffer::wrap(const std::vector<char>& data) {
  EnsureIds();
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr) {
    return CJNIByteBuffer();
  }
  jbyteArray bytes = env->NewByteArray(static_cast<jsize>(data.size()));
  if (bytes == nullptr) {
    return CJNIByteBuffer();
  }
  if (!data.empty()) {
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(data.size()),
                            reinterpret_cast<const jbyte*>(data.data()));
  }
  jobject local = env->CallStaticObjectMethod(GetIds().byteBufferClass, GetIds().byteBufferWrap, bytes);
  env->DeleteLocalRef(bytes);
  if (local == nullptr) {
    return CJNIByteBuffer();
  }
  jobject global = env->NewGlobalRef(local);
  env->DeleteLocalRef(local);
  return CJNIByteBuffer(global);
}

jobject CJNIByteBuffer::get_raw() const {
  return m_object;
}

CJNIAudioTrack::CJNIAudioTrack(jobject attributes,
                               jobject format,
                               int bufferSize,
                               int mode,
                               int audioSessionId) {
  EnsureIds();
  EnsureAudioConstants();
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr) {
    return;
  }
  jobject local = env->NewObject(GetIds().audioTrackClass, GetIds().audioTrackCtor, attributes,
                                 format, bufferSize, mode,
                                 audioSessionId == 0 ? CJNIAudioManager::AUDIO_SESSION_ID_GENERATE
                                                     : audioSessionId);
  if (local != nullptr) {
    m_audioTrack = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
  }
}

CJNIAudioTrack::~CJNIAudioTrack() {
  release();
}

int CJNIAudioTrack::getState() const {
  JNIEnv* env = GetEnvOrNull();
  return (env == nullptr || m_audioTrack == nullptr)
             ? STATE_UNINITIALIZED
             : env->CallIntMethod(m_audioTrack, GetIds().audioTrackGetState);
}

int CJNIAudioTrack::getPlayState() const {
  JNIEnv* env = GetEnvOrNull();
  return (env == nullptr || m_audioTrack == nullptr)
             ? PLAYSTATE_STOPPED
             : env->CallIntMethod(m_audioTrack, GetIds().audioTrackGetPlayState);
}

void CJNIAudioTrack::play() const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_audioTrack != nullptr) {
    env->CallVoidMethod(m_audioTrack, GetIds().audioTrackPlay);
  }
}

void CJNIAudioTrack::pause() const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_audioTrack != nullptr) {
    env->CallVoidMethod(m_audioTrack, GetIds().audioTrackPause);
  }
}

void CJNIAudioTrack::stop() const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_audioTrack != nullptr) {
    env->CallVoidMethod(m_audioTrack, GetIds().audioTrackStop);
  }
}

void CJNIAudioTrack::flush() const {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_audioTrack != nullptr) {
    env->CallVoidMethod(m_audioTrack, GetIds().audioTrackFlush);
  }
}

void CJNIAudioTrack::release() {
  JNIEnv* env = GetEnvOrNull();
  if (env != nullptr && m_audioTrack != nullptr) {
    env->CallVoidMethod(m_audioTrack, GetIds().audioTrackRelease);
    env->DeleteGlobalRef(m_audioTrack);
    m_audioTrack = nullptr;
  }
}

int CJNIAudioTrack::setVolume(float volume) const {
  JNIEnv* env = GetEnvOrNull();
  return (env == nullptr || m_audioTrack == nullptr)
             ? -1
             : env->CallIntMethod(m_audioTrack, GetIds().audioTrackSetVolume, volume);
}

int CJNIAudioTrack::write(const std::vector<float>& data, int offset, int size, int writeMode) {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr || m_audioTrack == nullptr || size <= 0) {
    return 0;
  }
  jfloatArray array = env->NewFloatArray(size);
  if (array == nullptr) {
    return -1;
  }
  env->SetFloatArrayRegion(array, 0, size, data.data() + offset);
  int written = env->CallIntMethod(m_audioTrack, GetIds().audioTrackWriteFloat, array, 0, size, writeMode);
  env->DeleteLocalRef(array);
  return written;
}

int CJNIAudioTrack::write(const std::vector<int16_t>& data, int offset, int size, int writeMode) {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr || m_audioTrack == nullptr || size <= 0) {
    return 0;
  }
  jshortArray array = env->NewShortArray(size);
  if (array == nullptr) {
    return -1;
  }
  env->SetShortArrayRegion(array, 0, size, reinterpret_cast<const jshort*>(data.data() + offset));
  int written = env->CallIntMethod(m_audioTrack, GetIds().audioTrackWriteShort, array, 0, size, writeMode);
  env->DeleteLocalRef(array);
  return written;
}

int CJNIAudioTrack::write(const std::vector<char>& data, int offset, int size, int writeMode) {
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr || m_audioTrack == nullptr || size <= 0) {
    return 0;
  }
  jbyteArray array = env->NewByteArray(size);
  if (array == nullptr) {
    return -1;
  }
  env->SetByteArrayRegion(array, 0, size, reinterpret_cast<const jbyte*>(data.data() + offset));
  int written = env->CallIntMethod(m_audioTrack, GetIds().audioTrackWriteByte, array, 0, size, writeMode);
  env->DeleteLocalRef(array);
  return written;
}

int CJNIAudioTrack::write(jobject byteBuffer, int size, int writeMode, int64_t timestamp) {
  JNIEnv* env = GetEnvOrNull();
  return (env == nullptr || m_audioTrack == nullptr || byteBuffer == nullptr)
             ? -1
             : env->CallIntMethod(m_audioTrack, GetIds().audioTrackWriteBuffer, byteBuffer, size,
                                  writeMode, static_cast<jlong>(timestamp));
}

int CJNIAudioTrack::getPlaybackHeadPosition() const {
  JNIEnv* env = GetEnvOrNull();
  return (env == nullptr || m_audioTrack == nullptr)
             ? 0
             : env->CallIntMethod(m_audioTrack, GetIds().audioTrackGetPlaybackHeadPosition);
}

int CJNIAudioTrack::getLatency() const {
  JNIEnv* env = GetEnvOrNull();
  return (env == nullptr || m_audioTrack == nullptr)
             ? -1
             : env->CallIntMethod(m_audioTrack, GetIds().audioTrackGetLatency);
}

int CJNIAudioTrack::getBufferSizeInFrames() const {
  JNIEnv* env = GetEnvOrNull();
  return (env == nullptr || m_audioTrack == nullptr)
             ? -1
             : env->CallIntMethod(m_audioTrack, GetIds().audioTrackGetBufferSizeInFrames);
}

int CJNIAudioTrack::getUnderrunCount() const {
  JNIEnv* env = GetEnvOrNull();
  return (env == nullptr || m_audioTrack == nullptr || GetIds().audioTrackGetUnderrunCount == nullptr)
             ? -1
             : env->CallIntMethod(m_audioTrack, GetIds().audioTrackGetUnderrunCount);
}

bool CJNIAudioTrack::getTimestamp(CJNIAudioTimestamp& timestamp) const {
  JNIEnv* env = GetEnvOrNull();
  return env != nullptr && m_audioTrack != nullptr && timestamp.get_raw() != nullptr &&
         env->CallBooleanMethod(m_audioTrack, GetIds().audioTrackGetTimestamp, timestamp.get_raw()) == JNI_TRUE;
}

int CJNIAudioTrack::getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat) {
  EnsureIds();
  JNIEnv* env = GetEnvOrNull();
  if (env == nullptr) {
    return -1;
  }
  EnsureAudioConstants();
  return env->CallStaticIntMethod(GetIds().audioTrackClass,
                                  GetIds().audioTrackGetMinBufferSize,
                                  sampleRateInHz, channelConfig, audioFormat);
}

int CJNIAudioTrack::getNativeOutputSampleRate(int streamType) {
  EnsureIds();
  JNIEnv* env = GetEnvOrNull();
  return env == nullptr
             ? -1
             : env->CallStaticIntMethod(GetIds().audioTrackClass,
                                        GetIds().audioTrackGetNativeOutputSampleRate, streamType);
}

} // namespace jni
