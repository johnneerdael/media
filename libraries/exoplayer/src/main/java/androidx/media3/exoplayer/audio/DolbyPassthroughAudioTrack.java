/*
 * Copyright (C) 2020 The Android Open Source Project
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

import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.ConditionVariable;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Message;
import java.util.concurrent.Semaphore;

/**
 * Fire TV legacy passthrough audio track wrapper that serializes direct track operations on a
 * dedicated thread.
 */
final class DolbyPassthroughAudioTrack extends AudioTrack {

  private static final String TRACK_HANDLER_THREAD_NAME = "dolbyTrackHandlerThread";
  private static final int MSG_WRITE_TO_TRACK = 1;
  private static final int MSG_PAUSE_TRACK = 2;
  private static final int MSG_PLAY_TRACK = 3;
  private static final int MSG_FLUSH_TRACK = 4;
  private static final int MSG_STOP_TRACK = 5;
  private static final int MSG_RELEASE_TRACK = 6;
  private static final int BUFFER_COUNT = 2;

  private HandlerThread trackHandlerThread;
  private Handler trackHandler;
  private ConditionVariable trackHandlerGate;
  private Semaphore pendingWriteSem;
  private byte[][] audioBuffer;
  private int nextBufferIndex;

  DolbyPassthroughAudioTrack(
      android.media.AudioAttributes attributes,
      AudioFormat format,
      int bufferSizeInBytes,
      int mode,
      int sessionId) {
    super(attributes, format, bufferSizeInBytes, mode, sessionId);
    initialize();
  }

  DolbyPassthroughAudioTrack(
      int streamType,
      int sampleRateInHz,
      int channelConfig,
      int audioFormat,
      int bufferSizeInBytes,
      int mode) {
    this(
        streamType,
        sampleRateInHz,
        channelConfig,
        audioFormat,
        bufferSizeInBytes,
        mode,
        0);
  }

  DolbyPassthroughAudioTrack(
      int streamType,
      int sampleRateInHz,
      int channelConfig,
      int audioFormat,
      int bufferSizeInBytes,
      int mode,
      int sessionId) {
    super(
        streamType,
        sampleRateInHz,
        channelConfig,
        audioFormat,
        bufferSizeInBytes,
        mode,
        sessionId);
    initialize();
  }

  private void initialize() {
    trackHandlerGate = new ConditionVariable(true);
    trackHandlerThread = new HandlerThread(TRACK_HANDLER_THREAD_NAME);
    pendingWriteSem = new Semaphore(BUFFER_COUNT);
    audioBuffer = new byte[BUFFER_COUNT][];
    trackHandlerThread.start();
    trackHandler =
        new Handler(trackHandlerThread.getLooper()) {
          @Override
          public void handleMessage(Message msg) {
            switch (msg.what) {
              case MSG_WRITE_TO_TRACK:
                superWrite(msg.arg1, msg.arg2);
                pendingWriteSem.release();
                break;
              case MSG_PAUSE_TRACK:
                DolbyPassthroughAudioTrack.super.pause();
                trackHandlerGate.open();
                break;
              case MSG_PLAY_TRACK:
                DolbyPassthroughAudioTrack.super.play();
                trackHandlerGate.open();
                break;
              case MSG_FLUSH_TRACK:
                DolbyPassthroughAudioTrack.super.flush();
                trackHandlerGate.open();
                break;
              case MSG_STOP_TRACK:
                DolbyPassthroughAudioTrack.super.stop();
                trackHandlerGate.open();
                break;
              case MSG_RELEASE_TRACK:
                if (DolbyPassthroughAudioTrack.super.getPlayState() != PLAYSTATE_STOPPED) {
                  DolbyPassthroughAudioTrack.super.stop();
                }
                DolbyPassthroughAudioTrack.super.release();
                trackHandlerGate.open();
                break;
              default:
                break;
            }
          }
        };
  }

  @Override
  public void play() throws IllegalStateException {
    sendBlocking(MSG_PLAY_TRACK);
  }

  @Override
  public void pause() throws IllegalStateException {
    sendBlocking(MSG_PAUSE_TRACK);
  }

  @Override
  public void flush() throws IllegalStateException {
    sendBlocking(MSG_FLUSH_TRACK);
  }

  @Override
  public void stop() throws IllegalStateException {
    if (getPlayState() == PLAYSTATE_STOPPED) {
      return;
    }
    sendBlocking(MSG_STOP_TRACK);
  }

  @Override
  public int write(byte[] audioData, int offsetInBytes, int sizeInBytes) {
    if (getPlayState() != PLAYSTATE_PLAYING) {
      return 0;
    }
    if (!pendingWriteSem.tryAcquire()) {
      return 0;
    }
    if (audioBuffer[nextBufferIndex] == null || audioBuffer[nextBufferIndex].length < sizeInBytes) {
      audioBuffer[nextBufferIndex] = new byte[sizeInBytes];
    }
    System.arraycopy(audioData, offsetInBytes, audioBuffer[nextBufferIndex], 0, sizeInBytes);
    trackHandler.sendMessage(
        trackHandler.obtainMessage(MSG_WRITE_TO_TRACK, sizeInBytes, nextBufferIndex));
    nextBufferIndex = (nextBufferIndex + 1) % BUFFER_COUNT;
    return sizeInBytes;
  }

  @Override
  public void release() {
    sendBlocking(MSG_RELEASE_TRACK);
    if (trackHandlerThread != null) {
      trackHandlerThread.quit();
      trackHandlerThread = null;
    }
    trackHandler = null;
    trackHandlerGate = null;
    pendingWriteSem = null;
    audioBuffer = null;
  }

  private void sendBlocking(int what) {
    if (trackHandler == null || trackHandlerGate == null) {
      return;
    }
    trackHandlerGate.close();
    trackHandler.sendMessage(trackHandler.obtainMessage(what));
    trackHandlerGate.block();
  }

  private void superWrite(int size, int bufferIndex) {
    byte[] buffer = audioBuffer[bufferIndex];
    if (buffer != null) {
      DolbyPassthroughAudioTrack.super.write(buffer, 0, size);
    }
  }
}
