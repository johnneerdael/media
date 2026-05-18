/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package androidx.media3.extractor.mp4;

import androidx.media3.common.Format;
import androidx.media3.common.util.UnstableApi;
import java.util.Arrays;

/** Immutable MP4 text track sample table exported after the moov atom is parsed. */
@UnstableApi
public final class Mp4TextTrackSampleTable {

  /** Ordinal among non-empty text tracks in moov order. */
  public final int textTrackOrdinal;

  /** The MP4 track identifier. */
  public final int trackId;

  /** Output format for the text track. */
  public final Format format;

  /** Number of samples in the text track. */
  public final int sampleCount;

  /** Sample offsets in bytes. */
  public final long[] offsets;

  /** Sample sizes in bytes. */
  public final int[] sizes;

  /** Sample timestamps in microseconds. */
  public final long[] timestampsUs;

  /** Sample flags. */
  public final int[] flags;

  /** Duration of the text track sample table in microseconds. */
  public final long durationUs;

  public Mp4TextTrackSampleTable(
      int textTrackOrdinal,
      int trackId,
      Format format,
      int sampleCount,
      long[] offsets,
      int[] sizes,
      long[] timestampsUs,
      int[] flags,
      long durationUs) {
    this.textTrackOrdinal = textTrackOrdinal;
    this.trackId = trackId;
    this.format = format;
    this.sampleCount = sampleCount;
    this.offsets = Arrays.copyOf(offsets, offsets.length);
    this.sizes = Arrays.copyOf(sizes, sizes.length);
    this.timestampsUs = Arrays.copyOf(timestampsUs, timestampsUs.length);
    this.flags = Arrays.copyOf(flags, flags.length);
    this.durationUs = durationUs;
  }
}
