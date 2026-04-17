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
package androidx.media3.extractor.mkv;

import static com.google.common.truth.Truth.assertThat;

import androidx.media3.common.C;
import androidx.media3.common.DataReader;
import androidx.media3.common.Format;
import androidx.media3.common.util.ParsableByteArray;
import androidx.media3.extractor.TrackOutput;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MatroskaDolbyVisionStreamingRewriterTest {

  @Test
  public void streamingTransformerInterface_hasRpuCallbackAndSampleDecision() {
    MatroskaExtractor.DolbyVisionSampleTransformer transformer =
        new MatroskaExtractor.DolbyVisionSampleTransformer() {};

    assertThat(
            transformer.shouldTransformHevcSampleNalByNal(
                /* sampleTimeUs= */ 12_345L,
                /* nalUnitLengthFieldLength= */ 4,
                /* blockAdditionalData= */ null,
                /* dolbyVisionConfigBytes= */ null))
        .isFalse();

    byte[] rpu = nal(/* type= */ 62, /* layerId= */ 1, new byte[] {0x11});
    assertThat(
            transformer.transformDolbyVisionRpuNal(
                rpu,
                /* sampleTimeUs= */ 12_345L,
                /* blockAdditionalData= */ null,
                /* dolbyVisionConfigBytes= */ null))
        .isNull();
  }

  private static byte[] nal(int type, int layerId, byte[] payload) {
    byte[] out = new byte[2 + payload.length];
    out[0] = (byte) ((type << 1) | ((layerId >>> 5) & 0x01));
    out[1] = (byte) (((layerId & 0x1F) << 3) | 0x01);
    System.arraycopy(payload, 0, out, 2, payload.length);
    return out;
  }

  private static final class CapturingTrackOutput implements TrackOutput {
    final List<byte[]> chunks = new ArrayList<>();

    @Override
    public void format(Format format) {}

    @Override
    public int sampleData(
        DataReader input, int length, boolean allowEndOfInput, int sampleDataPart)
        throws IOException {
      throw new UnsupportedOperationException("DataReader path is not used by this contract test");
    }

    @Override
    public void sampleData(ParsableByteArray data, int length, int sampleDataPart) {
      byte[] copy = new byte[length];
      data.readBytes(copy, 0, length);
      chunks.add(copy);
    }

    @Override
    public void sampleMetadata(
        long timeUs, @C.BufferFlags int flags, int size, int offset, CryptoData cryptoData) {}
  }
}
