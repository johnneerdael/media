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
import android.os.SystemClock;
import android.util.Log;
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

/** Dedicated Java bridge for the separate TrueHD path. */
@UnstableApi
public final class KodiTrueHdNativeAudioSink extends ForwardingAudioSink
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

  private static final String TAG = "KodiTrueHdSink";
  private static final long RETRY_DURATION_MS = 200;
  private static final long RETRY_DELAY_MS = 50;

  private long nativeHandle;
  @Nullable private AudioAttributes audioAttributes;
  @Nullable private AudioDeviceInfo preferredDevice;
  @Nullable private Format configuredFormat;
  private float volume;
  private PlaybackParameters playbackParameters;
  private long rendererClockUs;
  private boolean playCommandReceived;
  private boolean handledEndOfStream;
  private int pendingWriteErrorCode;
  private long pendingWriteErrorDeadlineMs;
  private long pendingWriteEarliestRetryMs;
  private long pendingReleaseUntilMs;

  public KodiTrueHdNativeAudioSink(AudioSink sink) {
    super(sink);
    nativeHandle = 0L;
    volume = 1f;
    playbackParameters = PlaybackParameters.DEFAULT;
    rendererClockUs = C.TIME_UNSET;
    playCommandReceived = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    pendingReleaseUntilMs = C.TIME_UNSET;
  }

  @Override
  public boolean supportsFormat(Format format) {
    return getFormatSupport(format) != SINK_FORMAT_UNSUPPORTED;
  }

  @Override
  public @SinkFormatSupport int getFormatSupport(Format format) {
    if (MimeTypes.AUDIO_TRUEHD.equals(format.sampleMimeType)
        && AudioCapabilities.isExperimentalFireOsIecPassthroughEnabled()) {
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
    handledEndOfStream = false;
    ensureSession(inputFormat);
    NativeConfig config =
        new NativeConfig(
            inputFormat.sampleMimeType,
            inputFormat.sampleRate,
            inputFormat.channelCount,
            inputFormat.pcmEncoding != Format.NO_VALUE ? inputFormat.pcmEncoding : C.ENCODING_INVALID,
            resolvePreferredDeviceName(preferredDevice),
            volume,
            AmazonQuirks.isFireOsIecSuperviseAudioDelayEnabled(),
            AudioCapabilities.isFireOsIecVerboseLoggingEnabled());
    boolean configured = configureWithRetry(config);
    if (!configured) {
      throw new ConfigurationException(
          new IllegalStateException("Failed to configure Kodi TrueHD native sink session"),
          inputFormat);
    }
    clearPendingWriteError();
    pendingReleaseUntilMs = C.TIME_UNSET;
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
    if (pendingWriteErrorCode != 0 && shouldWaitBeforeWriteRetry()) {
      return false;
    }
    int bytesConsumed =
        nWrite(
            nativeHandle,
            writeBuffer,
            writeBuffer.position(),
            writeBuffer.remaining(),
            presentationTimeUs,
            encodedAccessUnitCount);
    nConsumeLastWriteOutputBytes(nativeHandle);
    int nativeWriteErrorCode = nConsumeLastWriteErrorCode(nativeHandle);
    if (nativeWriteErrorCode < 0 && bytesConsumed <= 0) {
      maybeHandlePendingWriteError(nativeWriteErrorCode);
      return false;
    }
    if (bytesConsumed <= 0) {
      return false;
    }
    clearPendingWriteError();
    handledEndOfStream = false;
    buffer.position(buffer.position() + Math.min(bytesConsumed, originalRemaining));
    return bytesConsumed >= originalRemaining;
  }

  @Override
  public void play() {
    playCommandReceived = true;
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(TAG, "Media3 -> TrueHD AudioSink play() nativeHandle=" + nativeHandle);
    }
    if (nativeHandle != 0L) {
      nPlay(nativeHandle);
    }
  }

  @Override
  public void pause() {
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(TAG, "Media3 -> TrueHD AudioSink pause()");
    }
    if (nativeHandle != 0L) {
      nPause(nativeHandle);
    }
  }

  @Override
  public void handleDiscontinuity() {
    if (nativeHandle != 0L) {
      nHandleDiscontinuity(nativeHandle);
    }
  }

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
    handledEndOfStream = true;
  }

  @Override
  public boolean isEnded() {
    return nativeHandle == 0L || (handledEndOfStream && nIsEnded(nativeHandle));
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
    playCommandReceived = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    pendingReleaseUntilMs = SystemClock.elapsedRealtime() + RETRY_DURATION_MS;
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(TAG, "Media3 -> TrueHD AudioSink flush()");
    }
    if (nativeHandle != 0L) {
      nFlush(nativeHandle);
    }
  }

  @Override
  public void reset() {
    playCommandReceived = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    closeSession(true);
    configuredFormat = null;
    super.reset();
  }

  @Override
  public void release() {
    playCommandReceived = false;
    handledEndOfStream = false;
    clearPendingWriteError();
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
          new IllegalStateException("Failed to create Kodi TrueHD native sink session"), inputFormat);
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

  private boolean configureWithRetry(NativeConfig config) {
    long deadlineMs = SystemClock.elapsedRealtime() + RETRY_DURATION_MS;
    int attempts = 0;
    while (true) {
      if (isInPendingReleaseWindow()) {
        try {
          Thread.sleep(RETRY_DELAY_MS);
        } catch (InterruptedException e) {
          Thread.currentThread().interrupt();
          return false;
        }
        continue;
      }
      attempts++;
      boolean configured = nConfigure(nativeHandle, config);
      if (configured) {
        pendingReleaseUntilMs = C.TIME_UNSET;
        return true;
      }
      if (SystemClock.elapsedRealtime() >= deadlineMs) {
        if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
          Log.w(TAG, "TrueHD nConfigure retry deadline reached attempts=" + attempts);
        }
        return false;
      }
      try {
        Thread.sleep(RETRY_DELAY_MS);
      } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
        return false;
      }
    }
  }

  private boolean shouldWaitBeforeWriteRetry() {
    long nowMs = SystemClock.elapsedRealtime();
    if (isInPendingReleaseWindow()) {
      return true;
    }
    if (pendingWriteErrorDeadlineMs != C.TIME_UNSET && nowMs >= pendingWriteErrorDeadlineMs) {
      return false;
    }
    return pendingWriteEarliestRetryMs != C.TIME_UNSET && nowMs < pendingWriteEarliestRetryMs;
  }

  private void maybeHandlePendingWriteError(int errorCode) throws WriteException {
    long nowMs = SystemClock.elapsedRealtime();
    if (pendingWriteErrorCode == 0) {
      pendingWriteErrorCode = errorCode;
      pendingWriteErrorDeadlineMs = isInPendingReleaseWindow() ? C.TIME_UNSET : nowMs + RETRY_DURATION_MS;
      pendingWriteEarliestRetryMs = nowMs + RETRY_DELAY_MS;
      return;
    }
    if (pendingWriteErrorDeadlineMs == C.TIME_UNSET && !isInPendingReleaseWindow()) {
      pendingWriteErrorDeadlineMs = nowMs + RETRY_DURATION_MS;
    }
    if (pendingWriteErrorDeadlineMs != C.TIME_UNSET && nowMs >= pendingWriteErrorDeadlineMs) {
      int finalErrorCode = pendingWriteErrorCode;
      clearPendingWriteError();
      throw new WriteException(finalErrorCode, configuredFormat, /* isRecoverable= */ true);
    }
    pendingWriteEarliestRetryMs = nowMs + RETRY_DELAY_MS;
  }

  private void clearPendingWriteError() {
    pendingWriteErrorCode = 0;
    pendingWriteErrorDeadlineMs = C.TIME_UNSET;
    pendingWriteEarliestRetryMs = C.TIME_UNSET;
  }

  private boolean isInPendingReleaseWindow() {
    if (nativeHandle != 0L && nIsReleasePending(nativeHandle)) {
      return true;
    }
    return pendingReleaseUntilMs != C.TIME_UNSET && SystemClock.elapsedRealtime() < pendingReleaseUntilMs;
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

  private static native int nConsumeLastWriteOutputBytes(long nativeHandle);

  private static native int nConsumeLastWriteErrorCode(long nativeHandle);

  private static native boolean nIsReleasePending(long nativeHandle);

  private static native void nPlay(long nativeHandle);

  private static native void nPause(long nativeHandle);

  private static native void nFlush(long nativeHandle);

  private static native void nDrain(long nativeHandle);

  private static native void nHandleDiscontinuity(long nativeHandle);

  private static native void nSetVolume(long nativeHandle, float volume);

  private static native void nSetHostClockUs(long nativeHandle, long hostClockUs);

  private static native void nSetHostClockSpeed(long nativeHandle, double speed);

  private static native long nGetCurrentPositionUs(long nativeHandle);

  private static native boolean nHasPendingData(long nativeHandle);

  private static native boolean nIsEnded(long nativeHandle);

  private static native long nGetBufferSizeUs(long nativeHandle);

  private static native void nRelease(long nativeHandle);
}
