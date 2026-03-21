#
# Copyright (C) 2026 Nuvio
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#

cmake_minimum_required(VERSION 3.21.0 FATAL_ERROR)

project(libkodiCppAudioSinkJNI C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KODI_XBMC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../cpp_audiosink/kodi/xbmc")
set(FFMPEG_MODULE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../decoder_ffmpeg/src/main/jni")
set(FFMPEG_SOURCE_ROOT "${FFMPEG_MODULE_ROOT}/ffmpeg")
set(FFMPEG_HEADERS_ROOT "${FFMPEG_MODULE_ROOT}/ffmpeg-headers")
set(FFMPEG_BINARIES_ROOT "${FFMPEG_MODULE_ROOT}/ffmpeg/android-libs/${ANDROID_ABI}")
set(FMT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../fmt")
set(SPDLOG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../spdlog")

add_library(avcodec STATIC IMPORTED)
set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION "${FFMPEG_BINARIES_ROOT}/libavcodec.a")
add_library(avformat STATIC IMPORTED)
set_target_properties(avformat PROPERTIES IMPORTED_LOCATION "${FFMPEG_BINARIES_ROOT}/libavformat.a")
add_library(avfilter STATIC IMPORTED)
set_target_properties(avfilter PROPERTIES IMPORTED_LOCATION "${FFMPEG_BINARIES_ROOT}/libavfilter.a")
add_library(swresample STATIC IMPORTED)
set_target_properties(swresample PROPERTIES IMPORTED_LOCATION "${FFMPEG_BINARIES_ROOT}/libswresample.a")
add_library(avutil STATIC IMPORTED)
set_target_properties(avutil PROPERTIES IMPORTED_LOCATION "${FFMPEG_BINARIES_ROOT}/libavutil.a")

add_library(kodiCppAudioSinkJNI
            SHARED
            src/kodi_cpp_audiosink_bridge.cpp
            src/kodi_cpp_session_bridge.cpp
            src/kodi_cpp_truehd_session_bridge.cpp
            src/kodi_cpp_androidjni.cpp
            src/kodi_cpp_android_context.cpp
            src/KodiActiveAEEngine.cpp
            src/KodiTrueHdAEEngine.cpp
            src/KodiAudioTrackOutput.cpp
            src/KodiTrueHdAudioTrackOutput.cpp
            src/KodiIecPipeline.cpp
            src/KodiTrueHdFfmpegMatPacker.cpp
            src/KodiTrueHdIecPipeline.cpp
            ${FMT_ROOT}/src/format.cc
            ${KODI_XBMC_ROOT}/commons/Exception.cpp
            ${KODI_XBMC_ROOT}/cores/FFmpeg.cpp
            ${KODI_XBMC_ROOT}/ServiceBroker.cpp
            ${KODI_XBMC_ROOT}/utils/log.cpp
            ${KODI_XBMC_ROOT}/utils/TimeUtils.cpp
            ${KODI_XBMC_ROOT}/utils/ActorProtocol.cpp
            ${KODI_XBMC_ROOT}/utils/EndianSwap.cpp
            ${KODI_XBMC_ROOT}/utils/StringUtils.cpp
            ${KODI_XBMC_ROOT}/threads/Event.cpp
            ${KODI_XBMC_ROOT}/threads/Thread.cpp
            ${KODI_XBMC_ROOT}/platform/posix/threads/ThreadImplPosix.cpp
            ${KODI_XBMC_ROOT}/platform/posix/threads/RecursiveMutex.cpp
            ${KODI_XBMC_ROOT}/platform/android/utils/AndroidInterfaceForCLog.cpp
            ${KODI_XBMC_ROOT}/platform/android/MemUtils.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/AESinkFactory.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/AEResampleFactory.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Encoders/AEEncoderFFmpeg.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Engines/ActiveAE/ActiveAE.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Engines/ActiveAE/ActiveAEBuffer.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Engines/ActiveAE/ActiveAEFilter.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Engines/ActiveAE/ActiveAEResampleFFMPEG.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Engines/ActiveAE/ActiveAESink.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Engines/ActiveAE/ActiveAESettings.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Engines/ActiveAE/ActiveAESound.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Engines/ActiveAE/ActiveAEStream.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Sinks/AESinkAUDIOTRACK.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Utils/AELimiter.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Utils/AEChannelInfo.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Utils/AEDeviceInfo.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Utils/AEStreamInfo.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Utils/AEUtil.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Utils/AEBitstreamPacker.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Utils/AEPackIEC61937.cpp
            ${KODI_XBMC_ROOT}/cores/AudioEngine/Utils/PackerMAT.cpp)

target_compile_definitions(kodiCppAudioSinkJNI
                           PRIVATE
                           __STDC_CONSTANT_MACROS
                           __STDC_LIMIT_MACROS
                           TARGET_POSIX
                           TARGET_ANDROID
                           SPDLOG_FMT_EXTERNAL)

target_include_directories(kodiCppAudioSinkJNI
                           PRIVATE
                           ${CMAKE_CURRENT_LIST_DIR}/include
                           ${KODI_XBMC_ROOT}
                           ${KODI_XBMC_ROOT}/platform/posix
                           ${FMT_ROOT}/include
                           ${SPDLOG_ROOT}/include
                           ${FFMPEG_SOURCE_ROOT}
                           ${FFMPEG_HEADERS_ROOT}
                           ${FFMPEG_BINARIES_ROOT}/include)

find_library(android_log_lib log)
find_library(android_z_lib z)

target_link_libraries(kodiCppAudioSinkJNI
                      PRIVATE
                      avfilter
                      avcodec
                      avformat
                      swresample
                      avutil
                      ${android_log_lib}
                      ${android_z_lib})

target_link_options(kodiCppAudioSinkJNI
                    PRIVATE "-Wl,-z,max-page-size=16384")

if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_link_options(kodiCppAudioSinkJNI PRIVATE "-Wl,-Bsymbolic")
endif()
