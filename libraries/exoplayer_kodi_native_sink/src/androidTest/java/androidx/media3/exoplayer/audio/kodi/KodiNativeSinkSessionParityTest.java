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

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

import android.media.AudioFormat;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.ParserException;
import androidx.media3.common.util.Util;
import androidx.media3.extractor.Ac3Util;
import androidx.media3.extractor.DtsUtil;
import androidx.media3.extractor.Extractor;
import androidx.media3.extractor.mkv.MatroskaExtractor;
import androidx.media3.extractor.ts.TsExtractor;
import androidx.media3.test.utils.FakeExtractorOutput;
import androidx.media3.test.utils.FakeTrackOutput;
import androidx.media3.test.utils.TestUtil;
import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Native-byte parity checks for the Kodi-backed session path. */
@RunWith(AndroidJUnit4.class)
public final class KodiNativeSinkSessionParityTest {

  private static final int IEC61937_E_AC3 = 0x15;
  private static final int IEC61937_DTS1 = 0x0B;
  private static final int IEC61937_TRUEHD = 0x16;
  private static final int IEC61937_TRUEHD_PACKET_BYTES = 61_440;
  private static final byte[] E_AC3_SYNCFRAME_PREFIX =
      hex("0B7704FF3F86FBC41011007A01CA77B9");
  private static final int TRUEHD_FORMAT_MAJOR_SYNC = 0xF8726FBA;
  private static final int TRUEHD_MAT_BUFFER_LIMIT = IEC61937_TRUEHD_PACKET_BYTES - 24;
  private static final int TRUEHD_MAT_MIDDLE_POSITION = 30_708 + 8;
  private static final byte[] TRUEHD_SYNCFRAME_HEADER =
      new byte[] {
        (byte) 0xC0, 0x75, 0x04, (byte) 0xD8, (byte) 0xF8, 0x72, 0x6F, (byte) 0xBA, 0x00,
        (byte) 0x97, (byte) 0xC0, 0x0F, (byte) 0xB7, 0x52, 0x00, 0x00
      };
  private static final byte[] TRUEHD_MAT_START_CODE =
      new byte[] {
        0x07, (byte) 0x9E, 0x00, 0x03, (byte) 0x84, 0x01, 0x01, 0x01, (byte) 0x80, 0x00, 0x56,
        (byte) 0xA5, 0x3B, (byte) 0xF4, (byte) 0x81, (byte) 0x83, 0x49, (byte) 0x80, 0x77,
        (byte) 0xE0
      };
  private static final byte[] TRUEHD_MAT_MIDDLE_CODE =
      new byte[] {
        (byte) 0xC3, (byte) 0xC1, 0x42, 0x49, 0x3B, (byte) 0xFA, (byte) 0x82, (byte) 0x83, 0x49,
        (byte) 0x80, 0x77, (byte) 0xE0
      };
  private static final byte[] TRUEHD_MAT_END_CODE =
      new byte[] {
        (byte) 0xC3, (byte) 0xC2, (byte) 0xC0, (byte) 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, (byte) 0x97, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00
      };

  @Test
  public void pcmMode_packetMatchesDecodedInputBytes() throws Exception {
    byte[] pcmBytes =
        new byte[] {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00};

    try (KodiNativeSinkSession session = KodiNativeSinkSession.create()) {
      session.configure(
          new Format.Builder()
              .setSampleRate(48_000)
              .setChannelCount(2)
              .setPcmEncoding(C.ENCODING_PCM_16BIT)
              .build(),
          /* specifiedBufferSize= */ 0,
          null,
          fakeSnapshot(),
          new KodiNativePlaybackDecision(
              KodiNativePlaybackDecision.MODE_PCM,
              C.ENCODING_PCM_16BIT,
              AudioFormat.CHANNEL_OUT_STEREO,
              KodiNativePlaybackDecision.STREAM_TYPE_NULL,
              /* flags= */ 0));

      ByteBuffer input = ByteBuffer.allocateDirect(pcmBytes.length);
      input.put(pcmBytes).flip();
      session.queueInput(input, /* presentationTimeUs= */ 12_345, /* encodedAccessUnitCount= */ 1);
      KodiNativePacket packet = session.dequeuePacket();

      assertNotNull(packet);
      assertEquals(KodiNativePacketMetadata.KIND_PCM, packet.metadata.kind);
      assertEquals(3, packet.metadata.totalFrames);
      assertArrayEquals(pcmBytes, packet.data);
    }
  }

  @Test
  public void dtsCoreIecPacket_matchesKodiReferenceBytes() throws Exception {
    byte[] accessUnit =
        new byte[] {
          (byte) 0x7F, (byte) 0xFE, (byte) 0x80, 0x01, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70
        };

    try (KodiNativeSinkSession session = KodiNativeSinkSession.create()) {
      session.configure(
          new Format.Builder()
              .setSampleMimeType(androidx.media3.common.MimeTypes.AUDIO_DTS)
              .setSampleRate(48_000)
              .setChannelCount(6)
              .build(),
          /* specifiedBufferSize= */ 0,
          null,
          fakeSnapshot(),
          new KodiNativePlaybackDecision(
              KodiNativePlaybackDecision.MODE_PASSTHROUGH_IEC_STEREO,
              C.ENCODING_DTS,
              AudioFormat.CHANNEL_OUT_STEREO,
              KodiNativePlaybackDecision.STREAM_TYPE_DTSHD_CORE,
              /* flags= */ 0));

      ByteBuffer input = ByteBuffer.allocateDirect(accessUnit.length);
      input.put(accessUnit).flip();
      session.queueInput(input, /* presentationTimeUs= */ 0, /* encodedAccessUnitCount= */ 1);
      KodiNativePacket packet = session.dequeuePacket();

      assertNotNull(packet);
      assertArrayEquals(packKodiDtsCore(accessUnit, /* sampleCount= */ 512, /* littleEndian= */ false), packet.data);
    }
  }

  @Test
  public void eAc3IecPacket_matchesKodiReferenceBytes() throws Exception {
    byte[] accessUnit = createEAc3Syncframe(Ac3Util.SyncFrameInfo.STREAM_TYPE_TYPE0);

    try (KodiNativeSinkSession session = KodiNativeSinkSession.create()) {
      session.configure(
          new Format.Builder()
              .setSampleMimeType(androidx.media3.common.MimeTypes.AUDIO_E_AC3)
              .setSampleRate(48_000)
              .setChannelCount(6)
              .build(),
          /* specifiedBufferSize= */ 0,
          null,
          fakeSnapshot(),
          new KodiNativePlaybackDecision(
              KodiNativePlaybackDecision.MODE_PASSTHROUGH_IEC_STEREO,
              C.ENCODING_E_AC3,
              AudioFormat.CHANNEL_OUT_STEREO,
              KodiNativePlaybackDecision.STREAM_TYPE_EAC3,
              /* flags= */ 0));

      ByteBuffer input = ByteBuffer.allocateDirect(accessUnit.length);
      input.put(accessUnit).flip();
      session.queueInput(input, /* presentationTimeUs= */ 0, /* encodedAccessUnitCount= */ 1);
      KodiNativePacket packet = session.dequeuePacket();

      assertNotNull(packet);
      assertArrayEquals(packKodiEac3(accessUnit), packet.data);
    }
  }

  @Test
  public void trueHdIecPacket_matchesKodiReferenceBytes() throws Exception {
    List<byte[]> syncframes = new ArrayList<>();
    for (int i = 0; i < 30; i++) {
      syncframes.add(createTrueHdSyncframe(/* frameSizeBytes= */ 120, /* frameTime= */ i * 40));
    }

    try (KodiNativeSinkSession session = KodiNativeSinkSession.create()) {
      session.configure(
          new Format.Builder()
              .setSampleMimeType(androidx.media3.common.MimeTypes.AUDIO_TRUEHD)
              .setSampleRate(48_000)
              .setChannelCount(8)
              .build(),
          /* specifiedBufferSize= */ 0,
          null,
          fakeSnapshot(),
          new KodiNativePlaybackDecision(
              KodiNativePlaybackDecision.MODE_PASSTHROUGH_IEC_MULTICHANNEL,
              C.ENCODING_DOLBY_TRUEHD,
              AudioFormat.CHANNEL_OUT_7POINT1_SURROUND,
              KodiNativePlaybackDecision.STREAM_TYPE_TRUEHD,
              /* flags= */ 0));

      List<byte[]> actual = new ArrayList<>();
      for (byte[] syncframe : syncframes) {
        ByteBuffer input = ByteBuffer.allocateDirect(syncframe.length);
        input.put(syncframe).flip();
        session.queueInput(input, /* presentationTimeUs= */ 0, /* encodedAccessUnitCount= */ 1);
        while (true) {
          KodiNativePacket packet = session.dequeuePacket();
          if (packet == null) {
            break;
          }
          actual.add(packet.data);
        }
      }

      byte[] actualBytes = concat(actual);
      byte[] expectedBytes = new KodiReferenceMatPacker().pack(syncframes);
      assertArrayEquals(expectedBytes, actualBytes);
    }
  }

  @Test
  public void dtsPauseBurst_matchesKodiReferenceBytes() throws Exception {
    byte[] accessUnit = createDtsCoreFrame(/* frameSizeBytes= */ 16, /* sampleCount= */ 512);

    try (KodiNativeSinkSession session = KodiNativeSinkSession.create()) {
      session.configure(
          new Format.Builder()
              .setSampleMimeType(androidx.media3.common.MimeTypes.AUDIO_DTS)
              .setSampleRate(48_000)
              .setChannelCount(6)
              .build(),
          /* specifiedBufferSize= */ 0,
          null,
          fakeSnapshot(),
          new KodiNativePlaybackDecision(
              KodiNativePlaybackDecision.MODE_PASSTHROUGH_IEC_STEREO,
              C.ENCODING_DTS,
              AudioFormat.CHANNEL_OUT_STEREO,
              KodiNativePlaybackDecision.STREAM_TYPE_DTSHD_CORE,
              /* flags= */ 0));

      ByteBuffer input = ByteBuffer.allocateDirect(accessUnit.length);
      input.put(accessUnit).flip();
      session.queueInput(input, /* presentationTimeUs= */ 0, /* encodedAccessUnitCount= */ 1);
      assertNotNull(session.dequeuePacket());

      session.queuePause(/* millis= */ 200, /* iecBursts= */ true);
      KodiNativePacket pausePacket = session.dequeuePacket();

      assertNotNull(pausePacket);
      assertArrayEquals(packKodiPause(/* millis= */ 200, /* framesize= */ 4, /* samplerate= */ 48_000,
              /* repPeriod= */ 3, /* encodedRate= */ 48_000),
          pausePacket.data);
    }
  }

  @Test
  public void dtsHdTsFixture_matchesKodiReferenceBytes() throws Exception {
    assertDtsHdFixtureIecParity(
        "media/ts/sample_dts_hd.ts", new TsExtractor(), MimeTypes.AUDIO_DTS_HD);
  }

  @Test
  public void dtsHdMaMkvFixture_matchesKodiReferenceBytes() throws Exception {
    assertDtsHdFixtureIecParity(
        "media/mkv/sample_with_dts_hd_ma.mkv",
        new MatroskaExtractor(),
        MimeTypes.AUDIO_DTS_HD);
  }

  @Test
  public void dtsUhdTsFixture_directPackets_matchExtractedAccessUnits() throws Exception {
    assertDirectFixturePassthrough(
        "media/ts/sample_dts_uhd.ts",
        new TsExtractor(),
        MimeTypes.AUDIO_DTS_X,
        C.ENCODING_DTS_UHD_P2);
  }

  @Test
  public void dtsXMkvFixture_directPackets_matchExtractedAccessUnits() throws Exception {
    assertDirectFixturePassthrough(
        "media/mkv/sample_with_dts_x.mkv",
        new MatroskaExtractor(),
        MimeTypes.AUDIO_DTS_X,
        C.ENCODING_DTS_UHD_P2);
  }

  private static KodiNativeCapabilitySnapshot fakeSnapshot() {
    int[] encodings =
        new int[] {
          C.ENCODING_DTS, C.ENCODING_DTS_HD, C.ENCODING_DTS_UHD_P2, C.ENCODING_DOLBY_TRUEHD
        };
    return new KodiNativeCapabilitySnapshot(
        33,
        /* tv= */ true,
        /* automotive= */ false,
        C.INDEX_UNSET,
        /* routedDeviceType= */ 0,
        /* maxChannelCount= */ 8,
        encodings,
        new KodiNativeCapabilitySnapshot.ProbeResult(false, C.ENCODING_INVALID, AudioFormat.CHANNEL_INVALID),
        new KodiNativeCapabilitySnapshot.ProbeResult(true, C.ENCODING_E_AC3, AudioFormat.CHANNEL_OUT_STEREO),
        new KodiNativeCapabilitySnapshot.ProbeResult(true, C.ENCODING_DTS, AudioFormat.CHANNEL_OUT_STEREO),
        new KodiNativeCapabilitySnapshot.ProbeResult(true, C.ENCODING_DTS_HD, AudioFormat.CHANNEL_OUT_STEREO),
        new KodiNativeCapabilitySnapshot.ProbeResult(true, C.ENCODING_DOLBY_TRUEHD, AudioFormat.CHANNEL_OUT_7POINT1_SURROUND));
  }

  private static void assertDtsHdFixtureIecParity(
      String assetPath, Extractor extractor, String expectedMimeType) throws Exception {
    ExtractedAudioFixture fixture = extractAudioFixture(extractor, assetPath);
    assertEquals(expectedMimeType, fixture.format.sampleMimeType);

    List<byte[]> actualPackets = new ArrayList<>();
    List<byte[]> expectedPackets = new ArrayList<>();
    try (KodiNativeSinkSession session = KodiNativeSinkSession.create()) {
      session.configure(
          fixture.format,
          /* specifiedBufferSize= */ 0,
          null,
          fakeSnapshot(),
          new KodiNativePlaybackDecision(
              KodiNativePlaybackDecision.MODE_PASSTHROUGH_IEC_MULTICHANNEL,
              C.ENCODING_DTS_HD,
              AudioFormat.CHANNEL_OUT_7POINT1_SURROUND,
              KodiNativePlaybackDecision.STREAM_TYPE_DTSHD,
              /* flags= */ 0));

      for (EncodedSample sample : fixture.samples) {
        ByteBuffer input = ByteBuffer.allocateDirect(sample.data.length);
        input.put(sample.data).flip();
        session.queueInput(input, sample.timeUs, /* encodedAccessUnitCount= */ 1);
        expectedPackets.add(
            packKodiDtsHd(
                sample.data,
                deriveDtsHdRepetitionPeriod(sample.data, fixture.format.sampleRate)));
        drainPacketBytes(session, actualPackets);
      }
      drainPacketBytes(session, actualPackets);
    }

    assertArrayEquals(concat(expectedPackets), concat(actualPackets));
  }

  private static void assertDirectFixturePassthrough(
      String assetPath, Extractor extractor, String expectedMimeType, @C.Encoding int outputEncoding)
      throws Exception {
    ExtractedAudioFixture fixture = extractAudioFixture(extractor, assetPath);
    assertEquals(expectedMimeType, fixture.format.sampleMimeType);

    List<KodiNativePacket> actualPackets = new ArrayList<>();
    try (KodiNativeSinkSession session = KodiNativeSinkSession.create()) {
      session.configure(
          fixture.format,
          /* specifiedBufferSize= */ 0,
          null,
          fakeSnapshot(),
          new KodiNativePlaybackDecision(
              KodiNativePlaybackDecision.MODE_PASSTHROUGH_DIRECT,
              outputEncoding,
              fixture.format.channelCount >= 8
                  ? AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
                  : AudioFormat.CHANNEL_OUT_STEREO,
              KodiNativePlaybackDecision.STREAM_TYPE_NULL,
              /* flags= */ 0));

      for (EncodedSample sample : fixture.samples) {
        ByteBuffer input = ByteBuffer.allocateDirect(sample.data.length);
        input.put(sample.data).flip();
        session.queueInput(input, sample.timeUs, /* encodedAccessUnitCount= */ 1);
        drainPacketObjects(session, actualPackets);
      }
      drainPacketObjects(session, actualPackets);
    }

    assertEquals(fixture.samples.size(), actualPackets.size());
    for (int i = 0; i < fixture.samples.size(); i++) {
      KodiNativePacket packet = actualPackets.get(i);
      assertEquals(KodiNativePacketMetadata.KIND_PASSTHROUGH_DIRECT, packet.metadata.kind);
      assertArrayEquals(fixture.samples.get(i).data, packet.data);
    }
  }

  private static ExtractedAudioFixture extractAudioFixture(Extractor extractor, String assetPath)
      throws IOException {
    FakeExtractorOutput output =
        TestUtil.extractAllSamplesFromFile(
            extractor, ApplicationProvider.getApplicationContext(), assetPath);
    List<FakeTrackOutput> audioTracks = output.getTrackOutputsForType(C.TRACK_TYPE_AUDIO);
    assertEquals(1, audioTracks.size());
    FakeTrackOutput track = audioTracks.get(0);
    Format format = track.lastFormat;
    assertNotNull(format);
    List<EncodedSample> samples = new ArrayList<>();
    for (int i = 0; i < track.getSampleCount(); i++) {
      samples.add(new EncodedSample(track.getSampleData(i), track.getSampleTimeUs(i)));
    }
    return new ExtractedAudioFixture(format, samples);
  }

  private static void drainPacketBytes(KodiNativeSinkSession session, List<byte[]> packets)
      throws KodiNativeException {
    while (true) {
      KodiNativePacket packet = session.dequeuePacket();
      if (packet == null) {
        return;
      }
      packets.add(packet.data);
    }
  }

  private static void drainPacketObjects(
      KodiNativeSinkSession session, List<KodiNativePacket> packets)
      throws KodiNativeException {
    while (true) {
      KodiNativePacket packet = session.dequeuePacket();
      if (packet == null) {
        return;
      }
      packets.add(packet);
    }
  }

  private static byte[] concat(List<byte[]> frames) {
    int size = 0;
    for (byte[] frame : frames) {
      size += frame.length;
    }
    ByteBuffer buffer = ByteBuffer.allocate(size);
    for (byte[] frame : frames) {
      buffer.put(frame);
    }
    return buffer.array();
  }

  private static byte[] packKodiDtsCore(byte[] accessUnit, int sampleCount, boolean littleEndian) {
    int frameSize = sampleCount * 4;
    byte[] output = new byte[frameSize];
    int payloadOffset = 0;
    if (accessUnit.length != frameSize) {
      writePreamble(output, IEC61937_DTS1, accessUnit.length << 3);
      payloadOffset = 8;
    }
    writeMaybeWordSwapped(output, payloadOffset, accessUnit, accessUnit.length, !littleEndian);
    return output;
  }

  private static byte[] packKodiEac3(byte[] accessUnit) {
    byte[] output = new byte[24_576];
    writePreamble(output, IEC61937_E_AC3, accessUnit.length);
    writeMaybeWordSwapped(output, 8, accessUnit, accessUnit.length, true);
    return output;
  }

  private static byte[] packKodiDtsHd(byte[] accessUnit, int repetitionPeriodFrames) {
    int subtype = getKodiDtsHdSubtype(repetitionPeriodFrames);
    int burstSize = repetitionPeriodFrames * 4;
    byte[] payload = new byte[12 + accessUnit.length];
    payload[0] = 0x01;
    payload[8] = (byte) 0xFE;
    payload[9] = (byte) 0xFE;
    payload[10] = (byte) ((accessUnit.length >>> 8) & 0xFF);
    payload[11] = (byte) (accessUnit.length & 0xFF);
    System.arraycopy(accessUnit, 0, payload, 12, accessUnit.length);

    byte[] output = new byte[burstSize];
    writePreamble(
        output,
        IEC61937_DTS1 + 6 | (subtype << 8),
        alignTo16(payload.length + 8) - 8);
    writeMaybeWordSwapped(output, 8, payload, payload.length, true);
    return output;
  }

  private static int deriveDtsHdRepetitionPeriod(byte[] accessUnit, int inputSampleRateHz)
      throws ParserException {
    if (accessUnit.length < 4) {
      throw ParserException.createForMalformedContainer("DTS access unit too short", null);
    }
    int word = Util.getBigEndianInt(ByteBuffer.wrap(accessUnit, 0, 4), 0);
    @DtsUtil.FrameType int frameType = DtsUtil.getFrameType(word);
    switch (frameType) {
      case DtsUtil.FRAME_TYPE_EXTENSION_SUBSTREAM:
        return deriveDtsHdRepetitionPeriodFromHdHeader(accessUnit, inputSampleRateHz);
      case DtsUtil.FRAME_TYPE_UHD_SYNC:
      case DtsUtil.FRAME_TYPE_UHD_NON_SYNC:
        return deriveDtsHdRepetitionPeriodFromUhdHeader(accessUnit, inputSampleRateHz);
      case DtsUtil.FRAME_TYPE_CORE:
      default:
        return computeDtsHdRepetitionPeriod(
            DtsUtil.parseDtsAudioSampleCount(ByteBuffer.wrap(accessUnit)), inputSampleRateHz);
    }
  }

  private static int deriveDtsHdRepetitionPeriodFromHdHeader(byte[] accessUnit, int inputSampleRateHz)
      throws ParserException {
    int headerSize = DtsUtil.parseDtsHdHeaderSize(accessUnit);
    DtsUtil.DtsHeader header = DtsUtil.parseDtsHdHeader(Arrays.copyOf(accessUnit, headerSize));
    int sampleRateHz = header.sampleRate != Format.NO_VALUE ? header.sampleRate : inputSampleRateHz;
    int sampleCount = deriveSampleCountFromDurationUs(header.frameDurationUs, sampleRateHz);
    return computeDtsHdRepetitionPeriod(sampleCount, sampleRateHz);
  }

  private static int deriveDtsHdRepetitionPeriodFromUhdHeader(
      byte[] accessUnit, int inputSampleRateHz) throws ParserException {
    int headerSize = DtsUtil.parseDtsUhdHeaderSize(accessUnit);
    DtsUtil.DtsHeader header =
        DtsUtil.parseDtsUhdHeader(Arrays.copyOf(accessUnit, headerSize), new AtomicInteger());
    int sampleRateHz = header.sampleRate != Format.NO_VALUE ? header.sampleRate : inputSampleRateHz;
    int sampleCount = deriveSampleCountFromDurationUs(header.frameDurationUs, sampleRateHz);
    return computeDtsHdRepetitionPeriod(sampleCount, sampleRateHz);
  }

  private static int deriveSampleCountFromDurationUs(long frameDurationUs, int sampleRateHz) {
    if (frameDurationUs == C.TIME_UNSET || sampleRateHz <= 0) {
      throw new IllegalArgumentException("Unable to derive DTS sample count");
    }
    return (int) ((frameDurationUs * sampleRateHz) / 1_000_000L);
  }

  private static int computeDtsHdRepetitionPeriod(int sampleCount, int inputSampleRateHz) {
    int period = 192_000 * sampleCount / inputSampleRateHz;
    switch (period) {
      case 512:
      case 1_024:
      case 2_048:
      case 4_096:
      case 8_192:
      case 16_384:
        return period;
      default:
        throw new IllegalArgumentException(
            "Unsupported DTS-HD repetition period: " + period + " sampleCount=" + sampleCount);
    }
  }

  private static int getKodiDtsHdSubtype(int repetitionPeriodFrames) {
    switch (repetitionPeriodFrames) {
      case 512:
        return 0;
      case 1_024:
        return 1;
      case 2_048:
        return 2;
      case 4_096:
        return 3;
      case 8_192:
        return 4;
      case 16_384:
        return 5;
      default:
        throw new IllegalArgumentException(
            "Unsupported DTS-HD repetition period: " + repetitionPeriodFrames);
    }
  }

  private static int alignTo16(int value) {
    return (value + 15) & ~15;
  }

  private static byte[] packKodiPause(
      int millis, int framesize, int samplerate, int repPeriod, int encodedRate) {
    int periodInBytes = repPeriod * framesize;
    double periodInTime = (double) repPeriod / samplerate * 1000;
    int periodsNeeded = (int) (millis / periodInTime);
    int maxPeriods = 61_440 / periodInBytes;
    if (periodsNeeded > maxPeriods) {
      periodsNeeded = maxPeriods;
    }
    int gap = encodedRate * millis / 1000;
    byte[] output = new byte[periodsNeeded * periodInBytes];
    writePreamble(output, 3, 32);
    output[8] = (byte) (gap & 0xFF);
    output[9] = (byte) ((gap >>> 8) & 0xFF);
    for (int i = 1; i < periodsNeeded; i++) {
      System.arraycopy(output, 0, output, i * periodInBytes, periodInBytes);
    }
    return output;
  }

  private static byte[] createEAc3Syncframe(@Ac3Util.SyncFrameInfo.StreamType int streamType) {
    byte[] frame = new byte[2_560];
    System.arraycopy(E_AC3_SYNCFRAME_PREFIX, 0, frame, 0, E_AC3_SYNCFRAME_PREFIX.length);
    frame[2] = (byte) ((frame[2] & 0x3F) | (streamType << 6));
    return frame;
  }

  private static byte[] createDtsCoreFrame(int frameSizeBytes, int sampleCount) {
    byte[] frame = new byte[frameSizeBytes];
    frame[0] = 0x7F;
    frame[1] = (byte) 0xFE;
    frame[2] = (byte) 0x80;
    frame[3] = 0x01;
    int nblks = (sampleCount / 32) - 1;
    int frameSizeCode = frameSizeBytes - 1;
    frame[4] = (byte) ((nblks >>> 6) & 0x01);
    frame[5] = (byte) (((nblks & 0x3F) << 2) | ((frameSizeCode >>> 12) & 0x03));
    frame[6] = (byte) ((frameSizeCode >>> 4) & 0xFF);
    frame[7] = (byte) ((frameSizeCode & 0x0F) << 4);
    return frame;
  }

  private static byte[] createTrueHdSyncframe(int frameSizeBytes, int frameTime) {
    byte[] syncframe = new byte[frameSizeBytes];
    System.arraycopy(TRUEHD_SYNCFRAME_HEADER, 0, syncframe, 0, TRUEHD_SYNCFRAME_HEADER.length);
    syncframe[0] = (byte) (0xC0 | ((frameSizeBytes >> 9) & 0x0F));
    syncframe[1] = (byte) ((frameSizeBytes >> 1) & 0xFF);
    syncframe[2] = (byte) ((frameTime >> 8) & 0xFF);
    syncframe[3] = (byte) (frameTime & 0xFF);
    syncframe[8] = 0x00;
    int channelMap = 0x0006;
    syncframe[9] = 0x00;
    syncframe[10] = (byte) ((channelMap >> 8) & 0x1F);
    syncframe[11] = (byte) (channelMap & 0xFF);
    for (int i = TRUEHD_SYNCFRAME_HEADER.length; i < syncframe.length; i++) {
      syncframe[i] = (byte) (i & 0xFF);
    }
    return syncframe;
  }

  private static void writePreamble(byte[] target, int dataType, int lengthCode) {
    target[0] = 0x72;
    target[1] = (byte) 0xF8;
    target[2] = 0x1F;
    target[3] = 0x4E;
    target[4] = (byte) (dataType & 0xFF);
    target[5] = (byte) ((dataType >>> 8) & 0xFF);
    target[6] = (byte) (lengthCode & 0xFF);
    target[7] = (byte) ((lengthCode >>> 8) & 0xFF);
  }

  private static void writeMaybeWordSwapped(
      byte[] target, int targetOffset, byte[] source, int sourceLength, boolean swapWords) {
    int evenLength = sourceLength + (sourceLength & 1);
    for (int i = 0; i < evenLength; i += 2) {
      byte first = i < sourceLength ? source[i] : 0;
      byte second = i + 1 < sourceLength ? source[i + 1] : 0;
      if (swapWords) {
        target[targetOffset + i] = second;
        target[targetOffset + i + 1] = first;
      } else {
        target[targetOffset + i] = first;
        target[targetOffset + i + 1] = second;
      }
    }
  }

  private static final class KodiReferenceMatPacker {

    private final Deque<byte[]> outputFrames = new ArrayDeque<>();
    private boolean initialized;
    private int rateBits;
    private int previousFrameTime;
    private boolean hasPreviousFrameTime;
    private int matFrameSize;
    private int previousMatFrameSize;
    private int paddingBytes;
    private int bufferCount;
    private byte[] buffer = new byte[IEC61937_TRUEHD_PACKET_BYTES];

    public byte[] pack(List<byte[]> syncframes) {
      List<byte[]> packets = new ArrayList<>();
      for (byte[] syncframe : syncframes) {
        if (!packTrueHd(syncframe)) {
          continue;
        }
        while (!outputFrames.isEmpty()) {
          packets.add(packKodiTrueHd(outputFrames.removeFirst()));
        }
      }
      return concat(packets);
    }

    private boolean packTrueHd(byte[] data) {
      if (data.length < 10) {
        return false;
      }
      if (readBigEndianInt(data, 4) == TRUEHD_FORMAT_MAJOR_SYNC) {
        rateBits = (data[8] >>> 4) & 0x0F;
      } else if (!hasPreviousFrameTime) {
        return false;
      }

      int frameTime = readUnsignedShort(data, 2);
      int spaceSize = 0;
      if (hasPreviousFrameTime) {
        int delta = (frameTime - previousFrameTime) & 0xFFFF;
        spaceSize = delta * (64 >> (rateBits & 0x7));
      }
      if (spaceSize < previousMatFrameSize) {
        int alignment = 64 >> (rateBits & 0x7);
        spaceSize = alignUp(previousMatFrameSize, alignment);
      }
      paddingBytes += spaceSize - previousMatFrameSize;
      if (paddingBytes > IEC61937_TRUEHD_PACKET_BYTES * 5) {
        reset();
        return false;
      }

      previousFrameTime = frameTime;
      hasPreviousFrameTime = true;

      if (bufferCount == 0) {
        writeHeader();
        if (!initialized) {
          initialized = true;
          matFrameSize = 0;
        }
      }

      while (paddingBytes > 0) {
        writePadding();
        if (bufferCount == IEC61937_TRUEHD_PACKET_BYTES) {
          flushPacket();
          writeHeader();
        }
      }

      int remaining = fillDataBuffer(data, 0, data.length, true);
      if (remaining != 0 || bufferCount == IEC61937_TRUEHD_PACKET_BYTES) {
        flushPacket();
        if (remaining != 0) {
          int consumed = data.length - remaining;
          writeHeader();
          int secondPassRemaining = fillDataBuffer(data, consumed, remaining, true);
          if (secondPassRemaining != 0) {
            throw new IllegalStateException("MAT second pass overflow");
          }
        }
      }

      previousMatFrameSize = matFrameSize;
      matFrameSize = 0;
      return !outputFrames.isEmpty();
    }

    private void reset() {
      outputFrames.clear();
      initialized = false;
      rateBits = 0;
      previousFrameTime = 0;
      hasPreviousFrameTime = false;
      matFrameSize = 0;
      previousMatFrameSize = 0;
      paddingBytes = 0;
      bufferCount = 0;
      buffer = new byte[IEC61937_TRUEHD_PACKET_BYTES];
    }

    private void writeHeader() {
      buffer = new byte[IEC61937_TRUEHD_PACKET_BYTES];
      System.arraycopy(TRUEHD_MAT_START_CODE, 0, buffer, 8, TRUEHD_MAT_START_CODE.length);
      int size = 8 + TRUEHD_MAT_START_CODE.length;
      bufferCount = size;
      matFrameSize += size;
      if (paddingBytes > 0) {
        if (paddingBytes > size) {
          paddingBytes -= size;
          matFrameSize = 0;
        } else {
          matFrameSize = size - paddingBytes;
          paddingBytes = 0;
        }
      }
    }

    private void writePadding() {
      if (paddingBytes == 0) {
        return;
      }
      int remaining = fillDataBuffer(new byte[0], 0, paddingBytes, false);
      if (remaining >= 0) {
        paddingBytes = remaining;
        matFrameSize = 0;
      } else {
        paddingBytes = 0;
        matFrameSize = -remaining;
      }
    }

    private int fillDataBuffer(byte[] data, int offset, int length, boolean copyData) {
      if (bufferCount >= TRUEHD_MAT_BUFFER_LIMIT) {
        return length;
      }
      int remaining = length;
      if (bufferCount <= TRUEHD_MAT_MIDDLE_POSITION
          && bufferCount + length > TRUEHD_MAT_MIDDLE_POSITION) {
        int bytesBeforeMiddle = TRUEHD_MAT_MIDDLE_POSITION - bufferCount;
        appendData(data, offset, bytesBeforeMiddle, copyData);
        remaining -= bytesBeforeMiddle;
        appendLiteral(TRUEHD_MAT_MIDDLE_CODE);
        if (!copyData) {
          remaining -= TRUEHD_MAT_MIDDLE_CODE.length;
        }
        if (remaining > 0) {
          return fillDataBuffer(data, offset + bytesBeforeMiddle, remaining, copyData);
        }
        return 0;
      }
      if (bufferCount + length >= TRUEHD_MAT_BUFFER_LIMIT) {
        int bytesBeforeEnd = TRUEHD_MAT_BUFFER_LIMIT - bufferCount;
        appendData(data, offset, bytesBeforeEnd, copyData);
        remaining -= bytesBeforeEnd;
        appendLiteral(TRUEHD_MAT_END_CODE);
        if (!copyData) {
          remaining -= TRUEHD_MAT_END_CODE.length;
        }
        return remaining;
      }
      appendData(data, offset, length, copyData);
      return 0;
    }

    private void appendData(byte[] data, int offset, int length, boolean copyData) {
      if (copyData && length > 0) {
        System.arraycopy(data, offset, buffer, bufferCount, length);
      }
      matFrameSize += length;
      bufferCount += length;
    }

    private void appendLiteral(byte[] literal) {
      System.arraycopy(literal, 0, buffer, bufferCount, literal.length);
      bufferCount += literal.length;
      matFrameSize += literal.length;
    }

    private void flushPacket() {
      if (bufferCount == 0) {
        return;
      }
      outputFrames.addLast(Arrays.copyOf(buffer, buffer.length));
      buffer = new byte[IEC61937_TRUEHD_PACKET_BYTES];
      bufferCount = 0;
    }
  }

  private static byte[] packKodiTrueHd(byte[] matFrame) {
    byte[] output = new byte[IEC61937_TRUEHD_PACKET_BYTES];
    writePreamble(output, IEC61937_TRUEHD, matFrame.length - 8);
    writeMaybeWordSwapped(output, 8, Arrays.copyOfRange(matFrame, 8, matFrame.length), matFrame.length - 8, true);
    return output;
  }

  private static int readUnsignedShort(byte[] data, int offset) {
    return ((data[offset] & 0xFF) << 8) | (data[offset + 1] & 0xFF);
  }

  private static int readBigEndianInt(byte[] data, int offset) {
    return ((data[offset] & 0xFF) << 24)
        | ((data[offset + 1] & 0xFF) << 16)
        | ((data[offset + 2] & 0xFF) << 8)
        | (data[offset + 3] & 0xFF);
  }

  private static int alignUp(int value, int alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  private static byte[] hex(String value) {
    int length = value.length();
    byte[] bytes = new byte[length / 2];
    for (int i = 0; i < length; i += 2) {
      bytes[i / 2] =
          (byte) ((Character.digit(value.charAt(i), 16) << 4)
              | Character.digit(value.charAt(i + 1), 16));
    }
    return bytes;
  }

  private static final class ExtractedAudioFixture {
    public final Format format;
    public final List<EncodedSample> samples;

    public ExtractedAudioFixture(Format format, List<EncodedSample> samples) {
      this.format = format;
      this.samples = samples;
    }
  }

  private static final class EncodedSample {
    public final byte[] data;
    public final long timeUs;

    public EncodedSample(byte[] data, long timeUs) {
      this.data = data;
      this.timeUs = timeUs;
    }
  }
}
