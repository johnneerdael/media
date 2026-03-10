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

  private static final int DEFAULT_PAUSE_BURST_DURATION_MS = 200;

  private final Context context;
  private final boolean nativeLibraryReady;
  @Nullable private AudioAttributes audioAttributes;
  @Nullable private AudioDeviceInfo preferredDevice;
  @Nullable private KodiNativeSinkSession nativeSession;
  @Nullable private Format configuredFormat;
  @Nullable private AudioSink.Listener listener;
  @Nullable private KodiNativeCapabilitySnapshot lastCapabilitySnapshot;
  @Nullable private KodiNativePlaybackDecision lastPlaybackDecision;
  @Nullable private KodiNativePacket lastQueuedPacket;
  @Nullable private KodiNativePacketMetadata lastQueuedPacketMetadata;
  private boolean usingNativeTransport;
  private int lastPauseBurstDurationMs;
  private int lastSpecifiedBufferSize;
  @Nullable private int[] lastOutputChannels;
  private int audioSessionId;
  private float volume;

  public KodiNativeAudioSink(Context context, AudioSink sink) {
    super(sink);
    this.context = context.getApplicationContext();
    this.nativeLibraryReady = KodiNativeLibrary.passesSmokeTest();
    this.lastPauseBurstDurationMs = C.LENGTH_UNSET;
    this.lastSpecifiedBufferSize = 0;
    this.lastOutputChannels = null;
    this.audioSessionId = C.AUDIO_SESSION_ID_UNSET;
    this.volume = 1f;
  }

  /** Returns whether the native library is present and passed the JNI smoke test. */
  public boolean isNativeLibraryReady() {
    return nativeLibraryReady;
  }

  /** Returns the latest packet metadata observed from the native session, if any. */
  @Nullable
  public KodiNativePacketMetadata getLastQueuedPacketMetadata() {
    return lastQueuedPacketMetadata;
  }

  /** Returns the latest full packet observed from the native session, if any. */
  @Nullable
  public KodiNativePacket getLastQueuedPacket() {
    return lastQueuedPacket;
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
    maybeReconfigureActiveNativePath();
    super.setAudioAttributes(audioAttributes);
  }

  @Override
  public void setPreferredDevice(@Nullable AudioDeviceInfo audioDeviceInfo) {
    preferredDevice = audioDeviceInfo;
    maybeReconfigureActiveNativePath();
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
    try {
      AudioAttributes currentAudioAttributes =
          audioAttributes != null ? audioAttributes : AudioAttributes.DEFAULT;
      KodiNativeCapabilitySnapshot capabilitySnapshot =
          KodiNativeCapabilitySnapshot.fromSystem(context, currentAudioAttributes, preferredDevice);
      KodiNativePlaybackDecision playbackDecision =
          KodiNativeCapabilitySelector.evaluatePlaybackDecision(capabilitySnapshot, inputFormat);
      ensureSession()
          .configure(
              inputFormat,
              specifiedBufferSize,
              outputChannels,
              audioSessionId,
              volume,
              capabilitySnapshot,
              playbackDecision);
      usingNativeTransport = isSupportedNativeMode(playbackDecision);
      if (!usingNativeTransport) {
        throw new ConfigurationException(
            new KodiNativeException(
                "Kodi native sink does not support playback mode " + playbackDecision.mode),
            inputFormat);
      }
      lastCapabilitySnapshot = capabilitySnapshot;
      lastPlaybackDecision = playbackDecision;
      lastQueuedPacket = null;
      lastQueuedPacketMetadata = null;
      lastPauseBurstDurationMs = C.LENGTH_UNSET;
    } catch (KodiNativeException e) {
      throw new ConfigurationException(e, inputFormat);
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
        nativeSession.queueInput(pendingSnapshot, presentationTimeUs, encodedAccessUnitCount);
        drainNativePacketsToNativeTransport(/* countsTowardMediaPosition= */ true);
        lastPauseBurstDurationMs = C.LENGTH_UNSET;
        buffer.position(buffer.limit());
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
        nativeSession.play();
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
        nativeSession.pause();
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
        nativeSession.playToEndOfStream();
      } catch (KodiNativeException e) {
        throw new WriteException(/* errorCode= */ -1, configuredFormat, /* isRecoverable= */ false);
      }
    }
  }

  @Override
  public boolean isEnded() {
    if (usingNativeTransport && nativeSession != null) {
      try {
        return nativeSession.isEnded();
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
        return nativeSession.hasPendingData();
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
        return nativeSession.getBufferSizeUs();
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
      lastQueuedPacket = null;
      lastQueuedPacketMetadata = null;
      lastPauseBurstDurationMs = C.LENGTH_UNSET;
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
        lastQueuedPacket = null;
        lastQueuedPacketMetadata = null;
        usingNativeTransport = false;
        lastPauseBurstDurationMs = C.LENGTH_UNSET;
        lastSpecifiedBufferSize = 0;
        lastOutputChannels = null;
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
        lastQueuedPacket = null;
        lastQueuedPacketMetadata = null;
        usingNativeTransport = false;
        lastPauseBurstDurationMs = C.LENGTH_UNSET;
        lastSpecifiedBufferSize = 0;
        lastOutputChannels = null;
      }
    }
    super.release();
  }

  private void drainNativePacketsToNativeTransport(boolean countsTowardMediaPosition)
      throws KodiNativeException {
    lastQueuedPacket = null;
    lastQueuedPacketMetadata = null;
    KodiNativePacketMetadata metadata;
    while ((metadata = nativeSession.drainOnePacketToAudioTrack(countsTowardMediaPosition)) != null) {
      lastQueuedPacketMetadata = metadata;
    }
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
      nativeSession.queuePause(
          pauseBurstDurationMs,
          lastPlaybackDecision != null && lastPlaybackDecision.usesIecCarrier());
      drainNativePacketsToNativeTransport(/* countsTowardMediaPosition= */ false);
      lastPauseBurstDurationMs = pauseBurstDurationMs;
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
          && (!previousSnapshot.toString().equals(String.valueOf(lastCapabilitySnapshot))
              || !previousDecision.toString().equals(String.valueOf(lastPlaybackDecision)))) {
        listener.onAudioCapabilitiesChanged();
      }
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
}
