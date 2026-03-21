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
import android.media.AudioFormat;
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
import androidx.media3.exoplayer.audio.kodi.validation.TransportValidationNativeBurst;
import androidx.media3.exoplayer.audio.kodi.validation.TransportValidationRuntime;
import androidx.media3.exoplayer.audio.kodi.validation.TransportValidationRuntimeEventType;
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
  private boolean playCommandReceived;
  private boolean handledEndOfStream;
  private static final String TAG = "KodiNativeSink";
  private static final long RETRY_DURATION_MS = 200;
  private static final long RETRY_DELAY_MS = 50;

  private int pendingWriteErrorCode;
  private long pendingWriteErrorDeadlineMs;
  private long pendingWriteEarliestRetryMs;
  private long pendingReleaseUntilMs;
  private int lastReportedOutputUnderrunCount;
  private int lastReportedOutputRestartCount;

  public KodiNativeAudioSink(AudioSink sink) {
    super(sink);
    nativeHandle = 0L;
    volume = 1f;
    playbackParameters = PlaybackParameters.DEFAULT;
    rendererClockUs = C.TIME_UNSET;
    playCommandReceived = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    pendingReleaseUntilMs = C.TIME_UNSET;
    clearTransportValidationRuntimeOutputState();
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
          new IllegalStateException("Failed to configure Kodi native sink session"), inputFormat);
    }
    updateTransportValidationRoute();
    clearPendingWriteError();
    pendingReleaseUntilMs = C.TIME_UNSET;
    clearTransportValidationRuntimeOutputState();
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
    drainCapturedValidationBursts();
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
      recordTransportValidationWriteEvent(originalRemaining, bytesConsumed);
      return false;
    }
    if (bytesConsumed <= 0) {
      recordTransportValidationWriteEvent(originalRemaining, bytesConsumed);
      return false;
    }
    // Match DefaultAudioSink contract: once this call consumed bytes, report
    // forward progress and do not introduce extra retry gating for the same write.
    clearPendingWriteError();
    handledEndOfStream = false;
    maybeRecordPackerInput(writeBuffer, presentationTimeUs, bytesConsumed);
    recordTransportValidationWriteEvent(originalRemaining, bytesConsumed);
    buffer.position(buffer.position() + Math.min(bytesConsumed, originalRemaining));
    return bytesConsumed >= originalRemaining;
  }

  @Override
  public void play() {
    playCommandReceived = true;
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(
          TAG,
          "Media3 -> AudioSink play()"
              + " nativeHandle="
              + nativeHandle
              + " format="
              + (configuredFormat != null ? configuredFormat.sampleMimeType : "null"));
    }
    if (nativeHandle != 0L) {
      nPlay(nativeHandle);
    }
  }

  @Override
  public void pause() {
    if (AudioCapabilities.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(TAG, "Media3 -> AudioSink pause()");
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
    maybeRecordTransportValidationRuntimeEvents();
    long currentPositionUs = nativeHandle == 0L ? CURRENT_POSITION_NOT_SET : nGetCurrentPositionUs(nativeHandle);
    maybeRecordTransportValidationPlaybackHeadPosition(currentPositionUs);
    return currentPositionUs;
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
    maybeRecordTransportValidationRuntimeEvents();
    return nativeHandle == 0L || (handledEndOfStream && nIsEnded(nativeHandle));
  }

  @Override
  public boolean hasPendingData() {
    maybeRecordTransportValidationRuntimeEvents();
    return nativeHandle != 0L && nHasPendingData(nativeHandle);
  }

  @Override
  public long getAudioTrackBufferSizeUs() {
    maybeRecordTransportValidationRuntimeEvents();
    return nativeHandle == 0L ? 0L : nGetBufferSizeUs(nativeHandle);
  }

  @Override
  public void flush() {
    playCommandReceived = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    pendingReleaseUntilMs = SystemClock.elapsedRealtime() + RETRY_DURATION_MS;
    clearTransportValidationRuntimeOutputState();
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
    handledEndOfStream = false;
    clearPendingWriteError();
    closeSession(true);
    configuredFormat = null;
    clearTransportValidationRuntimeOutputState();
    super.reset();
  }

  @Override
  public void release() {
    playCommandReceived = false;
    handledEndOfStream = false;
    clearPendingWriteError();
    closeSession(false);
    configuredFormat = null;
    clearTransportValidationRuntimeOutputState();
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

  private void maybeRecordPackerInput(ByteBuffer buffer, long presentationTimeUs, int bytesConsumed) {
    if (!TransportValidationRuntime.isEnabled() || bytesConsumed <= 0) {
      return;
    }
    ByteBuffer duplicate = buffer.duplicate();
    int length = Math.min(bytesConsumed, duplicate.remaining());
    if (length <= 0) {
      return;
    }
    byte[] bytes = new byte[length];
    duplicate.get(bytes, 0, length);
    TransportValidationRuntime.recordPackerInput(presentationTimeUs, bytes);
  }

  private void drainCapturedValidationBursts() {
    if (!TransportValidationRuntime.isEnabled() || nativeHandle == 0L) {
      return;
    }
    for (TransportValidationNativeBurst burst = nConsumeNextCapturedPackedBurst(nativeHandle);
        burst != null;
        burst = nConsumeNextCapturedPackedBurst(nativeHandle)) {
      TransportValidationRuntime.recordPackedBurst(burst.sourcePtsUs, burst.bytes);
    }
    for (TransportValidationNativeBurst burst = nConsumeNextCapturedAudioTrackWriteBurst(nativeHandle);
        burst != null;
        burst = nConsumeNextCapturedAudioTrackWriteBurst(nativeHandle)) {
      TransportValidationRuntime.recordAudioTrackWrite(burst.sourcePtsUs, burst.bytes);
    }
  }

  private void updateTransportValidationRoute() {
    if (!TransportValidationRuntime.isEnabled()) {
      return;
    }
    int outputSampleRate = nativeHandle != 0L ? nGetOutputSampleRate(nativeHandle) : 0;
    int outputChannelCount = nativeHandle != 0L ? nGetOutputChannelCount(nativeHandle) : 0;
    int outputEncoding = nativeHandle != 0L ? nGetOutputEncoding(nativeHandle) : C.ENCODING_INVALID;
    int audioTrackState = nativeHandle != 0L ? nGetOutputAudioTrackState(nativeHandle) : 0;
    int directPlaybackSupportState =
        nativeHandle != 0L ? nGetDirectPlaybackSupportState(nativeHandle) : -1;
    TransportValidationRuntime.updateRouteSnapshot(
        resolvePreferredDeviceName(preferredDevice),
        encodingLabel(outputEncoding, configuredFormat != null ? configuredFormat.sampleMimeType : null),
        outputSampleRate,
        channelMaskLabel(outputChannelCount),
        directPlaybackSupportState > 0
            ? Boolean.TRUE
            : directPlaybackSupportState == 0 ? Boolean.FALSE : null,
        audioTrackState > 0 ? audioTrackState : null);
    maybeRecordTransportValidationRuntimeEvents();
  }

  private void maybeRecordTransportValidationRuntimeEvents() {
    if (nativeHandle == 0L) {
      return;
    }
    int outputUnderrunCount = nGetOutputUnderrunCount(nativeHandle);
    if (outputUnderrunCount >= 0) {
      if (lastReportedOutputUnderrunCount >= 0
          && outputUnderrunCount > lastReportedOutputUnderrunCount) {
        TransportValidationRuntime.recordRuntimeEvent(
            TransportValidationRuntimeEventType.AUDIO_UNDERRUN,
            outputUnderrunCount - lastReportedOutputUnderrunCount,
            "source=sink");
      }
      lastReportedOutputUnderrunCount = outputUnderrunCount;
    }
    int outputRestartCount = nGetOutputRestartCount(nativeHandle);
    if (outputRestartCount >= 0) {
      if (lastReportedOutputRestartCount >= 0
          && outputRestartCount > lastReportedOutputRestartCount) {
        TransportValidationRuntime.recordRuntimeEvent(
            TransportValidationRuntimeEventType.AUDIOTRACK_RESTART,
            outputRestartCount - lastReportedOutputRestartCount,
            "source=sink");
      }
      lastReportedOutputRestartCount = outputRestartCount;
    }
  }

  private void maybeRecordTransportValidationPlaybackHeadPosition(long currentPositionUs) {
    if (nativeHandle == 0L || currentPositionUs == CURRENT_POSITION_NOT_SET) {
      return;
    }
    TransportValidationRuntime.recordRuntimeEvent(
        TransportValidationRuntimeEventType.PLAYBACK_HEAD_POSITION, currentPositionUs, "unit=us");
  }

  private void recordTransportValidationWriteEvent(int requestedBytes, int bytesConsumed) {
    if (!TransportValidationRuntime.isEnabled() || requestedBytes <= 0) {
      return;
    }
    if (bytesConsumed <= 0) {
      TransportValidationRuntime.recordRuntimeEvent(
          TransportValidationRuntimeEventType.AUDIO_WRITE_ZERO,
          requestedBytes,
          "requestedBytes=" + requestedBytes);
      return;
    }
    if (bytesConsumed < requestedBytes) {
      TransportValidationRuntime.recordRuntimeEvent(
          TransportValidationRuntimeEventType.AUDIO_WRITE_PARTIAL,
          bytesConsumed,
          "requestedBytes=" + requestedBytes + " remainingBytes=" + (requestedBytes - bytesConsumed));
      return;
    }
    TransportValidationRuntime.recordRuntimeEvent(
        TransportValidationRuntimeEventType.AUDIO_WRITE_SUCCESS,
        bytesConsumed,
        "requestedBytes=" + requestedBytes);
  }

  private void clearTransportValidationRuntimeOutputState() {
    lastReportedOutputUnderrunCount = -1;
    lastReportedOutputRestartCount = -1;
  }

  private static String channelMaskLabel(int channelCount) {
    if (channelCount >= 8) {
      return "7.1";
    }
    if (channelCount >= 6) {
      return "5.1";
    }
    if (channelCount >= 2) {
      return "2.0";
    }
    return Integer.toString(channelCount);
  }

  private static String encodingLabel(int encoding, @Nullable String fallbackMimeType) {
    if (encoding == C.ENCODING_INVALID || encoding <= 0) {
      return fallbackMimeType != null ? fallbackMimeType : "unknown";
    }
    if (encoding == AudioFormat.ENCODING_IEC61937) {
      return "IEC61937";
    }
    return Integer.toString(encoding);
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

  private static native int nConsumeLastWriteOutputBytes(long nativeHandle);

  private static native @Nullable TransportValidationNativeBurst nConsumeNextCapturedPackedBurst(
      long nativeHandle);

  private static native @Nullable TransportValidationNativeBurst nConsumeNextCapturedAudioTrackWriteBurst(
      long nativeHandle);

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

  private static native int nGetOutputSampleRate(long nativeHandle);

  private static native int nGetOutputChannelCount(long nativeHandle);

  private static native int nGetOutputEncoding(long nativeHandle);

  private static native int nGetOutputAudioTrackState(long nativeHandle);

  private static native int nGetOutputUnderrunCount(long nativeHandle);

  private static native int nGetOutputRestartCount(long nativeHandle);

  private static native int nGetDirectPlaybackSupportState(long nativeHandle);

  private static native void nRelease(long nativeHandle);
}
