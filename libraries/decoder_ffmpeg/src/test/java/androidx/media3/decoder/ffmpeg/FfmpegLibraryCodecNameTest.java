/*
 * Copyright (C) 2026 Nuvio
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
package androidx.media3.decoder.ffmpeg;

import static com.google.common.truth.Truth.assertThat;

import androidx.media3.common.MimeTypes;
import org.junit.Test;

public final class FfmpegLibraryCodecNameTest {

  @Test
  public void getCodecName_mapsHighValueSoftwareFallbackFormats() {
    assertThat(FfmpegLibrary.getCodecName(MimeTypes.AUDIO_DTS_HD)).isEqualTo("dca");
    assertThat(FfmpegLibrary.getCodecName(MimeTypes.AUDIO_TRUEHD)).isEqualTo("truehd");
    assertThat(FfmpegLibrary.getCodecName(MimeTypes.VIDEO_VC1)).isEqualTo("vc1");
    assertThat(FfmpegLibrary.getCodecName(MimeTypes.VIDEO_AV1)).isEqualTo("av1");
  }
}
