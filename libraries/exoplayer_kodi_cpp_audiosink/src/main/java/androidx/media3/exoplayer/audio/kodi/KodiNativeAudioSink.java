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

@UnstableApi
public final class KodiNativeAudioSink extends ForwardingAudioSink
    implements RendererClockAwareAudioSink {
  private static final int E_AC3_IEC_BURST_WINDOW_ACCESS_UNITS = 6;

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
  @Nullable private final RendererClockAwareAudioSink rendererClockAwareDelegate;
  @Nullable private AudioAttributes audioAttributes;
  @Nullable private AudioDeviceInfo preferredDevice;
  @Nullable private Format configuredFormat;
  private float volume;
  private PlaybackParameters playbackParameters;
  private long rendererClockUs;
  private boolean playCommandReceived;
  private boolean nativePlayIssued;
  private boolean handledEndOfStream;
  private static final String TAG = "KodiNativeSink";
  private static final long RETRY_DURATION_MS = 200;
  private static final long RETRY_DELAY_MS = 50;
  private static final long MIN_PASSTHROUGH_STARTUP_WINDOW_US = 1_000;
  private static final int MAX_PASSTHROUGH_STARTUP_FLUSH_WRITES = 4;

  private int pendingWriteErrorCode;
  private long pendingWriteErrorDeadlineMs;
  private long pendingWriteEarliestRetryMs;
  private long pendingReleaseUntilMs;
  @Nullable private byte[] pendingPassthroughStartupData;
  private int pendingPassthroughStartupSize;
  private int pendingPassthroughStartupAcknowledgedBytes;
  private int pendingPassthroughStartupAccessUnits;
  private long pendingPassthroughStartupFirstPtsUs;
  private long pendingPassthroughStartupLastPtsUs;
  private long pendingPassthroughStartupLastDurationUs;
  @Nullable private byte[] pendingEac3BurstWindowData;
  private int pendingEac3BurstWindowSize;
  private int pendingEac3BurstWindowAcknowledgedBytes;
  private int pendingEac3BurstWindowAccessUnits;
  private long pendingEac3BurstWindowPtsUs;

  public KodiNativeAudioSink(AudioSink sink) {
    super(sink);
    rendererClockAwareDelegate =
        sink instanceof RendererClockAwareAudioSink ? (RendererClockAwareAudioSink) sink : null;
    nativeHandle = 0L;
    volume = 1f;
    playbackParameters = PlaybackParameters.DEFAULT;
    rendererClockUs = C.TIME_UNSET;
    playCommandReceived = false;
    nativePlayIssued = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    pendingReleaseUntilMs = C.TIME_UNSET;
    clearPendingPassthroughStartupWindow();
    clearPendingEac3BurstWindow();
  }

  @Override
  public boolean supportsFormat(Format format) {
    return getFormatSupport(format) != SINK_FORMAT_UNSUPPORTED;
  }

  @Override
  public @SinkFormatSupport int getFormatSupport(Format format) {
    if (AudioCapabilities.isExperimentalFireOsIecPassthroughEnabled()
        && isKodiPassthroughEnabledForFormat(format)) {
      return SINK_FORMAT_SUPPORTED_DIRECTLY;
    }
    return super.getFormatSupport(format);
  }

  @Override
  public void setAudioAttributes(AudioAttributes audioAttributes) {
    this.audioAttributes = audioAttributes;
    super.setAudioAttributes(audioAttributes);
  }

  @Override
  public @Nullable AudioAttributes getAudioAttributes() {
    return audioAttributes;
  }

  @Override
  public void setPreferredDevice(@Nullable AudioDeviceInfo audioDeviceInfo) {
    preferredDevice = audioDeviceInfo;
    super.setPreferredDevice(audioDeviceInfo);
  }

  @Override
  public void setVolume(float volume) {
    this.volume = volume;
    super.setVolume(volume);
    if (nativeHandle != 0L) {
      nSetVolume(nativeHandle, volume);
    }
  }

  @Override
  public void setPlaybackParameters(PlaybackParameters playbackParameters) {
    this.playbackParameters = playbackParameters;
    super.setPlaybackParameters(playbackParameters);
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
    nativePlayIssued = false;
    clearPendingPassthroughStartupWindow();
    clearPendingEac3BurstWindow();
    if (!shouldUseNativeKodiPath(inputFormat)) {
      closeSession(true);
      clearPendingWriteError();
      pendingReleaseUntilMs = C.TIME_UNSET;
      super.configure(inputFormat, specifiedBufferSize, outputChannels);
      return;
    }
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
          new IllegalStateException("Failed to configure Kodi native sink session"), inputFormat);
    }
    clearPendingWriteError();
    pendingReleaseUntilMs = C.TIME_UNSET;
  }

  @Override
  public boolean handleBuffer(ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount)
      throws InitializationException, WriteException {
    if (!buffer.hasRemaining()) {
      return true;
    }
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      return super.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount);
    }
    if (nativeHandle == 0L || configuredFormat == null) {
      throw new WriteException(/* errorCode= */ -1, configuredFormat, /* isRecoverable= */ false);
    }

    if (shouldUseEac3IecBurstWindow(configuredFormat)) {
      if (playCommandReceived && hasPendingEac3BurstWindow()) {
        maybeIssueNativePlayForPassthroughStartup();
        int flushedBytes = writePendingEac3BurstWindow();
        if (flushedBytes < 0) {
          return false;
        }
        if (hasPendingEac3BurstWindow()) {
          return false;
        }
      }

      appendToPendingEac3BurstWindow(buffer, presentationTimeUs, encodedAccessUnitCount);
      handledEndOfStream = false;
      if (!playCommandReceived
          || pendingEac3BurstWindowAccessUnits < E_AC3_IEC_BURST_WINDOW_ACCESS_UNITS) {
        return true;
      }

      int flushedBytes = writePendingEac3BurstWindow();
      if (flushedBytes < 0) {
        return false;
      }
      return !hasPendingEac3BurstWindow();
    }

    if (hasPendingPassthroughStartupWindow()) {
      if (playCommandReceived && nativePlayIssued) {
        int flushedBytes = writePendingPassthroughStartupWindow();
        if (flushedBytes < 0) {
          return false;
        }
        if (hasPendingPassthroughStartupWindow()) {
          return false;
        }
      } else if (isPassthroughStartupWindowFull()) {
        return false;
      }
    }

    if (!playCommandReceived || !nativePlayIssued) {
      maybeProbePassthroughStartupBuffer(buffer, presentationTimeUs, encodedAccessUnitCount);
      appendToPendingPassthroughStartupWindow(buffer, presentationTimeUs, encodedAccessUnitCount);
      handledEndOfStream = false;
      if (playCommandReceived) {
        maybeIssueNativePlayForPassthroughStartup();
        if (nativePlayIssued) {
          int flushedBytes = writePendingPassthroughStartupWindow();
          if (flushedBytes < 0) {
            return false;
          }
          return !hasPendingPassthroughStartupWindow();
        }
      }
      return true;
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
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()
        && bytesConsumed > 0
        && !playCommandReceived) {
      Log.w(
          TAG,
          "Accepted audio bytes before Media3 play() command; consumed="
              + bytesConsumed
              + " ptsUs="
              + presentationTimeUs);
    }
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
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      super.play();
      return;
    }
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(
          TAG,
          "Media3 -> AudioSink play()"
              + " nativeHandle="
              + nativeHandle
              + " format="
              + (configuredFormat != null ? configuredFormat.sampleMimeType : "null"));
    }
    if (shouldUseEac3IecBurstWindow(configuredFormat)) {
      if (nativeHandle != 0L) {
        nPlay(nativeHandle);
        nativePlayIssued = true;
      }
    } else {
      maybeIssueNativePlayForPassthroughStartup();
    }
    if (nativePlayIssued && hasPendingPassthroughStartupWindow()) {
      flushPendingPassthroughStartupWindowForPlay();
    }
    if (nativePlayIssued
        && shouldUseEac3IecBurstWindow(configuredFormat)
        && hasPendingEac3BurstWindow()) {
      flushPendingEac3BurstWindowForPlay();
    }
  }

  @Override
  public void pause() {
    playCommandReceived = false;
    nativePlayIssued = false;
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      super.pause();
      return;
    }
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(TAG, "Media3 -> AudioSink pause()");
    }
    if (nativeHandle != 0L) {
      nPause(nativeHandle);
    }
  }

  @Override
  public void handleDiscontinuity() {
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      super.handleDiscontinuity();
      return;
    }
    if (nativeHandle != 0L) {
      nHandleDiscontinuity(nativeHandle);
    }
  }

  @Override
  public long getCurrentPositionUs(boolean sourceEnded) {
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      return super.getCurrentPositionUs(sourceEnded);
    }
    return nativeHandle == 0L ? CURRENT_POSITION_NOT_SET : nGetCurrentPositionUs(nativeHandle);
  }

  @Override
  public void playToEndOfStream() throws WriteException {
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      super.playToEndOfStream();
      return;
    }
    if (nativeHandle == 0L || configuredFormat == null) {
      return;
    }
    if (!nativePlayIssued) {
      maybeIssueNativePlayForPassthroughStartup();
    }
    if (hasPendingPassthroughStartupWindow()) {
      writePendingPassthroughStartupWindow();
    }
    if (hasPendingEac3BurstWindow()) {
      writePendingEac3BurstWindow();
    }
    nDrain(nativeHandle);
    handledEndOfStream = true;
  }

  @Override
  public boolean isEnded() {
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      return super.isEnded();
    }
    return nativeHandle == 0L
        || (!hasPendingPassthroughStartupWindow()
            && !hasPendingEac3BurstWindow()
            && handledEndOfStream
            && nIsEnded(nativeHandle));
  }

  @Override
  public boolean hasPendingData() {
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      return super.hasPendingData();
    }
    boolean nativeHasPendingData = nativeHandle != 0L && nHasPendingData(nativeHandle);
    if (nativeHasPendingData) {
      return true;
    }
    if (!nativePlayIssued) {
      if (shouldUseEac3IecBurstWindow(configuredFormat)) {
        return hasPendingEac3BurstWindow()
            && pendingEac3BurstWindowAccessUnits >= E_AC3_IEC_BURST_WINDOW_ACCESS_UNITS;
      }
      // Renderer readiness is keyed off AudioSink.hasPendingData(). Before native play is issued,
      // Java-owned startup bytes should only count as readiness once the passthrough startup
      // reservoir has reached the real target size. Reporting less than that reintroduces
      // early video start; reporting nothing deadlocks startup in buffering forever.
      return isPassthroughStartupWindowFull();
    }
    return hasPendingPassthroughStartupWindow() || hasPendingEac3BurstWindow();
  }

  @Override
  public long getAudioTrackBufferSizeUs() {
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      return super.getAudioTrackBufferSizeUs();
    }
    return nativeHandle == 0L ? 0L : nGetBufferSizeUs(nativeHandle);
  }

  @Override
  public void flush() {
    playCommandReceived = false;
    nativePlayIssued = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    clearPendingPassthroughStartupWindow();
    clearPendingEac3BurstWindow();
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      super.flush();
      return;
    }
    pendingReleaseUntilMs = SystemClock.elapsedRealtime() + RETRY_DURATION_MS;
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(TAG, "Media3 -> AudioSink flush()");
    }
    if (nativeHandle != 0L) {
      nFlush(nativeHandle);
    }
  }

  @Override
  public void reset() {
    playCommandReceived = false;
    nativePlayIssued = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    clearPendingPassthroughStartupWindow();
    clearPendingEac3BurstWindow();
    closeSession(true);
    configuredFormat = null;
    super.reset();
  }

  @Override
  public void release() {
    playCommandReceived = false;
    nativePlayIssued = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    clearPendingPassthroughStartupWindow();
    clearPendingEac3BurstWindow();
    closeSession(false);
    configuredFormat = null;
    super.release();
  }

  @Override
  public void setRendererClockUs(long rendererClockUs) {
    this.rendererClockUs = rendererClockUs;
    if (!shouldUseNativeKodiPath(configuredFormat)) {
      if (rendererClockAwareDelegate != null) {
        rendererClockAwareDelegate.setRendererClockUs(rendererClockUs);
      }
      return;
    }
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

  private static boolean isKodiPassthroughEnabledForFormat(Format format) {
    @Nullable String sampleMimeType = format.sampleMimeType;
    if (!isKodiPassthroughMime(sampleMimeType)) {
      return false;
    }
    if (MimeTypes.AUDIO_AC3.equals(sampleMimeType)) {
      return AudioCapabilities.isIecPackerAc3PassthroughEnabled();
    }
    if (MimeTypes.AUDIO_E_AC3.equals(sampleMimeType)
        || MimeTypes.AUDIO_E_AC3_JOC.equals(sampleMimeType)) {
      return AudioCapabilities.isIecPackerEac3PassthroughEnabled();
    }
    if (MimeTypes.AUDIO_DTS.equals(sampleMimeType)) {
      return AudioCapabilities.isIecPackerDtsPassthroughEnabled();
    }
    if (MimeTypes.AUDIO_DTS_HD.equals(sampleMimeType)
        || "audio/vnd.dts.uhd;profile=p2".equals(sampleMimeType)) {
      return AudioCapabilities.isIecPackerDtshdPassthroughEnabled()
          || (AudioCapabilities.isIecPackerDtshdCoreFallbackEnabled()
              && AudioCapabilities.isIecPackerDtsPassthroughEnabled());
    }
    if (MimeTypes.AUDIO_TRUEHD.equals(sampleMimeType)) {
      return AudioCapabilities.isIecPackerTruehdPassthroughEnabled();
    }
    return false;
  }

  private static boolean shouldUseEac3IecBurstWindow(@Nullable Format format) {
    if (!shouldUseNativeKodiPath(format)) {
      return false;
    }
    return MimeTypes.AUDIO_E_AC3.equals(format.sampleMimeType)
        || MimeTypes.AUDIO_E_AC3_JOC.equals(format.sampleMimeType);
  }

  private static boolean shouldUseNativeKodiPath(@Nullable Format format) {
    return format != null
        && AudioCapabilities.isExperimentalFireOsIecPassthroughEnabled()
        && isKodiPassthroughEnabledForFormat(format);
  }

  private boolean hasPendingEac3BurstWindow() {
    return pendingEac3BurstWindowData != null && pendingEac3BurstWindowSize > 0;
  }

  private boolean hasPendingPassthroughStartupWindow() {
    return pendingPassthroughStartupData != null && pendingPassthroughStartupSize > 0;
  }

  private void clearPendingPassthroughStartupWindow() {
    pendingPassthroughStartupData = null;
    pendingPassthroughStartupSize = 0;
    pendingPassthroughStartupAcknowledgedBytes = 0;
    pendingPassthroughStartupAccessUnits = 0;
    pendingPassthroughStartupFirstPtsUs = C.TIME_UNSET;
    pendingPassthroughStartupLastPtsUs = C.TIME_UNSET;
    pendingPassthroughStartupLastDurationUs = 0L;
  }

  private void appendToPendingPassthroughStartupWindow(
      ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount) {
    int bytesToAppend = buffer.remaining();
    if (bytesToAppend <= 0) {
      return;
    }
    int existingSize = pendingPassthroughStartupSize;
    int requiredSize = existingSize + bytesToAppend;
    if (pendingPassthroughStartupData == null
        || pendingPassthroughStartupData.length < requiredSize) {
      int newCapacity =
          Math.max(
              requiredSize,
              pendingPassthroughStartupData == null
                  ? requiredSize
                  : pendingPassthroughStartupData.length * 2);
      byte[] newBuffer = new byte[newCapacity];
      if (pendingPassthroughStartupData != null && existingSize > 0) {
        System.arraycopy(pendingPassthroughStartupData, 0, newBuffer, 0, existingSize);
      }
      pendingPassthroughStartupData = newBuffer;
    }

    ByteBuffer duplicate = buffer.duplicate();
    duplicate.get(pendingPassthroughStartupData, existingSize, bytesToAppend);
    buffer.position(buffer.limit());
    pendingPassthroughStartupSize = requiredSize;
    pendingPassthroughStartupAccessUnits += Math.max(1, encodedAccessUnitCount);
    if (pendingPassthroughStartupFirstPtsUs == C.TIME_UNSET) {
      pendingPassthroughStartupFirstPtsUs = presentationTimeUs;
    }
    if (pendingPassthroughStartupLastPtsUs != C.TIME_UNSET
        && presentationTimeUs > pendingPassthroughStartupLastPtsUs) {
      pendingPassthroughStartupLastDurationUs =
          presentationTimeUs - pendingPassthroughStartupLastPtsUs;
    }
    pendingPassthroughStartupLastPtsUs = presentationTimeUs;
  }

  private void maybeProbePassthroughStartupBuffer(
      ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount) {
    if (nativeHandle == 0L
        || configuredFormat == null
        || shouldUseEac3IecBurstWindow(configuredFormat)
        || getAudioTrackBufferSizeUs() > 0L) {
      return;
    }

    ByteBuffer probeBuffer = buffer.isDirect() ? buffer.duplicate() : copyToDirectBuffer(buffer);
    nProbePassthroughStartupBuffer(
        nativeHandle,
        probeBuffer,
        probeBuffer.position(),
        probeBuffer.remaining(),
        presentationTimeUs,
        encodedAccessUnitCount);
  }

  private long getPendingPassthroughStartupWindowDurationUs() {
    if (!hasPendingPassthroughStartupWindow() || pendingPassthroughStartupFirstPtsUs == C.TIME_UNSET) {
      return 0L;
    }
    if (pendingPassthroughStartupLastPtsUs == C.TIME_UNSET
        || pendingPassthroughStartupLastPtsUs <= pendingPassthroughStartupFirstPtsUs) {
      return pendingPassthroughStartupLastDurationUs;
    }
    return (pendingPassthroughStartupLastPtsUs - pendingPassthroughStartupFirstPtsUs)
        + Math.max(0L, pendingPassthroughStartupLastDurationUs);
  }

  private long getPassthroughStartupWindowTargetUs() {
    long bufferSizeUs = getAudioTrackBufferSizeUs();
    return Math.max(MIN_PASSTHROUGH_STARTUP_WINDOW_US, bufferSizeUs);
  }

  private boolean isPassthroughStartupWindowFull() {
    return hasPendingPassthroughStartupWindow()
        && getPendingPassthroughStartupWindowDurationUs() >= getPassthroughStartupWindowTargetUs();
  }

  private void maybeIssueNativePlayForPassthroughStartup() {
    if (nativePlayIssued || nativeHandle == 0L || configuredFormat == null || !playCommandReceived) {
      return;
    }
    if (shouldUseEac3IecBurstWindow(configuredFormat)) {
      nPlay(nativeHandle);
      nativePlayIssued = true;
      return;
    }
    if (!hasPendingPassthroughStartupWindow() || !isPassthroughStartupWindowFull()) {
      return;
    }
    nPlay(nativeHandle);
    nativePlayIssued = true;
  }

  private void clearPendingEac3BurstWindow() {
    pendingEac3BurstWindowData = null;
    pendingEac3BurstWindowSize = 0;
    pendingEac3BurstWindowAcknowledgedBytes = 0;
    pendingEac3BurstWindowAccessUnits = 0;
    pendingEac3BurstWindowPtsUs = C.TIME_UNSET;
  }

  private void appendToPendingEac3BurstWindow(
      ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount) {
    int bytesToAppend = buffer.remaining();
    if (bytesToAppend <= 0) {
      return;
    }
    int existingSize = pendingEac3BurstWindowSize;
    int requiredSize = existingSize + bytesToAppend;
    if (pendingEac3BurstWindowData == null || pendingEac3BurstWindowData.length < requiredSize) {
      int newCapacity =
          Math.max(requiredSize, pendingEac3BurstWindowData == null ? requiredSize : pendingEac3BurstWindowData.length * 2);
      byte[] newBuffer = new byte[newCapacity];
      if (pendingEac3BurstWindowData != null && existingSize > 0) {
        System.arraycopy(pendingEac3BurstWindowData, 0, newBuffer, 0, existingSize);
      }
      pendingEac3BurstWindowData = newBuffer;
    }

    ByteBuffer duplicate = buffer.duplicate();
    duplicate.get(pendingEac3BurstWindowData, existingSize, bytesToAppend);
    buffer.position(buffer.limit());
    pendingEac3BurstWindowSize = requiredSize;
    pendingEac3BurstWindowAccessUnits += Math.max(1, encodedAccessUnitCount);
    if (pendingEac3BurstWindowPtsUs == C.TIME_UNSET) {
      pendingEac3BurstWindowPtsUs = presentationTimeUs;
    }
  }

  private int writePendingEac3BurstWindow() throws WriteException {
    if (!hasPendingEac3BurstWindow() || nativeHandle == 0L || configuredFormat == null) {
      return 0;
    }
    if (pendingWriteErrorCode != 0 && shouldWaitBeforeWriteRetry()) {
      return -1;
    }

    ByteBuffer writeBuffer =
        ByteBuffer.allocateDirect(
            pendingEac3BurstWindowSize - pendingEac3BurstWindowAcknowledgedBytes);
    writeBuffer.put(
        pendingEac3BurstWindowData,
        pendingEac3BurstWindowAcknowledgedBytes,
        pendingEac3BurstWindowSize - pendingEac3BurstWindowAcknowledgedBytes);
    writeBuffer.flip();

    int bytesConsumed =
        nWrite(
            nativeHandle,
            writeBuffer,
            writeBuffer.position(),
            writeBuffer.remaining(),
            pendingEac3BurstWindowPtsUs != C.TIME_UNSET ? pendingEac3BurstWindowPtsUs : 0L,
            pendingEac3BurstWindowAccessUnits);
    nConsumeLastWriteOutputBytes(nativeHandle);
    int nativeWriteErrorCode = nConsumeLastWriteErrorCode(nativeHandle);
    if (nativeWriteErrorCode < 0 && bytesConsumed <= 0) {
      maybeHandlePendingWriteError(nativeWriteErrorCode);
      return -1;
    }
    if (bytesConsumed <= 0) {
      return -1;
    }

    clearPendingWriteError();
    pendingEac3BurstWindowAcknowledgedBytes += bytesConsumed;
    if (pendingEac3BurstWindowAcknowledgedBytes >= pendingEac3BurstWindowSize) {
      clearPendingEac3BurstWindow();
    }
    return bytesConsumed;
  }

  private int writePendingPassthroughStartupWindow() throws WriteException {
    if (!hasPendingPassthroughStartupWindow() || nativeHandle == 0L || configuredFormat == null) {
      return 0;
    }
    if (pendingWriteErrorCode != 0 && shouldWaitBeforeWriteRetry()) {
      return -1;
    }

    ByteBuffer writeBuffer =
        ByteBuffer.allocateDirect(
            pendingPassthroughStartupSize - pendingPassthroughStartupAcknowledgedBytes);
    writeBuffer.put(
        pendingPassthroughStartupData,
        pendingPassthroughStartupAcknowledgedBytes,
        pendingPassthroughStartupSize - pendingPassthroughStartupAcknowledgedBytes);
    writeBuffer.flip();

    int bytesConsumed =
        nWrite(
            nativeHandle,
            writeBuffer,
            writeBuffer.position(),
            writeBuffer.remaining(),
            pendingPassthroughStartupFirstPtsUs != C.TIME_UNSET
                ? pendingPassthroughStartupFirstPtsUs
                : 0L,
            pendingPassthroughStartupAccessUnits);
    int outputBytesWritten = nConsumeLastWriteOutputBytes(nativeHandle);
    int nativeWriteErrorCode = nConsumeLastWriteErrorCode(nativeHandle);
    if (nativeWriteErrorCode < 0 && bytesConsumed <= 0) {
      maybeHandlePendingWriteError(nativeWriteErrorCode);
      return -1;
    }
    if (bytesConsumed <= 0) {
      return -1;
    }

    clearPendingWriteError();
    pendingPassthroughStartupAcknowledgedBytes += bytesConsumed;
    if (pendingPassthroughStartupAcknowledgedBytes >= pendingPassthroughStartupSize) {
      clearPendingPassthroughStartupWindow();
    }
    return outputBytesWritten;
  }

  private void flushPendingPassthroughStartupWindowForPlay() {
    try {
      int writes = 0;
      while (hasPendingPassthroughStartupWindow() && writes < MAX_PASSTHROUGH_STARTUP_FLUSH_WRITES) {
        int outputBytesWritten = writePendingPassthroughStartupWindow();
        if (outputBytesWritten <= 0) {
          break;
        }
        writes++;
      }
    } catch (WriteException e) {
      Log.w(TAG, "Failed to flush pending passthrough startup window on play", e);
    }
  }

  private void flushPendingEac3BurstWindowForPlay() {
    try {
      writePendingEac3BurstWindow();
    } catch (WriteException e) {
      Log.w(TAG, "Failed to flush pending E-AC3 IEC burst window on play", e);
    }
  }

  private boolean configureWithRetry(NativeConfig config) {
    long startMs = SystemClock.elapsedRealtime();
    long deadlineMs = startMs + RETRY_DURATION_MS;
    boolean configured = false;
    int attempts = 0;
    while (!configured) {
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
      configured = nConfigure(nativeHandle, config);
      if (configured) {
        pendingReleaseUntilMs = C.TIME_UNSET;
        return true;
      }
      long nowMs = SystemClock.elapsedRealtime();
      if (nowMs >= deadlineMs) {
        if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
          Log.w(TAG, "nConfigure retry deadline reached attempts=" + attempts);
        }
        return false;
      }
      if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
        Log.w(TAG, "nConfigure failed, retrying attempt=" + attempts);
      }
      try {
        Thread.sleep(RETRY_DELAY_MS);
      } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
        return false;
      }
    }
    return false;
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
      if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
        Log.w(TAG, "nWrite error=" + errorCode + " scheduling retry window");
      }
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

  private static native void nProbePassthroughStartupBuffer(
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
