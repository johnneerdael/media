# Android Sink Transplant Plan

## Goal

Replace the reduced custom native audio path with a JNI shell around copied Kodi
sources.

## Passes

1. Source mirror
   - Copy the Kodi source closure for AudioTrack / ActiveAE / passthrough /
     IEC packing into `cpp_audiosink/kodi/xbmc/...`.
   - Keep original file layout and file contents intact.

2. Dependency closure
   - Mirror the remaining Kodi headers/sources directly required by the Android
     sink path.
   - Resolve external-source dependencies such as `libandroidjni`.

3. Build shell
   - Add a new CMake/Gradle shell outside the copied subtree.
   - Compile copied Kodi sources without modifying the copied files.

4. JNI adapter
   - Write the minimal Nexio-specific bridge that passes Media3 input into the
     copied Kodi path.
   - Keep all custom logic out of the copied subtree.

5. Runtime switch
   - Introduce a separate experimental path targeting `cpp_audiosink`.
   - Compare behavior against the current reduced module.

6. Migration
   - Remove or bypass reduced native sink code once the copied Kodi path is
     stable on-device.
