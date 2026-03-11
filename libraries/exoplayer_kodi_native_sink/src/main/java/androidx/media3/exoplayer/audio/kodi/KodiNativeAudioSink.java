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

import android.content.Context;
import android.media.AudioDeviceInfo;
import android.util.Log;
import androidx.annotation.Nullable;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.util.AmazonQuirks;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.ForwardingAudioSink;
import java.nio.ByteBuffer;

/**
 * Java shell for the Kodi-backed native audio path.
 *
 * <p>The native backend owns normalization, packetization, and the active AudioTrack transport for
 * the native-enabled path. The delegated sink exists only to satisfy the Media3 shell boundary.
 */
@UnstableApi
public final class KodiNativeAudioSink extends ForwardingAudioSink {
  private final class RuntimeListener implements KodiNativeActiveAERuntime.Listener {
    @Override
    public void onUserAudioSettingsChanged() {
      maybeReconfigureActiveNativePath();
    }

    @Override
    public void onControlSettingsChanged() {
      if (nativeSession == null) {
        return;
      }
      try {
        applyKodiControlSettings();
      } catch (KodiNativeException e) {
        // Keep the active transport usable if control updates fail.
      }
    }

    @Override
    public void onAppFocusedChanged(boolean appFocused) {
      updateNativeAppFocusedState();
    }

    @Override
    public void onCapabilitySnapshotChanged(KodiNativeCapabilitySnapshot capabilitySnapshot) {
      if (!reconfiguringActivePath) {
        maybeReconfigureActiveNativePath();
      }
    }

    @Override
    public void onEngineStatsChanged(KodiNativeEngineStats stats) {}
  }

  public static final class ControlSettings {
    public static final ControlSettings DEFAULT = new ControlSettings(1, true);

    public final int silenceTimeoutMinutes;
    public final boolean streamNoiseEnabled;

    public ControlSettings(int silenceTimeoutMinutes, boolean streamNoiseEnabled) {
      this.silenceTimeoutMinutes = Math.max(0, silenceTimeoutMinutes);
      this.streamNoiseEnabled = streamNoiseEnabled;
    }
  }

  private static final int DEFAULT_PAUSE_BURST_DURATION_MS = 200;
  private static final String TAG = "KodiNativeSink";

  private final Context context;
  private final KodiNativeActiveAERuntime runtime;
  private final boolean ownsRuntime;
  private final RuntimeListener runtimeListener;
  private final boolean nativeLibraryReady;
  @Nullable private AudioAttributes audioAttributes;
  @Nullable private AudioDeviceInfo preferredDevice;
  @Nullable private KodiNativeSinkSession nativeSession;
  @Nullable private Format configuredFormat;
  @Nullable private AudioSink.Listener listener;
  @Nullable private KodiNativeCapabilitySnapshot lastCapabilitySnapshot;
  @Nullable private KodiNativePlaybackDecision lastPlaybackDecision;
  private boolean usingNativeTransport;
  private int lastPauseBurstDurationMs;
  private int lastSpecifiedBufferSize;
  @Nullable private int[] lastOutputChannels;
  private int audioSessionId;
  private float volume;
  private long lastLoggedPositionUs;
  private boolean reconfiguringActivePath;

  public KodiNativeAudioSink(Context context, AudioSink sink) {
    this(context, sink, KodiNativeActiveAERuntime.createDefault(context), true);
  }

  public KodiNativeAudioSink(
      Context context, AudioSink sink, KodiNativeActiveAERuntime runtime) {
    this(context, sink, runtime, false);
  }

  public KodiNativeAudioSink(Context context, AudioSink sink, ControlSettings controlSettings) {
    this(
        context,
        sink,
        new KodiNativeActiveAERuntime(
            context, KodiNativeUserAudioSettings.fromGlobals(), controlSettings),
        true);
  }

  private KodiNativeAudioSink(
      Context context, AudioSink sink, KodiNativeActiveAERuntime runtime, boolean ownsRuntime) {
    super(sink);
    this.context = context.getApplicationContext();
    this.runtime = runtime;
    this.ownsRuntime = ownsRuntime;
    this.runtimeListener = new RuntimeListener();
    this.nativeLibraryReady = KodiNativeLibrary.passesSmokeTest();
    this.lastPauseBurstDurationMs = C.LENGTH_UNSET;
    this.lastSpecifiedBufferSize = 0;
    this.lastOutputChannels = null;
    this.audioSessionId = C.AUDIO_SESSION_ID_UNSET;
    this.volume = 1f;
    this.lastLoggedPositionUs = CURRENT_POSITION_NOT_SET;
    runtime.addListener(runtimeListener);
    runtime.setRoutingContext(AudioAttributes.DEFAULT, preferredDevice);
  }

  /** Returns whether the native library is present and passed the JNI smoke test. */
  public boolean isNativeLibraryReady() {
    return nativeLibraryReady;
  }

  /** Returns the latest capability snapshot used to configure the native session, if any. */
  @Nullable
  public KodiNativeCapabilitySnapshot getLastCapabilitySnapshot() {
    return lastCapabilitySnapshot;
  }

  /** Returns the latest native playback decision used to configure the native session, if any. */
  @Nullable
  public KodiNativePlaybackDecision getLastPlaybackDecision() {
    return lastPlaybackDecision;
  }

  @Override
  public void setAudioAttributes(AudioAttributes audioAttributes) {
    this.audioAttributes = audioAttributes;
    runtime.setRoutingContext(audioAttributes, preferredDevice);
    super.setAudioAttributes(audioAttributes);
  }

  @Override
  public void setPreferredDevice(@Nullable AudioDeviceInfo audioDeviceInfo) {
    preferredDevice = audioDeviceInfo;
    runtime.setRoutingContext(audioAttributes != null ? audioAttributes : AudioAttributes.DEFAULT, preferredDevice);
    super.setPreferredDevice(audioDeviceInfo);
  }

  @Override
  public void setListener(Listener listener) {
    this.listener = listener;
    super.setListener(listener);
  }

  @Override
  public void setAudioSessionId(int audioSessionId) {
    this.audioSessionId = audioSessionId;
    maybeReconfigureActiveNativePath();
    super.setAudioSessionId(audioSessionId);
  }

  @Override
  public void setVolume(float volume) {
    this.volume = volume;
    if (nativeSession != null) {
      try {
        nativeSession.setVolume(volume);
      } catch (KodiNativeException e) {
        // Keep the current transport alive if native volume update fails.
      }
    }
    publishRuntimeStats();
    super.setVolume(volume);
  }

  @Override
  public void setOutputStreamOffsetUs(long outputStreamOffsetUs) {
    super.setOutputStreamOffsetUs(outputStreamOffsetUs);
  }

  @Override
  public void configure(Format inputFormat, int specifiedBufferSize, @Nullable int[] outputChannels)
      throws ConfigurationException {
    configuredFormat = inputFormat;
    lastSpecifiedBufferSize = specifiedBufferSize;
    lastOutputChannels = outputChannels != null ? outputChannels.clone() : null;
    configureActivePath(inputFormat, specifiedBufferSize, lastOutputChannels);
  }

  private void configureActivePath(
      Format inputFormat, int specifiedBufferSize, @Nullable int[] outputChannels)
      throws ConfigurationException {
    if (!nativeLibraryReady) {
      throw new ConfigurationException(
          new KodiNativeException("Kodi native sink library is unavailable"), inputFormat);
    }
    reconfiguringActivePath = true;
    try {
      AudioAttributes currentAudioAttributes =
          audioAttributes != null ? audioAttributes : AudioAttributes.DEFAULT;
      runtime.setRoutingContext(currentAudioAttributes, preferredDevice);
      KodiNativeCapabilitySnapshot capabilitySnapshot = runtime.getCapabilitySnapshot();
      KodiNativePlaybackDecision playbackDecision =
          KodiNativeCapabilitySelector.evaluatePlaybackDecision(
              capabilitySnapshot, inputFormat, runtime.getUserAudioSettings());
      ensureSession()
          .configure(
              inputFormat,
              specifiedBufferSize,
              outputChannels,
              audioSessionId,
              volume,
              AmazonQuirks.isFireOsIecVerboseLoggingEnabled(),
              AmazonQuirks.isFireOsIecSuperviseAudioDelayEnabled(),
              capabilitySnapshot,
              playbackDecision);
      applyKodiControlSettings();
      usingNativeTransport = isSupportedNativeMode(playbackDecision);
      if (!usingNativeTransport) {
        throw new ConfigurationException(
            new KodiNativeException(
                "Kodi native sink does not support playback mode " + playbackDecision.mode),
            inputFormat);
      }
      lastCapabilitySnapshot = capabilitySnapshot;
      lastPlaybackDecision = playbackDecision;
      lastPauseBurstDurationMs = C.LENGTH_UNSET;
      publishRuntimeStats();
      logVerbose(
          "configure"
              + " mime="
              + inputFormat.sampleMimeType
              + " sampleRate="
              + inputFormat.sampleRate
              + " channels="
              + inputFormat.channelCount
              + " mode="
              + playbackDecision.mode
              + " streamType="
              + playbackDecision.streamType
              + " encoding="
              + playbackDecision.outputEncoding
              + " channelConfig="
              + playbackDecision.channelConfig
              + " sessionId="
              + audioSessionId
              + " buffer="
              + specifiedBufferSize);
    } catch (KodiNativeException e) {
      throw new ConfigurationException(e, inputFormat);
    } finally {
      reconfiguringActivePath = false;
    }
  }

  @Override
  public boolean handleBuffer(
      ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount)
      throws InitializationException, WriteException {
    if (!nativeLibraryReady || nativeSession == null || configuredFormat == null) {
      throw new WriteException(/* errorCode= */ -1, configuredFormat, /* isRecoverable= */ false);
    }
    ByteBuffer pendingSnapshot = buffer.duplicate();
    try {
      if (usingNativeTransport) {
        logVerbose(
            "handleBuffer queue"
                + " size="
                + pendingSnapshot.remaining()
                + " ptsUs="
                + presentationTimeUs
                + " accessUnits="
                + encodedAccessUnitCount
                + " mode="
                + (lastPlaybackDecision != null ? lastPlaybackDecision.mode : -1));
        if (!nativeSession.handleBufferToSink(
            pendingSnapshot,
            presentationTimeUs,
            encodedAccessUnitCount)) {
          logVerbose(
              "handleBuffer backpressure"
                  + " size="
                  + pendingSnapshot.remaining()
                  + " ptsUs="
                  + presentationTimeUs
                  + " accessUnits="
                  + encodedAccessUnitCount
                  + " mode="
                  + (lastPlaybackDecision != null ? lastPlaybackDecision.mode : -1));
          return false;
        }
        lastPauseBurstDurationMs = C.LENGTH_UNSET;
        buffer.position(buffer.limit());
        publishRuntimeStats();
        return true;
      }
    } catch (KodiNativeException e) {
      throw new WriteException(/* errorCode= */ -1, configuredFormat, /* isRecoverable= */ false);
    }
    throw new WriteException(/* errorCode= */ -1, configuredFormat, /* isRecoverable= */ false);
  }

  @Override
  public void play() {
    if (nativeSession != null) {
      try {
        updateNativeAppFocusedState();
        nativeSession.play();
        publishRuntimeStats();
      } catch (KodiNativeException e) {
        throw new IllegalStateException("Kodi native sink play failed", e);
      }
    }
  }

  @Override
  public void pause() {
    queuePauseBurstIfEnabled();
    if (nativeSession != null) {
      try {
        updateNativeAppFocusedState();
        nativeSession.pause();
        publishRuntimeStats();
      } catch (KodiNativeException e) {
        throw new IllegalStateException("Kodi native sink pause failed", e);
      }
    }
  }

  @Override
  public void handleDiscontinuity() {
    // Native transport keeps its own stream position.
  }

  @Override
  public long getCurrentPositionUs(boolean sourceEnded) {
    if (usingNativeTransport && nativeSession != null) {
      try {
        long positionUs = nativeSession.getCurrentPositionUs();
        if (positionUs != Long.MIN_VALUE / 2) {
          publishRuntimeStats();
          maybeLogPosition(positionUs, sourceEnded);
          return positionUs;
        }
      } catch (KodiNativeException e) {
        return CURRENT_POSITION_NOT_SET;
      }
    }
    return CURRENT_POSITION_NOT_SET;
  }

  @Override
  public void playToEndOfStream() throws WriteException {
    if (usingNativeTransport && nativeSession != null) {
      try {
        nativeSession.drain();
        publishRuntimeStats();
      } catch (KodiNativeException e) {
        throw new WriteException(/* errorCode= */ -1, configuredFormat, /* isRecoverable= */ false);
      }
    }
  }

  @Override
  public boolean isEnded() {
    if (usingNativeTransport && nativeSession != null) {
      try {
        boolean ended = nativeSession.isEnded();
        publishRuntimeStats();
        return ended;
      } catch (KodiNativeException e) {
        return false;
      }
    }
    return false;
  }

  @Override
  public boolean hasPendingData() {
    if (usingNativeTransport && nativeSession != null) {
      try {
        boolean pending = nativeSession.hasPendingData();
        publishRuntimeStats();
        return pending;
      } catch (KodiNativeException e) {
        return false;
      }
    }
    return false;
  }

  @Override
  public long getAudioTrackBufferSizeUs() {
    if (usingNativeTransport && nativeSession != null) {
      try {
        long bufferSizeUs = nativeSession.getBufferSizeUs();
        publishRuntimeStats();
        return bufferSizeUs;
      } catch (KodiNativeException e) {
        return 0L;
      }
    }
    return 0L;
  }

  @Override
  public void flush() {
    queuePauseBurstIfEnabled();
    if (nativeLibraryReady && nativeSession != null) {
      try {
        nativeSession.flush();
      } catch (KodiNativeException e) {
        return;
      }
      lastPauseBurstDurationMs = C.LENGTH_UNSET;
      publishRuntimeStats();
    }
  }

  @Override
  public void reset() {
    if (nativeSession != null) {
      try {
        nativeSession.reset();
        nativeSession.close();
      } catch (KodiNativeException e) {
        // The delegate sink remains the active output path until the native backend takes over.
      } finally {
        nativeSession = null;
        configuredFormat = null;
        lastCapabilitySnapshot = null;
        lastPlaybackDecision = null;
        usingNativeTransport = false;
        lastPauseBurstDurationMs = C.LENGTH_UNSET;
        lastSpecifiedBufferSize = 0;
        lastOutputChannels = null;
        publishRuntimeStats();
      }
    }
  }

  @Override
  public void release() {
    if (nativeSession != null) {
      try {
        nativeSession.stop();
        nativeSession.close();
      } catch (KodiNativeException e) {
        // The delegate sink remains the active output path until the native backend takes over.
      } finally {
        nativeSession = null;
        configuredFormat = null;
        lastCapabilitySnapshot = null;
        lastPlaybackDecision = null;
        usingNativeTransport = false;
        lastPauseBurstDurationMs = C.LENGTH_UNSET;
        lastSpecifiedBufferSize = 0;
        lastOutputChannels = null;
        publishRuntimeStats();
      }
    }
    runtime.removeListener(runtimeListener);
    if (ownsRuntime) {
      runtime.close();
    }
    super.release();
  }

  private void queuePauseBurstIfEnabled() {
    if (!shouldQueuePauseBurst() || nativeSession == null) {
      return;
    }
    int pauseBurstDurationMs = getPauseBurstDurationMs();
    if (pauseBurstDurationMs <= 0 || pauseBurstDurationMs == lastPauseBurstDurationMs) {
      return;
    }
    try {
      nativeSession.queuePauseToSink(
          pauseBurstDurationMs,
          lastPlaybackDecision != null && lastPlaybackDecision.usesIecCarrier());
      lastPauseBurstDurationMs = pauseBurstDurationMs;
      logVerbose(
          "queuePauseBurst durationMs="
              + pauseBurstDurationMs
              + " iec="
              + (lastPlaybackDecision != null && lastPlaybackDecision.usesIecCarrier()));
    } catch (KodiNativeException e) {
      // Keep the active transport usable even if synthetic keep-alive bursts fail.
    }
  }

  private void maybeReconfigureActiveNativePath() {
    if (!nativeLibraryReady || configuredFormat == null) {
      return;
    }
    try {
      KodiNativeCapabilitySnapshot previousSnapshot = lastCapabilitySnapshot;
      KodiNativePlaybackDecision previousDecision = lastPlaybackDecision;
      configureActivePath(configuredFormat, lastSpecifiedBufferSize, lastOutputChannels);
      if (listener != null
          && previousSnapshot != null
          && previousDecision != null
          && (!previousSnapshot.equals(lastCapabilitySnapshot)
              || !previousDecision.equals(lastPlaybackDecision))) {
        listener.onAudioCapabilitiesChanged();
      }
      publishRuntimeStats();
    } catch (ConfigurationException e) {
      // Keep the current sink alive if route-based reconfiguration fails.
    }
  }

  private boolean shouldQueuePauseBurst() {
    return usingNativeTransport
        && lastPlaybackDecision != null
        && lastPlaybackDecision.isPassthrough()
        && AmazonQuirks.isFireOsIecSuperviseAudioDelayEnabled();
  }

  private int getPauseBurstDurationMs() {
    long bufferDurationUs = 0;
    if (nativeSession != null) {
      try {
        bufferDurationUs = nativeSession.getBufferSizeUs();
      } catch (KodiNativeException e) {
        bufferDurationUs = 0;
      }
    }
    if (bufferDurationUs <= 0) {
      return DEFAULT_PAUSE_BURST_DURATION_MS;
    }
    long millis = (bufferDurationUs + 999L) / 1_000L;
    return (int) Math.max(50L, Math.min(500L, millis));
  }

  private static boolean isSupportedNativeMode(
      KodiNativePlaybackDecision playbackDecision) {
    return playbackDecision.mode == KodiNativePlaybackDecision.MODE_PCM
        || playbackDecision.mode == KodiNativePlaybackDecision.MODE_PASSTHROUGH_DIRECT
        || playbackDecision.mode == KodiNativePlaybackDecision.MODE_PASSTHROUGH_IEC_STEREO
        || playbackDecision.mode == KodiNativePlaybackDecision.MODE_PASSTHROUGH_IEC_MULTICHANNEL;
  }

  private KodiNativeSinkSession ensureSession() throws KodiNativeException {
    if (nativeSession == null) {
      nativeSession = KodiNativeSinkSession.create();
    }
    return nativeSession;
  }

  private void applyKodiControlSettings() throws KodiNativeException {
    if (nativeSession == null) {
      return;
    }
    ControlSettings controlSettings = runtime.getControlSettings();
    nativeSession.setSilenceTimeoutMinutes(controlSettings.silenceTimeoutMinutes);
    nativeSession.setStreamNoise(controlSettings.streamNoiseEnabled);
    nativeSession.setAppFocused(runtime.isAppFocused());
  }

  private void updateNativeAppFocusedState() {
    if (nativeSession == null) {
      return;
    }
    try {
      nativeSession.setAppFocused(runtime.isAppFocused());
    } catch (KodiNativeException e) {
      // Keep the active transport usable if app-focus propagation fails.
    }
    publishRuntimeStats();
  }

  private void publishRuntimeStats() {
    boolean pendingData = false;
    boolean ended = false;
    long positionUs = 0;
    long bufferSizeUs = 0;
    if (nativeSession != null) {
      try {
        pendingData = nativeSession.hasPendingData();
        ended = nativeSession.isEnded();
        positionUs = nativeSession.getCurrentPositionUs();
        bufferSizeUs = nativeSession.getBufferSizeUs();
      } catch (KodiNativeException e) {
        pendingData = false;
        ended = false;
        positionUs = 0;
        bufferSizeUs = 0;
      }
    }
    runtime.setEngineStats(
        new KodiNativeEngineStats(
            usingNativeTransport,
            runtime.isAppFocused(),
            nativeSession == null,
            pendingData,
            ended,
            positionUs,
            bufferSizeUs,
            volume,
            configuredFormat,
            lastCapabilitySnapshot != null ? lastCapabilitySnapshot : runtime.getCapabilitySnapshot(),
            lastPlaybackDecision));
  }

  private void maybeLogPosition(long positionUs, boolean sourceEnded) {
    if (!AmazonQuirks.isFireOsIecVerboseLoggingEnabled()) {
      return;
    }
    if (lastLoggedPositionUs != CURRENT_POSITION_NOT_SET
        && Math.abs(positionUs - lastLoggedPositionUs) < 50_000) {
      return;
    }
    lastLoggedPositionUs = positionUs;
    logVerbose(
        "position"
            + " currentUs="
            + positionUs
            + " sourceEnded="
            + sourceEnded
            + " pending="
            + hasPendingData()
            + " bufferUs="
            + getAudioTrackBufferSizeUs());
  }

  private static void logVerbose(String message) {
    if (AmazonQuirks.isFireOsIecVerboseLoggingEnabled()) {
      Log.i(TAG, message);
    }
  }
}
