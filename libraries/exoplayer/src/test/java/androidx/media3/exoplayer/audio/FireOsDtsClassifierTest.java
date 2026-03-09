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
package androidx.media3.exoplayer.audio;

import static com.google.common.truth.Truth.assertThat;

import android.media.AudioFormat;
import androidx.media3.common.MimeTypes;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class FireOsDtsClassifierTest {

  @Test
  public void classifyFromHints_dtsUhd_staysUnknownAndUsesKodiDefaultOutputPolicy() {
    FireOsStreamInfo streamInfo =
        FireOsDtsClassifier.classifyFromHints(
            /* sampleCount= */ 4096,
            /* repetitionPeriodFrames= */ 4096,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 8,
            /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.NONE,
            /* hasExtension= */ true,
            /* hasUhd= */ true);

    assertThat(streamInfo.dtsStreamType).isEqualTo(FireOsStreamInfo.DtsStreamType.UNKNOWN);
    assertThat(streamInfo.dtsPeriodFrames).isEqualTo(4096);
    assertThat(streamInfo.outputRateHz).isEqualTo(48_000);
    assertThat(streamInfo.logicalPassthroughChannelMask).isEqualTo(AudioFormat.CHANNEL_OUT_STEREO);
  }

  @Test
  public void classifyFromHints_dtsHdCodecHintWithoutParsedBitstream_staysUnknown() {
    FireOsStreamInfo streamInfo =
        FireOsDtsClassifier.classifyFromHints(
            /* sampleCount= */ 4096,
            /* repetitionPeriodFrames= */ 4096,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 6,
            /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.NONE,
            /* hasExtension= */ true,
            /* hasUhd= */ false);

    assertThat(streamInfo.dtsStreamType).isEqualTo(FireOsStreamInfo.DtsStreamType.UNKNOWN);
    assertThat(streamInfo.outputChannelCount).isEqualTo(2);
    assertThat(streamInfo.outputRateHz).isEqualTo(48_000);
    assertThat(streamInfo.sampleMimeType).isEqualTo(MimeTypes.AUDIO_DTS);
  }

  @Test
  public void classifyFromHints_dtsHdWithoutCodecHint_staysUnknown() {
    FireOsStreamInfo streamInfo =
        FireOsDtsClassifier.classifyFromHints(
            /* sampleCount= */ 4096,
            /* repetitionPeriodFrames= */ 4096,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 6,
            /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.NONE,
            /* hasExtension= */ true,
            /* hasUhd= */ false);

    assertThat(streamInfo.dtsStreamType).isEqualTo(FireOsStreamInfo.DtsStreamType.UNKNOWN);
    assertThat(streamInfo.outputChannelCount).isEqualTo(2);
    assertThat(streamInfo.outputRateHz).isEqualTo(48_000);
  }

  @Test
  public void classifyFromHints_dtsExpress_staysUnknownAndUsesKodiDefaultOutputPolicy() {
    FireOsStreamInfo streamInfo =
        FireOsDtsClassifier.classifyFromHints(
            /* sampleCount= */ 4096,
            /* repetitionPeriodFrames= */ 4096,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 2,
            /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.NONE,
            /* hasExtension= */ false,
            /* hasUhd= */ false);

    assertThat(streamInfo.dtsStreamType).isEqualTo(FireOsStreamInfo.DtsStreamType.UNKNOWN);
    assertThat(streamInfo.outputRateHz).isEqualTo(48_000);
    assertThat(streamInfo.outputChannelCount).isEqualTo(2);
  }

  @Test
  public void classifyCoreFrame_dts1024_usesExactKodiCoreType() {
    FireOsStreamInfo.DtsStreamType streamType =
        FireOsDtsClassifier.classifyFromHints(
                /* sampleCount= */ 1024,
                /* repetitionPeriodFrames= */ 1024,
                /* inputSampleRateHz= */ 48_000,
                /* inputChannelCount= */ 2,
                /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.NONE,
                /* hasExtension= */ false,
                /* hasUhd= */ false)
            .dtsStreamType;

    assertThat(streamType).isEqualTo(FireOsStreamInfo.DtsStreamType.DTS_1024);
  }

  @Test
  public void classifyFromHints_dtsHdCore_usesCorePath() {
    FireOsStreamInfo streamInfo =
        FireOsDtsClassifier.classifyFromHints(
            /* sampleCount= */ 512,
            /* repetitionPeriodFrames= */ 512,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 2,
            /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.NONE,
            /* hasExtension= */ false,
            /* hasUhd= */ false);

    assertThat(streamInfo.dtsStreamType).isEqualTo(FireOsStreamInfo.DtsStreamType.DTS_512);
    assertThat(streamInfo.dtsPeriodFrames).isEqualTo(512);
    assertThat(streamInfo.outputChannelCount).isEqualTo(2);
  }

  @Test
  public void classifyFromHints_ambiguousExtension_keepsPreviousKodiType() {
    FireOsStreamInfo streamInfo =
        FireOsDtsClassifier.classifyFromHints(
            /* accessUnit= */ new byte[0],
            /* sampleCount= */ 4096,
            /* repetitionPeriodFrames= */ 4096,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 6,
            /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.DTSHD_MA,
            /* hasExtension= */ true,
            /* hasUhd= */ false);

    assertThat(streamInfo.dtsStreamType).isEqualTo(FireOsStreamInfo.DtsStreamType.DTSHD_MA);
    assertThat(streamInfo.dtsPeriodFrames).isEqualTo(16_384);
    assertThat(streamInfo.outputChannelCount).isEqualTo(8);
    assertThat(streamInfo.outputRateHz).isEqualTo(192_000);
    assertThat(streamInfo.sampleMimeType).isEqualTo(MimeTypes.AUDIO_DTS_HD);
  }

  @Test
  public void classifyFromHints_xchMarkerClassifiesAsDtsHd() {
    FireOsStreamInfo streamInfo =
        FireOsDtsClassifier.classifyFromHints(
            /* accessUnit= */ new byte[] {0x5A, 0x5A, 0x5A, 0x5A, 0x00, 0x00, 0x00, 0x00},
            /* sampleCount= */ 4096,
            /* repetitionPeriodFrames= */ 4096,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 6,
            /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.NONE,
            /* hasExtension= */ true,
            /* hasUhd= */ false);

    assertThat(streamInfo.dtsStreamType).isEqualTo(FireOsStreamInfo.DtsStreamType.DTSHD);
  }

  @Test
  public void classifyFromHints_xllMarkerClassifiesAsDtsHdMa() {
    FireOsStreamInfo streamInfo =
        FireOsDtsClassifier.classifyFromHints(
            /* accessUnit= */ new byte[] {0x41, (byte) 0xA2, (byte) 0x95, 0x47, 0x00, 0x00, 0x00, 0x00},
            /* sampleCount= */ 16_384,
            /* repetitionPeriodFrames= */ 16_384,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 8,
            /* previousStreamType= */ FireOsStreamInfo.DtsStreamType.NONE,
            /* hasExtension= */ true,
            /* hasUhd= */ false);

    assertThat(streamInfo.dtsStreamType).isEqualTo(FireOsStreamInfo.DtsStreamType.DTSHD_MA);
  }
}
