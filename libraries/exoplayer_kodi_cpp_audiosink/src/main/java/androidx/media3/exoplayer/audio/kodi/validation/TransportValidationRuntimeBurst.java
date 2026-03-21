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

public final class TransportValidationRuntimeBurst {
  private final TransportValidationRuntimeBoundary boundary;
  private final String sampleId;
  private final String codecFamily;
  private final int burstIndex;
  private final long sourcePtsUs;
  private final byte[] bytes;

  public TransportValidationRuntimeBurst(
      TransportValidationRuntimeBoundary boundary,
      String sampleId,
      String codecFamily,
      int burstIndex,
      long sourcePtsUs,
      byte[] bytes) {
    this.boundary = boundary;
    this.sampleId = sampleId;
    this.codecFamily = codecFamily;
    this.burstIndex = burstIndex;
    this.sourcePtsUs = sourcePtsUs;
    this.bytes = bytes;
  }

  public TransportValidationRuntimeBoundary getBoundary() {
    return boundary;
  }

  public String getSampleId() {
    return sampleId;
  }

  public String getCodecFamily() {
    return codecFamily;
  }

  public int getBurstIndex() {
    return burstIndex;
  }

  public long getSourcePtsUs() {
    return sourcePtsUs;
  }

  public byte[] getBytes() {
    return bytes;
  }
}
