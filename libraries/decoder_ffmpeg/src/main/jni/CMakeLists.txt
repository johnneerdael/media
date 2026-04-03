#
# Copyright 2021 The Android Open Source Project
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

# LINT.IfChange
cmake_minimum_required(VERSION 3.21.0 FATAL_ERROR)
# LINT.ThenChange(../../../build.gradle)

# Enable C++11 features.
set(CMAKE_CXX_STANDARD 11)

project(libffmpegJNI C CXX)

set(ffmpeg_location "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg")
set(ffmpeg_headers "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-headers")
set(ffmpeg_binaries "${ffmpeg_location}/android-libs/${ANDROID_ABI}")

foreach(ffmpeg_lib avutil swresample swscale avcodec avformat)
    set(ffmpeg_lib_filename lib${ffmpeg_lib}.a)
    set(ffmpeg_lib_file_path ${ffmpeg_binaries}/${ffmpeg_lib_filename})
    add_library(
            ${ffmpeg_lib}
            STATIC
            IMPORTED)
    set_target_properties(
            ${ffmpeg_lib} PROPERTIES
            IMPORTED_LOCATION
            ${ffmpeg_lib_file_path})
endforeach()

set(ffmpeg_avfilter_file_path "${ffmpeg_binaries}/libavfilter.a")
set(ffmpeg_has_avfilter FALSE)
set(ffmpeg_extension_static_libs "")
if(EXISTS "${ffmpeg_avfilter_file_path}")
    add_library(
            avfilter
            STATIC
            IMPORTED)
    set_target_properties(
            avfilter PROPERTIES
            IMPORTED_LOCATION
            "${ffmpeg_avfilter_file_path}")
    set(ffmpeg_has_avfilter TRUE)
    # When FFmpeg is built with static libplacebo/shaderc, libavfilter.a keeps
    # unresolved references that must be linked by this final JNI shared lib.
    file(GLOB CONFIGURE_DEPENDS ffmpeg_extension_static_libs "${ffmpeg_binaries}/*.a")
    list(FILTER ffmpeg_extension_static_libs EXCLUDE REGEX "/lib(avutil|swresample|swscale|avcodec|avfilter)\\.a$")
    list(FILTER ffmpeg_extension_static_libs INCLUDE REGEX "\\.a$")
    list(SORT ffmpeg_extension_static_libs)
endif()

# Prefer staged FFmpeg headers. This avoids collisions with FFmpeg top-level
# files (for example "version") when compiling C++.
if(EXISTS "${ffmpeg_headers}/config.h")
    include_directories(${ffmpeg_headers})
else()
    include_directories(${ffmpeg_location})
endif()
if(EXISTS "${ffmpeg_binaries}/include")
    include_directories(${ffmpeg_binaries}/include)
endif()
find_library(android_log_lib log)
find_library(android_dl_lib dl)
find_library(android_vulkan_lib vulkan)
find_library(android_z_lib z)

add_library(ffmpegJNI
            SHARED
            ffmain.cpp
            ffcommon.cpp
            ffaudio.cpp
            ffvideo.cpp)

if(ffmpeg_has_avfilter)
    target_compile_definitions(ffmpegJNI PRIVATE FFMPEG_TONEMAP_FILTERS=1)
else()
    target_compile_definitions(ffmpegJNI PRIVATE FFMPEG_TONEMAP_FILTERS=0)
endif()

target_link_libraries(ffmpegJNI
                      PRIVATE android
                      PRIVATE avformat
                      PRIVATE swscale
                      PRIVATE swresample
                      PRIVATE avcodec
                      PRIVATE avutil
                      PRIVATE ${android_log_lib})
if(ffmpeg_has_avfilter)
    target_link_libraries(ffmpegJNI PRIVATE avfilter)
    if(ffmpeg_extension_static_libs)
        target_link_libraries(ffmpegJNI PRIVATE ${ffmpeg_extension_static_libs})
    endif()
endif()
if(android_dl_lib)
    target_link_libraries(ffmpegJNI PRIVATE ${android_dl_lib})
endif()
if(android_vulkan_lib)
    target_link_libraries(ffmpegJNI PRIVATE ${android_vulkan_lib})
endif()
if(android_z_lib)
    target_link_libraries(ffmpegJNI PRIVATE ${android_z_lib})
endif()

# Additional flags needed for "arm64-v8a" from NDK 23.1.7779620 and above.
# See https://github.com/google/ExoPlayer/issues/9933#issuecomment-1029775358.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_link_options(ffmpegJNI PRIVATE "-Wl,-Bsymbolic")
endif()

# Enable 16 KB ELF alignment.
target_link_options(ffmpegJNI
                    PRIVATE "-Wl,-z,max-page-size=16384")
