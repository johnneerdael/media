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
package androidx.media3.exoplayer.audio.kodi.validation;

import android.os.SystemClock;
import androidx.annotation.Nullable;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class TransportValidationRuntime {
  private static final Object LOCK = new Object();

  @Nullable private static SessionConfig sessionConfig;
  @Nullable private static TransportValidationRuntimeRouteSnapshot routeSnapshot;
  private static final List<TransportValidationRuntimeRouteSample> routeSnapshots = new ArrayList<>();
  private static final List<TransportValidationRuntimeBurst> packerInputBursts = new ArrayList<>();
  private static final List<TransportValidationRuntimeBurst> packedBursts = new ArrayList<>();
  private static final List<TransportValidationRuntimeBurst> audioTrackWriteBursts = new ArrayList<>();
  private static final List<TransportValidationRuntimeEvent> runtimeEvents = new ArrayList<>();
  private static int nextPackerInputIndex = 0;
  private static int nextPackedBurstIndex = 0;
  private static int nextAudioTrackWriteBurstIndex = 0;

  private TransportValidationRuntime() {}

  public static void beginSession(String sampleId, String codecFamily, int maxBurstsPerBoundary) {
    synchronized (LOCK) {
      sessionConfig =
          new SessionConfig(sampleId, codecFamily, Math.max(1, maxBurstsPerBoundary));
      routeSnapshot = null;
      routeSnapshots.clear();
      packerInputBursts.clear();
      packedBursts.clear();
      audioTrackWriteBursts.clear();
      runtimeEvents.clear();
      nextPackerInputIndex = 0;
      nextPackedBurstIndex = 0;
      nextAudioTrackWriteBurstIndex = 0;
    }
  }

  public static void clearSession() {
    synchronized (LOCK) {
      sessionConfig = null;
      routeSnapshot = null;
      routeSnapshots.clear();
      packerInputBursts.clear();
      packedBursts.clear();
      audioTrackWriteBursts.clear();
      runtimeEvents.clear();
      nextPackerInputIndex = 0;
      nextPackedBurstIndex = 0;
      nextAudioTrackWriteBurstIndex = 0;
    }
  }

  public static boolean isEnabled() {
    synchronized (LOCK) {
      return sessionConfig != null;
    }
  }

  public static void updateRouteSnapshot(
      @Nullable String deviceName,
      @Nullable String encoding,
      int sampleRate,
      @Nullable String channelMask,
      @Nullable Boolean directPlaybackSupported,
      @Nullable Integer audioTrackState) {
    synchronized (LOCK) {
      if (sessionConfig == null) {
        return;
      }
      routeSnapshot =
          new TransportValidationRuntimeRouteSnapshot(
              deviceName, encoding, sampleRate, channelMask, directPlaybackSupported, audioTrackState);
      if (routeSnapshots.isEmpty()
          || !areEquivalent(routeSnapshots.get(routeSnapshots.size() - 1).getSnapshot(), routeSnapshot)) {
        routeSnapshots.add(
            new TransportValidationRuntimeRouteSample(SystemClock.elapsedRealtime(), routeSnapshot));
      }
    }
  }

  public static void recordPackerInput(long sourcePtsUs, byte[] bytes) {
    recordBurst(TransportValidationRuntimeBoundary.PACKER_INPUT, sourcePtsUs, bytes);
  }

  public static void recordPackedBurst(long sourcePtsUs, byte[] bytes) {
    recordBurst(TransportValidationRuntimeBoundary.PACKED_BURST, sourcePtsUs, bytes);
  }

  public static void recordAudioTrackWrite(long sourcePtsUs, byte[] bytes) {
    recordBurst(TransportValidationRuntimeBoundary.AUDIOTRACK_WRITE, sourcePtsUs, bytes);
  }

  public static void recordRuntimeEvent(String type, long value, @Nullable String detail) {
    synchronized (LOCK) {
      if (sessionConfig == null) {
        return;
      }
      runtimeEvents.add(
          new TransportValidationRuntimeEvent(
              SystemClock.elapsedRealtime(), type, value, detail));
    }
  }

  @Nullable
  public static TransportValidationRuntimeSnapshot snapshot() {
    synchronized (LOCK) {
      if (sessionConfig == null) {
        return null;
      }
      return new TransportValidationRuntimeSnapshot(
          sessionConfig.sampleId,
          sessionConfig.codecFamily,
          immutableCopy(packerInputBursts),
          immutableCopy(packedBursts),
          immutableCopy(audioTrackWriteBursts),
          routeSnapshot,
          immutableCopy(routeSnapshots),
          immutableCopy(runtimeEvents));
    }
  }

  private static boolean areEquivalent(
      TransportValidationRuntimeRouteSnapshot first, TransportValidationRuntimeRouteSnapshot second) {
    return areEqual(first.getDeviceName(), second.getDeviceName())
        && areEqual(first.getEncoding(), second.getEncoding())
        && first.getSampleRate() == second.getSampleRate()
        && areEqual(first.getChannelMask(), second.getChannelMask())
        && areEqual(first.getDirectPlaybackSupported(), second.getDirectPlaybackSupported())
        && areEqual(first.getAudioTrackState(), second.getAudioTrackState());
  }

  private static boolean areEqual(@Nullable Object first, @Nullable Object second) {
    return first == second || (first != null && first.equals(second));
  }

  private static void recordBurst(
      TransportValidationRuntimeBoundary boundary, long sourcePtsUs, @Nullable byte[] bytes) {
    if (bytes == null || bytes.length == 0) {
      return;
    }
    synchronized (LOCK) {
      if (sessionConfig == null) {
        return;
      }
      final List<TransportValidationRuntimeBurst> target;
      final int burstIndex;
      switch (boundary) {
        case PACKER_INPUT:
          target = packerInputBursts;
          burstIndex = nextPackerInputIndex++;
          break;
        case PACKED_BURST:
          target = packedBursts;
          burstIndex = nextPackedBurstIndex++;
          break;
        case AUDIOTRACK_WRITE:
        default:
          target = audioTrackWriteBursts;
          burstIndex = nextAudioTrackWriteBurstIndex++;
          break;
      }
      if (target.size() >= sessionConfig.maxBurstsPerBoundary) {
        return;
      }
      byte[] copiedBytes = new byte[bytes.length];
      System.arraycopy(bytes, 0, copiedBytes, 0, bytes.length);
      target.add(
          new TransportValidationRuntimeBurst(
              boundary,
              sessionConfig.sampleId,
              sessionConfig.codecFamily,
              burstIndex,
              sourcePtsUs,
              copiedBytes));
    }
  }

  private static <T> List<T> immutableCopy(List<T> source) {
    return Collections.unmodifiableList(new ArrayList<>(source));
  }

  private static final class SessionConfig {
    public final String sampleId;
    public final String codecFamily;
    public final int maxBurstsPerBoundary;

    public SessionConfig(String sampleId, String codecFamily, int maxBurstsPerBoundary) {
      this.sampleId = sampleId;
      this.codecFamily = codecFamily;
      this.maxBurstsPerBoundary = maxBurstsPerBoundary;
    }
  }
}
