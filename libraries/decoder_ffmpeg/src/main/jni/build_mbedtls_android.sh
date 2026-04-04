#!/bin/bash
#
# Builds static mbedTLS prebuilts for Android ABIs used by decoder_ffmpeg.
#
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../../../../../" && pwd)"

NDK_PATH="$1"
HOST_PLATFORM="$2"
ANDROID_API="${3:-21}"
OUTPUT_ROOT="${4:-${PROJECT_ROOT}/third_party/mbedtls-prebuilt}"
SOURCE_ROOT="${5:-${PROJECT_ROOT}/third_party/mbedtls-src}"

MBEDTLS_REPO="${MBEDTLS_REPO:-https://github.com/Mbed-TLS/mbedtls.git}"
MBEDTLS_REF="${MBEDTLS_REF:-mbedtls-3.6.6}"
JOBS="$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)"

echo "NDK path is ${NDK_PATH}"
echo "Host platform is ${HOST_PLATFORM}"
echo "Android API is ${ANDROID_API}"
echo "mbedTLS output root is ${OUTPUT_ROOT}"
echo "mbedTLS source root is ${SOURCE_ROOT}"
echo "mbedTLS repo is ${MBEDTLS_REPO}"
if [[ -n "${MBEDTLS_REF}" ]]; then
  echo "mbedTLS ref is ${MBEDTLS_REF}"
fi

TOOLCHAIN_FILE="${NDK_PATH}/build/cmake/android.toolchain.cmake"
if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
  echo "Android toolchain file not found: ${TOOLCHAIN_FILE}"
  exit 1
fi

if [[ ! -d "${SOURCE_ROOT}/.git" ]]; then
  rm -rf "${SOURCE_ROOT}"
  git clone "${MBEDTLS_REPO}" "${SOURCE_ROOT}"
fi

git -C "${SOURCE_ROOT}" submodule update --init --recursive

if [[ -n "${MBEDTLS_REF}" ]]; then
  git -C "${SOURCE_ROOT}" fetch --all --tags --prune
  git -C "${SOURCE_ROOT}" checkout "${MBEDTLS_REF}"
  git -C "${SOURCE_ROOT}" submodule update --init --recursive
fi

build_for_abi() {
  local abi="$1"
  local build_dir="${SOURCE_ROOT}/build-android-${abi}"
  local install_dir="${OUTPUT_ROOT}/${abi}"
  local pkgconfig_dir="${install_dir}/lib/pkgconfig"

  rm -rf "${build_dir}" "${install_dir}"
  mkdir -p "${build_dir}" "${install_dir}" "${pkgconfig_dir}"

  cmake -S "${SOURCE_ROOT}" -B "${build_dir}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DANDROID_ABI="${abi}" \
    -DANDROID_PLATFORM="android-${ANDROID_API}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${install_dir}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON

  cmake --build "${build_dir}" --target mbedcrypto mbedx509 mbedtls --parallel "${JOBS}"
  cmake --install "${build_dir}"

  local version
  version="$(grep -E '^#define[[:space:]]+MBEDTLS_VERSION_STRING[[:space:]]+"[^"]+"' \
    "${install_dir}/include/mbedtls/version.h" | sed -E 's/.*"([^"]+)".*/\1/' || true)"
  if [[ -z "${version}" ]]; then
    version="unknown"
  fi

  cat > "${pkgconfig_dir}/mbedtls.pc" <<EOF
prefix=${install_dir}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: mbedtls
Description: mbedTLS
Version: ${version}
Libs: -L\${libdir} -lmbedtls -lmbedx509 -lmbedcrypto
Cflags: -I\${includedir}
EOF
}

build_for_abi "armeabi-v7a"
build_for_abi "arm64-v8a"
build_for_abi "x86"
build_for_abi "x86_64"

echo "mbedTLS prebuilts ready at ${OUTPUT_ROOT}"
