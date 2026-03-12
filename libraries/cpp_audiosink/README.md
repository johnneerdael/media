# cpp_audiosink

This directory is the staging area for a strict Kodi-source transplant of the
Android AudioTrack / ActiveAE / IEC packer path.

Rules for the copied Kodi subtree:

- `kodi/xbmc/...` must remain byte-for-byte copied from the local `./xbmc/xbmc`
  source tree.
- Do not hand-edit files under `kodi/xbmc/...`.
- Any Nexio-specific adaptation belongs outside the copied subtree.

Current intent:

1. Keep the original Kodi file structure intact under `kodi/xbmc/...`.
2. Build a minimal JNI/Android adapter around those copied files.
3. Keep the JNI/Android shell in `media/libraries/exoplayer_kodi_cpp_audiosink`
   as the only native audio sink integration path.

Current copied coverage includes:

- `xbmc/cores/AudioEngine/...`
- `xbmc/cores/VideoPlayer/DVDCodecs/...`
- `xbmc/cores/VideoPlayer/DVDStreamInfo.*`
- `xbmc/utils/...`
- `xbmc/threads/...`
- `xbmc/settings/...`
- `xbmc/platform/android/activity/...`
- `xbmc/ServiceBroker.h`

Known remaining integration blockers:

- Kodi's Android sink depends on `libandroidjni` headers (`androidjni/...`),
  which are not currently vendored in this repo as source files.
- The copied subtree still expects broader Kodi infrastructure that must be
  handled by a thin Nexio-side adapter/build shell, not by editing copied files.
