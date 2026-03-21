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
package androidx.media3.exoplayer.audio.kodi;

import android.media.AudioDeviceInfo;
import androidx.annotation.Nullable;
import androidx.annotation.RequiresApi;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.AuxEffectInfo;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackParameters;
import androidx.media3.common.util.Clock;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.analytics.PlayerId;
import androidx.media3.exoplayer.audio.AudioOffloadSupport;
import androidx.media3.exoplayer.audio.AudioOutputProvider;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.DefaultAudioSink;
import androidx.media3.exoplayer.audio.ForwardingAudioSink;
import androidx.media3.exoplayer.audio.RendererClockAwareAudioSink;
import java.nio.ByteBuffer;

@UnstableApi
public final class KodiTrueHdEntryAudioSink extends ForwardingAudioSink
    implements RendererClockAwareAudioSink {

  private final KodiNativeAudioSink baselineSink;
  private final KodiTrueHdNativeAudioSink trueHdSink;
  private AudioSink activeSink;
  @Nullable private Listener listener;
  @Nullable private PlayerId playerId;
  @Nullable private Clock clock;
  private PlaybackParameters playbackParameters;
  private boolean skipSilenceEnabled;
  @Nullable private AudioAttributes audioAttributes;
  private int audioSessionId;
  @Nullable private AuxEffectInfo auxEffectInfo;
  @Nullable private AudioDeviceInfo preferredDevice;
  private int virtualDeviceId;
  private long outputStreamOffsetUs;
  private int offloadMode;
  private int offloadDelayInFrames;
  private int offloadPaddingInFrames;
  @Nullable private AudioOutputProvider audioOutputProvider;
  private float volume;
  private long rendererClockUs;

  public static KodiTrueHdEntryAudioSink create(
      DefaultAudioSink baselineDelegate, DefaultAudioSink trueHdDelegate) {
    return new KodiTrueHdEntryAudioSink(
        new KodiNativeAudioSink(baselineDelegate), new KodiTrueHdNativeAudioSink(trueHdDelegate));
  }

  public KodiTrueHdEntryAudioSink(
      KodiNativeAudioSink baselineSink, KodiTrueHdNativeAudioSink trueHdSink) {
    super(baselineSink);
    this.baselineSink = baselineSink;
    this.trueHdSink = trueHdSink;
    this.activeSink = baselineSink;
    this.playbackParameters = PlaybackParameters.DEFAULT;
    this.audioSessionId = C.AUDIO_SESSION_ID_UNSET;
    this.virtualDeviceId = C.INDEX_UNSET;
    this.outputStreamOffsetUs = 0L;
    this.offloadMode = OFFLOAD_MODE_DISABLED;
    this.volume = 1f;
    this.rendererClockUs = C.TIME_UNSET;
  }

  @Override
  public void setListener(Listener listener) {
    this.listener = listener;
    if (activeSink == baselineSink) {
      super.setListener(listener);
    } else {
      activeSink.setListener(listener);
    }
  }

  @Override
  public void setPlayerId(@Nullable PlayerId playerId) {
    this.playerId = playerId;
    if (activeSink == baselineSink) {
      super.setPlayerId(playerId);
    } else {
      activeSink.setPlayerId(playerId);
    }
  }

  @Override
  public void setClock(Clock clock) {
    this.clock = clock;
    if (activeSink == baselineSink) {
      super.setClock(clock);
    } else {
      activeSink.setClock(clock);
    }
  }

  @Override
  public boolean supportsFormat(Format format) {
    return selectSink(format).supportsFormat(format);
  }

  @Override
  public @SinkFormatSupport int getFormatSupport(Format format) {
    return selectSink(format).getFormatSupport(format);
  }

  @Override
  public AudioOffloadSupport getFormatOffloadSupport(Format format) {
    return selectSink(format).getFormatOffloadSupport(format);
  }

  @Override
  public long getCurrentPositionUs(boolean sourceEnded) {
    return activeSink == baselineSink
        ? super.getCurrentPositionUs(sourceEnded)
        : activeSink.getCurrentPositionUs(sourceEnded);
  }

  @Override
  public void configure(Format inputFormat, int specifiedBufferSize, @Nullable int[] outputChannels)
      throws ConfigurationException {
    AudioSink targetSink = selectSink(inputFormat);
    if (activeSink != targetSink) {
      activeSink.reset();
      activeSink = targetSink;
      applyStoredState(activeSink);
    }
    if (activeSink == baselineSink) {
      super.configure(inputFormat, specifiedBufferSize, outputChannels);
    } else {
      activeSink.configure(inputFormat, specifiedBufferSize, outputChannels);
    }
  }

  @Override
  public void play() {
    if (activeSink == baselineSink) {
      super.play();
    } else {
      activeSink.play();
    }
  }

  @Override
  public void handleDiscontinuity() {
    if (activeSink == baselineSink) {
      super.handleDiscontinuity();
    } else {
      activeSink.handleDiscontinuity();
    }
  }

  @Override
  public boolean handleBuffer(ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount)
      throws InitializationException, WriteException {
    return activeSink == baselineSink
        ? super.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount)
        : activeSink.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount);
  }

  @Override
  public void playToEndOfStream() throws WriteException {
    if (activeSink == baselineSink) {
      super.playToEndOfStream();
    } else {
      activeSink.playToEndOfStream();
    }
  }

  @Override
  public boolean isEnded() {
    return activeSink == baselineSink ? super.isEnded() : activeSink.isEnded();
  }

  @Override
  public boolean hasPendingData() {
    return activeSink == baselineSink ? super.hasPendingData() : activeSink.hasPendingData();
  }

  @Override
  public void setPlaybackParameters(PlaybackParameters playbackParameters) {
    this.playbackParameters = playbackParameters;
    if (activeSink == baselineSink) {
      super.setPlaybackParameters(playbackParameters);
    } else {
      activeSink.setPlaybackParameters(playbackParameters);
    }
  }

  @Override
  public PlaybackParameters getPlaybackParameters() {
    return activeSink == baselineSink ? super.getPlaybackParameters() : activeSink.getPlaybackParameters();
  }

  @Override
  public void setSkipSilenceEnabled(boolean skipSilenceEnabled) {
    this.skipSilenceEnabled = skipSilenceEnabled;
    if (activeSink == baselineSink) {
      super.setSkipSilenceEnabled(skipSilenceEnabled);
    } else {
      activeSink.setSkipSilenceEnabled(skipSilenceEnabled);
    }
  }

  @Override
  public boolean getSkipSilenceEnabled() {
    return activeSink == baselineSink ? super.getSkipSilenceEnabled() : activeSink.getSkipSilenceEnabled();
  }

  @Override
  public void setAudioAttributes(AudioAttributes audioAttributes) {
    this.audioAttributes = audioAttributes;
    if (activeSink == baselineSink) {
      super.setAudioAttributes(audioAttributes);
    } else {
      activeSink.setAudioAttributes(audioAttributes);
    }
  }

  @Override
  @Nullable
  public AudioAttributes getAudioAttributes() {
    return activeSink == baselineSink ? super.getAudioAttributes() : activeSink.getAudioAttributes();
  }

  @Override
  public void setAudioSessionId(int audioSessionId) {
    this.audioSessionId = audioSessionId;
    if (activeSink == baselineSink) {
      super.setAudioSessionId(audioSessionId);
    } else {
      activeSink.setAudioSessionId(audioSessionId);
    }
  }

  @Override
  public void setAuxEffectInfo(AuxEffectInfo auxEffectInfo) {
    this.auxEffectInfo = auxEffectInfo;
    if (activeSink == baselineSink) {
      super.setAuxEffectInfo(auxEffectInfo);
    } else {
      activeSink.setAuxEffectInfo(auxEffectInfo);
    }
  }

  @Override
  public void setPreferredDevice(@Nullable AudioDeviceInfo audioDeviceInfo) {
    this.preferredDevice = audioDeviceInfo;
    if (activeSink == baselineSink) {
      super.setPreferredDevice(audioDeviceInfo);
    } else {
      activeSink.setPreferredDevice(audioDeviceInfo);
    }
  }

  @Override
  public void setVirtualDeviceId(int virtualDeviceId) {
    this.virtualDeviceId = virtualDeviceId;
    if (activeSink == baselineSink) {
      super.setVirtualDeviceId(virtualDeviceId);
    } else {
      activeSink.setVirtualDeviceId(virtualDeviceId);
    }
  }

  @Override
  public void setOutputStreamOffsetUs(long outputStreamOffsetUs) {
    this.outputStreamOffsetUs = outputStreamOffsetUs;
    if (activeSink == baselineSink) {
      super.setOutputStreamOffsetUs(outputStreamOffsetUs);
    } else {
      activeSink.setOutputStreamOffsetUs(outputStreamOffsetUs);
    }
  }

  @Override
  public long getAudioTrackBufferSizeUs() {
    return activeSink == baselineSink ? super.getAudioTrackBufferSizeUs() : activeSink.getAudioTrackBufferSizeUs();
  }

  @Override
  public void enableTunnelingV21() {
    if (activeSink == baselineSink) {
      super.enableTunnelingV21();
    } else {
      activeSink.enableTunnelingV21();
    }
  }

  @Override
  public void disableTunneling() {
    if (activeSink == baselineSink) {
      super.disableTunneling();
    } else {
      activeSink.disableTunneling();
    }
  }

  @Override
  @RequiresApi(29)
  public void setOffloadMode(@OffloadMode int offloadMode) {
    this.offloadMode = offloadMode;
    if (activeSink == baselineSink) {
      super.setOffloadMode(offloadMode);
    } else {
      activeSink.setOffloadMode(offloadMode);
    }
  }

  @Override
  @RequiresApi(29)
  public void setOffloadDelayPadding(int delayInFrames, int paddingInFrames) {
    this.offloadDelayInFrames = delayInFrames;
    this.offloadPaddingInFrames = paddingInFrames;
    if (activeSink == baselineSink) {
      super.setOffloadDelayPadding(delayInFrames, paddingInFrames);
    } else {
      activeSink.setOffloadDelayPadding(delayInFrames, paddingInFrames);
    }
  }

  @Override
  public void setAudioOutputProvider(AudioOutputProvider audioOutputProvider) {
    this.audioOutputProvider = audioOutputProvider;
    if (activeSink == baselineSink) {
      super.setAudioOutputProvider(audioOutputProvider);
    } else {
      activeSink.setAudioOutputProvider(audioOutputProvider);
    }
  }

  @Override
  public void setVolume(float volume) {
    this.volume = volume;
    if (activeSink == baselineSink) {
      super.setVolume(volume);
    } else {
      activeSink.setVolume(volume);
    }
  }

  @Override
  public void pause() {
    if (activeSink == baselineSink) {
      super.pause();
    } else {
      activeSink.pause();
    }
  }

  @Override
  public void flush() {
    if (activeSink == baselineSink) {
      super.flush();
    } else {
      activeSink.flush();
    }
  }

  @Override
  public void reset() {
    super.reset();
    trueHdSink.reset();
    activeSink = baselineSink;
    applyStoredState(baselineSink);
  }

  @Override
  public void release() {
    super.release();
    trueHdSink.release();
    activeSink = baselineSink;
  }

  @Override
  public void setRendererClockUs(long rendererClockUs) {
    this.rendererClockUs = rendererClockUs;
    if (activeSink instanceof RendererClockAwareAudioSink) {
      ((RendererClockAwareAudioSink) activeSink).setRendererClockUs(rendererClockUs);
    }
  }

  private AudioSink selectSink(@Nullable Format format) {
    return format != null && MimeTypes.AUDIO_TRUEHD.equals(format.sampleMimeType)
        ? trueHdSink
        : baselineSink;
  }

  private void applyStoredState(AudioSink sink) {
    if (listener != null) {
      sink.setListener(listener);
    }
    sink.setPlayerId(playerId);
    if (clock != null) {
      sink.setClock(clock);
    }
    sink.setPlaybackParameters(playbackParameters);
    sink.setSkipSilenceEnabled(skipSilenceEnabled);
    if (audioAttributes != null) {
      sink.setAudioAttributes(audioAttributes);
    }
    if (audioSessionId != C.AUDIO_SESSION_ID_UNSET) {
      sink.setAudioSessionId(audioSessionId);
    }
    if (auxEffectInfo != null) {
      sink.setAuxEffectInfo(auxEffectInfo);
    }
    sink.setPreferredDevice(preferredDevice);
    if (virtualDeviceId != C.INDEX_UNSET) {
      sink.setVirtualDeviceId(virtualDeviceId);
    }
    sink.setOutputStreamOffsetUs(outputStreamOffsetUs);
    if (audioOutputProvider != null) {
      sink.setAudioOutputProvider(audioOutputProvider);
    }
    sink.setVolume(volume);
    if (sink instanceof RendererClockAwareAudioSink && rendererClockUs != C.TIME_UNSET) {
      ((RendererClockAwareAudioSink) sink).setRendererClockUs(rendererClockUs);
    }
  }
}
