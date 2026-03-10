#!/bin/bash
#
# Builds Vulkan/libplacebo dependencies for Android FFmpeg decoder_ffmpeg module.
#
# Output layout:
#   <OUTPUT_ROOT>/<abi>/{include,lib,lib/pkgconfig}
#
set -euo pipefail

if [[ $# -lt 4 ]]; then
  echo "Usage: $0 <OUTPUT_ROOT> <NDK_PATH> <HOST_PLATFORM> <ANDROID_API>"
  exit 1
fi

OUTPUT_ROOT="$1"
NDK_PATH="$2"
HOST_PLATFORM="$3"
ANDROID_API="$4"

ABIS="${ABIS:-armeabi-v7a arm64-v8a x86 x86_64}"
WORK_DIR="${WORK_DIR:-$(pwd)/.libplacebo-build}"

LIBPLACEBO_REPO="${LIBPLACEBO_REPO:-https://code.videolan.org/videolan/libplacebo.git}"
LIBPLACEBO_TAG="${LIBPLACEBO_TAG:-v5.264.1}"
SUPPORTED_LIBPLACEBO_TAGS="${SUPPORTED_LIBPLACEBO_TAGS:-v5.264.1 v5.229.2}"
ALLOW_UNSUPPORTED_LIBPLACEBO_TAG="${ALLOW_UNSUPPORTED_LIBPLACEBO_TAG:-0}"
LIBPLACEBO_PYTHON="${LIBPLACEBO_PYTHON:-/usr/bin/python3}"

VULKAN_HEADERS_REPO="${VULKAN_HEADERS_REPO:-https://github.com/KhronosGroup/Vulkan-Headers.git}"
VULKAN_HEADERS_TAG="${VULKAN_HEADERS_TAG:-v1.3.283}"

SHADERC_REPO="${SHADERC_REPO:-https://github.com/google/shaderc.git}"
SHADERC_COMMIT="${SHADERC_COMMIT:-c4b0af6c3664cd8b33ffddf452514e02a173b4d6}"

SPIRV_CROSS_REPO="${SPIRV_CROSS_REPO:-https://github.com/KhronosGroup/SPIRV-Cross.git}"
SPIRV_CROSS_COMMIT="${SPIRV_CROSS_COMMIT:-28184c1e138f18c330256eeb2f56b9f9fbc53921}"

TOOLCHAIN_PREFIX="${NDK_PATH}/toolchains/llvm/prebuilt/${HOST_PLATFORM}/bin"
TOOLCHAIN_FILE="${NDK_PATH}/build/cmake/android.toolchain.cmake"

for tool in git cmake ninja meson pkg-config; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing required tool: $tool"
    exit 1
  fi
done

if [[ ! -x "${LIBPLACEBO_PYTHON}" ]]; then
  echo "Configured LIBPLACEBO_PYTHON is not executable: ${LIBPLACEBO_PYTHON}"
  exit 1
fi

if [[ ! -d "${TOOLCHAIN_PREFIX}" ]]; then
  echo "Invalid NDK/HOST platform; toolchain not found at ${TOOLCHAIN_PREFIX}"
  exit 1
fi
if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
  echo "Missing Android CMake toolchain file: ${TOOLCHAIN_FILE}"
  exit 1
fi

if [[ "${ALLOW_UNSUPPORTED_LIBPLACEBO_TAG}" != "1" ]]; then
  supported=0
  for tag in ${SUPPORTED_LIBPLACEBO_TAGS}; do
    if [[ "${LIBPLACEBO_TAG}" == "${tag}" ]]; then
      supported=1
      break
    fi
  done
  if [[ "${supported}" != "1" ]]; then
    echo "Unsupported LIBPLACEBO_TAG=${LIBPLACEBO_TAG}"
    echo "Supported tags: ${SUPPORTED_LIBPLACEBO_TAGS}"
    echo "Set ALLOW_UNSUPPORTED_LIBPLACEBO_TAG=1 to bypass."
    exit 1
  fi
fi

mkdir -p "${WORK_DIR}" "${OUTPUT_ROOT}"

git_clone_or_update() {
  local repo="$1"
  local ref="$2"
  local dest="$3"
  if [[ ! -d "${dest}/.git" ]]; then
    git clone --recursive "${repo}" "${dest}"
  fi
  git -C "${dest}" fetch --tags --force origin
  git -C "${dest}" checkout --force "${ref}"
  git -C "${dest}" submodule update --init --recursive
}

abi_to_target() {
  case "$1" in
    armeabi-v7a) echo "armv7a-linux-androideabi" ;;
    arm64-v8a) echo "aarch64-linux-android" ;;
    x86) echo "i686-linux-android" ;;
    x86_64) echo "x86_64-linux-android" ;;
    *) echo "Unsupported ABI: $1" ; exit 1 ;;
  esac
}

abi_to_cpu_family() {
  case "$1" in
    armeabi-v7a) echo "arm" ;;
    arm64-v8a) echo "aarch64" ;;
    x86) echo "x86" ;;
    x86_64) echo "x86_64" ;;
    *) echo "Unsupported ABI: $1" ; exit 1 ;;
  esac
}

abi_to_cpu() {
  case "$1" in
    armeabi-v7a) echo "armv7" ;;
    arm64-v8a) echo "armv8" ;;
    x86) echo "i686" ;;
    x86_64) echo "x86_64" ;;
    *) echo "Unsupported ABI: $1" ; exit 1 ;;
  esac
}

build_vulkan_headers() {
  local prefix="$1"
  mkdir -p "${prefix}/include" "${prefix}/lib/pkgconfig" "${prefix}/share/vulkan/registry"
  cp -R "${WORK_DIR}/src/Vulkan-Headers/include/vulkan" "${prefix}/include/"
  if [[ -d "${WORK_DIR}/src/Vulkan-Headers/include/vk_video" ]]; then
    cp -R "${WORK_DIR}/src/Vulkan-Headers/include/vk_video" "${prefix}/include/"
  fi
  cp "${WORK_DIR}/src/Vulkan-Headers/registry/vk.xml" "${prefix}/share/vulkan/registry/vk.xml"
  cat > "${prefix}/lib/pkgconfig/vulkan.pc" <<EOF
prefix=${prefix}
includedir=\${prefix}/include

Name: vulkan
Version: ${VULKAN_HEADERS_TAG#v}
Description: Vulkan headers
Cflags: -I\${includedir}
EOF
}

build_spirv_cross() {
  local abi="$1"
  local prefix="$2"
  local build_dir="${WORK_DIR}/build/spirv-cross-${abi}"
  rm -rf "${build_dir}"
  cmake -S "${WORK_DIR}/src/SPIRV-Cross" -B "${build_dir}" -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DANDROID_ABI="${abi}" \
    -DANDROID_PLATFORM="android-${ANDROID_API}" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DSPIRV_CROSS_SHARED=OFF \
    -DSPIRV_CROSS_STATIC=ON \
    -DSPIRV_CROSS_CLI=OFF \
    -DSPIRV_CROSS_ENABLE_TESTS=OFF \
    -DSPIRV_CROSS_FORCE_PIC=ON \
    -DSPIRV_CROSS_ENABLE_CPP=OFF
  cmake --build "${build_dir}" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  cmake --install "${build_dir}"
}

build_shaderc() {
  local abi="$1"
  local prefix="$2"
  local build_dir="${WORK_DIR}/build/shaderc-${abi}"
  local marker="${WORK_DIR}/src/shaderc/.deps_synced"

  if [[ ! -f "${marker}" ]]; then
    (cd "${WORK_DIR}/src/shaderc" && ./utils/git-sync-deps)
    touch "${marker}"
  fi

  rm -rf "${build_dir}"
  cmake -S "${WORK_DIR}/src/shaderc" -B "${build_dir}" -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DANDROID_ABI="${abi}" \
    -DANDROID_PLATFORM="android-${ANDROID_API}" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DBUILD_SHARED_LIBS=OFF \
    -DSHADERC_SKIP_TESTS=ON \
    -DSHADERC_SKIP_EXAMPLES=ON \
    -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
    -DENABLE_EXCEPTIONS=ON \
    -DENABLE_CTEST=OFF \
    -DENABLE_GLSLANG_BINARIES=OFF \
    -DSPIRV_SKIP_EXECUTABLES=ON \
    -DSPIRV_TOOLS_BUILD_STATIC=ON
  cmake --build "${build_dir}" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  cmake --install "${build_dir}"

  if [[ -f "${build_dir}/libshaderc_util/libshaderc_util.a" ]]; then
    cp "${build_dir}/libshaderc_util/libshaderc_util.a" "${prefix}/lib/"
  fi

  mkdir -p "${prefix}/lib/pkgconfig"
  if [[ -f "${prefix}/lib/pkgconfig/shaderc.pc" ]]; then
    cp "${prefix}/lib/pkgconfig/shaderc.pc" "${prefix}/lib/pkgconfig/spirv_compiler.pc"
  elif [[ -f "${prefix}/lib/pkgconfig/shaderc_combined.pc" ]]; then
    cp "${prefix}/lib/pkgconfig/shaderc_combined.pc" "${prefix}/lib/pkgconfig/spirv_compiler.pc"
  else
    cat > "${prefix}/lib/pkgconfig/spirv_compiler.pc" <<EOF
prefix=${prefix}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: shaderc
Description: shaderc
Version: 2023.0
Libs: -L\${libdir} -lshaderc
Cflags: -I\${includedir}
EOF
  fi
}

build_libplacebo() {
  local abi="$1"
  local prefix="$2"
  local target
  target="$(abi_to_target "${abi}")"
  local cpu_family
  cpu_family="$(abi_to_cpu_family "${abi}")"
  local cpu
  cpu="$(abi_to_cpu "${abi}")"

  local build_dir="${WORK_DIR}/build/libplacebo-${abi}"
  local cross_file="${WORK_DIR}/build/meson-cross-${abi}.ini"
  rm -rf "${build_dir}"
  mkdir -p "$(dirname "${cross_file}")"

  cat > "${cross_file}" <<EOF
[binaries]
c = '${TOOLCHAIN_PREFIX}/${target}${ANDROID_API}-clang'
cpp = '${TOOLCHAIN_PREFIX}/${target}${ANDROID_API}-clang++'
ar = '${TOOLCHAIN_PREFIX}/llvm-ar'
strip = '${TOOLCHAIN_PREFIX}/llvm-strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'android'
cpu_family = '${cpu_family}'
cpu = '${cpu}'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF

  if ! grep -q "DPL_STATIC" "${WORK_DIR}/src/libplacebo/src/meson.build"; then
    sed -i.bak 's/DPL_EXPORT/DPL_STATIC/' "${WORK_DIR}/src/libplacebo/src/meson.build"
  fi

  # Force libplacebo's meson generators to use a Python runtime compatible with
  # its v5 scripts (notably glad + xml tooling on some host Python versions).
  sed -i.bak \
    "s|python = import('python').find_installation()|python = import('python').find_installation('${LIBPLACEBO_PYTHON}')|" \
    "${WORK_DIR}/src/libplacebo/meson.build"

  # Python 3.14 tightened xml.etree.ElementTree constructor behavior; keep this
  # generator source compatible regardless of host runtime.
  sed -i.bak \
    "s|registry = VkXML(ET.parse(xmlfile))|registry = VkXML(ET.parse(xmlfile).getroot())|" \
    "${WORK_DIR}/src/libplacebo/src/vulkan/utils_gen.py"

  PKG_CONFIG_LIBDIR="${prefix}/lib/pkgconfig" \
  PKG_CONFIG_PATH="${prefix}/lib/pkgconfig" \
  meson setup "${build_dir}" "${WORK_DIR}/src/libplacebo" \
    --cross-file "${cross_file}" \
    --prefix "${prefix}" \
    --buildtype release \
    --default-library static \
    -Dvulkan=enabled \
    -Dvk-proc-addr=disabled \
    -Dvulkan-registry="${prefix}/share/vulkan/registry/vk.xml" \
    -Dshaderc=enabled \
    -Dglslang=disabled \
    -Ddemos=false \
    -Dtests=false \
    -Dbench=false \
    -Dfuzz=false

  meson compile -C "${build_dir}" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  meson install -C "${build_dir}"

  if [[ -f "${prefix}/lib/pkgconfig/libplacebo.pc" ]]; then
    echo "Libs.private: -lstdc++" >> "${prefix}/lib/pkgconfig/libplacebo.pc"
  fi
}

echo "Preparing sources in ${WORK_DIR}/src"
mkdir -p "${WORK_DIR}/src" "${WORK_DIR}/build"
git_clone_or_update "${VULKAN_HEADERS_REPO}" "${VULKAN_HEADERS_TAG}" "${WORK_DIR}/src/Vulkan-Headers"
git_clone_or_update "${SPIRV_CROSS_REPO}" "${SPIRV_CROSS_COMMIT}" "${WORK_DIR}/src/SPIRV-Cross"
git_clone_or_update "${SHADERC_REPO}" "${SHADERC_COMMIT}" "${WORK_DIR}/src/shaderc"
git_clone_or_update "${LIBPLACEBO_REPO}" "${LIBPLACEBO_TAG}" "${WORK_DIR}/src/libplacebo"

for abi in ${ABIS}; do
  prefix="${OUTPUT_ROOT}/${abi}"
  echo "Building dependencies for ${abi} -> ${prefix}"
  rm -rf "${prefix}"
  mkdir -p "${prefix}/lib/pkgconfig"
  build_vulkan_headers "${prefix}"
  build_spirv_cross "${abi}" "${prefix}"
  build_shaderc "${abi}" "${prefix}"
  build_libplacebo "${abi}" "${prefix}"
done

echo "Done. Staged prebuilts at ${OUTPUT_ROOT}"
