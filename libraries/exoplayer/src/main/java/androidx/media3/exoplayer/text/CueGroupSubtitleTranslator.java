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

import androidx.annotation.Nullable;
import androidx.media3.common.Format;
import androidx.media3.common.text.CueGroup;
import androidx.media3.common.util.UnstableApi;
import java.util.List;

/**
 * Translates future subtitle {@link CueGroup cue groups} asynchronously before they're rendered.
 *
 * <p>The translator is expected to preserve the input order and each cue group's presentation
 * timestamp in its response.
 */
@UnstableApi
public interface CueGroupSubtitleTranslator {

  /**
   * Returns a stable token for the active translation configuration, or {@code null} if
   * translation is disabled for the current stream.
   */
  @Nullable
  String getConfigurationToken(Format format);

  /** Returns how far ahead subtitle cue groups should be prefetched, in microseconds. */
  long getPrefetchDurationUs();

  /**
   * Translates the given cue groups asynchronously.
   *
   * <p>The callback may be invoked on any thread.
   */
  void translate(Format format, List<CueGroup> cueGroups, TranslationCallback callback);

  /** Callback for translation results. */
  interface TranslationCallback {
    void onSuccess(List<CueGroup> translatedCueGroups);

    void onFailure(Exception exception);
  }
}
