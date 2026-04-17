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
package androidx.media3.common.util;

import static com.google.common.truth.Truth.assertThat;

import androidx.media3.common.MimeTypes;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Unit tests for {@link DolbyVisionCompatibility}. */
@RunWith(AndroidJUnit4.class)
public final class DolbyVisionCompatibilityTest {

  @After
  public void tearDown() {
    DolbyVisionCompatibility.setMapDv7ToHevcEnabled(false);
  }

  @Test
  public void shouldMapDolbyVisionProfile7_disabled_returnsFalse() {
    DolbyVisionCompatibility.setMapDv7ToHevcEnabled(false);

    assertThat(
            DolbyVisionCompatibility.shouldMapDolbyVisionProfile7(
                MimeTypes.VIDEO_DOLBY_VISION, "dvhe.07.06"))
        .isFalse();
  }

  @Test
  public void shouldMapDolbyVisionProfile7_enabledForProfile7_returnsTrue() {
    DolbyVisionCompatibility.setMapDv7ToHevcEnabled(true);

    assertThat(
            DolbyVisionCompatibility.shouldMapDolbyVisionProfile7(
                MimeTypes.VIDEO_DOLBY_VISION, "dvhe.07.06"))
        .isTrue();
  }

  @Test
  public void shouldMapDolbyVisionProfile7_enabledForNonProfile7_returnsFalse() {
    DolbyVisionCompatibility.setMapDv7ToHevcEnabled(true);

    assertThat(
            DolbyVisionCompatibility.shouldMapDolbyVisionProfile7(
                MimeTypes.VIDEO_DOLBY_VISION, "dvhe.08.06"))
        .isFalse();
  }

  @Test
  public void chooseHevcCodecsString_prefersPrimaryHevcString() {
    assertThat(DolbyVisionCompatibility.chooseHevcCodecsString("hvc1.2.4.L153.B0", "dvhe.07.06"))
        .isEqualTo("hvc1.2.4.L153.B0");
  }

  @Test
  public void chooseHevcCodecsString_findsPrimaryHevcStringInCodecList() {
    assertThat(
            DolbyVisionCompatibility.chooseHevcCodecsString(
                "hvc1.2.4.L153.B0,dvhe.07.06", null))
        .isEqualTo("hvc1.2.4.L153.B0");
  }

  @Test
  public void chooseHevcCodecsString_usesSupplementalHevcStringWhenPrimaryIsDolbyVision() {
    assertThat(DolbyVisionCompatibility.chooseHevcCodecsString("dvhe.07.06", "hev1.2.4.L153.B0"))
        .isEqualTo("hev1.2.4.L153.B0");
  }
}
