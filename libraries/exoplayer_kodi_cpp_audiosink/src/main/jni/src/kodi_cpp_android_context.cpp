#include "androidjni/ActivityManager.h"

#include "kodi_cpp_jni_support.h"

namespace
{

struct AndroidContextIds
{
  jclass activityThreadClass = nullptr;
  jmethodID currentApplication = nullptr;

  jclass contextClass = nullptr;
  jmethodID getSystemService = nullptr;

  jclass activityManagerClass = nullptr;
  jmethodID getMemoryInfo = nullptr;

  jclass memoryInfoClass = nullptr;
  jmethodID memoryInfoCtor = nullptr;
  jfieldID totalMemField = nullptr;
  jfieldID availMemField = nullptr;

  bool loaded = false;
};

AndroidContextIds& GetIds()
{
  static AndroidContextIds ids;
  return ids;
}

jclass LoadGlobalClass(JNIEnv* env, const char* class_name)
{
  jclass local = env->FindClass(class_name);
  if (local == nullptr)
  {
    env->ExceptionClear();
    return nullptr;
  }

  jclass global = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  return global;
}

bool EnsureIds()
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env == nullptr)
    return false;

  AndroidContextIds& ids = GetIds();
  if (ids.loaded)
    return true;

  ids.activityThreadClass = LoadGlobalClass(env, "android/app/ActivityThread");
  ids.contextClass = LoadGlobalClass(env, "android/content/Context");
  ids.activityManagerClass = LoadGlobalClass(env, "android/app/ActivityManager");
  ids.memoryInfoClass = LoadGlobalClass(env, "android/app/ActivityManager$MemoryInfo");
  if (ids.activityThreadClass == nullptr || ids.contextClass == nullptr ||
      ids.activityManagerClass == nullptr || ids.memoryInfoClass == nullptr)
  {
    return false;
  }

  ids.currentApplication =
      env->GetStaticMethodID(ids.activityThreadClass, "currentApplication",
                             "()Landroid/app/Application;");
  ids.getSystemService =
      env->GetMethodID(ids.contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
  ids.getMemoryInfo =
      env->GetMethodID(ids.activityManagerClass, "getMemoryInfo",
                       "(Landroid/app/ActivityManager$MemoryInfo;)V");
  ids.memoryInfoCtor = env->GetMethodID(ids.memoryInfoClass, "<init>", "()V");
  ids.totalMemField = env->GetFieldID(ids.memoryInfoClass, "totalMem", "J");
  ids.availMemField = env->GetFieldID(ids.memoryInfoClass, "availMem", "J");

  ids.loaded = ids.currentApplication != nullptr && ids.getSystemService != nullptr &&
               ids.getMemoryInfo != nullptr && ids.memoryInfoCtor != nullptr &&
               ids.totalMemField != nullptr && ids.availMemField != nullptr;
  return ids.loaded;
}

} // namespace

namespace jni
{

jobject CJNIContext::getSystemService(const char* name)
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env == nullptr || name == nullptr || !EnsureIds())
    return nullptr;

  jobject application =
      env->CallStaticObjectMethod(GetIds().activityThreadClass, GetIds().currentApplication);
  if (application == nullptr)
    return nullptr;

  jstring service_name = env->NewStringUTF(name);
  if (service_name == nullptr)
  {
    env->DeleteLocalRef(application);
    return nullptr;
  }

  jobject service =
      env->CallObjectMethod(application, GetIds().getSystemService, service_name);
  env->DeleteLocalRef(service_name);
  env->DeleteLocalRef(application);
  return service;
}

CJNIActivityManager::MemoryInfo::MemoryInfo()
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env == nullptr || !EnsureIds())
    return;

  jobject local = env->NewObject(GetIds().memoryInfoClass, GetIds().memoryInfoCtor);
  if (local == nullptr)
    return;

  m_object = env->NewGlobalRef(local);
  env->DeleteLocalRef(local);
}

CJNIActivityManager::MemoryInfo::~MemoryInfo()
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env != nullptr && m_object != nullptr)
    env->DeleteGlobalRef(m_object);
}

int64_t CJNIActivityManager::MemoryInfo::totalMem() const
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env == nullptr || m_object == nullptr || !EnsureIds())
    return 0;

  return static_cast<int64_t>(env->GetLongField(m_object, GetIds().totalMemField));
}

int64_t CJNIActivityManager::MemoryInfo::availMem() const
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env == nullptr || m_object == nullptr || !EnsureIds())
    return 0;

  return static_cast<int64_t>(env->GetLongField(m_object, GetIds().availMemField));
}

jobject CJNIActivityManager::MemoryInfo::get_raw() const
{
  return m_object;
}

CJNIActivityManager::CJNIActivityManager(jobject object)
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env == nullptr || object == nullptr)
    return;

  m_object = env->NewGlobalRef(object);
  env->DeleteLocalRef(object);
}

CJNIActivityManager::~CJNIActivityManager()
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env != nullptr && m_object != nullptr)
    env->DeleteGlobalRef(m_object);
}

void CJNIActivityManager::getMemoryInfo(MemoryInfo& info) const
{
  JNIEnv* env = kodi_cpp_get_env();
  if (env == nullptr || m_object == nullptr || info.get_raw() == nullptr || !EnsureIds())
    return;

  env->CallVoidMethod(m_object, GetIds().getMemoryInfo, info.get_raw());
}

} // namespace jni
