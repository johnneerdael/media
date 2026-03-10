
#include <android/log.h>
#include <jni.h>
#include <cstdlib>
#include <cstring>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/native_window_jni.h>
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <vector>
#include "config.h"
#include "config_components.h"
#include "ffcommon.h"

#ifndef FFMPEG_TONEMAP_FILTERS
#define FFMPEG_TONEMAP_FILTERS 0
#endif

#if FFMPEG_TONEMAP_FILTERS && defined(CONFIG_LIBPLACEBO_FILTER) && CONFIG_LIBPLACEBO_FILTER && \
    defined(CONFIG_LIBPLACEBO) && CONFIG_LIBPLACEBO && defined(CONFIG_VULKAN) && CONFIG_VULKAN
#define FFMPEG_TONEMAP_FILTERS_ACTIVE 1
#else
#define FFMPEG_TONEMAP_FILTERS_ACTIVE 0
#endif

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <cstdint>
#endif
#include <libavcodec/avcodec.h>
#include "ffmpeg/libavcodec/dovi_rpu.h"
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#if FFMPEG_TONEMAP_FILTERS_ACTIVE
#include <libplacebo/colorspace.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>
#include <vulkan/vulkan.h>
#endif
#if FFMPEG_TONEMAP_FILTERS_ACTIVE
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#endif
}

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static const int VIDEO_DECODER_SUCCESS = 0;
static const int VIDEO_DECODER_ERROR_INVALID_DATA = -1;
static const int VIDEO_DECODER_ERROR_OTHER = -2;
static const int VIDEO_DECODER_ERROR_READ_FRAME = -3;
struct JniContext;
bool ffmpegRenderExperimentalDv5HardwareFramePure(
        JNIEnv *env,
        int64_t presentationTimeUs,
        jobject hardwareBufferObject,
        int32_t displayedWidth,
        int32_t displayedHeight,
        jobject outputSurface);
static AVFrame *applyToneMapToFrame(JniContext *jniContext, AVFrame *sourceFrame);

namespace {
constexpr int64_t kExternalRpuMatchToleranceUs = 50000;
constexpr size_t kExternalRpuMaxEntries = 1024;
constexpr int64_t kExternalRpuPruneWindowUs = 2000000;
constexpr int64_t kExternalRpuSummaryInterval = 120;

struct ExternalRpuBridgeState {
    std::mutex mutex;
    bool enabled = false;
    std::map<int64_t, std::vector<uint8_t>> queuedByTimeUs;
    int64_t pushedCount = 0;
    int64_t matchedCount = 0;
    int64_t missCount = 0;
    int64_t dropNoPtsCount = 0;
    int64_t dropOverflowCount = 0;
};

ExternalRpuBridgeState gExternalRpuBridgeState;
std::mutex gHardwareToneMapContextMutex;
JniContext *gHardwareToneMapContext = nullptr;
#if FFMPEG_TONEMAP_FILTERS_ACTIVE
struct PureHardwareRendererContext {
    pl_log log = nullptr;
    pl_vk_inst vk_instance = nullptr;
    pl_vulkan vulkan = nullptr;
    pl_renderer renderer = nullptr;
    pl_swapchain swapchain = nullptr;
    ANativeWindow *native_window = nullptr;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    pl_tex plane_textures[3] = {nullptr, nullptr, nullptr};
    jobject surface_object = nullptr;
    bool cpu_chroma_layout_logged = false;
};

struct ImportedHardwareBufferTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    pl_tex texture = nullptr;
};

struct VulkanDeviceFns {
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_properties = nullptr;
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkBindImageMemory bind_image_memory = nullptr;
};
std::mutex gPureHardwareRendererContextMutex;
PureHardwareRendererContext *gPureHardwareRendererContext = nullptr;
#endif

void maybeLogExternalRpuSummaryLocked(const ExternalRpuBridgeState &state, const char *source) {
    int64_t totalDecisions = state.matchedCount + state.missCount;
    if (totalDecisions <= 0 || (totalDecisions % kExternalRpuSummaryInterval) != 0) {
        return;
    }
    __android_log_print(
            ANDROID_LOG_INFO,
            LOG_TAG,
            "DV5_HW_RPU: source=%s queued=%zu pushed=%" PRId64 " matched=%" PRId64
            " misses=%" PRId64 " dropNoPts=%" PRId64 " dropOverflow=%" PRId64,
            source,
            state.queuedByTimeUs.size(),
            state.pushedCount,
            state.matchedCount,
            state.missCount,
            state.dropNoPtsCount,
            state.dropOverflowCount);
}

void maybeAttachExternalDv5RpuSideData(JniContext *jniContext, AVFrame *frame);
int renderAvFrameToSurface(
        JNIEnv *env,
        JniContext *jniContext,
        AVFrame *frame,
        jobject surface,
        int displayedWidth,
        int displayedHeight);
bool copyHardwareBufferToAvFrame(
        AHardwareBuffer *hardwareBuffer, int width, int height, AVFrame *destinationFrame);
JniContext *ensureHardwareToneMapContext();
#if FFMPEG_TONEMAP_FILTERS_ACTIVE
bool ensurePureHardwareRendererContext(
        JNIEnv *env, jobject outputSurface, int displayedWidth, int displayedHeight);
void destroyPureHardwareRendererContext(JNIEnv *env, PureHardwareRendererContext **contextRef);
bool renderHardwareBufferWithPureRenderer(
        JNIEnv *env,
        PureHardwareRendererContext *context,
        AHardwareBuffer *hardwareBuffer,
        int64_t presentationTimeUs,
        int frameWidth,
        int frameHeight);
bool tryGetExternalDv5RpuForTimeUs(
        int64_t frameTimeUs,
        bool consumeMatch,
        std::vector<uint8_t> *matchedRpu,
        int64_t *matchedSampleTimeUs,
        const char *source);
void consumeExternalDv5RpuBySampleTime(int64_t sampleTimeUs, const char *source);
bool maybeApplyExternalDv5RpuToPureFrame(
        int64_t presentationTimeUs,
        pl_frame *sourceFrame,
        pl_dovi_metadata *doviMetadata,
        int64_t *matchedSampleTimeUs);
bool loadVulkanDeviceFns(PureHardwareRendererContext *context, VulkanDeviceFns *fns);
bool vulkanDeviceHasExtension(const PureHardwareRendererContext *context, const char *extensionName);
uint32_t chooseVulkanMemoryTypeIndex(
        uint32_t candidateTypeBits, const VkPhysicalDeviceMemoryProperties &memoryProperties);
bool importHardwareBufferAsVulkanTexture(
        PureHardwareRendererContext *context,
        AHardwareBuffer *hardwareBuffer,
        int frameWidth,
        int frameHeight,
        ImportedHardwareBufferTexture *outImportedTexture);
void destroyImportedHardwareBufferTexture(
        PureHardwareRendererContext *context,
        const VulkanDeviceFns &fns,
        ImportedHardwareBufferTexture *importedTexture);
bool fillSourceFrameFromImportedTexture(
        const ImportedHardwareBufferTexture &importedTexture,
        int frameWidth,
        int frameHeight,
        pl_frame *sourceFrame);
void maybeLogCpuReadbackChromaLayout(
        PureHardwareRendererContext *context,
        AHardwareBuffer *hardwareBuffer,
        const AHardwareBuffer_Planes &planes);
void onPlaceboLog(void *logPriv, enum pl_log_level level, const char *message);
#endif

typedef void (*AHardwareBufferDescribeFn)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
typedef int (*AHardwareBufferLockPlanesFn)(
        AHardwareBuffer *, uint64_t, int32_t, const ARect *, AHardwareBuffer_Planes *);
typedef int (*AHardwareBufferUnlockFn)(AHardwareBuffer *, int32_t *);
typedef AHardwareBuffer *(*AHardwareBufferFromJniFn)(JNIEnv *, jobject);

AHardwareBufferDescribeFn getHardwareBufferDescribeFn() {
    static AHardwareBufferDescribeFn fn = reinterpret_cast<AHardwareBufferDescribeFn>(
            dlsym(RTLD_DEFAULT, "AHardwareBuffer_describe"));
    return fn;
}

AHardwareBufferLockPlanesFn getHardwareBufferLockPlanesFn() {
    static AHardwareBufferLockPlanesFn fn = reinterpret_cast<AHardwareBufferLockPlanesFn>(
            dlsym(RTLD_DEFAULT, "AHardwareBuffer_lockPlanes"));
    return fn;
}

AHardwareBufferUnlockFn getHardwareBufferUnlockFn() {
    static AHardwareBufferUnlockFn fn = reinterpret_cast<AHardwareBufferUnlockFn>(
            dlsym(RTLD_DEFAULT, "AHardwareBuffer_unlock"));
    return fn;
}

AHardwareBufferFromJniFn getHardwareBufferFromJniFn() {
    static AHardwareBufferFromJniFn fn = reinterpret_cast<AHardwareBufferFromJniFn>(
            dlsym(RTLD_DEFAULT, "AHardwareBuffer_fromHardwareBuffer"));
    return fn;
}

#if FFMPEG_TONEMAP_FILTERS_ACTIVE
PFN_vkGetInstanceProcAddr getVkGetInstanceProcAddrFn() {
    static PFN_vkGetInstanceProcAddr fn = []() -> PFN_vkGetInstanceProcAddr {
        const char *kVulkanLibName = "libvulkan.so";
        void *handle = dlopen(kVulkanLibName, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            __android_log_print(
                    ANDROID_LOG_WARN,
                    LOG_TAG,
                    "DV5_HW_PURE: unable to load %s: %s",
                    kVulkanLibName,
                    dlerror());
            return nullptr;
        }
        auto resolver = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                dlsym(handle, "vkGetInstanceProcAddr"));
        if (!resolver) {
            __android_log_print(
                    ANDROID_LOG_WARN,
                    LOG_TAG,
                    "DV5_HW_PURE: vkGetInstanceProcAddr symbol unavailable.");
            return nullptr;
        }
        return resolver;
    }();
    return fn;
}
#endif

#if FFMPEG_TONEMAP_FILTERS_ACTIVE
void onPlaceboLog(void *logPriv, enum pl_log_level level, const char *message) {
    int androidLevel = ANDROID_LOG_DEBUG;
    switch (level) {
        case PL_LOG_FATAL:
            androidLevel = ANDROID_LOG_FATAL;
            break;
        case PL_LOG_ERR:
            androidLevel = ANDROID_LOG_ERROR;
            break;
        case PL_LOG_WARN:
            androidLevel = ANDROID_LOG_WARN;
            break;
        case PL_LOG_INFO:
            androidLevel = ANDROID_LOG_INFO;
            break;
        case PL_LOG_DEBUG:
        case PL_LOG_TRACE:
        case PL_LOG_NONE:
        default:
            androidLevel = ANDROID_LOG_DEBUG;
            break;
    }
    __android_log_print(
            androidLevel,
            LOG_TAG,
            "DV5_HW_PURE[%s]: %s",
            logPriv ? reinterpret_cast<const char *>(logPriv) : "placebo",
            message ? message : "");
}

void destroyPureHardwareRendererContext(JNIEnv *env, PureHardwareRendererContext **contextRef) {
    if (!contextRef || !*contextRef) {
        return;
    }
    PureHardwareRendererContext *context = *contextRef;
    if (context->vulkan) {
        for (pl_tex &texture : context->plane_textures) {
            if (texture) {
                pl_tex_destroy(context->vulkan->gpu, &texture);
            }
        }
    }
    if (context->swapchain) {
        pl_swapchain_destroy(&context->swapchain);
    }
    if (context->renderer) {
        pl_renderer_destroy(&context->renderer);
    }
    if (context->vulkan) {
        pl_vulkan_destroy(&context->vulkan);
    }
    if (context->vk_surface != VK_NULL_HANDLE && context->vk_instance) {
        auto destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
                context->vk_instance->get_proc_addr(context->vk_instance->instance, "vkDestroySurfaceKHR"));
        if (destroySurface) {
            destroySurface(context->vk_instance->instance, context->vk_surface, nullptr);
        }
        context->vk_surface = VK_NULL_HANDLE;
    }
    if (context->native_window) {
        ANativeWindow_release(context->native_window);
        context->native_window = nullptr;
    }
    if (env && context->surface_object) {
        env->DeleteGlobalRef(context->surface_object);
        context->surface_object = nullptr;
    }
    if (context->vk_instance) {
        pl_vk_inst_destroy(&context->vk_instance);
    }
    if (context->log) {
        pl_log_destroy(&context->log);
    }
    delete context;
    *contextRef = nullptr;
}

bool ensurePureHardwareRendererContext(
        JNIEnv *env, jobject outputSurface, int displayedWidth, int displayedHeight) {
    if (!env || !outputSurface || displayedWidth <= 0 || displayedHeight <= 0) {
        return false;
    }
    if (gPureHardwareRendererContext &&
        gPureHardwareRendererContext->surface_object &&
        env->IsSameObject(outputSurface, gPureHardwareRendererContext->surface_object)) {
        int width = displayedWidth;
        int height = displayedHeight;
        return gPureHardwareRendererContext->swapchain &&
               pl_swapchain_resize(gPureHardwareRendererContext->swapchain, &width, &height);
    }

    destroyPureHardwareRendererContext(env, &gPureHardwareRendererContext);

    auto *context = new PureHardwareRendererContext();
    const char *instanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    struct pl_log_params logParams = {};
    logParams.log_cb = onPlaceboLog;
    logParams.log_priv = const_cast<char *>("ctx");
    logParams.log_level = PL_LOG_WARN;
    context->log = pl_log_create(PL_API_VER, &logParams);
    if (!context->log) {
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }

    struct pl_vk_inst_params vkInstParams = {};
    vkInstParams.extensions = instanceExtensions;
    vkInstParams.num_extensions = 2;
    vkInstParams.get_proc_addr = getVkGetInstanceProcAddrFn();
    if (!vkInstParams.get_proc_addr) {
        LOGE("DV5_HW_PURE: vkGetInstanceProcAddr unavailable.");
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }
    context->vk_instance = pl_vk_inst_create(context->log, &vkInstParams);
    if (!context->vk_instance) {
        LOGE("DV5_HW_PURE: failed to create Vulkan instance.");
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }

    context->native_window = ANativeWindow_fromSurface(env, outputSurface);
    if (!context->native_window) {
        LOGE("DV5_HW_PURE: failed to acquire ANativeWindow.");
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }

    auto createSurface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            context->vk_instance->get_proc_addr(context->vk_instance->instance, "vkCreateAndroidSurfaceKHR"));
    if (!createSurface) {
        LOGE("DV5_HW_PURE: vkCreateAndroidSurfaceKHR unavailable.");
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }
    VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.pNext = nullptr;
    surfaceInfo.flags = 0;
    surfaceInfo.window = context->native_window;
    VkResult vkResult =
            createSurface(context->vk_instance->instance, &surfaceInfo, nullptr, &context->vk_surface);
    if (vkResult != VK_SUCCESS) {
        LOGE("DV5_HW_PURE: failed to create Vulkan surface: %d", static_cast<int>(vkResult));
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }

    struct pl_vulkan_params vkParams = pl_vulkan_default_params;
    const char *optionalDeviceExtensions[] = {
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME
    };
    vkParams.instance = context->vk_instance->instance;
    vkParams.get_proc_addr = context->vk_instance->get_proc_addr;
    vkParams.surface = context->vk_surface;
    vkParams.opt_extensions = optionalDeviceExtensions;
    vkParams.num_opt_extensions = 3;
    // Shield/Tegra Vulkan runtime is 1.1; keep libplacebo on that ceiling.
    vkParams.max_api_version = VK_API_VERSION_1_1;
    vkParams.allow_software = false;
    context->vulkan = pl_vulkan_create(context->log, &vkParams);
    if (!context->vulkan) {
        LOGE("DV5_HW_PURE: failed to create libplacebo Vulkan context.");
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }

    struct pl_vulkan_swapchain_params swapchainParams = {};
    swapchainParams.surface = context->vk_surface;
    swapchainParams.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainParams.swapchain_depth = 3;
    swapchainParams.allow_suboptimal = true;
    context->swapchain = pl_vulkan_create_swapchain(context->vulkan, &swapchainParams);
    if (!context->swapchain) {
        LOGE("DV5_HW_PURE: failed to create swapchain.");
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }
    context->renderer = pl_renderer_create(context->log, context->vulkan->gpu);
    if (!context->renderer) {
        LOGE("DV5_HW_PURE: failed to create renderer.");
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }

    int resizeWidth = displayedWidth;
    int resizeHeight = displayedHeight;
    if (!pl_swapchain_resize(context->swapchain, &resizeWidth, &resizeHeight)) {
        LOGE("DV5_HW_PURE: initial swapchain resize failed.");
        destroyPureHardwareRendererContext(env, &context);
        return false;
    }
    context->surface_object = env->NewGlobalRef(outputSurface);
    gPureHardwareRendererContext = context;
    return true;
}

bool vulkanDeviceHasExtension(const PureHardwareRendererContext *context, const char *extensionName) {
    if (!context || !context->vulkan || !extensionName) {
        return false;
    }
    for (int i = 0; i < context->vulkan->num_extensions; ++i) {
        const char *enabledExtension = context->vulkan->extensions[i];
        if (enabledExtension && strcmp(enabledExtension, extensionName) == 0) {
            return true;
        }
    }
    return false;
}

bool loadVulkanDeviceFns(PureHardwareRendererContext *context, VulkanDeviceFns *fns) {
    if (!context || !context->vk_instance || !context->vulkan || !fns) {
        return false;
    }
    memset(fns, 0, sizeof(*fns));
    if (!context->vk_instance->get_proc_addr) {
        return false;
    }
    fns->get_physical_device_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                    context->vk_instance->get_proc_addr(
                            context->vk_instance->instance, "vkGetPhysicalDeviceMemoryProperties"));
    fns->get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            context->vk_instance->get_proc_addr(context->vk_instance->instance, "vkGetDeviceProcAddr"));
    if (!fns->get_physical_device_memory_properties || !fns->get_device_proc_addr) {
        return false;
    }
    VkDevice device = context->vulkan->device;
    fns->get_ahb_properties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            fns->get_device_proc_addr(device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    fns->create_image =
            reinterpret_cast<PFN_vkCreateImage>(fns->get_device_proc_addr(device, "vkCreateImage"));
    fns->destroy_image =
            reinterpret_cast<PFN_vkDestroyImage>(fns->get_device_proc_addr(device, "vkDestroyImage"));
    fns->get_image_memory_requirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
            fns->get_device_proc_addr(device, "vkGetImageMemoryRequirements"));
    fns->allocate_memory =
            reinterpret_cast<PFN_vkAllocateMemory>(fns->get_device_proc_addr(device, "vkAllocateMemory"));
    fns->free_memory =
            reinterpret_cast<PFN_vkFreeMemory>(fns->get_device_proc_addr(device, "vkFreeMemory"));
    fns->bind_image_memory =
            reinterpret_cast<PFN_vkBindImageMemory>(fns->get_device_proc_addr(device, "vkBindImageMemory"));

    return fns->get_ahb_properties && fns->create_image && fns->destroy_image &&
           fns->get_image_memory_requirements && fns->allocate_memory && fns->free_memory &&
           fns->bind_image_memory;
}

uint32_t chooseVulkanMemoryTypeIndex(
        uint32_t candidateTypeBits, const VkPhysicalDeviceMemoryProperties &memoryProperties) {
    for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
        if ((candidateTypeBits & (1u << index)) != 0u) {
            return index;
        }
    }
    return UINT32_MAX;
}

void destroyImportedHardwareBufferTexture(
        PureHardwareRendererContext *context,
        const VulkanDeviceFns &fns,
        ImportedHardwareBufferTexture *importedTexture) {
    if (!context || !context->vulkan || !importedTexture) {
        return;
    }
    if (importedTexture->texture) {
        pl_tex_destroy(context->vulkan->gpu, &importedTexture->texture);
    }
    VkDevice device = context->vulkan->device;
    if (importedTexture->memory != VK_NULL_HANDLE && fns.free_memory) {
        fns.free_memory(device, importedTexture->memory, nullptr);
        importedTexture->memory = VK_NULL_HANDLE;
    }
    if (importedTexture->image != VK_NULL_HANDLE && fns.destroy_image) {
        fns.destroy_image(device, importedTexture->image, nullptr);
        importedTexture->image = VK_NULL_HANDLE;
    }
}

bool importHardwareBufferAsVulkanTexture(
        PureHardwareRendererContext *context,
        AHardwareBuffer *hardwareBuffer,
        int frameWidth,
        int frameHeight,
        ImportedHardwareBufferTexture *outImportedTexture) {
    if (!context || !context->vulkan || !hardwareBuffer || !outImportedTexture || frameWidth <= 0 ||
        frameHeight <= 0) {
        return false;
    }
    if (!vulkanDeviceHasExtension(
                context, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
        return false;
    }

    VulkanDeviceFns fns = {};
    if (!loadVulkanDeviceFns(context, &fns)) {
        return false;
    }

    AHardwareBufferDescribeFn describeFn = getHardwareBufferDescribeFn();
    if (!describeFn) {
        return false;
    }
    AHardwareBuffer_Desc bufferDesc = {};
    describeFn(hardwareBuffer, &bufferDesc);
    if (bufferDesc.width == 0 || bufferDesc.height == 0) {
        return false;
    }

    VkAndroidHardwareBufferFormatPropertiesANDROID formatProps = {};
    formatProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID bufferProps = {};
    bufferProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    bufferProps.pNext = &formatProps;
    VkResult result =
            fns.get_ahb_properties(context->vulkan->device, hardwareBuffer, &bufferProps);
    if (result != VK_SUCCESS) {
        LOGE("DV5_HW_PURE: vkGetAndroidHardwareBufferPropertiesANDROID failed: %d", result);
        return false;
    }
    if (formatProps.format == VK_FORMAT_UNDEFINED) {
        LOGE(
                "DV5_HW_PURE: AHB has VK_FORMAT_UNDEFINED (externalFormat=%" PRIu64
                "); cannot wrap with libplacebo v5",
                static_cast<uint64_t>(formatProps.externalFormat));
        return false;
    }

    VkExternalMemoryImageCreateInfo externalMemoryImageInfo = {};
    externalMemoryImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalMemoryImageInfo.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalMemoryImageInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = formatProps.format;
    imageInfo.extent.width = static_cast<uint32_t>(frameWidth);
    imageInfo.extent.height = static_cast<uint32_t>(frameHeight);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    result = fns.create_image(context->vulkan->device, &imageInfo, nullptr, &outImportedTexture->image);
    if (result != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memoryRequirements = {};
    fns.get_image_memory_requirements(
            context->vulkan->device, outImportedTexture->image, &memoryRequirements);

    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    fns.get_physical_device_memory_properties(context->vulkan->phys_device, &memoryProperties);
    const uint32_t memoryTypeIndex = chooseVulkanMemoryTypeIndex(
            memoryRequirements.memoryTypeBits & bufferProps.memoryTypeBits, memoryProperties);
    if (memoryTypeIndex == UINT32_MAX) {
        destroyImportedHardwareBufferTexture(context, fns, outImportedTexture);
        return false;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo = {};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    importInfo.buffer = hardwareBuffer;

    VkMemoryDedicatedAllocateInfo dedicatedInfo = {};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.image = outImportedTexture->image;
    importInfo.pNext = &dedicatedInfo;

    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.pNext = &importInfo;
    allocateInfo.allocationSize = bufferProps.allocationSize;
    allocateInfo.memoryTypeIndex = memoryTypeIndex;

    result = fns.allocate_memory(
            context->vulkan->device, &allocateInfo, nullptr, &outImportedTexture->memory);
    if (result != VK_SUCCESS) {
        destroyImportedHardwareBufferTexture(context, fns, outImportedTexture);
        return false;
    }

    result = fns.bind_image_memory(
            context->vulkan->device, outImportedTexture->image, outImportedTexture->memory, 0);
    if (result != VK_SUCCESS) {
        destroyImportedHardwareBufferTexture(context, fns, outImportedTexture);
        return false;
    }

    struct pl_vulkan_wrap_params wrapParams = {};
    wrapParams.image = outImportedTexture->image;
    wrapParams.width = frameWidth;
    wrapParams.height = frameHeight;
    wrapParams.format = formatProps.format;
    wrapParams.usage = imageInfo.usage;
    outImportedTexture->texture = pl_vulkan_wrap(context->vulkan->gpu, &wrapParams);
    if (!outImportedTexture->texture) {
        destroyImportedHardwareBufferTexture(context, fns, outImportedTexture);
        return false;
    }

    struct pl_vulkan_release_params releaseParams = {};
    releaseParams.tex = outImportedTexture->texture;
    releaseParams.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    releaseParams.qf = VK_QUEUE_FAMILY_IGNORED;
    pl_vulkan_release_ex(context->vulkan->gpu, &releaseParams);
    return true;
}

bool fillSourceFrameFromImportedTexture(
        const ImportedHardwareBufferTexture &importedTexture,
        int frameWidth,
        int frameHeight,
        pl_frame *sourceFrame) {
    if (!sourceFrame || !importedTexture.texture || frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }
    memset(sourceFrame, 0, sizeof(*sourceFrame));
    int planeCount = 1;
    if (importedTexture.texture->params.format &&
        importedTexture.texture->params.format->num_planes > 0) {
        planeCount = std::min(importedTexture.texture->params.format->num_planes, PL_MAX_PLANES);
    }
    sourceFrame->num_planes = planeCount;
    for (int planeIndex = 0; planeIndex < planeCount; ++planeIndex) {
        pl_tex planeTexture =
                (planeCount > 1) ? importedTexture.texture->planes[planeIndex] : importedTexture.texture;
        if (!planeTexture) {
            return false;
        }
        struct pl_plane *plane = &sourceFrame->planes[planeIndex];
        plane->texture = planeTexture;
        plane->flipped = false;
        plane->shift_x = 0.0f;
        plane->shift_y = 0.0f;
        memset(plane->component_mapping, -1, sizeof(plane->component_mapping));

        const int numComponents = planeTexture->params.format
                                          ? std::max(1, planeTexture->params.format->num_components)
                                          : 1;
        if (planeCount == 2 && planeIndex == 1 && numComponents >= 2) {
            plane->components = 2;
            plane->component_mapping[0] = 1;
            plane->component_mapping[1] = 2;
        } else if (planeCount >= 3 && planeIndex < 3) {
            plane->components = 1;
            plane->component_mapping[0] = planeIndex;
        } else {
            plane->components = std::min(3, numComponents);
            for (int c = 0; c < plane->components; ++c) {
                plane->component_mapping[c] = c;
            }
        }
    }
    sourceFrame->repr = pl_color_repr_uhdtv;
    sourceFrame->color = pl_color_space_hdr10;
    sourceFrame->crop.x0 = 0.0f;
    sourceFrame->crop.y0 = 0.0f;
    sourceFrame->crop.x1 = static_cast<float>(frameWidth);
    sourceFrame->crop.y1 = static_cast<float>(frameHeight);
    pl_frame_set_chroma_location(sourceFrame, PL_CHROMA_LEFT);
    return true;
}

bool uploadPlaneToPureRenderer(
        pl_gpu gpu,
        const AHardwareBuffer_Plane &bufferPlane,
        int width,
        int height,
        int componentCount,
        const int componentMap[4],
        pl_tex *texture,
        pl_plane *outputPlane) {
    if (!gpu || !texture || !outputPlane || !bufferPlane.data || width <= 0 || height <= 0 ||
        componentCount <= 0) {
        return false;
    }
    struct pl_plane_data planeData = {};
    planeData.type = PL_FMT_UNORM;
    planeData.width = width;
    planeData.height = height;
    for (int componentIndex = 0; componentIndex < 4; ++componentIndex) {
        if (componentIndex < componentCount) {
            planeData.component_size[componentIndex] = 8;
            planeData.component_pad[componentIndex] = 0;
            planeData.component_map[componentIndex] = componentMap[componentIndex];
        } else {
            planeData.component_size[componentIndex] = 0;
            planeData.component_pad[componentIndex] = 0;
            planeData.component_map[componentIndex] = -1;
        }
    }
    planeData.pixel_stride = static_cast<size_t>(std::max<uint32_t>(1, bufferPlane.pixelStride));
    planeData.row_stride = static_cast<size_t>(std::max<uint32_t>(
            static_cast<uint32_t>(planeData.pixel_stride * width), bufferPlane.rowStride));
    planeData.pixels = bufferPlane.data;

    if (!pl_upload_plane(gpu, outputPlane, texture, &planeData)) {
        return false;
    }
    outputPlane->flipped = false;
    outputPlane->shift_x = 0.0f;
    outputPlane->shift_y = 0.0f;
    return true;
}

void maybeLogCpuReadbackChromaLayout(
        PureHardwareRendererContext *context,
        AHardwareBuffer *hardwareBuffer,
        const AHardwareBuffer_Planes &planes) {
    if (!context || context->cpu_chroma_layout_logged) {
        return;
    }
    context->cpu_chroma_layout_logged = true;

    const AHardwareBufferDescribeFn describeFn = getHardwareBufferDescribeFn();
    AHardwareBuffer_Desc desc = {};
    if (describeFn && hardwareBuffer) {
        describeFn(hardwareBuffer, &desc);
    }

    const uint32_t planeCount = planes.planeCount;
    const uint32_t uvPixelStride = planeCount > 1 ? planes.planes[1].pixelStride : 0;
    const uint32_t uvRowStride = planeCount > 1 ? planes.planes[1].rowStride : 0;
    const uint32_t yPixelStride = planeCount > 0 ? planes.planes[0].pixelStride : 0;
    const uint32_t yRowStride = planeCount > 0 ? planes.planes[0].rowStride : 0;
    const uint32_t vPixelStride = planeCount > 2 ? planes.planes[2].pixelStride : 0;
    const uint32_t vRowStride = planeCount > 2 ? planes.planes[2].rowStride : 0;

    const char *layout = "UNKNOWN";
    int64_t uvAliasDelta = INT64_MAX;
    if (planeCount >= 3 && planes.planes[1].pixelStride == 1 && planes.planes[2].pixelStride == 1) {
        layout = "I420_PLANAR";
    } else if (planeCount >= 2 && planes.planes[1].pixelStride == 2) {
        layout = "NV12_INTERLEAVED";
        if (planeCount >= 3 && planes.planes[1].data && planes.planes[2].data &&
            planes.planes[2].rowStride == planes.planes[1].rowStride &&
            planes.planes[2].pixelStride == 2) {
            uvAliasDelta = reinterpret_cast<intptr_t>(planes.planes[2].data) -
                           reinterpret_cast<intptr_t>(planes.planes[1].data);
            if (uvAliasDelta == -1) {
                layout = "NV21_INTERLEAVED";
            } else if (uvAliasDelta == 1) {
                layout = "NV12_INTERLEAVED";
            } else {
                layout = "NV12_OR_NV21_INTERLEAVED";
            }
        }
    }

    if (uvAliasDelta == INT64_MAX) {
        __android_log_print(
                ANDROID_LOG_INFO,
                LOG_TAG,
                "DV5_HW_RENDER: cpu-readback chromaLayout=%s format=%u planeCount=%u "
                "y(pixel=%u,row=%u) uv(pixel=%u,row=%u) v(pixel=%u,row=%u)",
                layout,
                desc.format,
                planeCount,
                yPixelStride,
                yRowStride,
                uvPixelStride,
                uvRowStride,
                vPixelStride,
                vRowStride);
    } else {
        __android_log_print(
                ANDROID_LOG_INFO,
                LOG_TAG,
                "DV5_HW_RENDER: cpu-readback chromaLayout=%s format=%u planeCount=%u "
                "y(pixel=%u,row=%u) uv(pixel=%u,row=%u) v(pixel=%u,row=%u) uvAliasDelta=%" PRId64,
                layout,
                desc.format,
                planeCount,
                yPixelStride,
                yRowStride,
                uvPixelStride,
                uvRowStride,
                vPixelStride,
                vRowStride,
                uvAliasDelta);
    }
}

bool renderHardwareBufferWithPureRenderer(
        JNIEnv *env,
        PureHardwareRendererContext *context,
        AHardwareBuffer *hardwareBuffer,
        int64_t presentationTimeUs,
        int frameWidth,
        int frameHeight) {
    if (!env || !context || !context->vulkan || !context->swapchain || !context->renderer ||
        !hardwareBuffer || frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }
    struct pl_frame sourceFrame = {};
    ImportedHardwareBufferTexture importedTexture = {};
    VulkanDeviceFns vulkanFns = {};
    bool usingImportedTexture = false;
    auto cleanupImportedTexture = [&]() {
        if (!usingImportedTexture) {
            return;
        }
        pl_gpu_finish(context->vulkan->gpu);
        destroyImportedHardwareBufferTexture(context, vulkanFns, &importedTexture);
        usingImportedTexture = false;
    };

    if (loadVulkanDeviceFns(context, &vulkanFns) &&
        importHardwareBufferAsVulkanTexture(
                context, hardwareBuffer, frameWidth, frameHeight, &importedTexture) &&
        fillSourceFrameFromImportedTexture(importedTexture, frameWidth, frameHeight, &sourceFrame)) {
        usingImportedTexture = true;
    } else {
        destroyImportedHardwareBufferTexture(context, vulkanFns, &importedTexture);
        AHardwareBufferLockPlanesFn lockPlanesFn = getHardwareBufferLockPlanesFn();
        AHardwareBufferUnlockFn unlockFn = getHardwareBufferUnlockFn();
        if (!lockPlanesFn || !unlockFn) {
            return false;
        }
        AHardwareBuffer_Planes planes = {};
        int lockResult =
                lockPlanesFn(hardwareBuffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &planes);
        if (lockResult != 0) {
            return false;
        }
        maybeLogCpuReadbackChromaLayout(context, hardwareBuffer, planes);
        if (planes.planeCount < 2) {
            unlockFn(hardwareBuffer, nullptr);
            LOGE("DV5_HW_PURE: expected >=2 CPU planes, got %u", planes.planeCount);
            return false;
        }

        const int chromaWidth = (frameWidth + 1) / 2;
        const int chromaHeight = (frameHeight + 1) / 2;
        const int yMap[4] = {0, -1, -1, -1};
        bool uploaded = uploadPlaneToPureRenderer(
                context->vulkan->gpu,
                planes.planes[0],
                frameWidth,
                frameHeight,
                1,
                yMap,
                &context->plane_textures[0],
                &sourceFrame.planes[0]);
        if (!uploaded) {
            unlockFn(hardwareBuffer, nullptr);
            return false;
        }

        bool usesInterleavedUvPlane = planes.planes[1].pixelStride == 2;
        if (usesInterleavedUvPlane) {
            // YUV_420_888 on Shield typically arrives as NV12 (pixelStride=2).
            sourceFrame.num_planes = 2;
            const AHardwareBuffer_Plane *uvPlane = &planes.planes[1];
            int uvMap[4] = {1, 2, -1, -1};
            if (planes.planeCount >= 3 && planes.planes[2].pixelStride == 2 &&
                planes.planes[2].rowStride == planes.planes[1].rowStride &&
                planes.planes[2].data != nullptr) {
                const intptr_t delta = reinterpret_cast<intptr_t>(planes.planes[2].data) -
                                       reinterpret_cast<intptr_t>(planes.planes[1].data);
                if (delta == -1) {
                    // NV21-style aliasing: V appears before U, so swap semantic mapping.
                    uvPlane = &planes.planes[2];
                    uvMap[0] = 2;
                    uvMap[1] = 1;
                }
            }
            uploaded = uploadPlaneToPureRenderer(
                    context->vulkan->gpu,
                    *uvPlane,
                    chromaWidth,
                    chromaHeight,
                    2,
                    uvMap,
                    &context->plane_textures[1],
                    &sourceFrame.planes[1]);
        } else if (planes.planeCount >= 3 && planes.planes[1].pixelStride == 1 &&
                   planes.planes[2].pixelStride == 1) {
            sourceFrame.num_planes = 3;
            const int uMap[4] = {1, -1, -1, -1};
            const int vMap[4] = {2, -1, -1, -1};
            uploaded = uploadPlaneToPureRenderer(
                    context->vulkan->gpu,
                    planes.planes[1],
                    chromaWidth,
                    chromaHeight,
                    1,
                    uMap,
                    &context->plane_textures[1],
                    &sourceFrame.planes[1]);
            uploaded = uploaded && uploadPlaneToPureRenderer(
                    context->vulkan->gpu,
                    planes.planes[2],
                    chromaWidth,
                    chromaHeight,
                    1,
                    vMap,
                    &context->plane_textures[2],
                    &sourceFrame.planes[2]);
        } else {
            sourceFrame.num_planes = 0;
            uploaded = false;
            LOGE(
                    "DV5_HW_PURE: unsupported CPU chroma layout planeCount=%u uvPixelStride=%u",
                    planes.planeCount,
                    planes.planes[1].pixelStride);
        }
        unlockFn(hardwareBuffer, nullptr);
        if (!uploaded) {
            return false;
        }
        sourceFrame.repr = pl_color_repr_uhdtv;
        sourceFrame.color = pl_color_space_hdr10;
        sourceFrame.crop.x0 = 0.0f;
        sourceFrame.crop.y0 = 0.0f;
        sourceFrame.crop.x1 = static_cast<float>(frameWidth);
        sourceFrame.crop.y1 = static_cast<float>(frameHeight);
        pl_frame_set_chroma_location(&sourceFrame, PL_CHROMA_LEFT);
    }

    pl_dovi_metadata doviMetadata = {};
    int64_t matchedRpuSampleTimeUs = AV_NOPTS_VALUE;
    const bool appliedDoviFromRpu = maybeApplyExternalDv5RpuToPureFrame(
            presentationTimeUs, &sourceFrame, &doviMetadata, &matchedRpuSampleTimeUs);

    int resizeWidth = frameWidth;
    int resizeHeight = frameHeight;
    if (!pl_swapchain_resize(context->swapchain, &resizeWidth, &resizeHeight)) {
        cleanupImportedTexture();
        return false;
    }
    struct pl_swapchain_frame swapchainFrame = {};
    if (!pl_swapchain_start_frame(context->swapchain, &swapchainFrame)) {
        cleanupImportedTexture();
        return false;
    }
    struct pl_frame targetFrame = {};
    pl_frame_from_swapchain(&targetFrame, &swapchainFrame);
    targetFrame.color = pl_color_space_bt709;

    const bool rendered = pl_render_image(
            context->renderer, &sourceFrame, &targetFrame, &pl_render_default_params);
    if (!rendered) {
        cleanupImportedTexture();
        return false;
    }
    if (appliedDoviFromRpu && matchedRpuSampleTimeUs != AV_NOPTS_VALUE) {
        consumeExternalDv5RpuBySampleTime(matchedRpuSampleTimeUs, "pure");
    }
    if (!pl_swapchain_submit_frame(context->swapchain)) {
        cleanupImportedTexture();
        return false;
    }
    pl_swapchain_swap_buffers(context->swapchain);
    cleanupImportedTexture();
    return true;
}
#endif
} // namespace

void ffmpegSetExperimentalDv5HardwareToneMapRpuBridgeEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(gExternalRpuBridgeState.mutex);
    gExternalRpuBridgeState.enabled = enabled;
    gExternalRpuBridgeState.queuedByTimeUs.clear();
    gExternalRpuBridgeState.pushedCount = 0;
    gExternalRpuBridgeState.matchedCount = 0;
    gExternalRpuBridgeState.missCount = 0;
    gExternalRpuBridgeState.dropNoPtsCount = 0;
    gExternalRpuBridgeState.dropOverflowCount = 0;
    __android_log_print(
            ANDROID_LOG_INFO, LOG_TAG, "DV5_HW_RPU: bridgeEnabled=%d", enabled ? 1 : 0);
}

void ffmpegPushExperimentalDv5HardwareRpuSample(
        int64_t sampleTimeUs, const uint8_t *payload, size_t payloadSize) {
    if (!payload || payloadSize == 0) return;
    if (sampleTimeUs == AV_NOPTS_VALUE) {
        std::lock_guard<std::mutex> lock(gExternalRpuBridgeState.mutex);
        if (gExternalRpuBridgeState.enabled) {
            gExternalRpuBridgeState.dropNoPtsCount++;
        }
        return;
    }
    std::lock_guard<std::mutex> lock(gExternalRpuBridgeState.mutex);
    if (!gExternalRpuBridgeState.enabled) return;
    gExternalRpuBridgeState.queuedByTimeUs[sampleTimeUs] =
            std::vector<uint8_t>(payload, payload + payloadSize);
    gExternalRpuBridgeState.pushedCount++;
    while (gExternalRpuBridgeState.queuedByTimeUs.size() > kExternalRpuMaxEntries) {
        gExternalRpuBridgeState.queuedByTimeUs.erase(gExternalRpuBridgeState.queuedByTimeUs.begin());
        gExternalRpuBridgeState.dropOverflowCount++;
    }
}

void ffmpegNotifyExperimentalDv5HardwareFramePresented(int64_t presentationTimeUs) {
    if (presentationTimeUs == AV_NOPTS_VALUE) return;
    std::lock_guard<std::mutex> lock(gExternalRpuBridgeState.mutex);
    if (!gExternalRpuBridgeState.enabled || gExternalRpuBridgeState.queuedByTimeUs.empty()) return;
    const int64_t thresholdUs = presentationTimeUs - kExternalRpuPruneWindowUs;
    auto it = gExternalRpuBridgeState.queuedByTimeUs.begin();
    while (it != gExternalRpuBridgeState.queuedByTimeUs.end() && it->first < thresholdUs) {
        it = gExternalRpuBridgeState.queuedByTimeUs.erase(it);
        gExternalRpuBridgeState.dropOverflowCount++;
    }
}

bool ffmpegRenderExperimentalDv5HardwareFrame(
        JNIEnv *env,
        int64_t presentationTimeUs,
        jobject hardwareBufferObject,
        int32_t displayedWidth,
        int32_t displayedHeight,
        jobject outputSurface) {
#if !FFMPEG_TONEMAP_FILTERS_ACTIVE
    (void) env;
    (void) presentationTimeUs;
    (void) hardwareBufferObject;
    (void) displayedWidth;
    (void) displayedHeight;
    (void) outputSurface;
    return false;
#else
    if (!env || !hardwareBufferObject || !outputSurface) {
        return false;
    }

    AHardwareBufferFromJniFn fromJni = getHardwareBufferFromJniFn();
    if (!fromJni) {
        LOGE("DV5_HW_RENDER: AHardwareBuffer_fromHardwareBuffer unavailable.");
        return false;
    }
    AHardwareBuffer *hardwareBuffer = fromJni(env, hardwareBufferObject);
    if (!hardwareBuffer) {
        LOGE("DV5_HW_RENDER: failed to map HardwareBuffer.");
        return false;
    }

    std::lock_guard<std::mutex> lock(gHardwareToneMapContextMutex);
    JniContext *jniContext = ensureHardwareToneMapContext();
    if (!jniContext) {
        return false;
    }

    AVFrame *sourceFrame = av_frame_alloc();
    if (!sourceFrame) {
        LOGE("DV5_HW_RENDER: failed to allocate source frame.");
        return false;
    }

    if (!copyHardwareBufferToAvFrame(
                hardwareBuffer,
                displayedWidth,
                displayedHeight,
                sourceFrame)) {
        av_frame_free(&sourceFrame);
        return false;
    }

    sourceFrame->pts = presentationTimeUs;
    sourceFrame->best_effort_timestamp = presentationTimeUs;
    sourceFrame->color_trc = AVCOL_TRC_SMPTE2084;
    sourceFrame->color_primaries = AVCOL_PRI_BT2020;
    sourceFrame->colorspace = AVCOL_SPC_BT2020_NCL;
    sourceFrame->color_range = AVCOL_RANGE_MPEG;
    AVFrame *outputFrame = applyToneMapToFrame(jniContext, sourceFrame);
    int renderResult = renderAvFrameToSurface(
            env,
            jniContext,
            outputFrame,
            outputSurface,
            displayedWidth,
            displayedHeight);

    av_frame_free(&sourceFrame);
    return renderResult == VIDEO_DECODER_SUCCESS;
#endif
}

bool ffmpegRenderExperimentalDv5HardwareFramePure(
        JNIEnv *env,
        int64_t presentationTimeUs,
        jobject hardwareBufferObject,
        int32_t displayedWidth,
        int32_t displayedHeight,
        jobject outputSurface) {
#if !FFMPEG_TONEMAP_FILTERS_ACTIVE
    (void) env;
    (void) presentationTimeUs;
    (void) hardwareBufferObject;
    (void) displayedWidth;
    (void) displayedHeight;
    (void) outputSurface;
    return false;
#else
    if (!env || !hardwareBufferObject || !outputSurface) {
        return false;
    }
    const int frameWidth = displayedWidth > 0 ? displayedWidth : 0;
    const int frameHeight = displayedHeight > 0 ? displayedHeight : 0;
    if (frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }

    AHardwareBufferFromJniFn fromJni = getHardwareBufferFromJniFn();
    if (!fromJni) {
        return false;
    }
    AHardwareBuffer *hardwareBuffer = fromJni(env, hardwareBufferObject);
    if (!hardwareBuffer) {
        return false;
    }

    std::lock_guard<std::mutex> lock(gPureHardwareRendererContextMutex);
    if (!ensurePureHardwareRendererContext(env, outputSurface, frameWidth, frameHeight) ||
        !gPureHardwareRendererContext) {
        return false;
    }
    const bool rendered = renderHardwareBufferWithPureRenderer(
            env,
            gPureHardwareRendererContext,
            hardwareBuffer,
            presentationTimeUs,
            frameWidth,
            frameHeight);
    if (rendered) {
        ffmpegNotifyExperimentalDv5HardwareFramePresented(presentationTimeUs);
    }
    return rendered;
#endif
}

#if FFMPEG_TONEMAP_FILTERS_ACTIVE
static const char *kDv5ToneMapFilterGraph =
        "libplacebo=tonemapping=bt.2390:peak_detect=1:colorspace=bt709:color_primaries=bt709:"
        "color_trc=bt709:range=tv,format=pix_fmts=yuv420p";
#endif


// YUV plane indices.
const int kPlaneY = 0;
const int kPlaneU = 1;
const int kPlaneV = 2;
const int kMaxPlanes = 3;

// Android YUV format. See:
// https://developer.android.com/reference/android/graphics/ImageFormat.html#YV12.
const int kImageFormatYV12 = 0x32315659;

struct JniContext {
    ~JniContext() {
#if FFMPEG_TONEMAP_FILTERS_ACTIVE
        if (filter_graph) {
            avfilter_graph_free(&filter_graph);
        }
        if (filtered_frame) {
            av_frame_free(&filtered_frame);
        }
#endif
        if (native_window) {
            ANativeWindow_release(native_window);
        }
    }

    bool MaybeAcquireNativeWindow(JNIEnv *env, jobject new_surface) {
        if (surface == new_surface) {
            return true;
        }
        if (native_window) {
            ANativeWindow_release(native_window);
        }
        native_window_width = 0;
        native_window_height = 0;
        native_window = ANativeWindow_fromSurface(env, new_surface);
        if (native_window == nullptr) {
            LOGE("kJniStatusANativeWindowError");
            surface = nullptr;
            return false;
        }
        surface = new_surface;
        return true;
    }

    jfieldID data_field{};
    jfieldID yuvPlanes_field{};
    jfieldID yuvStrides_field{};
    jmethodID init_for_yuv_frame_method{};
    jmethodID init_method{};

    AVCodecContext *codecContext{};
    SwsContext *swsContext{};
#if FFMPEG_TONEMAP_FILTERS_ACTIVE
    AVFilterGraph *filter_graph{};
    AVFilterContext *buffer_source_context{};
    AVFilterContext *buffer_sink_context{};
    AVFrame *filtered_frame{};
    int filter_width = 0;
    int filter_height = 0;
    AVPixelFormat filter_input_format = AV_PIX_FMT_NONE;
#endif
    bool enable_tone_map_to_sdr = false;
    int64_t last_queued_input_time_us = AV_NOPTS_VALUE;

    ANativeWindow *native_window = nullptr;
    jobject surface = nullptr;
    int native_window_width = 0;
    int native_window_height = 0;
};

namespace {
int64_t resolveFrameTimeUs(const JniContext *jniContext, const AVFrame *frame) {
    int64_t frameTimeUs = frame->best_effort_timestamp;
    if (frameTimeUs == AV_NOPTS_VALUE) {
        frameTimeUs = frame->pts;
    }
    if (frameTimeUs == AV_NOPTS_VALUE && jniContext) {
        frameTimeUs = jniContext->last_queued_input_time_us;
    }
    return frameTimeUs;
}

bool tryGetExternalDv5RpuForTimeUs(
        int64_t frameTimeUs,
        bool consumeMatch,
        std::vector<uint8_t> *matchedRpu,
        int64_t *matchedSampleTimeUs,
        const char *source) {
    if (!matchedRpu || frameTimeUs == AV_NOPTS_VALUE) {
        return false;
    }
    matchedRpu->clear();
    if (matchedSampleTimeUs) {
        *matchedSampleTimeUs = AV_NOPTS_VALUE;
    }

    std::lock_guard<std::mutex> lock(gExternalRpuBridgeState.mutex);
    if (!gExternalRpuBridgeState.enabled || gExternalRpuBridgeState.queuedByTimeUs.empty()) {
        return false;
    }

    auto floorIt = gExternalRpuBridgeState.queuedByTimeUs.upper_bound(frameTimeUs);
    if (floorIt != gExternalRpuBridgeState.queuedByTimeUs.begin()) {
        --floorIt;
    } else {
        floorIt = gExternalRpuBridgeState.queuedByTimeUs.end();
    }
    auto ceilIt = gExternalRpuBridgeState.queuedByTimeUs.lower_bound(frameTimeUs);
    auto bestIt = gExternalRpuBridgeState.queuedByTimeUs.end();
    int64_t bestDelta = INT64_MAX;
    if (floorIt != gExternalRpuBridgeState.queuedByTimeUs.end()) {
        bestIt = floorIt;
        bestDelta = std::llabs(frameTimeUs - floorIt->first);
    }
    if (ceilIt != gExternalRpuBridgeState.queuedByTimeUs.end()) {
        int64_t ceilDelta = std::llabs(ceilIt->first - frameTimeUs);
        if (bestIt == gExternalRpuBridgeState.queuedByTimeUs.end() || ceilDelta < bestDelta) {
            bestIt = ceilIt;
            bestDelta = ceilDelta;
        }
    }
    if (bestIt == gExternalRpuBridgeState.queuedByTimeUs.end() ||
        bestDelta > kExternalRpuMatchToleranceUs) {
        if (consumeMatch) {
            gExternalRpuBridgeState.missCount++;
            maybeLogExternalRpuSummaryLocked(gExternalRpuBridgeState, source);
        }
        return false;
    }

    *matchedRpu = bestIt->second;
    if (matchedSampleTimeUs) {
        *matchedSampleTimeUs = bestIt->first;
    }
    if (consumeMatch) {
        gExternalRpuBridgeState.queuedByTimeUs.erase(bestIt);
        gExternalRpuBridgeState.matchedCount++;
        maybeLogExternalRpuSummaryLocked(gExternalRpuBridgeState, source);
    }
    return !matchedRpu->empty();
}

void consumeExternalDv5RpuBySampleTime(int64_t sampleTimeUs, const char *source) {
    if (sampleTimeUs == AV_NOPTS_VALUE) {
        return;
    }
    std::lock_guard<std::mutex> lock(gExternalRpuBridgeState.mutex);
    if (!gExternalRpuBridgeState.enabled) {
        return;
    }
    auto it = gExternalRpuBridgeState.queuedByTimeUs.find(sampleTimeUs);
    if (it != gExternalRpuBridgeState.queuedByTimeUs.end()) {
        gExternalRpuBridgeState.queuedByTimeUs.erase(it);
        gExternalRpuBridgeState.matchedCount++;
        maybeLogExternalRpuSummaryLocked(gExternalRpuBridgeState, source);
    }
}

void maybeAttachExternalDv5RpuSideData(JniContext *jniContext, AVFrame *frame) {
    if (!jniContext || !frame) return;
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA) != nullptr ||
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER) != nullptr) {
        return;
    }

    const int64_t frameTimeUs = resolveFrameTimeUs(jniContext, frame);
    if (frameTimeUs == AV_NOPTS_VALUE) {
        std::lock_guard<std::mutex> lock(gExternalRpuBridgeState.mutex);
        if (gExternalRpuBridgeState.enabled) {
            gExternalRpuBridgeState.dropNoPtsCount++;
        }
        return;
    }

    std::vector<uint8_t> matchedRpu;
    (void) tryGetExternalDv5RpuForTimeUs(
            frameTimeUs,
            /* consumeMatch= */ true,
            &matchedRpu,
            /* matchedSampleTimeUs= */ nullptr,
            "frame");

    if (matchedRpu.empty()) return;

    AVFrameSideData *sideData = av_frame_new_side_data(
            frame, AV_FRAME_DATA_DOVI_RPU_BUFFER, static_cast<size_t>(matchedRpu.size()));
    if (!sideData || !sideData->data) {
        return;
    }
    memcpy(sideData->data, matchedRpu.data(), matchedRpu.size());

    if (frame->color_trc == AVCOL_TRC_UNSPECIFIED) {
        frame->color_trc = AVCOL_TRC_SMPTE2084;
    }
    if (frame->color_primaries == AVCOL_PRI_UNSPECIFIED) {
        frame->color_primaries = AVCOL_PRI_BT2020;
    }
    if (frame->colorspace == AVCOL_SPC_UNSPECIFIED) {
        frame->colorspace = AVCOL_SPC_BT2020_NCL;
    }
}

#if FFMPEG_TONEMAP_FILTERS_ACTIVE
bool maybeApplyExternalDv5RpuToPureFrame(
        int64_t presentationTimeUs,
        pl_frame *sourceFrame,
        pl_dovi_metadata *doviMetadata,
        int64_t *matchedSampleTimeUs) {
    if (!sourceFrame || !doviMetadata) {
        return false;
    }
    std::vector<uint8_t> matchedRpu;
    int64_t sampleTimeUs = AV_NOPTS_VALUE;
    if (!tryGetExternalDv5RpuForTimeUs(
                presentationTimeUs,
                /* consumeMatch= */ false,
                &matchedRpu,
                &sampleTimeUs,
                "pure-preview") ||
        matchedRpu.size() <= 2) {
        return false;
    }

    const int kHevcDoviRpuNalType = 62;
    size_t rpuOffset = 0;
    if (((matchedRpu[0] >> 1) & 0x3F) == kHevcDoviRpuNalType) {
        rpuOffset = 2;
    }
    if (matchedRpu.size() <= rpuOffset) {
        return false;
    }

    DOVIContext doviContext = {};
    const int parseResult = ff_dovi_rpu_parse(
            &doviContext,
            matchedRpu.data() + rpuOffset,
            matchedRpu.size() - rpuOffset);
    if (parseResult < 0 || !doviContext.mapping || !doviContext.color ||
        !doviContext.header.disable_residual_flag) {
        ff_dovi_ctx_unref(&doviContext);
        return false;
    }

    const AVDOVIDataMapping *mapping = doviContext.mapping;
    const AVDOVIColorMetadata *color = doviContext.color;
    const AVDOVIRpuDataHeader *header = &doviContext.header;

    for (int i = 0; i < 3; i++) {
        doviMetadata->nonlinear_offset[i] = static_cast<float>(av_q2d(color->ycc_to_rgb_offset[i]));
    }
    for (int i = 0; i < 9; i++) {
        float *nonlinear = &doviMetadata->nonlinear.m[0][0];
        float *linear = &doviMetadata->linear.m[0][0];
        nonlinear[i] = static_cast<float>(av_q2d(color->ycc_to_rgb_matrix[i]));
        linear[i] = static_cast<float>(av_q2d(color->rgb_to_lms_matrix[i]));
    }
    for (int c = 0; c < 3; c++) {
        const AVDOVIReshapingCurve *curve = &mapping->curves[c];
        auto *component = &doviMetadata->comp[c];
        component->num_pivots = curve->num_pivots;
        for (int i = 0; i < curve->num_pivots; i++) {
            const float pivotScale = 1.0f / static_cast<float>((1 << header->bl_bit_depth) - 1);
            component->pivots[i] = pivotScale * curve->pivots[i];
        }
        for (int i = 0; i < curve->num_pivots - 1; i++) {
            const float coeffScale = 1.0f / static_cast<float>(1 << header->coef_log2_denom);
            component->method[i] = curve->mapping_idc[i];
            switch (curve->mapping_idc[i]) {
                case AV_DOVI_MAPPING_POLYNOMIAL:
                    for (int k = 0; k < 3; k++) {
                        component->poly_coeffs[i][k] =
                                (k <= curve->poly_order[i]) ? coeffScale * curve->poly_coef[i][k] : 0.0f;
                    }
                    break;
                case AV_DOVI_MAPPING_MMR:
                    component->mmr_order[i] = curve->mmr_order[i];
                    component->mmr_constant[i] = coeffScale * curve->mmr_constant[i];
                    for (int j = 0; j < curve->mmr_order[i]; j++) {
                        for (int k = 0; k < 7; k++) {
                            component->mmr_coeffs[i][j][k] = coeffScale * curve->mmr_coef[i][j][k];
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }

    sourceFrame->repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
    sourceFrame->repr.dovi = doviMetadata;
    sourceFrame->color.primaries = PL_COLOR_PRIM_BT_2020;
    sourceFrame->color.transfer = PL_COLOR_TRC_PQ;
    sourceFrame->color.hdr.min_luma =
            pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, color->source_min_pq / 4095.0f);
    sourceFrame->color.hdr.max_luma =
            pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, color->source_max_pq / 4095.0f);

    if (matchedSampleTimeUs) {
        *matchedSampleTimeUs = sampleTimeUs;
    }
    ff_dovi_ctx_unref(&doviContext);
    return true;
}
#endif

static void copyPlaneWithPixelStride(
        uint8_t *dst,
        int dstStride,
        const uint8_t *src,
        int srcStride,
        int pixelStride,
        int componentByteOffset,
        int width,
        int height) {
    if (!dst || !src || dstStride <= 0 || srcStride <= 0 || width <= 0 || height <= 0) return;
    const int safePixelStride = std::max(1, pixelStride);
    const int safeComponentByteOffset =
            std::max(0, std::min(componentByteOffset, safePixelStride - 1));
    for (int y = 0; y < height; ++y) {
        const uint8_t *srcRow = src + static_cast<size_t>(y) * srcStride;
        uint8_t *dstRow = dst + static_cast<size_t>(y) * dstStride;
        for (int x = 0; x < width; ++x) {
            dstRow[x] = srcRow[x * safePixelStride + safeComponentByteOffset];
        }
    }
}

static void copyInterleavedChromaWithPixelStride(
        uint8_t *dstU,
        int dstUStride,
        uint8_t *dstV,
        int dstVStride,
        const uint8_t *src,
        int srcStride,
        int pixelStride,
        int uComponentByteOffset,
        int vComponentByteOffset,
        int width,
        int height) {
    if (!dstU || !dstV || !src || dstUStride <= 0 || dstVStride <= 0 || srcStride <= 0 ||
        width <= 0 || height <= 0) {
        return;
    }
    const int safePixelStride = std::max(1, pixelStride);
    const int safeUByteOffset = std::max(0, std::min(uComponentByteOffset, safePixelStride - 1));
    const int safeVByteOffset = std::max(0, std::min(vComponentByteOffset, safePixelStride - 1));
    for (int y = 0; y < height; ++y) {
        const uint8_t *srcRow = src + static_cast<size_t>(y) * srcStride;
        uint8_t *dstURow = dstU + static_cast<size_t>(y) * dstUStride;
        uint8_t *dstVRow = dstV + static_cast<size_t>(y) * dstVStride;
        for (int x = 0; x < width; ++x) {
            const int srcOffset = x * safePixelStride;
            dstURow[x] = srcRow[srcOffset + safeUByteOffset];
            dstVRow[x] = srcRow[srcOffset + safeVByteOffset];
        }
    }
}

bool copyHardwareBufferToAvFrame(
        AHardwareBuffer *hardwareBuffer, int width, int height, AVFrame *destinationFrame) {
    if (!hardwareBuffer || !destinationFrame) {
        return false;
    }

    AHardwareBufferDescribeFn describeFn = getHardwareBufferDescribeFn();
    AHardwareBufferLockPlanesFn lockPlanesFn = getHardwareBufferLockPlanesFn();
    AHardwareBufferUnlockFn unlockFn = getHardwareBufferUnlockFn();
    if (!describeFn || !lockPlanesFn || !unlockFn) {
        LOGE("DV5_HW_RENDER: required AHardwareBuffer symbols unavailable.");
        return false;
    }

    AHardwareBuffer_Desc desc{};
    describeFn(hardwareBuffer, &desc);
    const int frameWidth = width > 0 ? width : static_cast<int>(desc.width);
    const int frameHeight = height > 0 ? height : static_cast<int>(desc.height);
    if (frameWidth <= 0 || frameHeight <= 0) {
        LOGE("DV5_HW_RENDER: invalid frame size %dx%d", frameWidth, frameHeight);
        return false;
    }

    AHardwareBuffer_Planes planes{};
    int lockResult =
            lockPlanesFn(hardwareBuffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &planes);
    if (lockResult != 0) {
        LOGE("DV5_HW_RENDER: AHardwareBuffer_lockPlanes failed: %d", lockResult);
        return false;
    }

    destinationFrame->format = AV_PIX_FMT_YUV420P;
    destinationFrame->width = frameWidth;
    destinationFrame->height = frameHeight;
    if (av_frame_get_buffer(destinationFrame, 32) < 0) {
        unlockFn(hardwareBuffer, nullptr);
        LOGE("DV5_HW_RENDER: failed to allocate destination frame buffer.");
        return false;
    }

    if (planes.planeCount < 2) {
        unlockFn(hardwareBuffer, nullptr);
        LOGE("DV5_HW_RENDER: expected >=2 planes, got %u", planes.planeCount);
        return false;
    }

    const int chromaWidth = (frameWidth + 1) / 2;
    const int chromaHeight = (frameHeight + 1) / 2;
    const bool isP010Format = desc.format == AHARDWAREBUFFER_FORMAT_YCbCr_P010;
    const int yComponentByteOffset =
            (isP010Format && planes.planes[0].pixelStride >= 2) ? 1 : 0;

    copyPlaneWithPixelStride(
            destinationFrame->data[0],
            destinationFrame->linesize[0],
            reinterpret_cast<const uint8_t *>(planes.planes[0].data),
            static_cast<int>(planes.planes[0].rowStride),
            static_cast<int>(planes.planes[0].pixelStride),
            yComponentByteOffset,
            frameWidth,
            frameHeight);
    if (planes.planeCount >= 3) {
        copyPlaneWithPixelStride(
                destinationFrame->data[1],
                destinationFrame->linesize[1],
                reinterpret_cast<const uint8_t *>(planes.planes[1].data),
                static_cast<int>(planes.planes[1].rowStride),
                static_cast<int>(planes.planes[1].pixelStride),
                0,
                chromaWidth,
                chromaHeight);
        copyPlaneWithPixelStride(
                destinationFrame->data[2],
                destinationFrame->linesize[2],
                reinterpret_cast<const uint8_t *>(planes.planes[2].data),
                static_cast<int>(planes.planes[2].rowStride),
                static_cast<int>(planes.planes[2].pixelStride),
                0,
                chromaWidth,
                chromaHeight);
    } else {
        const int uvPixelStride = static_cast<int>(planes.planes[1].pixelStride);
        if (uvPixelStride < 2) {
            unlockFn(hardwareBuffer, nullptr);
            LOGE("DV5_HW_RENDER: unsupported interleaved UV pixelStride=%d", uvPixelStride);
            return false;
        }
        const int uComponentByteOffset = (isP010Format && uvPixelStride >= 4) ? 1 : 0;
        const int vComponentByteOffset = (isP010Format && uvPixelStride >= 4) ? 3 : 1;
        copyInterleavedChromaWithPixelStride(
                destinationFrame->data[1],
                destinationFrame->linesize[1],
                destinationFrame->data[2],
                destinationFrame->linesize[2],
                reinterpret_cast<const uint8_t *>(planes.planes[1].data),
                static_cast<int>(planes.planes[1].rowStride),
                uvPixelStride,
                uComponentByteOffset,
                vComponentByteOffset,
                chromaWidth,
                chromaHeight);
    }

    unlockFn(hardwareBuffer, nullptr);
    return true;
}

int renderAvFrameToSurface(
        JNIEnv *env,
        JniContext *jniContext,
        AVFrame *frame,
        jobject surface,
        int displayedWidth,
        int displayedHeight) {
    if (!env || !jniContext || !frame || !surface) {
        return VIDEO_DECODER_ERROR_OTHER;
    }

    const int targetWidth = displayedWidth > 0 ? displayedWidth : frame->width;
    const int targetHeight = displayedHeight > 0 ? displayedHeight : frame->height;
    if (targetWidth <= 0 || targetHeight <= 0) {
        return VIDEO_DECODER_ERROR_OTHER;
    }

    if (!jniContext->MaybeAcquireNativeWindow(env, surface)) {
        return VIDEO_DECODER_ERROR_OTHER;
    }

    if (jniContext->native_window_width != targetWidth ||
        jniContext->native_window_height != targetHeight ||
        jniContext->swsContext == nullptr) {
        if (ANativeWindow_setBuffersGeometry(
                    jniContext->native_window, targetWidth, targetHeight, kImageFormatYV12)) {
            LOGE("kJniStatusANativeWindowError");
            return VIDEO_DECODER_ERROR_OTHER;
        }
        jniContext->native_window_width = targetWidth;
        jniContext->native_window_height = targetHeight;
        sws_freeContext(jniContext->swsContext);
        jniContext->swsContext = sws_getContext(
                frame->width,
                frame->height,
                static_cast<AVPixelFormat>(frame->format),
                targetWidth,
                targetHeight,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
        if (!jniContext->swsContext) {
            LOGE("DV5_HW_RENDER: failed to allocate sws context.");
            return VIDEO_DECODER_ERROR_OTHER;
        }
    }

    ANativeWindow_Buffer nativeWindowBuffer{};
    int lockResult = ANativeWindow_lock(jniContext->native_window, &nativeWindowBuffer, nullptr);
    if (lockResult == -19) {
        jniContext->surface = nullptr;
        return VIDEO_DECODER_SUCCESS;
    } else if (lockResult || nativeWindowBuffer.bits == nullptr) {
        LOGE("kJniStatusANativeWindowError");
        return VIDEO_DECODER_ERROR_OTHER;
    }

    const int32_t nativeWindowUvHeight = (nativeWindowBuffer.height + 1) / 2;
    auto *nativeWindowBits = reinterpret_cast<uint8_t *>(nativeWindowBuffer.bits);
    const int nativeWindowUvStride = ALIGN(nativeWindowBuffer.stride / 2, 16);
    const int vPlaneHeight = std::min(nativeWindowUvHeight, targetHeight);
    const int yPlaneSize = nativeWindowBuffer.stride * nativeWindowBuffer.height;
    const int vPlaneSize = vPlaneHeight * nativeWindowUvStride;

    uint8_t *dest[3] = {
            nativeWindowBits,
            nativeWindowBits + yPlaneSize + vPlaneSize,
            nativeWindowBits + yPlaneSize};
    int destStride[3] = {
            nativeWindowBuffer.stride,
            nativeWindowUvStride,
            nativeWindowUvStride};
    int sourceStride[3] = {frame->linesize[0], frame->linesize[1], frame->linesize[2]};
    sws_scale(
            jniContext->swsContext,
            frame->data,
            sourceStride,
            0,
            frame->height,
            dest,
            destStride);

    if (ANativeWindow_unlockAndPost(jniContext->native_window)) {
        LOGE("kJniStatusANativeWindowError");
        return VIDEO_DECODER_ERROR_OTHER;
    }
    return VIDEO_DECODER_SUCCESS;
}

JniContext *ensureHardwareToneMapContext() {
    if (gHardwareToneMapContext != nullptr) {
        return gHardwareToneMapContext;
    }
    auto *context = new JniContext();
    context->enable_tone_map_to_sdr = true;
    context->last_queued_input_time_us = AV_NOPTS_VALUE;
    gHardwareToneMapContext = context;
    return gHardwareToneMapContext;
}
} // namespace

JniContext *createVideoContext(JNIEnv *env,
                               AVCodec *codec,
                               jbyteArray extraData,
                               jint threads,
                               jboolean enableToneMapToSdr) {
    auto *jniContext = new JniContext();

    AVCodecContext *codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        LOGE("Failed to allocate context.");
        return nullptr;
    }

    if (extraData) {
        jsize size = env->GetArrayLength(extraData);
        codecContext->extradata_size = size;
        codecContext->extradata = (uint8_t *) av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!codecContext->extradata) {
            LOGE("Failed to allocate extradata.");
            releaseContext(codecContext);
            return nullptr;
        }
        env->GetByteArrayRegion(extraData, 0, size, (jbyte *) codecContext->extradata);
    }

    codecContext->thread_count = threads;
    codecContext->err_recognition = AV_EF_IGNORE_ERR;
    int result = avcodec_open2(codecContext, codec, nullptr);
    if (result < 0) {
        logError("avcodec_open2", result);
        releaseContext(codecContext);
        return nullptr;
    }

    jniContext->codecContext = codecContext;
#if FFMPEG_TONEMAP_FILTERS_ACTIVE
    jniContext->enable_tone_map_to_sdr = enableToneMapToSdr;
#else
    (void) enableToneMapToSdr;
    jniContext->enable_tone_map_to_sdr = false;
#endif

    // Populate JNI References.
    jclass outputBufferClass = env->FindClass("androidx/media3/decoder/VideoDecoderOutputBuffer");
    jniContext->data_field = env->GetFieldID(outputBufferClass, "data", "Ljava/nio/ByteBuffer;");
    jniContext->yuvStrides_field = env->GetFieldID(outputBufferClass, "yuvStrides", "[I");
    jniContext->yuvPlanes_field = env->GetFieldID(outputBufferClass, "yuvPlanes", "[Ljava/nio/ByteBuffer;");
    jniContext->init_for_yuv_frame_method = env->GetMethodID(outputBufferClass, "initForYuvFrame", "(IIIII)Z");
    jniContext->init_method = env->GetMethodID(outputBufferClass, "init", "(JILjava/nio/ByteBuffer;)V");

    return jniContext;
}

#if FFMPEG_TONEMAP_FILTERS_ACTIVE
static bool shouldApplyToneMap(const AVFrame *frame) {
    if (!frame) return false;
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER) != nullptr) {
        return true;
    }
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA) != nullptr) {
        return true;
    }
    if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        return true;
    }
    const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get((AVPixelFormat) frame->format);
    if (descriptor && descriptor->comp[0].depth > 8) {
        return true;
    }
    return false;
}

static void clearToneMapFilterGraph(JniContext *jniContext) {
    if (!jniContext) return;
    if (jniContext->filter_graph) {
        avfilter_graph_free(&jniContext->filter_graph);
    }
    if (jniContext->filtered_frame) {
        av_frame_free(&jniContext->filtered_frame);
    }
    jniContext->buffer_source_context = nullptr;
    jniContext->buffer_sink_context = nullptr;
    jniContext->filter_width = 0;
    jniContext->filter_height = 0;
    jniContext->filter_input_format = AV_PIX_FMT_NONE;
}

static bool ensureToneMapFilterGraph(JniContext *jniContext, const AVFrame *sourceFrame) {
    if (!jniContext || !sourceFrame) return false;

    if (jniContext->filter_graph &&
        jniContext->filter_width == sourceFrame->width &&
        jniContext->filter_height == sourceFrame->height &&
        jniContext->filter_input_format == sourceFrame->format) {
        return true;
    }

    clearToneMapFilterGraph(jniContext);

    AVFilterGraph *filterGraph = avfilter_graph_alloc();
    if (!filterGraph) {
        LOGE("Failed to allocate filter graph.");
        return false;
    }

    const AVFilter *bufferSource = avfilter_get_by_name("buffer");
    const AVFilter *bufferSink = avfilter_get_by_name("buffersink");
    if (!bufferSource || !bufferSink) {
        LOGE("Missing buffer/buffersink filters.");
        avfilter_graph_free(&filterGraph);
        return false;
    }

    AVRational timeBase =
            jniContext->codecContext != nullptr &&
                    jniContext->codecContext->pkt_timebase.num > 0 &&
                    jniContext->codecContext->pkt_timebase.den > 0
            ? jniContext->codecContext->pkt_timebase
            : AVRational{1, 1000000};
    AVRational sampleAspectRatio =
            sourceFrame->sample_aspect_ratio.num > 0 && sourceFrame->sample_aspect_ratio.den > 0
            ? sourceFrame->sample_aspect_ratio
            : AVRational{1, 1};
    char args[256];
    snprintf(
            args,
            sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            sourceFrame->width,
            sourceFrame->height,
            sourceFrame->format,
            timeBase.num,
            timeBase.den,
            sampleAspectRatio.num,
            sampleAspectRatio.den);

    AVFilterContext *bufferSourceContext = nullptr;
    int result = avfilter_graph_create_filter(
            &bufferSourceContext, bufferSource, "in", args, nullptr, filterGraph);
    if (result < 0) {
        logError("avfilter_graph_create_filter(buffer)", result);
        avfilter_graph_free(&filterGraph);
        return false;
    }

    AVFilterContext *bufferSinkContext = nullptr;
    result = avfilter_graph_create_filter(
            &bufferSinkContext, bufferSink, "out", nullptr, nullptr, filterGraph);
    if (result < 0) {
        logError("avfilter_graph_create_filter(buffersink)", result);
        avfilter_graph_free(&filterGraph);
        return false;
    }

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        LOGE("Failed to allocate AVFilterInOut.");
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&filterGraph);
        return false;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = bufferSourceContext;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = bufferSinkContext;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    result = avfilter_graph_parse_ptr(
            filterGraph, kDv5ToneMapFilterGraph, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (result < 0) {
        logError("avfilter_graph_parse_ptr", result);
        avfilter_graph_free(&filterGraph);
        return false;
    }

    result = avfilter_graph_config(filterGraph, nullptr);
    if (result < 0) {
        logError("avfilter_graph_config", result);
        avfilter_graph_free(&filterGraph);
        return false;
    }

    AVFrame *filteredFrame = av_frame_alloc();
    if (!filteredFrame) {
        LOGE("Failed to allocate filtered frame.");
        avfilter_graph_free(&filterGraph);
        return false;
    }

    jniContext->filter_graph = filterGraph;
    jniContext->buffer_source_context = bufferSourceContext;
    jniContext->buffer_sink_context = bufferSinkContext;
    jniContext->filtered_frame = filteredFrame;
    jniContext->filter_width = sourceFrame->width;
    jniContext->filter_height = sourceFrame->height;
    jniContext->filter_input_format = (AVPixelFormat) sourceFrame->format;
    return true;
}

static AVFrame *applyToneMapToFrame(JniContext *jniContext, AVFrame *sourceFrame) {
    if (!jniContext || !sourceFrame) return sourceFrame;
    maybeAttachExternalDv5RpuSideData(jniContext, sourceFrame);
    if (!jniContext->enable_tone_map_to_sdr || !shouldApplyToneMap(sourceFrame)) {
        return sourceFrame;
    }
    if (!ensureToneMapFilterGraph(jniContext, sourceFrame)) {
        LOGE("Disabling tone-map filter graph after initialization failure.");
        jniContext->enable_tone_map_to_sdr = false;
        return sourceFrame;
    }

    int result = av_buffersrc_add_frame_flags(
            jniContext->buffer_source_context, sourceFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (result < 0) {
        logError("av_buffersrc_add_frame_flags", result);
        return sourceFrame;
    }

    av_frame_unref(jniContext->filtered_frame);
    result = av_buffersink_get_frame(jniContext->buffer_sink_context, jniContext->filtered_frame);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return sourceFrame;
    }
    if (result < 0) {
        logError("av_buffersink_get_frame", result);
        return sourceFrame;
    }
    return jniContext->filtered_frame;
}
#else
static void clearToneMapFilterGraph(JniContext *jniContext) {
    (void) jniContext;
}

static AVFrame *applyToneMapToFrame(JniContext *jniContext, AVFrame *sourceFrame) {
    (void) jniContext;
    return sourceFrame;
}
#endif


extern "C"
JNIEXPORT jlong JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_ffmpegInitialize(JNIEnv *env,
                                                                                 jobject thiz,
                                                                                 jstring codec_name,
                                                                                 jbyteArray extra_data,
                                                                                 jint threads,
                                                                                 jboolean enable_tone_map_to_sdr) {
    AVCodec *codec = getCodecByName(env, codec_name);
    if (!codec) {
        LOGE("Codec not found.");
        return 0L;
    }

    return (jlong) createVideoContext(env, codec, extra_data, threads, enable_tone_map_to_sdr);
}

extern "C"
JNIEXPORT jlong JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_ffmpegReset(JNIEnv *env, jobject thiz,
                                                                            jlong jContext) {
    auto *const jniContext = reinterpret_cast<JniContext *>(jContext);
    AVCodecContext *context = jniContext->codecContext;
    if (!context) {
        LOGE("Tried to reset without a context.");
        return 0L;
    }

    avcodec_flush_buffers(context);
    clearToneMapFilterGraph(jniContext);
    return (jlong) jniContext;
}

extern "C"
JNIEXPORT void JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_ffmpegRelease(JNIEnv *env, jobject thiz,
                                                                              jlong jContext) {
    auto *const jniContext = reinterpret_cast<JniContext *>(jContext);
    AVCodecContext *context = jniContext->codecContext;
    if (context) {
        sws_freeContext(jniContext->swsContext);
        clearToneMapFilterGraph(jniContext);
        releaseContext(context);
        delete jniContext;
    }
}

extern "C"
JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_ffmpegRenderFrame(JNIEnv *env,
                                                                                  jobject thiz,
                                                                                  jlong jContext,
                                                                                  jobject surface,
                                                                                  jobject output_buffer,
                                                                                  jint displayed_width,
                                                                                  jint displayed_height) {
    auto *const jniContext = reinterpret_cast<JniContext *>(jContext);
    if (!jniContext->MaybeAcquireNativeWindow(env, surface)) {
        return VIDEO_DECODER_ERROR_OTHER;
    }

    if (jniContext->native_window_width != displayed_width ||
        jniContext->native_window_height != displayed_height) {

        if (ANativeWindow_setBuffersGeometry(
                jniContext->native_window,
                displayed_width,
                displayed_height,
                kImageFormatYV12)) {
            LOGE("kJniStatusANativeWindowError");
            return VIDEO_DECODER_ERROR_OTHER;
        }

        jniContext->native_window_width = displayed_width;
        jniContext->native_window_height = displayed_height;

        // Initializing swsContext with AV_PIX_FMT_YUV420P, which is equivalent to YV12.
        // The only difference is the order of the u and v planes.
        SwsContext *swsContext = sws_getContext(displayed_width, displayed_height,
                                                AV_PIX_FMT_YUV420P,
                                                displayed_width, displayed_height,
                                                AV_PIX_FMT_YUV420P,
                                                SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!swsContext) {
            LOGE("Failed to allocate swsContext.");
            return VIDEO_DECODER_ERROR_OTHER;
        }
        jniContext->swsContext = swsContext;
    }

    ANativeWindow_Buffer native_window_buffer;
    int result = ANativeWindow_lock(jniContext->native_window, &native_window_buffer, nullptr);
    if (result == -19) {
        jniContext->surface = nullptr;
        return VIDEO_DECODER_SUCCESS;
    } else if (result || native_window_buffer.bits == nullptr) {
        LOGE("kJniStatusANativeWindowError");
        return VIDEO_DECODER_ERROR_OTHER;
    }

    // source planes from VideoDecoderOutputBuffer
    jobject yuvPlanes_object = env->GetObjectField(output_buffer, jniContext->yuvPlanes_field);
    auto yuvPlanes_array = jobjectArray(yuvPlanes_object);
    jobject yuvPlanesY = env->GetObjectArrayElement(yuvPlanes_array, kPlaneY);
    jobject yuvPlanesU = env->GetObjectArrayElement(yuvPlanes_array, kPlaneU);
    jobject yuvPlanesV = env->GetObjectArrayElement(yuvPlanes_array, kPlaneV);

    auto *planeY = reinterpret_cast<uint8_t *>(env->GetDirectBufferAddress(yuvPlanesY));
    auto *planeU = reinterpret_cast<uint8_t *>(env->GetDirectBufferAddress(yuvPlanesU));
    auto *planeV = reinterpret_cast<uint8_t *>(env->GetDirectBufferAddress(yuvPlanesV));

    // source strides from VideoDecoderOutputBuffer
    jobject yuvStrides_object = env->GetObjectField(output_buffer, jniContext->yuvStrides_field);
    auto *yuvStrides_array = reinterpret_cast<jintArray *>(&yuvStrides_object);
    int *yuvStrides = env->GetIntArrayElements(*yuvStrides_array, nullptr);

    int strideY = yuvStrides[kPlaneY];
    int strideU = yuvStrides[kPlaneU];
    int strideV = yuvStrides[kPlaneV];


    const int32_t native_window_buffer_uv_height = (native_window_buffer.height + 1) / 2;
    auto native_window_buffer_bits = reinterpret_cast<uint8_t *>(native_window_buffer.bits);
    const int native_window_buffer_uv_stride = ALIGN(native_window_buffer.stride / 2, 16);
    const int v_plane_height = std::min(native_window_buffer_uv_height, displayed_height);

    const int y_plane_size = native_window_buffer.stride * native_window_buffer.height;
    const int v_plane_size = v_plane_height * native_window_buffer_uv_stride;

    // source data
    uint8_t *src[3] = {planeY, planeU, planeV};

    // source strides
    int src_stride[3] = {strideY, strideU, strideV};

    // destination data with u and v swapped
    uint8_t *dest[3] = {native_window_buffer_bits,
                        native_window_buffer_bits + y_plane_size + v_plane_size,
                        native_window_buffer_bits + y_plane_size};

    // destination strides
    int dest_stride[3] = {native_window_buffer.stride,
                          native_window_buffer_uv_stride,
                          native_window_buffer_uv_stride};


    //Perform color space conversion using sws_scale.
    //Convert the source data (src) with specified strides (src_stride) and displayed height,
    //and store the result in the destination data (dest) with corresponding strides (dest_stride).
    sws_scale(jniContext->swsContext,
              src, src_stride,
              0, displayed_height,
              dest, dest_stride);

    env->ReleaseIntArrayElements(*yuvStrides_array, yuvStrides, 0);

    if (ANativeWindow_unlockAndPost(jniContext->native_window)) {
        LOGE("kJniStatusANativeWindowError");
        return VIDEO_DECODER_ERROR_OTHER;
    }

    return VIDEO_DECODER_SUCCESS;
}

extern "C"
JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_ffmpegSendPacket(JNIEnv *env,
                                                                                 jobject thiz,
                                                                                 jlong jContext,
                                                                                 jobject encoded_data,
                                                                                 jint length,
                                                                                 jlong input_time) {
    auto *const jniContext = reinterpret_cast<JniContext *>(jContext);
    AVCodecContext *avContext = jniContext->codecContext;

    auto *inputBuffer = (uint8_t *) env->GetDirectBufferAddress(encoded_data);
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        LOGE("Failed to allocate packet.");
        return VIDEO_DECODER_ERROR_OTHER;
    }
    packet->data = inputBuffer;
    packet->size = length;
    packet->pts = input_time;
    packet->dts = input_time;
    jniContext->last_queued_input_time_us = input_time;

    // Queue input data.
    int result = avcodec_send_packet(avContext, packet);
    av_packet_free(&packet);
    if (result) {
        logError("avcodec_send_packet", result);
        if (result == AVERROR_INVALIDDATA) {
            // need more data
            return VIDEO_DECODER_ERROR_INVALID_DATA;
        } else if (result == AVERROR(EAGAIN)) {
            // need read frame
            return VIDEO_DECODER_ERROR_READ_FRAME;
        } else {
            return VIDEO_DECODER_ERROR_OTHER;
        }
    }
    return result;
}

extern "C"
JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_ffmpegReceiveFrame(JNIEnv *env,
                                                                                   jobject thiz,
                                                                                   jlong jContext,
                                                                                   jint output_mode,
                                                                                   jobject output_buffer,
                                                                                   jboolean decode_only) {
    auto *const jniContext = reinterpret_cast<JniContext *>(jContext);
    AVCodecContext *avContext = jniContext->codecContext;

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        LOGE("Failed to allocate output frame.");
        return VIDEO_DECODER_ERROR_OTHER;
    }
    int result = avcodec_receive_frame(avContext, frame);

    // fail
    if (decode_only || result == AVERROR(EAGAIN)) {
        // This is not an error. The input data was decode-only or no displayable
        // frames are available.
        av_frame_free(&frame);
        return VIDEO_DECODER_ERROR_INVALID_DATA;
    }
    if (result) {
        av_frame_free(&frame);
        logError("avcodec_receive_frame", result);
        return VIDEO_DECODER_ERROR_OTHER;
    }

    AVFrame *outputFrame = applyToneMapToFrame(jniContext, frame);
    int64_t outputTimeUs = outputFrame->best_effort_timestamp;
    if (outputTimeUs == AV_NOPTS_VALUE) {
        outputTimeUs = outputFrame->pts;
    }
    if (outputTimeUs == AV_NOPTS_VALUE) {
        outputTimeUs = jniContext->last_queued_input_time_us;
    }
    if (outputTimeUs == AV_NOPTS_VALUE) {
        outputTimeUs = 0;
    } else {
        jniContext->last_queued_input_time_us = outputTimeUs;
    }

    // success
    // init time and mode
    env->CallVoidMethod(
            output_buffer, jniContext->init_method, outputTimeUs, output_mode, nullptr);

    // init data
    const jboolean init_result = env->CallBooleanMethod(
            output_buffer, jniContext->init_for_yuv_frame_method,
            outputFrame->width,
            outputFrame->height,
            outputFrame->linesize[0], outputFrame->linesize[1],
            0);
    if (env->ExceptionCheck()) {
        // Exception is thrown in Java when returning from the native call.
        return VIDEO_DECODER_ERROR_OTHER;
    }
    if (!init_result) {
        return VIDEO_DECODER_ERROR_OTHER;
    }

    jobject data_object = env->GetObjectField(output_buffer, jniContext->data_field);
    auto *data = reinterpret_cast<jbyte *>(env->GetDirectBufferAddress(data_object));
    const int32_t uvHeight = (outputFrame->height + 1) / 2;
    const uint64_t yLength = outputFrame->linesize[0] * outputFrame->height;
    const uint64_t uvLength = outputFrame->linesize[1] * uvHeight;

    // TODO: Support rotate YUV data

    memcpy(data, outputFrame->data[0], yLength);
    memcpy(data + yLength, outputFrame->data[1], uvLength);
    memcpy(data + yLength + uvLength, outputFrame->data[2], uvLength);

    av_frame_free(&frame);

    return result;
}
