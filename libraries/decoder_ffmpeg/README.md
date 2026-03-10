# FFmpeg decoder module

The FFmpeg module provides `FfmpegAudioRenderer`, which uses FFmpeg for decoding
and can render audio encoded in a variety of formats.

## License note

Please note that whilst the code in this repository is licensed under
[Apache 2.0][], using this module also requires building and including one or
more external libraries as described below. These are licensed separately.

[Apache 2.0]: ../../LICENSE

## Build instructions (Linux, macOS)

To use the module you need to clone this GitHub project and depend on its
modules locally. Instructions for doing this can be found in the
[top level README][]. The module is not provided via Google's Maven repository
(see [ExoPlayer issue 2781][] for more information).

In addition, it's necessary to manually build the FFmpeg library, so that gradle
can bundle the FFmpeg binaries in the APK:

* Set the following shell variable:

```
cd "<path to project checkout>"
FFMPEG_MODULE_PATH="$(pwd)/libraries/decoder_ffmpeg/src/main"
```

*   Download the [Android NDK][] and set its location in a shell variable. This
    build configuration has been tested on NDK r26b (r23c if ANDROID_ABI is less
    than 21).

```
NDK_PATH="<path to Android NDK>"
```

* Set the host platform (use "darwin-x86_64" for Mac OS X):

```
HOST_PLATFORM="linux-x86_64"
```

*   Set the ABI version for native code (typically it's equal to minSdk and must
    not exceed it):

```
ANDROID_ABI=21
```

*   Fetch FFmpeg and checkout an appropriate branch. We cannot guarantee
    compatibility with all versions of FFmpeg. We currently recommend version
    6.1 (or 6.0):

```
cd "<preferred location for ffmpeg>" && \
git clone git://source.ffmpeg.org/ffmpeg && \
cd ffmpeg && \
git checkout release/6.1 && \
FFMPEG_PATH="$(pwd)"
```

`build_ffmpeg.sh` detects the FFmpeg version from `VERSION`, `RELEASE`, or
`libavcodec/version_major.h`.

* Configure the decoders to include. See the [Supported formats][] page for
  details of the available decoders, and which formats they support.

```
ENABLED_DECODERS=(aac mp3 ac3 eac3 truehd dca vorbis opus amrnb amrwb flac alac pcm_mulaw pcm_alaw h264 hevc vc1 mpegvideo mpeg2video vp8 vp9)
```

*   Add a link to the FFmpeg source code in the FFmpeg module `jni` directory.

```
cd "${FFMPEG_MODULE_PATH}/jni" && \
ln -s "$FFMPEG_PATH" ffmpeg
```

* Execute `build_ffmpeg.sh` to build FFmpeg for `armeabi-v7a`, `arm64-v8a`,
  `x86` and `x86_64`. The script can be edited if you need to build for
  different architectures:

```
cd "${FFMPEG_MODULE_PATH}/jni" && \
./build_ffmpeg.sh \
  "${FFMPEG_MODULE_PATH}" "${NDK_PATH}" "${HOST_PLATFORM}" "${ANDROID_ABI}" "${ENABLED_DECODERS[@]}"
```

### Optional: DV5 tone mapping with libplacebo (experimental)

To enable the experimental DV5 software tone-mapping path, build FFmpeg with
`libavfilter + libplacebo` enabled:

1. Build static `libplacebo`, `shaderc` and Vulkan headers per ABI, and stage
   them in:

```
<LIBPLACEBO_PREBUILT_ROOT>/<abi>/include
<LIBPLACEBO_PREBUILT_ROOT>/<abi>/lib
<LIBPLACEBO_PREBUILT_ROOT>/<abi>/lib/pkgconfig
```

2. Build Shield-safe libplacebo dependencies (Vulkan 1.1 runtime target):

```
cd "${FFMPEG_MODULE_PATH}/jni" && \
./build_libplacebo_android.sh \
  "<path to libplacebo prebuilts>" "${NDK_PATH}" "${HOST_PLATFORM}" "${ANDROID_ABI}"
```

Notes:
- Default `LIBPLACEBO_TAG` is `v5.264.1`.
- `v5.229.2` is also supported:
  `LIBPLACEBO_TAG=v5.229.2 ./build_libplacebo_android.sh ...`

3. Re-run the FFmpeg build script with:

```
FFMPEG_ENABLE_LIBPLACEBO=1 \
LIBPLACEBO_PREBUILT_ROOT="<path to staged libplacebo prebuilts>" \
./build_ffmpeg.sh \
  "${FFMPEG_MODULE_PATH}" "${NDK_PATH}" "${HOST_PLATFORM}" "${ANDROID_ABI}" "${ENABLED_DECODERS[@]}"
```

Notes:
- If `libavfilter.a` is absent, the JNI builds without the tone-map path.
- For Android TV boxes limited to Vulkan 1.1, use FFmpeg 6.x and `libplacebo`
  v5.x (`v5.264.1` default; `v5.229.2` also supported by
  `build_libplacebo_android.sh`).
- FFmpeg 7.x is not supported for this path. `build_ffmpeg.sh` will fail fast
  unless the detected FFmpeg version is 6.x when
  `FFMPEG_ENABLE_LIBPLACEBO=1`.

* [Install CMake][]

Having followed these steps, gradle will build the module automatically when run
on the command line or via Android Studio, using [CMake][] and [Ninja][] to
configure and build the module's [JNI wrapper library][].

## Build instructions (Windows)

We do not provide support for building this module on Windows, however it should
be possible to follow the Linux instructions in [Windows PowerShell][].

[Windows PowerShell]: https://docs.microsoft.com/en-us/powershell/scripting/getting-started/getting-started-with-windows-powershell

## Using the module with ExoPlayer

Once you've followed the instructions above to check out, build and depend on
the module, the next step is to tell ExoPlayer to use `FfmpegAudioRenderer`. How
you do this depends on which player API you're using:

*   If you're passing a `DefaultRenderersFactory` to `ExoPlayer.Builder`, you
    can enable using the module by setting the `extensionRendererMode` parameter
    of the `DefaultRenderersFactory` constructor to
    `EXTENSION_RENDERER_MODE_ON`. This will use `FfmpegAudioRenderer` for
    playback if `MediaCodecAudioRenderer` doesn't support the input format. Pass
    `EXTENSION_RENDERER_MODE_PREFER` to give `FfmpegAudioRenderer` priority over
    `MediaCodecAudioRenderer`.
*   If you've subclassed `DefaultRenderersFactory`, add an `FfmpegAudioRenderer`
    to the output list in `buildAudioRenderers`. ExoPlayer will use the first
    `Renderer` in the list that supports the input media format.
*   If you've implemented your own `RenderersFactory`, return an
    `FfmpegAudioRenderer` instance from `createRenderers`. ExoPlayer will use
    the first `Renderer` in the returned array that supports the input media
    format.
*   If you're using `ExoPlayer.Builder`, pass an `FfmpegAudioRenderer` in the
    array of `Renderer`s. ExoPlayer will use the first `Renderer` in the list
    that supports the input media format.

Note: These instructions assume you're using `DefaultTrackSelector`. If you have
a custom track selector the choice of `Renderer` is up to your implementation,
so you need to make sure you are passing an `FfmpegAudioRenderer` to the player,
then implement your own logic to use the renderer for a given track.

[top level README]: ../../README.md
[Android NDK]: https://developer.android.com/tools/sdk/ndk/index.html
[Ninja]: https://ninja-build.org/
[Install CMake]: https://developer.android.com/studio/projects/install-ndk
[CMake]: https://cmake.org/
[JNI wrapper library]: src/main/jni/ffmain.cpp
[ExoPlayer issue 2781]: https://github.com/google/ExoPlayer/issues/2781
[Supported formats]: https://developer.android.com/media/media3/exoplayer/supported-formats#ffmpeg-library

## Links

*   [Troubleshooting using decoding extensions][]

[Troubleshooting using decoding extensions]: https://developer.android.com/media/media3/exoplayer/troubleshooting#how-can-i-get-a-decoding-library-to-load-and-be-used-for-playback
