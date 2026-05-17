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
package androidx.media3.exoplayer.text;

import static androidx.media3.test.utils.FakeSampleStream.FakeSampleStreamItem.END_OF_STREAM_ITEM;
import static androidx.media3.test.utils.FakeSampleStream.FakeSampleStreamItem.sample;
import static com.google.common.truth.Truth.assertThat;

import androidx.annotation.Nullable;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.text.Cue;
import androidx.media3.common.text.CueGroup;
import androidx.media3.exoplayer.drm.DrmSessionEventListener;
import androidx.media3.exoplayer.drm.DrmSessionManager;
import androidx.media3.exoplayer.source.MediaSource;
import androidx.media3.exoplayer.upstream.DefaultAllocator;
import androidx.media3.extractor.text.CueEncoder;
import androidx.media3.test.utils.FakeSampleStream;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import com.google.common.collect.ImmutableList;
import java.util.ArrayList;
import java.util.List;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Unit tests for {@link TextRenderer}. */
@RunWith(AndroidJUnit4.class)
public final class TextRendererTest {

  private static final Format MEDIA3_CUES_FORMAT =
      new Format.Builder()
          .setSampleMimeType(MimeTypes.APPLICATION_MEDIA3_CUES)
          .setCueReplacementBehavior(Format.CUE_REPLACEMENT_BEHAVIOR_MERGE)
          .build();

  private final CueEncoder cueEncoder = new CueEncoder();

  @Test
  public void renderFromCuesWithTiming_prefetchesAllAvailableCueSamples() throws Exception {
    CapturingTranslator translator = new CapturingTranslator();
    TextRenderer renderer =
        new TextRenderer(
            /* output= */ cueGroup -> {},
            /* outputLooper= */ null,
            SubtitleDecoderFactory.DEFAULT,
            translator);
    FakeSampleStream fakeSampleStream =
        new FakeSampleStream(
            new DefaultAllocator(/* trimOnReset= */ true, C.DEFAULT_BUFFER_SEGMENT_SIZE),
            /* mediaSourceEventDispatcher= */ null,
            DrmSessionManager.DRM_UNSUPPORTED,
            new DrmSessionEventListener.EventDispatcher(),
            MEDIA3_CUES_FORMAT,
            ImmutableList.of(
                cuesSample(/* timeUs= */ 0, "one"),
                cuesSample(/* timeUs= */ 1_000_000, "two"),
                cuesSample(/* timeUs= */ 2_000_000, "three"),
                cuesSample(/* timeUs= */ 3_000_000, "four"),
                END_OF_STREAM_ITEM));
    fakeSampleStream.writeData(/* startPositionUs= */ 0);
    renderer.replaceStream(
        new Format[] {MEDIA3_CUES_FORMAT},
        fakeSampleStream,
        /* startPositionUs= */ 0,
        /* offsetUs= */ 0,
        new MediaSource.MediaPeriodId(new Object()));

    renderer.render(/* positionUs= */ 0, /* elapsedRealtimeUs= */ 0);
    renderer.render(/* positionUs= */ 0, /* elapsedRealtimeUs= */ 0);

    assertThat(translator.requestSizes).containsExactly(4);
  }

  @Test
  public void renderFromCuesWithTiming_keepsSourceCueWhileTranslationIsPending() throws Exception {
    PendingTranslator translator = new PendingTranslator();
    ArrayList<CueGroup> outputs = new ArrayList<>();
    TextRenderer renderer =
        new TextRenderer(
            outputs::add,
            /* outputLooper= */ null,
            SubtitleDecoderFactory.DEFAULT,
            translator);
    FakeSampleStream fakeSampleStream =
        createFakeSampleStream(ImmutableList.of(cuesSample(/* timeUs= */ 0, "bonjour")));
    fakeSampleStream.writeData(/* startPositionUs= */ 0);
    renderer.replaceStream(
        new Format[] {MEDIA3_CUES_FORMAT},
        fakeSampleStream,
        /* startPositionUs= */ 0,
        /* offsetUs= */ 0,
        new MediaSource.MediaPeriodId(new Object()));

    renderer.render(/* positionUs= */ 0, /* elapsedRealtimeUs= */ 0);
    renderer.render(/* positionUs= */ 0, /* elapsedRealtimeUs= */ 0);

    assertThat(lastOutputText(outputs)).isEqualTo("bonjour");

    translator.completeWith("hallo");
    renderer.render(/* positionUs= */ 0, /* elapsedRealtimeUs= */ 0);

    assertThat(lastOutputText(outputs)).isEqualTo("hallo");
  }

  @Test
  public void renderFromCuesWithTiming_rendersTranslatorStoreHitWithoutMissCallback()
      throws Exception {
    StoreHitTranslator translator = new StoreHitTranslator();
    ArrayList<CueGroup> outputs = new ArrayList<>();
    TextRenderer renderer =
        new TextRenderer(
            outputs::add,
            /* outputLooper= */ null,
            SubtitleDecoderFactory.DEFAULT,
            translator);
    FakeSampleStream fakeSampleStream =
        createFakeSampleStream(ImmutableList.of(cuesSample(/* timeUs= */ 0, "bonjour")));
    fakeSampleStream.writeData(/* startPositionUs= */ 0);
    renderer.replaceStream(
        new Format[] {MEDIA3_CUES_FORMAT},
        fakeSampleStream,
        /* startPositionUs= */ 0,
        /* offsetUs= */ 0,
        new MediaSource.MediaPeriodId(new Object()));

    renderer.render(/* positionUs= */ 0, /* elapsedRealtimeUs= */ 0);
    renderer.render(/* positionUs= */ 0, /* elapsedRealtimeUs= */ 0);

    assertThat(lastOutputText(outputs)).isEqualTo("hallo");
    assertThat(translator.renderedWithoutTranslationCount).isEqualTo(0);
  }

  private FakeSampleStream.FakeSampleStreamItem cuesSample(long timeUs, String text) {
    byte[] data =
        cueEncoder.encode(
            ImmutableList.of(new Cue.Builder().setText(text).build()),
            /* durationUs= */ 500_000);
    return sample(timeUs, C.BUFFER_FLAG_KEY_FRAME, data);
  }

  private static FakeSampleStream createFakeSampleStream(
      ImmutableList<FakeSampleStream.FakeSampleStreamItem> samples) {
    return new FakeSampleStream(
        new DefaultAllocator(/* trimOnReset= */ true, C.DEFAULT_BUFFER_SEGMENT_SIZE),
        /* mediaSourceEventDispatcher= */ null,
        DrmSessionManager.DRM_UNSUPPORTED,
        new DrmSessionEventListener.EventDispatcher(),
        MEDIA3_CUES_FORMAT,
        samples);
  }

  private static String lastOutputText(List<CueGroup> outputs) {
    if (outputs.isEmpty()) {
      return "";
    }
    CueGroup cueGroup = outputs.get(outputs.size() - 1);
    if (cueGroup.cues.isEmpty() || cueGroup.cues.get(0).text == null) {
      return "";
    }
    return cueGroup.cues.get(0).text.toString();
  }

  private static final class CapturingTranslator implements CueGroupSubtitleTranslator {
    public final List<Integer> requestSizes;

    public CapturingTranslator() {
      requestSizes = new ArrayList<>();
    }

    @Override
    @Nullable
    public String getConfigurationToken(Format format) {
      return "test";
    }

    @Override
    public long getPrefetchDurationUs() {
      return Long.MAX_VALUE / 2;
    }

    @Override
    public void translate(
        Format format, List<CueGroup> cueGroups, TranslationCallback callback) {
      requestSizes.add(cueGroups.size());
      callback.onSuccess(cueGroups);
    }
  }

  private static final class PendingTranslator implements CueGroupSubtitleTranslator {
    private List<CueGroup> cueGroups;
    @Nullable private TranslationCallback callback;

    @Override
    @Nullable
    public String getConfigurationToken(Format format) {
      return "test";
    }

    @Override
    public long getPrefetchDurationUs() {
      return Long.MAX_VALUE / 2;
    }

    @Override
    public void translate(
        Format format, List<CueGroup> cueGroups, TranslationCallback callback) {
      this.cueGroups = cueGroups;
      this.callback = callback;
    }

    public void completeWith(String text) {
      TranslationCallback callback = this.callback;
      assertThat(callback).isNotNull();
      assertThat(cueGroups).isNotNull();
      CueGroup sourceCueGroup = cueGroups.get(0);
      callback.onSuccess(
          ImmutableList.of(
              new CueGroup(
                  ImmutableList.of(new Cue.Builder().setText(text).build()),
                  sourceCueGroup.presentationTimeUs)));
    }
  }

  private static final class StoreHitTranslator implements CueGroupSubtitleTranslator {
    public int renderedWithoutTranslationCount;

    @Override
    @Nullable
    public String getConfigurationToken(Format format) {
      return "test";
    }

    @Override
    public long getPrefetchDurationUs() {
      return Long.MAX_VALUE / 2;
    }

    @Override
    @Nullable
    public CueGroup getTranslatedCueGroup(Format format, CueGroup sourceCueGroup) {
      if (lastOutputText(ImmutableList.of(sourceCueGroup)).equals("bonjour")) {
        return new CueGroup(
            ImmutableList.of(new Cue.Builder().setText("hallo").build()),
            sourceCueGroup.presentationTimeUs);
      }
      return null;
    }

    @Override
    public void onCueGroupRenderedWithoutTranslation(Format format, CueGroup sourceCueGroup) {
      renderedWithoutTranslationCount++;
    }

    @Override
    public void translate(
        Format format, List<CueGroup> cueGroups, TranslationCallback callback) {
      callback.onSuccess(ImmutableList.of());
    }
  }
}
