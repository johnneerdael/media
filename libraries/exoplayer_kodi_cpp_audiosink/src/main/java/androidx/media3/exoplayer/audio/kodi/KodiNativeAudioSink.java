/*
 * Copyright (C) 2026 Nuvio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
package androidx.media3.exoplayer.audio.kodi;

import android.media.AudioDeviceInfo;
import androidx.annotation.Nullable;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackParameters;
import androidx.media3.common.util.AmazonQuirks;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.audio.AudioCapabilities;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.ForwardingAudioSink;
import androidx.media3.exoplayer.audio.RendererClockAwareAudioSink;
import java.nio.ByteBuffer;

@UnstableApi
public final class KodiNativeAudioSink extends ForwardingAudioSink
    implements RendererClockAwareAudioSink {

  static {
    System.loadLibrary("kodiCppAudioSinkJNI");
  }

  private static final class NativeConfig {
    public final @Nullable String sampleMimeType;
    public final int sampleRate;
    public final int channelCount;
    public final int pcmEncoding;
    public final String preferredDevice;
    public final float volume;
    public final boolean superviseAudioDelay;
    public final boolean iecVerboseLogging;

    NativeConfig(
        @Nullable String sampleMimeType,
        int sampleRate,
        int channelCount,
        int pcmEncoding,
        String preferredDevice,
        float volume,
        boolean superviseAudioDelay,
        boolean iecVerboseLogging) {
      this.sampleMimeType = sampleMimeType;
      this.sampleRate = sampleRate;
      this.channelCount = channelCount;
      this.pcmEncoding = pcmEncoding;
      this.preferredDevice = preferredDevice;
      this.volume = volume;
      this.superviseAudioDelay = superviseAudioDelay;
      this.iecVerboseLogging = iecVerboseLogging;
    }
  }

  private long nativeHandle;
  @Nullable private AudioAttributes audioAttributes;
  @Nullable private AudioDeviceInfo preferredDevice;
  @Nullable private Format configuredFormat;
  private float volume;
  private PlaybackParameters playbackParameters;
  private long rendererClockUs;

  public KodiNativeAudioSink(AudioSink sink) {
    super(sink);
    nativeHandle = 0L;
    volume = 1f;
    playbackParameters = PlaybackParameters.DEFAULT;
    rendererClockUs = C.TIME_UNSET;
  }

  @Override
  public boolean supportsFormat(Format format) {
    return getFormatSupport(format) != SINK_FORMAT_UNSUPPORTED;
  }

  @Override
  public @SinkFormatSupport int getFormatSupport(Format format) {
    if (AudioCapabilities.isExperimentalFireOsIecPassthroughEnabled()
        && isKodiPassthroughMime(format.sampleMimeType)) {
      return SINK_FORMAT_SUPPORTED_DIRECTLY;
    }
    return super.getFormatSupport(format);
  }

  @Override
  public void setAudioAttributes(AudioAttributes audioAttributes) {
    this.audioAttributes = audioAttributes;
  }

  @Override
  public @Nullable AudioAttributes getAudioAttributes() {
    return audioAttributes;
  }

  @Override
  public void setPreferredDevice(@Nullable AudioDeviceInfo audioDeviceInfo) {
    preferredDevice = audioDeviceInfo;
  }

  @Override
  public void setVolume(float volume) {
    this.volume = volume;
    if (nativeHandle != 0L) {
      nSetVolume(nativeHandle, volume);
    }
  }

  @Override
  public void setPlaybackParameters(PlaybackParameters playbackParameters) {
    this.playbackParameters = playbackParameters;
    if (nativeHandle != 0L) {
      nSetHostClockSpeed(nativeHandle, playbackParameters.speed);
    }
  }

  @Override
  public PlaybackParameters getPlaybackParameters() {
    return playbackParameters;
  }

  @Override
  public void configure(Format inputFormat, int specifiedBufferSize, @Nullable int[] outputChannels)
      throws ConfigurationException {
    configuredFormat = inputFormat;
    ensureSession(inputFormat);
    boolean configured =
        nConfigure(
            nativeHandle,
            new NativeConfig(
                inputFormat.sampleMimeType,
                inputFormat.sampleRate,
                inputFormat.channelCount,
                inputFormat.pcmEncoding != Format.NO_VALUE ? inputFormat.pcmEncoding : C.ENCODING_INVALID,
                resolvePreferredDeviceName(preferredDevice),
                volume,
                AmazonQuirks.isFireOsIecSuperviseAudioDelayEnabled(),
                AudioCapabilities.isFireOsIecVerboseLoggingEnabled()));
    if (!configured) {
      throw new ConfigurationException(
          new IllegalStateException("Failed to configure Kodi native sink session"), inputFormat);
    }
  }

  @Override
  public boolean handleBuffer(ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount)
      throws WriteException {
    if (!buffer.hasRemaining()) {
      return true;
    }
    if (nativeHandle == 0L || configuredFormat == null) {
      throw new WriteException(/* errorCode= */ -1, configuredFormat, /* isRecoverable= */ false);
    }

    int originalRemaining = buffer.remaining();
    ByteBuffer writeBuffer = buffer.isDirect() ? buffer.slice() : copyToDirectBuffer(buffer);
    int bytesConsumed =
        nWrite(
            nativeHandle,
            writeBuffer,
            writeBuffer.position(),
            writeBuffer.remaining(),
            presentationTimeUs,
            encodedAccessUnitCount);
    if (bytesConsumed <= 0) {
      return false;
    }
    buffer.position(buffer.position() + Math.min(bytesConsumed, originalRemaining));
    return bytesConsumed >= originalRemaining;
  }

  @Override
  public void play() {
    if (nativeHandle != 0L) {
      nPlay(nativeHandle);
    }
  }

  @Override
  public void pause() {
    if (nativeHandle != 0L) {
      nPause(nativeHandle);
    }
  }

  @Override
  public void handleDiscontinuity() {}

  @Override
  public long getCurrentPositionUs(boolean sourceEnded) {
    return nativeHandle == 0L ? CURRENT_POSITION_NOT_SET : nGetCurrentPositionUs(nativeHandle);
  }

  @Override
  public void playToEndOfStream() throws WriteException {
    if (nativeHandle == 0L || configuredFormat == null) {
      return;
    }
    nDrain(nativeHandle);
  }

  @Override
  public boolean isEnded() {
    return nativeHandle == 0L || nIsEnded(nativeHandle);
  }

  @Override
  public boolean hasPendingData() {
    return nativeHandle != 0L && nHasPendingData(nativeHandle);
  }

  @Override
  public long getAudioTrackBufferSizeUs() {
    return nativeHandle == 0L ? 0L : nGetBufferSizeUs(nativeHandle);
  }

  @Override
  public void flush() {
    if (nativeHandle != 0L) {
      nFlush(nativeHandle);
    }
  }

  @Override
  public void reset() {
    closeSession(true);
    configuredFormat = null;
    super.reset();
  }

  @Override
  public void release() {
    closeSession(false);
    configuredFormat = null;
    super.release();
  }

  @Override
  public void setRendererClockUs(long rendererClockUs) {
    this.rendererClockUs = rendererClockUs;
    if (nativeHandle != 0L && rendererClockUs != C.TIME_UNSET) {
      nSetHostClockUs(nativeHandle, rendererClockUs);
    }
  }

  private void ensureSession(Format inputFormat) throws ConfigurationException {
    if (nativeHandle != 0L) {
      return;
    }
    nativeHandle = nCreate();
    if (nativeHandle == 0L) {
      throw new ConfigurationException(
          new IllegalStateException("Failed to create Kodi native sink session"), inputFormat);
    }
    nSetHostClockSpeed(nativeHandle, playbackParameters.speed);
    if (rendererClockUs != C.TIME_UNSET) {
      nSetHostClockUs(nativeHandle, rendererClockUs);
    }
  }

  private void closeSession(boolean resetFirst) {
    if (nativeHandle == 0L) {
      return;
    }
    try {
      if (resetFirst && configuredFormat != null) {
        nFlush(nativeHandle);
      }
      nRelease(nativeHandle);
    } finally {
      nativeHandle = 0L;
    }
  }

  private static String resolvePreferredDeviceName(@Nullable AudioDeviceInfo audioDeviceInfo) {
    if (audioDeviceInfo == null || audioDeviceInfo.getProductName() == null) {
      return "";
    }
    return audioDeviceInfo.getProductName().toString();
  }

  private static ByteBuffer copyToDirectBuffer(ByteBuffer source) {
    ByteBuffer directBuffer = ByteBuffer.allocateDirect(source.remaining());
    directBuffer.put(source.duplicate());
    directBuffer.flip();
    return directBuffer;
  }

  private static boolean isKodiPassthroughMime(@Nullable String sampleMimeType) {
    return MimeTypes.AUDIO_AC3.equals(sampleMimeType)
        || MimeTypes.AUDIO_E_AC3.equals(sampleMimeType)
        || MimeTypes.AUDIO_E_AC3_JOC.equals(sampleMimeType)
        || MimeTypes.AUDIO_DTS.equals(sampleMimeType)
        || MimeTypes.AUDIO_DTS_HD.equals(sampleMimeType)
        || MimeTypes.AUDIO_TRUEHD.equals(sampleMimeType)
        || "audio/vnd.dts.uhd;profile=p2".equals(sampleMimeType);
  }

  private static native long nCreate();

  private static native boolean nConfigure(long nativeHandle, NativeConfig config);

  private static native int nWrite(
      long nativeHandle,
      ByteBuffer buffer,
      int offset,
      int size,
      long presentationTimeUs,
      int encodedAccessUnitCount);

  private static native void nPlay(long nativeHandle);

  private static native void nPause(long nativeHandle);

  private static native void nFlush(long nativeHandle);

  private static native void nDrain(long nativeHandle);

  private static native void nSetVolume(long nativeHandle, float volume);

  private static native void nSetHostClockUs(long nativeHandle, long hostClockUs);

  private static native void nSetHostClockSpeed(long nativeHandle, double speed);

  private static native long nGetCurrentPositionUs(long nativeHandle);

  private static native boolean nHasPendingData(long nativeHandle);

  private static native boolean nIsEnded(long nativeHandle);

  private static native long nGetBufferSizeUs(long nativeHandle);

  private static native void nRelease(long nativeHandle);
}
