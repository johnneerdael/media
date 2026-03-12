#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"

src_root="$repo_root/xbmc/xbmc"
dst_root="$repo_root/media/libraries/cpp_audiosink/kodi/xbmc"

mkdir -p "$dst_root"

rsync -a "$src_root/cores/AudioEngine/" "$dst_root/cores/AudioEngine/"
rsync -a "$src_root/cores/VideoPlayer/DVDCodecs/" "$dst_root/cores/VideoPlayer/DVDCodecs/"
rsync -a "$src_root/utils/" "$dst_root/utils/"
rsync -a "$src_root/threads/" "$dst_root/threads/"
rsync -a "$src_root/settings/" "$dst_root/settings/"
rsync -a "$src_root/platform/android/activity/" "$dst_root/platform/android/activity/"

cp "$src_root/cores/VideoPlayer/DVDStreamInfo.h" "$dst_root/cores/VideoPlayer/DVDStreamInfo.h"
cp "$src_root/cores/VideoPlayer/DVDStreamInfo.cpp" "$dst_root/cores/VideoPlayer/DVDStreamInfo.cpp"
cp "$src_root/ServiceBroker.h" "$dst_root/ServiceBroker.h"
