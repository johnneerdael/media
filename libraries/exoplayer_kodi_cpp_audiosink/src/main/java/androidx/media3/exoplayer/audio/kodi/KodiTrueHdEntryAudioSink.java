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
import android.os.Build;
import androidx.annotation.Nullable;
import androidx.annotation.RequiresApi;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.AuxEffectInfo;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackParameters;
import androidx.media3.common.util.Clock;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.analytics.PlayerId;
import androidx.media3.exoplayer.audio.AudioOffloadSupport;
import androidx.media3.exoplayer.audio.AudioOutputProvider;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.RendererClockAwareAudioSink;
import java.nio.ByteBuffer;

/**
 * Entry sink that keeps the shared path unchanged for non-TrueHD and establishes a separate
 * delegate boundary once the real configured format is known.
 */
@UnstableApi
public final class KodiTrueHdEntryAudioSink implements AudioSink, RendererClockAwareAudioSink {

  private final AudioSink baselineSink;
  private final AudioSink trueHdSink;
  private AudioSink activeSink;
  @Nullable private Listener listener;
  @Nullable private PlayerId playerId;
  @Nullable private Clock clock;
  @Nullable private AudioAttributes audioAttributes;
  @Nullable private AudioDeviceInfo preferredDevice;
  @Nullable private AuxEffectInfo auxEffectInfo;
  private PlaybackParameters playbackParameters;
  private float volume;
  private boolean skipSilenceEnabled;
  private int audioSessionId;
  private int virtualDeviceId;
  private long outputStreamOffsetUs;
  private long rendererClockUs;
  @OffloadMode private int offloadMode;
  private int offloadDelayInFrames;
  private int offloadPaddingInFrames;
  @Nullable private AudioOutputProvider audioOutputProvider;

  public KodiTrueHdEntryAudioSink(AudioSink baselineSink, AudioSink trueHdSink) {
    this.baselineSink = baselineSink;
    this.trueHdSink = trueHdSink;
    activeSink = baselineSink;
    playbackParameters = PlaybackParameters.DEFAULT;
    volume = 1f;
    offloadMode = OFFLOAD_MODE_DISABLED;
  }

  @Override
  public void setListener(Listener listener) {
    this.listener = listener;
    baselineSink.setListener(listener);
    trueHdSink.setListener(listener);
  }

  @Override
  public void setPlayerId(@Nullable PlayerId playerId) {
    this.playerId = playerId;
    baselineSink.setPlayerId(playerId);
    trueHdSink.setPlayerId(playerId);
  }

  @Override
  public void setClock(Clock clock) {
    this.clock = clock;
    baselineSink.setClock(clock);
    trueHdSink.setClock(clock);
  }

  @Override
  public boolean supportsFormat(Format format) {
    return routeForFormat(format).supportsFormat(format);
  }

  @Override
  public @SinkFormatSupport int getFormatSupport(Format format) {
    return routeForFormat(format).getFormatSupport(format);
  }

  @Override
  public AudioOffloadSupport getFormatOffloadSupport(Format format) {
    return routeForFormat(format).getFormatOffloadSupport(format);
  }

  @Override
  public long getCurrentPositionUs(boolean sourceEnded) {
    return activeSink.getCurrentPositionUs(sourceEnded);
  }

  @Override
  public void configure(Format inputFormat, int specifiedBufferSize, @Nullable int[] outputChannels)
      throws ConfigurationException {
    AudioSink selectedSink = routeForFormat(inputFormat);
    if (activeSink != selectedSink) {
      activeSink.flush();
      activeSink.reset();
      activeSink = selectedSink;
      reapplyState(activeSink);
    }
    activeSink.configure(inputFormat, specifiedBufferSize, outputChannels);
  }

  @Override
  public void play() {
    activeSink.play();
  }

  @Override
  public void handleDiscontinuity() {
    activeSink.handleDiscontinuity();
  }

  @Override
  public boolean handleBuffer(ByteBuffer buffer, long presentationTimeUs, int encodedAccessUnitCount)
      throws InitializationException, WriteException {
    return activeSink.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount);
  }

  @Override
  public void playToEndOfStream() throws WriteException {
    activeSink.playToEndOfStream();
  }

  @Override
  public boolean isEnded() {
    return activeSink.isEnded();
  }

  @Override
  public boolean hasPendingData() {
    return activeSink.hasPendingData();
  }

  @Override
  public void setPlaybackParameters(PlaybackParameters playbackParameters) {
    this.playbackParameters = playbackParameters;
    baselineSink.setPlaybackParameters(playbackParameters);
    trueHdSink.setPlaybackParameters(playbackParameters);
  }

  @Override
  public PlaybackParameters getPlaybackParameters() {
    return playbackParameters;
  }

  @Override
  public void setSkipSilenceEnabled(boolean skipSilenceEnabled) {
    this.skipSilenceEnabled = skipSilenceEnabled;
    baselineSink.setSkipSilenceEnabled(skipSilenceEnabled);
    trueHdSink.setSkipSilenceEnabled(skipSilenceEnabled);
  }

  @Override
  public boolean getSkipSilenceEnabled() {
    return skipSilenceEnabled;
  }

  @Override
  public void setAudioAttributes(AudioAttributes audioAttributes) {
    this.audioAttributes = audioAttributes;
    baselineSink.setAudioAttributes(audioAttributes);
    trueHdSink.setAudioAttributes(audioAttributes);
  }

  @Override
  @Nullable
  public AudioAttributes getAudioAttributes() {
    return audioAttributes;
  }

  @Override
  public void setAudioSessionId(int audioSessionId) {
    this.audioSessionId = audioSessionId;
    baselineSink.setAudioSessionId(audioSessionId);
    trueHdSink.setAudioSessionId(audioSessionId);
  }

  @Override
  public void setAuxEffectInfo(AuxEffectInfo auxEffectInfo) {
    this.auxEffectInfo = auxEffectInfo;
    baselineSink.setAuxEffectInfo(auxEffectInfo);
    trueHdSink.setAuxEffectInfo(auxEffectInfo);
  }

  @Override
  public void setPreferredDevice(@Nullable AudioDeviceInfo audioDeviceInfo) {
    preferredDevice = audioDeviceInfo;
    baselineSink.setPreferredDevice(audioDeviceInfo);
    trueHdSink.setPreferredDevice(audioDeviceInfo);
  }

  @Override
  public void setVirtualDeviceId(int virtualDeviceId) {
    this.virtualDeviceId = virtualDeviceId;
    baselineSink.setVirtualDeviceId(virtualDeviceId);
    trueHdSink.setVirtualDeviceId(virtualDeviceId);
  }

  @Override
  public void setOutputStreamOffsetUs(long outputStreamOffsetUs) {
    this.outputStreamOffsetUs = outputStreamOffsetUs;
    baselineSink.setOutputStreamOffsetUs(outputStreamOffsetUs);
    trueHdSink.setOutputStreamOffsetUs(outputStreamOffsetUs);
  }

  @Override
  public long getAudioTrackBufferSizeUs() {
    return activeSink.getAudioTrackBufferSizeUs();
  }

  @Override
  public void enableTunnelingV21() {
    baselineSink.enableTunnelingV21();
    trueHdSink.enableTunnelingV21();
  }

  @Override
  public void disableTunneling() {
    baselineSink.disableTunneling();
    trueHdSink.disableTunneling();
  }

  @Override
  @RequiresApi(29)
  public void setOffloadMode(@OffloadMode int offloadMode) {
    this.offloadMode = offloadMode;
    baselineSink.setOffloadMode(offloadMode);
    trueHdSink.setOffloadMode(offloadMode);
  }

  @Override
  @RequiresApi(29)
  public void setOffloadDelayPadding(int delayInFrames, int paddingInFrames) {
    offloadDelayInFrames = delayInFrames;
    offloadPaddingInFrames = paddingInFrames;
    baselineSink.setOffloadDelayPadding(delayInFrames, paddingInFrames);
    trueHdSink.setOffloadDelayPadding(delayInFrames, paddingInFrames);
  }

  @Override
  public void setAudioOutputProvider(AudioOutputProvider audioOutputProvider) {
    this.audioOutputProvider = audioOutputProvider;
    baselineSink.setAudioOutputProvider(audioOutputProvider);
    trueHdSink.setAudioOutputProvider(audioOutputProvider);
  }

  @Override
  public void setVolume(float volume) {
    this.volume = volume;
    baselineSink.setVolume(volume);
    trueHdSink.setVolume(volume);
  }

  @Override
  public void pause() {
    activeSink.pause();
  }

  @Override
  public void flush() {
    activeSink.flush();
  }

  @Override
  public void reset() {
    baselineSink.reset();
    trueHdSink.reset();
    activeSink = baselineSink;
  }

  @Override
  public void release() {
    baselineSink.release();
    trueHdSink.release();
  }

  @Override
  public void setRendererClockUs(long rendererClockUs) {
    this.rendererClockUs = rendererClockUs;
    if (baselineSink instanceof RendererClockAwareAudioSink) {
      ((RendererClockAwareAudioSink) baselineSink).setRendererClockUs(rendererClockUs);
    }
    if (trueHdSink instanceof RendererClockAwareAudioSink) {
      ((RendererClockAwareAudioSink) trueHdSink).setRendererClockUs(rendererClockUs);
    }
  }

  private AudioSink routeForFormat(@Nullable Format format) {
    return isTrueHdFormat(format) ? trueHdSink : baselineSink;
  }

  private static boolean isTrueHdFormat(@Nullable Format format) {
    return format != null && MimeTypes.AUDIO_TRUEHD.equals(format.sampleMimeType);
  }

  private void reapplyState(AudioSink sink) {
    if (listener != null) {
      sink.setListener(listener);
    }
    sink.setPlayerId(playerId);
    if (clock != null) {
      sink.setClock(clock);
    }
    if (audioAttributes != null) {
      sink.setAudioAttributes(audioAttributes);
    }
    sink.setAudioSessionId(audioSessionId);
    if (auxEffectInfo != null) {
      sink.setAuxEffectInfo(auxEffectInfo);
    }
    sink.setPreferredDevice(preferredDevice);
    sink.setVirtualDeviceId(virtualDeviceId);
    sink.setOutputStreamOffsetUs(outputStreamOffsetUs);
    sink.setPlaybackParameters(playbackParameters);
    sink.setSkipSilenceEnabled(skipSilenceEnabled);
    sink.setVolume(volume);
    if (audioOutputProvider != null) {
      sink.setAudioOutputProvider(audioOutputProvider);
    }
    if (Build.VERSION.SDK_INT >= 29) {
      sink.setOffloadMode(offloadMode);
      sink.setOffloadDelayPadding(offloadDelayInFrames, offloadPaddingInFrames);
    }
    if (sink instanceof RendererClockAwareAudioSink) {
      ((RendererClockAwareAudioSink) sink).setRendererClockUs(rendererClockUs);
    }
  }
}
