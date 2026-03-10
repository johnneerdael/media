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

import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.util.UnstableApi;
import java.nio.ByteBuffer;

/** Thin Java owner for one native Kodi passthrough session. */
@UnstableApi
public final class KodiNativeSinkSession implements AutoCloseable {

  private long nativeHandle;

  private KodiNativeSinkSession(long nativeHandle) {
    this.nativeHandle = nativeHandle;
  }

  public static KodiNativeSinkSession create() throws KodiNativeException {
    if (!KodiNativeLibrary.isAvailable()) {
      throw new KodiNativeException("Kodi native sink library is unavailable");
    }
    long nativeHandle = nCreate();
    if (nativeHandle == 0) {
      throw new KodiNativeException("Failed to create Kodi native sink session");
    }
    return new KodiNativeSinkSession(nativeHandle);
  }

  public void configure(
      Format format,
      int specifiedBufferSize,
      @Nullable int[] outputChannels,
      int audioSessionId,
      float volume,
      boolean verboseLoggingEnabled,
      KodiNativeCapabilitySnapshot capabilitySnapshot,
      KodiNativePlaybackDecision playbackDecision)
      throws KodiNativeException {
    ensureOpen();
    nConfigure(
        nativeHandle,
        KodiNativeCapabilitySelector.getMimeKind(format.sampleMimeType),
        format.sampleRate,
        format.channelCount,
        format.pcmEncoding != Format.NO_VALUE ? format.pcmEncoding : C.ENCODING_INVALID,
        specifiedBufferSize,
        outputChannels != null ? outputChannels.length : 0,
        audioSessionId,
        volume,
        verboseLoggingEnabled,
        capabilitySnapshot.sdkInt,
        capabilitySnapshot.tv,
        capabilitySnapshot.automotive,
        capabilitySnapshot.routedDeviceId,
        capabilitySnapshot.routedDeviceType,
        capabilitySnapshot.maxChannelCount,
        capabilitySnapshot.ac3.supported,
        capabilitySnapshot.ac3.encoding,
        capabilitySnapshot.ac3.channelConfig,
        capabilitySnapshot.eAc3.supported,
        capabilitySnapshot.eAc3.encoding,
        capabilitySnapshot.eAc3.channelConfig,
        capabilitySnapshot.dts.supported,
        capabilitySnapshot.dts.encoding,
        capabilitySnapshot.dts.channelConfig,
        capabilitySnapshot.dtsHd.supported,
        capabilitySnapshot.dtsHd.encoding,
        capabilitySnapshot.dtsHd.channelConfig,
        capabilitySnapshot.trueHd.supported,
        capabilitySnapshot.trueHd.encoding,
        capabilitySnapshot.trueHd.channelConfig,
        playbackDecision.mode,
        playbackDecision.outputEncoding,
        playbackDecision.channelConfig,
        playbackDecision.streamType,
        playbackDecision.flags);
  }

  public void queueInput(ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount)
      throws KodiNativeException {
    ensureOpen();
    nQueueInput(
        nativeHandle,
        buffer,
        buffer.position(),
        buffer.remaining(),
        presentationTimeUs,
        encodedAccessUnitCount);
  }

  public void queuePause(int millis, boolean iecBursts) throws KodiNativeException {
    ensureOpen();
    nQueuePause(nativeHandle, millis, iecBursts);
  }

  @Nullable
  public KodiNativePacket dequeuePacket() throws KodiNativeException {
    ensureOpen();
    long[] values = nDequeuePacketMetadata(nativeHandle);
    if (values == null) {
      return null;
    }
    if (values.length != 5) {
      throw new KodiNativeException("Unexpected native packet metadata shape");
    }
    byte[] data = nDequeuePacketBytes(nativeHandle);
    if (data == null) {
      throw new KodiNativeException("Native session returned metadata without packet bytes");
    }
    return new KodiNativePacket(
        new KodiNativePacketMetadata(
            (int) values[0], (int) values[1], values[2], (int) values[3], values[4]),
        data);
  }

  public int getPendingPacketCount() throws KodiNativeException {
    ensureOpen();
    return nGetPendingPacketCount(nativeHandle);
  }

  public long getQueuedInputBytes() throws KodiNativeException {
    ensureOpen();
    return nGetQueuedInputBytes(nativeHandle);
  }

  public void play() throws KodiNativeException {
    ensureOpen();
    nPlay(nativeHandle);
  }

  public void pause() throws KodiNativeException {
    ensureOpen();
    nPause(nativeHandle);
  }

  public void flush() throws KodiNativeException {
    ensureOpen();
    nFlush(nativeHandle);
  }

  public void stop() throws KodiNativeException {
    ensureOpen();
    nStop(nativeHandle);
  }

  public void reset() throws KodiNativeException {
    ensureOpen();
    nReset(nativeHandle);
  }

  public void playToEndOfStream() throws KodiNativeException {
    ensureOpen();
    nPlayToEndOfStream(nativeHandle);
  }

  public void setVolume(float volume) throws KodiNativeException {
    ensureOpen();
    nSetVolume(nativeHandle, volume);
  }

  @Nullable
  public KodiNativePacketMetadata drainOnePacketToAudioTrack(boolean countsTowardMediaPosition)
      throws KodiNativeException {
    ensureOpen();
    long[] values = nDrainOnePacketToAudioTrack(nativeHandle, countsTowardMediaPosition);
    if (values == null) {
      return null;
    }
    if (values.length != 5) {
      throw new KodiNativeException("Unexpected drained packet metadata shape");
    }
    return new KodiNativePacketMetadata(
        (int) values[0], (int) values[1], values[2], (int) values[3], values[4]);
  }

  public long getCurrentPositionUs() throws KodiNativeException {
    ensureOpen();
    return nGetCurrentPositionUs(nativeHandle);
  }

  public boolean hasPendingData() throws KodiNativeException {
    ensureOpen();
    return nHasPendingData(nativeHandle);
  }

  public boolean isEnded() throws KodiNativeException {
    ensureOpen();
    return nIsEnded(nativeHandle);
  }

  public long getBufferSizeUs() throws KodiNativeException {
    ensureOpen();
    return nGetBufferSizeUs(nativeHandle);
  }

  @Override
  public void close() throws KodiNativeException {
    if (nativeHandle == 0) {
      return;
    }
    nRelease(nativeHandle);
    nativeHandle = 0;
  }

  private void ensureOpen() throws KodiNativeException {
    if (nativeHandle == 0) {
      throw new KodiNativeException("Kodi native sink session is closed");
    }
  }

  private static native long nCreate();

  private static native void nConfigure(
      long nativeHandle,
      int mimeKind,
      int sampleRate,
      int channelCount,
      int pcmEncoding,
      int specifiedBufferSize,
      int outputChannelCount,
      int audioSessionId,
      float volume,
      boolean verboseLoggingEnabled,
      int sdkInt,
      boolean tv,
      boolean automotive,
      int routedDeviceId,
      int routedDeviceType,
      int maxChannelCount,
      boolean ac3Supported,
      int ac3Encoding,
      int ac3ChannelConfig,
      boolean eAc3Supported,
      int eAc3Encoding,
      int eAc3ChannelConfig,
      boolean dtsSupported,
      int dtsEncoding,
      int dtsChannelConfig,
      boolean dtsHdSupported,
      int dtsHdEncoding,
      int dtsHdChannelConfig,
      boolean trueHdSupported,
      int trueHdEncoding,
      int trueHdChannelConfig,
      int playbackMode,
      int playbackEncoding,
      int playbackChannelConfig,
      int playbackStreamType,
      int playbackFlags);

  private static native void nQueueInput(
      long nativeHandle,
      ByteBuffer buffer,
      int offset,
      int size,
      long presentationTimeUs,
      int encodedAccessUnitCount);

  private static native void nQueuePause(long nativeHandle, int millis, boolean iecBursts);

  @Nullable
  private static native long[] nDequeuePacketMetadata(long nativeHandle);

  @Nullable
  private static native byte[] nDequeuePacketBytes(long nativeHandle);

  private static native int nGetPendingPacketCount(long nativeHandle);

  private static native long nGetQueuedInputBytes(long nativeHandle);

  private static native void nPlay(long nativeHandle);

  private static native void nPause(long nativeHandle);

  private static native void nFlush(long nativeHandle);

  private static native void nStop(long nativeHandle);

  private static native void nReset(long nativeHandle);

  private static native void nPlayToEndOfStream(long nativeHandle);

  private static native void nSetVolume(long nativeHandle, float volume);

  @Nullable
  private static native long[] nDrainOnePacketToAudioTrack(
      long nativeHandle, boolean countsTowardMediaPosition);

  private static native long nGetCurrentPositionUs(long nativeHandle);

  private static native boolean nHasPendingData(long nativeHandle);

  private static native boolean nIsEnded(long nativeHandle);

  private static native long nGetBufferSizeUs(long nativeHandle);

  private static native void nRelease(long nativeHandle);
}
