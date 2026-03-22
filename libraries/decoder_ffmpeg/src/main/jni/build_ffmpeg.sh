#!/bin/bash
#
# Copyright (C) 2019 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
set -eu

FFMPEG_MODULE_PATH="$1"
echo "FFMPEG_MODULE_PATH is ${FFMPEG_MODULE_PATH}"
NDK_PATH="$2"
echo "NDK path is ${NDK_PATH}"
HOST_PLATFORM="$3"
echo "Host platform is ${HOST_PLATFORM}"
ANDROID_ABI="$4"
echo "ANDROID_ABI is ${ANDROID_ABI}"
ENABLED_DECODERS=()
if [[ "$#" -gt 4 ]]
then
    ENABLED_DECODERS=("${@:5}")
fi
ENABLED_PARSERS=()
REQUIRED_DECODERS=(
    aac
    mp3
    ac3
    eac3
    truehd
    dca
    vorbis
    opus
    amrnb
    amrwb
    flac
    alac
    pcm_mulaw
    pcm_alaw
    h263
    h264
    hevc
    av1
    vc1
    mjpeg
    mpeg4
    mpegvideo
    mpeg2video
    vp8
    vp9
)

append_unique_decoder() {
    local decoder="$1"
    local existing
    for existing in "${ENABLED_DECODERS[@]-}"
    do
        if [[ "${existing}" == "${decoder}" ]]
        then
            return
        fi
    done
    ENABLED_DECODERS+=("${decoder}")
}

for decoder in "${REQUIRED_DECODERS[@]}"
do
    append_unique_decoder "${decoder}"
done

echo "Enabled decoders are ${ENABLED_DECODERS[@]-}"
JOBS="$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)"
echo "Using $JOBS jobs for make"
FFMPEG_ENABLE_LIBPLACEBO="${FFMPEG_ENABLE_LIBPLACEBO:-0}"
LIBPLACEBO_PREBUILT_ROOT="${LIBPLACEBO_PREBUILT_ROOT:-}"
LIBPLACEBO_MIN_VERSION="${LIBPLACEBO_MIN_VERSION:-7.360.0}"
LIBPLACEBO_MAX_VERSION="${LIBPLACEBO_MAX_VERSION:-7.360.0}"
FFMPEG_ENABLE_LIBDOVI="${FFMPEG_ENABLE_LIBDOVI:-0}"
LIBDOVI_PREBUILT_ROOT="${LIBDOVI_PREBUILT_ROOT:-${FFMPEG_MODULE_PATH}/../../../../../third_party/libdovi}"
FFMPEG_REQUIRE_LIBDOVI="${FFMPEG_REQUIRE_LIBDOVI:-0}"
echo "FFMPEG_ENABLE_LIBPLACEBO is ${FFMPEG_ENABLE_LIBPLACEBO}"
echo "FFMPEG_ENABLE_LIBDOVI is ${FFMPEG_ENABLE_LIBDOVI}"
COMMON_OPTIONS="
    --target-os=android
    --pkg-config=pkg-config
    --enable-static
    --enable-pic
    --disable-shared
    --disable-doc
    --disable-programs
    --disable-everything
    --disable-avdevice
    --enable-swscale
    --enable-avfilter
    --disable-symver
    --enable-swresample
    --extra-ldexeflags=-pie
    --disable-v4l2-m2m
    "

if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" == "1" ]]
then
    COMMON_OPTIONS="${COMMON_OPTIONS}
        --enable-vulkan
        --enable-libshaderc
        --enable-libplacebo
        --enable-filter=libplacebo
        --enable-filter=scale
        --enable-filter=format
    "
else
    COMMON_OPTIONS="${COMMON_OPTIONS}
        --disable-vulkan
    "
fi

TOOLCHAIN_PREFIX="${NDK_PATH}/toolchains/llvm/prebuilt/${HOST_PLATFORM}/bin"
if [[ ! -d "${TOOLCHAIN_PREFIX}" ]]
then
    echo "Please set correct NDK_PATH, $NDK_PATH is incorrect"
    exit 1
fi

FFMPEG_SOURCE_PATH="${FFMPEG_MODULE_PATH}/jni/ffmpeg"

detect_ffmpeg_version() {
    local ffmpeg_source_path="$1"
    local ffmpeg_version=""
    if [[ -f "${ffmpeg_source_path}/VERSION" ]]
    then
        ffmpeg_version="$(tr -d '[:space:]' < "${ffmpeg_source_path}/VERSION")"
    elif [[ -f "${ffmpeg_source_path}/RELEASE" ]]
    then
        ffmpeg_version="$(tr -d '[:space:]' < "${ffmpeg_source_path}/RELEASE")"
    elif [[ -s "${ffmpeg_source_path}/.version" ]]
    then
        ffmpeg_version="$(tr -d '[:space:]' < "${ffmpeg_source_path}/.version")"
    elif [[ -f "${ffmpeg_source_path}/libavcodec/version_major.h" ]]
    then
        local major_version
        major_version="$(
            sed -n 's/^#define[[:space:]]\+LIBAVCODEC_VERSION_MAJOR[[:space:]]\+\([0-9]\+\)$/\1/p' \
                "${ffmpeg_source_path}/libavcodec/version_major.h" | head -n 1
        )"
        if [[ -n "${major_version}" ]]
        then
            ffmpeg_version="${major_version}.0.0"
        fi
    fi

    if [[ -z "${ffmpeg_version}" ]]
    then
        echo "Unable to detect FFmpeg version from ${ffmpeg_source_path}."
        echo "Expected one of VERSION, RELEASE, non-empty .version, or libavcodec/version_major.h."
        exit 1
    fi
    echo "${ffmpeg_version}"
}

extract_major_version() {
    local version="$1"
    if [[ "${version}" =~ ^[Nn]?([0-9]+)(\..*)?$ ]]
    then
        echo "${BASH_REMATCH[1]}"
        return 0
    fi
    return 1
}

FFMPEG_VERSION="$(detect_ffmpeg_version "${FFMPEG_SOURCE_PATH}")"
FFMPEG_MAJOR_VERSION="$(extract_major_version "${FFMPEG_VERSION}" || true)"
if [[ -z "${FFMPEG_MAJOR_VERSION}" ]]
then
    echo "Unable to parse FFmpeg major version from ${FFMPEG_VERSION}"
    exit 1
fi
echo "Detected FFmpeg version ${FFMPEG_VERSION}"

configure_supports_option() {
    local option_name="$1"
    "${FFMPEG_SOURCE_PATH}/configure" --help 2>/dev/null | grep -q -- "${option_name}"
}

if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" == "1" && "${FFMPEG_MAJOR_VERSION}" != "8" ]]
then
    echo "libplacebo tone-map path requires FFmpeg 8.x for this repository's decoder stack."
    echo "Current FFmpeg source is ${FFMPEG_VERSION} (${FFMPEG_SOURCE_PATH})."
    echo "Use the repo FFmpeg checkout at media/FFmpeg (8.0.git) and rebuild."
    exit 1
fi

if [[ "${FFMPEG_ENABLE_LIBDOVI}" == "1" ]]
then
    if [[ ! -d "${LIBDOVI_PREBUILT_ROOT}" ]]
    then
        echo "LIBDOVI_PREBUILT_ROOT does not exist: ${LIBDOVI_PREBUILT_ROOT}"
        exit 1
    fi
    if ! configure_supports_option "--enable-libdovi"
    then
        echo "Current FFmpeg source (${FFMPEG_SOURCE_PATH}) does not support --enable-libdovi."
        echo "This is expected for standard FFmpeg 8.x: DV metadata is parsed via internal DOVI_RPU."
        echo "External --enable-libdovi is optional and only applies to custom FFmpeg forks."
        if [[ "${FFMPEG_REQUIRE_LIBDOVI}" == "1" ]]
        then
            echo "Set FFMPEG_REQUIRE_LIBDOVI=0 to continue with internal DOVI_RPU + libplacebo."
            exit 1
        fi
        echo "Continuing without --enable-libdovi (internal DOVI_RPU path)."
        FFMPEG_ENABLE_LIBDOVI=0
    fi
fi

if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" == "1" && -z "${LIBPLACEBO_PREBUILT_ROOT}" ]]
then
    echo "LIBPLACEBO_PREBUILT_ROOT must be set when FFMPEG_ENABLE_LIBPLACEBO=1"
    exit 1
fi

if [[ "${FFMPEG_ENABLE_LIBDOVI}" == "1" ]]
then
    COMMON_OPTIONS="${COMMON_OPTIONS}
        --enable-libdovi
    "
fi

compare_versions() {
    local lhs="$1"
    local rhs="$2"
    local lhs_parts rhs_parts
    IFS='.' read -r -a lhs_parts <<< "${lhs}"
    IFS='.' read -r -a rhs_parts <<< "${rhs}"
    local lhs_len="${#lhs_parts[@]}"
    local rhs_len="${#rhs_parts[@]}"
    local max_len="$lhs_len"
    if [[ "$rhs_len" -gt "$max_len" ]]; then
        max_len="$rhs_len"
    fi
    for ((i = 0; i < max_len; i++)); do
        local lhs_val="${lhs_parts[i]:-0}"
        local rhs_val="${rhs_parts[i]:-0}"
        if ((10#${lhs_val} > 10#${rhs_val})); then
            echo 1
            return
        fi
        if ((10#${lhs_val} < 10#${rhs_val})); then
            echo -1
            return
        fi
    done
    echo 0
}

validate_libplacebo_pc_version() {
    local pkg_config_dir="$1"
    local libplacebo_pc="${pkg_config_dir}/libplacebo.pc"
    local version_line
    version_line="$(grep -E '^Version:' "${libplacebo_pc}" || true)"
    local libplacebo_version
    libplacebo_version="$(echo "${version_line}" | awk '{print $2}')"
    if [[ -z "${libplacebo_version}" ]]
    then
        echo "Unable to parse libplacebo version from ${libplacebo_pc}"
        exit 1
    fi
    local libplacebo_major="${libplacebo_version%%.*}"
    if [[ "${libplacebo_major}" != "7" ]]
    then
        echo "Unsupported libplacebo version ${libplacebo_version}; this build is pinned to libplacebo v7.x."
        exit 1
    fi
    local version_cmp
    version_cmp="$(compare_versions "${libplacebo_version}" "${LIBPLACEBO_MAX_VERSION}")"
    if [[ "${version_cmp}" == "1" ]]
    then
        echo "libplacebo ${libplacebo_version} is newer than the validated version range."
        echo "Use ${LIBPLACEBO_MAX_VERSION} (current repo default) or override LIBPLACEBO_MAX_VERSION deliberately."
        exit 1
    fi
    version_cmp="$(compare_versions "${libplacebo_version}" "${LIBPLACEBO_MIN_VERSION}")"
    if [[ "${version_cmp}" == "-1" ]]
    then
        echo "Warning: libplacebo ${libplacebo_version} is older than recommended ${LIBPLACEBO_MIN_VERSION}."
    fi
}

require_ffmpeg_define_enabled() {
    local header_path="$1"
    local define_name="$2"
    if ! grep -qE "^#define[[:space:]]+${define_name}[[:space:]]+1$" "${header_path}"
    then
        echo "Expected ${define_name}=1 in ${header_path}, but it is missing/disabled."
        exit 1
    fi
}

validate_ffmpeg_tonemap_config() {
    if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" != "1" && "${FFMPEG_ENABLE_LIBDOVI}" != "1" ]]
    then
        return
    fi
    local config_h="${FFMPEG_SOURCE_PATH}/config.h"
    if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" == "1" ]]
    then
        local config_components_h="${FFMPEG_SOURCE_PATH}/config_components.h"
        require_ffmpeg_define_enabled "${config_h}" "CONFIG_AVFILTER"
        require_ffmpeg_define_enabled "${config_h}" "CONFIG_LIBPLACEBO"
        require_ffmpeg_define_enabled "${config_h}" "CONFIG_VULKAN"
        require_ffmpeg_define_enabled "${config_h}" "CONFIG_DOVI_RPU"
        require_ffmpeg_define_enabled "${config_components_h}" "CONFIG_LIBPLACEBO_FILTER"
    fi
    if [[ "${FFMPEG_ENABLE_LIBDOVI}" == "1" ]]
    then
        require_ffmpeg_define_enabled "${config_h}" "CONFIG_LIBDOVI"
    fi
}

libdovi_abi_dir_for_target() {
    local target_abi="$1"
    case "${target_abi}" in
        armeabi-v7a)
            echo "android-armeabi-v7a"
            ;;
        arm64-v8a)
            echo "android-arm64"
            ;;
        x86)
            echo "android-x86"
            ;;
        x86_64)
            echo "android-x86_64"
            ;;
        *)
            echo "Unsupported ABI for libdovi: ${target_abi}"
            exit 1
            ;;
    esac
}

libdovi_abi_root_for_target() {
    local target_abi="$1"
    local libdovi_abi_dir
    libdovi_abi_dir="$(libdovi_abi_dir_for_target "${target_abi}")"
    echo "${LIBDOVI_PREBUILT_ROOT}/${libdovi_abi_dir}"
}

validate_libdovi_prebuilts_for_abi() {
    local target_abi="$1"
    if [[ "${FFMPEG_ENABLE_LIBDOVI}" != "1" ]]
    then
        return
    fi
    local abi_root
    abi_root="$(libdovi_abi_root_for_target "${target_abi}")"
    if [[ ! -f "${abi_root}/include/libdovi/rpu_parser.h" ]]
    then
        echo "Missing libdovi headers for ${target_abi}: ${abi_root}/include/libdovi/rpu_parser.h"
        exit 1
    fi
    if [[ ! -f "${abi_root}/lib/libdovi.a" ]]
    then
        echo "Missing libdovi static library for ${target_abi}: ${abi_root}/lib/libdovi.a"
        exit 1
    fi
    if [[ ! -f "${abi_root}/lib/pkgconfig/dovi.pc" ]]
    then
        echo "Missing dovi.pc for ${target_abi}: ${abi_root}/lib/pkgconfig/dovi.pc"
        exit 1
    fi
}

prepare_libdovi_pkgconfig_for_abi() {
    local target_abi="$1"
    local abi_root
    abi_root="$(libdovi_abi_root_for_target "${target_abi}")"
    local source_pc="${abi_root}/lib/pkgconfig/dovi.pc"
    local staged_pkgconfig_dir="${FFMPEG_MODULE_PATH}/jni/.libdovi-pkgconfig/${target_abi}"
    mkdir -p "${staged_pkgconfig_dir}"
    sed "s|^prefix=.*$|prefix=${abi_root}|" "${source_pc}" > "${staged_pkgconfig_dir}/dovi.pc"
    echo "${staged_pkgconfig_dir}"
}

validate_tonemap_prebuilts_for_abi() {
    local target_abi="$1"
    if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" != "1" ]]
    then
        return
    fi
    local abi_root="${LIBPLACEBO_PREBUILT_ROOT}/${target_abi}"
    local include_dir="${abi_root}/include"
    local lib_dir="${abi_root}/lib"
    local pkg_config_dir="${lib_dir}/pkgconfig"
    if [[ ! -f "${include_dir}/vulkan/vulkan.h" ]]
    then
        echo "Missing Vulkan headers for ${target_abi}: ${include_dir}/vulkan/vulkan.h"
        exit 1
    fi
    if [[ ! -f "${include_dir}/libplacebo/vulkan.h" ]]
    then
        echo "Missing libplacebo headers for ${target_abi}: ${include_dir}/libplacebo/vulkan.h"
        exit 1
    fi
    if [[ ! -f "${lib_dir}/libplacebo.a" ]]
    then
        echo "Missing libplacebo static library for ${target_abi}: ${lib_dir}/libplacebo.a"
        exit 1
    fi
    if ! find "${lib_dir}" -maxdepth 1 -name "libshaderc*.a" | grep -q .
    then
        echo "Missing shaderc static library for ${target_abi} in ${lib_dir}"
        exit 1
    fi
    if [[ ! -f "${pkg_config_dir}/libplacebo.pc" ]]
    then
        echo "Missing libplacebo.pc for ${target_abi}: ${pkg_config_dir}/libplacebo.pc"
        exit 1
    fi
}

validate_tonemap_outputs_for_abi() {
    local target_abi="$1"
    if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" != "1" ]]
    then
        return
    fi
    local lib_dir="${FFMPEG_SOURCE_PATH}/android-libs/${target_abi}"
    if [[ ! -f "${lib_dir}/libavfilter.a" ]]
    then
        echo "Missing libavfilter.a for ${target_abi}; libplacebo filter path is not built."
        exit 1
    fi
}

stage_tonemap_linker_libs_for_abi() {
    local target_abi="$1"
    if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" != "1" ]]
    then
        return
    fi
    local source_lib_dir="${LIBPLACEBO_PREBUILT_ROOT}/${target_abi}/lib"
    local target_lib_dir="${FFMPEG_SOURCE_PATH}/android-libs/${target_abi}"
    if [[ ! -d "${source_lib_dir}" ]]
    then
        echo "Missing libplacebo library directory for ${target_abi}: ${source_lib_dir}"
        exit 1
    fi
    mkdir -p "${target_lib_dir}"
    find "${source_lib_dir}" -maxdepth 1 -type f -name "*.a" -exec cp "{}" "${target_lib_dir}/" \;
    local source_include_dir="${LIBPLACEBO_PREBUILT_ROOT}/${target_abi}/include"
    local target_include_dir="${target_lib_dir}/include"
    if [[ -d "${source_include_dir}" ]]
    then
        rm -rf "${target_include_dir}"
        cp -R "${source_include_dir}" "${target_include_dir}"
    fi
}

apply_ffmpeg_vulkan_compat_patch() {
    if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" != "1" ]]
    then
        return
    fi
    local vulkan_source_file="${FFMPEG_SOURCE_PATH}/libavutil/hwcontext_vulkan.c"
    if [[ ! -f "${vulkan_source_file}" ]]
    then
        return
    fi
    if grep -q "static VKAPI_ATTR VkBool32 VKAPI_CALL vk_dbg_callback" "${vulkan_source_file}"
    then
        return
    fi
    sed -i.bak \
        "s/static VkBool32 VKAPI_CALL vk_dbg_callback/static VKAPI_ATTR VkBool32 VKAPI_CALL vk_dbg_callback/" \
        "${vulkan_source_file}"
}

for decoder in "${ENABLED_DECODERS[@]}"
do
    COMMON_OPTIONS="${COMMON_OPTIONS} --enable-decoder=${decoder}"
    case "${decoder}" in
        ac3|eac3)
            ENABLED_PARSERS+=("ac3")
            ;;
        av1)
            ENABLED_PARSERS+=("av1")
            ;;
        dca)
            ENABLED_PARSERS+=("dca")
            ;;
        truehd)
            ENABLED_PARSERS+=("mlp")
            ;;
        vc1)
            ENABLED_PARSERS+=("vc1")
            ;;
    esac
done

deduped_parsers=()
for parser in "${ENABLED_PARSERS[@]-}"
do
    already_added=0
    for existing in "${deduped_parsers[@]-}"
    do
        if [[ "${existing}" == "${parser}" ]]
        then
            already_added=1
            break
        fi
    done
    if [[ "${already_added}" -eq 0 ]]
    then
        deduped_parsers+=("${parser}")
    fi
done
ENABLED_PARSERS=("${deduped_parsers[@]}")

for parser in "${ENABLED_PARSERS[@]}"
do
    COMMON_OPTIONS="${COMMON_OPTIONS} --enable-parser=${parser}"
done

ARMV7_CLANG="${TOOLCHAIN_PREFIX}/armv7a-linux-androideabi${ANDROID_ABI}-clang"
if [[ ! -e "$ARMV7_CLANG" ]]
then
    echo "AVMv7 Clang compiler with path $ARMV7_CLANG does not exist"
    echo "It's likely your NDK version doesn't support ANDROID_ABI $ANDROID_ABI"
    echo "Either use older version of NDK or raise ANDROID_ABI (be aware that ANDROID_ABI must not be greater than your application's minSdk)"
    exit 1
fi
ANDROID_ABI_64BIT="$ANDROID_ABI"
if [[ "$ANDROID_ABI_64BIT" -lt 21 ]]
then
    echo "Using ANDROID_ABI 21 for 64-bit architectures"
    ANDROID_ABI_64BIT=21
fi

configure_ffmpeg() {
    local target_abi="$1"
    shift
    local -a pkg_config_dirs=()
    local pkg_config_dir=""
    if [[ "${FFMPEG_ENABLE_LIBPLACEBO}" == "1" ]]
    then
        validate_tonemap_prebuilts_for_abi "${target_abi}"
        pkg_config_dir="${LIBPLACEBO_PREBUILT_ROOT}/${target_abi}/lib/pkgconfig"
        if [[ ! -d "${pkg_config_dir}" ]]
        then
            echo "Missing pkg-config directory for ${target_abi}: ${pkg_config_dir}"
            exit 1
        fi
        if [[ ! -f "${pkg_config_dir}/libplacebo.pc" ]]
        then
            echo "Missing libplacebo.pc for ${target_abi}: ${pkg_config_dir}/libplacebo.pc"
            exit 1
        fi
        validate_libplacebo_pc_version "${pkg_config_dir}"
        if [[ ! -f "${pkg_config_dir}/shaderc.pc" && ! -f "${pkg_config_dir}/shaderc_combined.pc" ]]
        then
            echo "Missing shaderc pkg-config file for ${target_abi} in ${pkg_config_dir}"
            exit 1
        fi
        if [[ ! -f "${pkg_config_dir}/spirv_compiler.pc" ]]
        then
            if [[ -f "${pkg_config_dir}/shaderc.pc" ]]
            then
                cp "${pkg_config_dir}/shaderc.pc" "${pkg_config_dir}/spirv_compiler.pc"
            else
                cp "${pkg_config_dir}/shaderc_combined.pc" "${pkg_config_dir}/spirv_compiler.pc"
            fi
        fi
        pkg_config_dirs+=("${pkg_config_dir}")
    fi

    if [[ "${FFMPEG_ENABLE_LIBDOVI}" == "1" ]]
    then
        validate_libdovi_prebuilts_for_abi "${target_abi}"
        pkg_config_dirs+=("$(prepare_libdovi_pkgconfig_for_abi "${target_abi}")")
    fi

    local pkg_config_joined=""
    if [[ "${#pkg_config_dirs[@]}" -gt 0 ]]
    then
        pkg_config_joined="$(IFS=: ; echo "${pkg_config_dirs[*]}")"
    fi

    if [[ -n "${pkg_config_joined}" ]]
    then
        PKG_CONFIG_LIBDIR="${pkg_config_joined}" \
        PKG_CONFIG_PATH="${pkg_config_joined}" \
        ./configure "$@" ${COMMON_OPTIONS}
    else
        ./configure "$@" ${COMMON_OPTIONS}
    fi
    validate_ffmpeg_tonemap_config
}

cd "${FFMPEG_MODULE_PATH}/jni/ffmpeg"
apply_ffmpeg_vulkan_compat_patch
configure_ffmpeg "armeabi-v7a" \
    --libdir=android-libs/armeabi-v7a \
    --arch=arm \
    --cpu=armv7-a \
    --cross-prefix="${TOOLCHAIN_PREFIX}/armv7a-linux-androideabi${ANDROID_ABI}-" \
    --nm="${TOOLCHAIN_PREFIX}/llvm-nm" \
    --ar="${TOOLCHAIN_PREFIX}/llvm-ar" \
    --ranlib="${TOOLCHAIN_PREFIX}/llvm-ranlib" \
    --strip="${TOOLCHAIN_PREFIX}/llvm-strip" \
    --extra-cflags="-fPIC -march=armv7-a -mfloat-abi=softfp" \
    --extra-ldflags="-Wl,--fix-cortex-a8"
make -j$JOBS
make install-libs
validate_tonemap_outputs_for_abi "armeabi-v7a"
stage_tonemap_linker_libs_for_abi "armeabi-v7a"
make clean

configure_ffmpeg "arm64-v8a" \
    --libdir=android-libs/arm64-v8a \
    --arch=aarch64 \
    --cpu=armv8-a \
    --cross-prefix="${TOOLCHAIN_PREFIX}/aarch64-linux-android${ANDROID_ABI_64BIT}-" \
    --nm="${TOOLCHAIN_PREFIX}/llvm-nm" \
    --ar="${TOOLCHAIN_PREFIX}/llvm-ar" \
    --ranlib="${TOOLCHAIN_PREFIX}/llvm-ranlib" \
    --strip="${TOOLCHAIN_PREFIX}/llvm-strip" \
    --extra-cflags="-fPIC"
make -j$JOBS
make install-libs
validate_tonemap_outputs_for_abi "arm64-v8a"
stage_tonemap_linker_libs_for_abi "arm64-v8a"
make clean

configure_ffmpeg "x86" \
    --libdir=android-libs/x86 \
    --arch=x86 \
    --cpu=i686 \
    --cross-prefix="${TOOLCHAIN_PREFIX}/i686-linux-android${ANDROID_ABI}-" \
    --nm="${TOOLCHAIN_PREFIX}/llvm-nm" \
    --ar="${TOOLCHAIN_PREFIX}/llvm-ar" \
    --ranlib="${TOOLCHAIN_PREFIX}/llvm-ranlib" \
    --strip="${TOOLCHAIN_PREFIX}/llvm-strip" \
    --extra-cflags="-fPIC" \
    --disable-asm
make -j$JOBS
make install-libs
validate_tonemap_outputs_for_abi "x86"
stage_tonemap_linker_libs_for_abi "x86"
make clean

configure_ffmpeg "x86_64" \
    --libdir=android-libs/x86_64 \
    --arch=x86_64 \
    --cpu=x86-64 \
    --cross-prefix="${TOOLCHAIN_PREFIX}/x86_64-linux-android${ANDROID_ABI_64BIT}-" \
    --nm="${TOOLCHAIN_PREFIX}/llvm-nm" \
    --ar="${TOOLCHAIN_PREFIX}/llvm-ar" \
    --ranlib="${TOOLCHAIN_PREFIX}/llvm-ranlib" \
    --strip="${TOOLCHAIN_PREFIX}/llvm-strip" \
    --extra-cflags="-fPIC" \
    --disable-asm
make -j$JOBS
make install-libs
validate_tonemap_outputs_for_abi "x86_64"
stage_tonemap_linker_libs_for_abi "x86_64"
make clean

HEADER_STAGING_DIR="${FFMPEG_MODULE_PATH}/jni/ffmpeg-headers"
rm -rf "${HEADER_STAGING_DIR}"
mkdir -p "${HEADER_STAGING_DIR}"
for header_dir in libavcodec libavformat libavutil libswresample libswscale libavfilter
do
    ln -s "../ffmpeg/${header_dir}" "${HEADER_STAGING_DIR}/${header_dir}"
done
cp "${FFMPEG_MODULE_PATH}/jni/ffmpeg/config.h" "${HEADER_STAGING_DIR}/config.h"
cp "${FFMPEG_MODULE_PATH}/jni/ffmpeg/config_components.h" "${HEADER_STAGING_DIR}/config_components.h"
