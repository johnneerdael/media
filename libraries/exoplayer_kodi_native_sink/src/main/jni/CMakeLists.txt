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

set(CMAKE_CXX_STANDARD 17)

project(libkodiNativeSinkJNI C CXX)

string(TOLOWER "${CMAKE_BUILD_TYPE}" build_type)
if(build_type MATCHES "^rel")
    add_compile_options("-O2")
endif()

add_library(kodiNativeSinkJNI
            SHARED
            jni_bridge.cpp
            AEChannelInfo.cpp
            AEDeviceInfo.cpp
            AEStreamInfo.cpp
            AEBitstreamPacker.cpp
            AEPackIEC61937.cpp
            AESinkAUDIOTRACK.cpp
            ActiveAESettings.cpp
            ActiveAESink.cpp
            ActiveAEPolicy.cpp
            KodiActorProtocol.cpp
            CJNIAudioAttributes.cpp
            CJNIAudioFormat.cpp
            CJNIAudioTrack.cpp
            CJNIAudioTimestamp.cpp
            DVDAudioCodecPassthrough.cpp
            KodiActiveAEBufferCompat.cpp
            KodiNativeAudioTrackSink.cpp
            PackerMAT.cpp
            KodiCapabilitySelector.cpp
            KodiNativeSinkSession.cpp)

find_library(android_log_lib log)

target_link_libraries(kodiNativeSinkJNI
                      PRIVATE ${android_log_lib})

target_link_options(kodiNativeSinkJNI
                    PRIVATE "-Wl,-z,max-page-size=16384")
