/*
 * Copyright 2026 The Android Open Source Project
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
package androidx.media3.exoplayer.trackselection;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.fail;

import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.TrackGroup;
import androidx.media3.exoplayer.source.chunk.MediaChunk;
import androidx.media3.exoplayer.source.chunk.MediaChunkIterator;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.util.List;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Unit test for {@link BaseTrackSelection}. */
@RunWith(AndroidJUnit4.class)
public final class BaseTrackSelectionTest {

  @Test
  public void isTrackExcluded_withNegativeIndex_returnsFalse() {
    BaseTrackSelection trackSelection = createTrackSelection();

    try {
      assertThat(trackSelection.isTrackExcluded(/* index= */ -1, /* nowMs= */ 0)).isFalse();
    } catch (ArrayIndexOutOfBoundsException e) {
      fail("Negative track indexes should be treated as not excluded.");
    }
  }

  @Test
  public void isTrackExcluded_withIndexEqualToLength_returnsFalse() {
    BaseTrackSelection trackSelection = createTrackSelection();

    try {
      assertThat(trackSelection.isTrackExcluded(trackSelection.length(), /* nowMs= */ 0)).isFalse();
    } catch (ArrayIndexOutOfBoundsException e) {
      fail("Out-of-range track indexes should be treated as not excluded.");
    }
  }

  private static BaseTrackSelection createTrackSelection() {
    return new FakeTrackSelection(
        new TrackGroup(
            new Format.Builder().setAverageBitrate(1_000).build(),
            new Format.Builder().setAverageBitrate(500).build()),
        /* tracks= */ 0,
        1);
  }

  private static final class FakeTrackSelection extends BaseTrackSelection {

    public FakeTrackSelection(TrackGroup group, int... tracks) {
      super(group, tracks);
    }

    @Override
    public void updateSelectedTrack(
        long playbackPositionUs,
        long bufferedDurationUs,
        long availableDurationUs,
        List<? extends MediaChunk> queue,
        MediaChunkIterator[] mediaChunkIterators) {
      // Do nothing.
    }

    @Override
    public int getSelectedIndex() {
      return 0;
    }

    @Override
    public int getSelectionReason() {
      return C.SELECTION_REASON_UNKNOWN;
    }

    @Nullable
    @Override
    public Object getSelectionData() {
      return null;
    }
  }
}
