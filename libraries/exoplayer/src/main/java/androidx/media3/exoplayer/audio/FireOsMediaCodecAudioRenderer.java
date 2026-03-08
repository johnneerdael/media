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

import android.content.Context;
import android.os.Handler;
import androidx.annotation.Nullable;
import androidx.media3.common.Format;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.mediacodec.MediaCodecAdapter;
import androidx.media3.exoplayer.mediacodec.MediaCodecSelector;
import java.nio.ByteBuffer;

/** Fire OS specific audio renderer that hands richer encoded packet metadata to the custom sink. */
@UnstableApi
public final class FireOsMediaCodecAudioRenderer extends MediaCodecAudioRenderer {

  private final FireOsDefaultAudioSink fireOsAudioSink;

  public FireOsMediaCodecAudioRenderer(
      Context context,
      MediaCodecAdapter.Factory codecAdapterFactory,
      MediaCodecSelector mediaCodecSelector,
      boolean enableDecoderFallback,
      @Nullable Handler eventHandler,
      @Nullable AudioRendererEventListener eventListener,
      FireOsDefaultAudioSink audioSink) {
    super(
        context,
        codecAdapterFactory,
        mediaCodecSelector,
        enableDecoderFallback,
        eventHandler,
        eventListener,
        audioSink);
    this.fireOsAudioSink = audioSink;
  }

  @Override
  protected boolean handleBufferWithAudioSink(
      ByteBuffer buffer, long bufferPresentationTimeUs, int sampleCount, Format format)
      throws AudioSink.InitializationException, AudioSink.WriteException {
    return fireOsAudioSink.handleBufferWithMetadata(
        buffer, bufferPresentationTimeUs, sampleCount, format);
  }
}
