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
package androidx.media3.exoplayer.audio;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.assertThrows;

import android.media.AudioFormat;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.ParsableBitArray;
import androidx.media3.common.util.Util;
import androidx.media3.extractor.Ac3Util;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.nio.ByteBuffer;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowBuild;

/** Unit tests for {@link FireOsIec61937AudioOutputProvider}. */
@RunWith(AndroidJUnit4.class)
@Config(sdk = 33)
public class FireOsIec61937AudioOutputProviderTest {

  private static final AudioOutputProvider.FormatSupport DIRECT_SUPPORT =
      new AudioOutputProvider.FormatSupport.Builder()
          .setFormatSupportLevel(AudioOutputProvider.FORMAT_SUPPORTED_DIRECTLY)
          .build();
  private static final byte[] TRUEHD_SYNCFRAME_HEADER =
      Util.getBytesFromHexString("C07504D8F8726FBA0097C00FB7520000");
  private static final byte[] E_AC3_SYNCFRAME_PREFIX =
      Util.getBytesFromHexString("0B7704FF3F86FBC41011007A01CA77B9");

  @Before
  public void setUp() {
    AudioCapabilities.setExperimentalFireOsIecPassthroughEnabled(true);
    AudioCapabilities.setFireOsCompatibilityFallbackEnabled(false);
    ShadowBuild.setManufacturer("Amazon");
    ShadowBuild.setModel("AFTMM");
  }

  @After
  public void tearDown() {
    AudioCapabilities.setExperimentalFireOsIecPassthroughEnabled(false);
    AudioCapabilities.setFireOsCompatibilityFallbackEnabled(false);
    ShadowBuild.setManufacturer("Google");
    ShadowBuild.setModel("AOSP");
  }

  @Test
  public void getFormatSupport_kodiModeRejectsWrappedRawPassthroughWhenIecProbeFails() {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(C.ENCODING_DTS, 48_000, AudioFormat.CHANNEL_OUT_STEREO));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ false, /* stereo192= */ false, /* multi192= */ false);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_DTS)
                    .setSampleRate(48_000)
                    .setChannelCount(6)
                    .build())
            .build();

    AudioOutputProvider.FormatSupport formatSupport = provider.getFormatSupport(formatConfig);

    assertThat(formatSupport.supportLevel).isEqualTo(AudioOutputProvider.FORMAT_UNSUPPORTED);
    assertThrows(
        AudioOutputProvider.ConfigurationException.class,
        () -> provider.getOutputConfig(formatConfig));
  }

  @Test
  public void getOutputConfig_trueHd44k1UsesKodi192kProbeModel() throws Exception {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(
                C.ENCODING_DOLBY_TRUEHD,
                44_100,
                AudioFormat.CHANNEL_OUT_7POINT1_SURROUND));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ true, /* multi192= */ true);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_TRUEHD)
                    .setSampleRate(44_100)
                    .setChannelCount(8)
                    .build())
            .build();

    AudioOutputProvider.FormatSupport formatSupport = provider.getFormatSupport(formatConfig);
    AudioOutputProvider.OutputConfig outputConfig = provider.getOutputConfig(formatConfig);

    assertThat(formatSupport.supportLevel).isEqualTo(AudioOutputProvider.FORMAT_SUPPORTED_DIRECTLY);
    assertThat(outputConfig.encoding).isEqualTo(C.ENCODING_DOLBY_TRUEHD);
    assertThat(outputConfig.sampleRate).isEqualTo(44_100);
    assertThat(outputConfig.channelMask).isEqualTo(AudioFormat.CHANNEL_OUT_7POINT1_SURROUND);
  }

  @Test
  public void getOutputConfig_trueHd44k1RejectsWithoutBaseIecProbe() {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(
                C.ENCODING_DOLBY_TRUEHD,
                44_100,
                AudioFormat.CHANNEL_OUT_7POINT1_SURROUND));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ false, /* stereo192= */ true, /* multi192= */ true);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_TRUEHD)
                    .setSampleRate(44_100)
                    .setChannelCount(8)
                    .build())
            .build();

    AudioOutputProvider.FormatSupport formatSupport = provider.getFormatSupport(formatConfig);

    assertThat(formatSupport.supportLevel).isEqualTo(AudioOutputProvider.FORMAT_UNSUPPORTED);
    assertThrows(
        AudioOutputProvider.ConfigurationException.class,
        () -> provider.getOutputConfig(formatConfig));
  }

  @Test
  public void getOutputConfig_trueHdRejectsStereoOnlyHighBitrateCarrier() {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(
                C.ENCODING_DOLBY_TRUEHD,
                48_000,
                AudioFormat.CHANNEL_OUT_7POINT1_SURROUND));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ true, /* multi192= */ false);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_TRUEHD)
                    .setSampleRate(48_000)
                    .setChannelCount(8)
                    .build())
            .build();

    assertThrows(
        AudioOutputProvider.ConfigurationException.class,
        () -> provider.getOutputConfig(formatConfig));
  }

  @Test
  public void supportsCarrierForKodiProbeModel_maps176k4To192kProbe() {
    assertThat(
            FireOsIec61937AudioOutputProvider.supportsCarrierForKodiProbeModel(
                176_400,
                AudioFormat.CHANNEL_OUT_STEREO,
                /* stereo48Supported= */ true,
                /* stereo192Supported= */ true,
                /* multichannel192Supported= */ false))
        .isTrue();
    assertThat(
            FireOsIec61937AudioOutputProvider.supportsCarrierForKodiProbeModel(
                176_400,
                AudioFormat.CHANNEL_OUT_7POINT1_SURROUND,
                /* stereo48Supported= */ true,
                /* stereo192Supported= */ false,
                /* multichannel192Supported= */ true))
        .isTrue();
  }

  @Test
  public void getOutputConfig_dtsHdKodiModeUsesProvisionalClassification() throws Exception {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(C.ENCODING_DTS_HD, 48_000, AudioFormat.CHANNEL_OUT_STEREO));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ false, /* multi192= */ false);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_DTS_HD)
                    .setSampleRate(48_000)
                    .setChannelCount(8)
                    .build())
            .build();

    AudioOutputProvider.OutputConfig outputConfig = provider.getOutputConfig(formatConfig);

    assertThat(outputConfig.encoding).isEqualTo(C.ENCODING_DTS_HD);
    assertThat(outputConfig.sampleRate).isEqualTo(48_000);
    assertThat(outputConfig.channelMask).isEqualTo(AudioFormat.CHANNEL_OUT_STEREO);
  }

  @Test
  public void getFormatSupport_dtsHdKodiModeUsesProvisionalClassification() {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(C.ENCODING_DTS_HD, 48_000, AudioFormat.CHANNEL_OUT_STEREO));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ false, /* multi192= */ false);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_DTS_HD)
                    .setSampleRate(48_000)
                    .setChannelCount(8)
                    .build())
            .build();

    AudioOutputProvider.FormatSupport formatSupport = provider.getFormatSupport(formatConfig);

    assertThat(formatSupport.supportLevel)
        .isEqualTo(AudioOutputProvider.FORMAT_SUPPORTED_DIRECTLY);
  }

  @Test
  public void getOutputConfig_dtsHdMaCodecHintDefaultsToKodiCoreBeforeParsedClassification()
      throws Exception {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(
                C.ENCODING_DTS_HD, 48_000, AudioFormat.CHANNEL_OUT_7POINT1_SURROUND));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ true, /* multi192= */ true);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_DTS_HD)
                    .setCodecs("dtsl")
                    .setSampleRate(48_000)
                    .setChannelCount(8)
                    .build())
            .build();

    AudioOutputProvider.OutputConfig outputConfig = provider.getOutputConfig(formatConfig);

    assertThat(outputConfig.encoding).isEqualTo(C.ENCODING_DTS_HD);
    assertThat(outputConfig.channelMask).isEqualTo(AudioFormat.CHANNEL_OUT_STEREO);
  }

  @Test
  public void getOutputConfig_dtsXKodiModeFallsBackToKodiCoreClassification() throws Exception {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(C.ENCODING_DTS_UHD_P2, 48_000, AudioFormat.CHANNEL_OUT_STEREO));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ false, /* multi192= */ false);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_DTS_X)
                    .setSampleRate(48_000)
                    .setChannelCount(8)
                    .build())
            .build();

    AudioOutputProvider.FormatSupport formatSupport = provider.getFormatSupport(formatConfig);
    AudioOutputProvider.OutputConfig outputConfig = provider.getOutputConfig(formatConfig);

    assertThat(formatSupport.supportLevel)
        .isEqualTo(AudioOutputProvider.FORMAT_SUPPORTED_DIRECTLY);
    assertThat(outputConfig.channelMask).isEqualTo(AudioFormat.CHANNEL_OUT_STEREO);
  }

  @Test
  public void getOutputConfig_dtsHdMaRejectsStereoOnlyHighBitrateCarrier() {
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_DTS_HD)
            .setSampleRate(48_000)
            .setChannelCount(8)
            .build();
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(C.ENCODING_DTS_HD, 48_000, AudioFormat.CHANNEL_OUT_STEREO));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ true, /* multi192= */ false);
    provider.updateStreamInfo(
        format,
        FireOsStreamInfo.createForDts(
            MimeTypes.AUDIO_DTS_HD,
            FireOsStreamInfo.DtsStreamType.DTSHD_MA,
            /* inputSampleRateHz= */ 48_000,
            /* inputChannelCount= */ 8,
            /* dtsPeriodFrames= */ 16_384,
            "test"));

    assertThrows(
        AudioOutputProvider.ConfigurationException.class,
        () ->
            provider.getOutputConfig(
                new AudioOutputProvider.FormatConfig.Builder(format).build()));
  }

  @Test
  public void prepareEncodedPacket_trueHdSplitsRechunkedBufferIntoSyncframes() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    byte[] firstFrame = createTrueHdSyncframe(/* frameSizeBytes= */ 234, /* frameTime= */ 0x04D8);
    byte[] secondFrame = createTrueHdSyncframe(/* frameSizeBytes= */ 234, /* frameTime= */ 0x0510);
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_TRUEHD)
            .setSampleRate(48_000)
            .setChannelCount(8)
            .build();

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(format, ByteBuffer.wrap(join(firstFrame, secondFrame)), 2);

    assertThat(preparedPacket).isNotNull();
    assertThat(preparedPacket.metadata.normalizedAccessUnitCount).isEqualTo(2);
    assertThat(preparedPacket.metadata.totalFrames).isEqualTo(80);
    int[] payloadSizes =
        FireOsIec61937AudioOutputProvider.getNormalizedPayloadSizesForTesting(preparedPacket);
    int[] sampleCounts =
        FireOsIec61937AudioOutputProvider.getNormalizedSampleCountsForTesting(preparedPacket);
    assertThat(payloadSizes).hasLength(2);
    assertThat(payloadSizes[0]).isEqualTo(234);
    assertThat(payloadSizes[1]).isEqualTo(234);
    assertThat(sampleCounts).hasLength(2);
    assertThat(sampleCounts[0]).isEqualTo(40);
    assertThat(sampleCounts[1]).isEqualTo(40);
  }

  @Test
  public void prepareEncodedPacket_trueHdUsesParsedMajorSyncMetadata() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    byte[] syncframe = createTrueHdSyncframe(/* frameSizeBytes= */ 234, /* frameTime= */ 0x04D8);
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_TRUEHD)
            .setSampleRate(176_400)
            .setChannelCount(2)
            .build();

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(format, ByteBuffer.wrap(syncframe), 1);

    assertThat(preparedPacket).isNotNull();
    assertThat(preparedPacket.metadata.streamInfo.inputSampleRateHz).isEqualTo(48_000);
    assertThat(preparedPacket.metadata.streamInfo.inputChannelCount).isEqualTo(6);
    assertThat(preparedPacket.metadata.streamInfo.requireMultichannelIecCarrier).isTrue();
  }

  @Test
  public void prepareEncodedPacket_trueHdSplitsSubframesAfterMajorSyncAcrossBuffers() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_TRUEHD)
            .setSampleRate(48_000)
            .setChannelCount(8)
            .build();

    provider.prepareEncodedPacket(
        format,
        ByteBuffer.wrap(createTrueHdSyncframe(/* frameSizeBytes= */ 234, /* frameTime= */ 0x04D8)),
        1);

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(
            format,
            ByteBuffer.wrap(
                join(
                    createTrueHdSubframe(/* frameSizeBytes= */ 12, /* frameTime= */ 0x0510),
                    createTrueHdSubframe(/* frameSizeBytes= */ 12, /* frameTime= */ 0x0538))),
            2);

    assertThat(preparedPacket).isNotNull();
    assertThat(preparedPacket.metadata.normalizedAccessUnitCount).isEqualTo(2);
    assertThat(preparedPacket.metadata.totalFrames).isEqualTo(80);
    assertThat(FireOsIec61937AudioOutputProvider.getNormalizedPayloadSizesForTesting(preparedPacket))
        .asList()
        .containsExactly(12, 12)
        .inOrder();
    assertThat(FireOsIec61937AudioOutputProvider.getNormalizedSampleCountsForTesting(preparedPacket))
        .asList()
        .containsExactly(40, 40)
        .inOrder();
  }

  @Test
  public void prepareEncodedPacket_trueHdRejectsInvalidMajorSyncCrcMetadata() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    byte[] syncframe = createTrueHdSyncframe(/* frameSizeBytes= */ 234, /* frameTime= */ 0x04D8);
    syncframe[30] ^= 0x01;
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_TRUEHD)
            .setSampleRate(176_400)
            .setChannelCount(2)
            .build();

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(format, ByteBuffer.wrap(syncframe), 1);

    assertThat(preparedPacket).isNull();
  }

  @Test
  public void prepareEncodedPacket_trueHdResynchronizesInsteadOfWholeBufferFallback() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    byte[] syncframe = createTrueHdSyncframe(/* frameSizeBytes= */ 234, /* frameTime= */ 0x04D8);
    byte[] input = new byte[syncframe.length + 1];
    input[0] = 0x55;
    System.arraycopy(syncframe, 0, input, 1, syncframe.length);
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_TRUEHD)
            .setSampleRate(48_000)
            .setChannelCount(8)
            .build();

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(format, ByteBuffer.wrap(input), 1);

    assertThat(preparedPacket).isNotNull();
    assertThat(preparedPacket.metadata.normalizedAccessUnitCount).isEqualTo(1);
    assertThat(FireOsIec61937AudioOutputProvider.getNormalizedPayloadSizesForTesting(preparedPacket))
        .asList()
        .containsExactly(syncframe.length);
  }

  @Test
  public void getOutputConfig_strictKodiModeIgnoresRecoverableFallbackState() throws Exception {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(C.ENCODING_AC3, 48_000, AudioFormat.CHANNEL_OUT_STEREO));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ true, /* multi192= */ true);
    provider.requestFallbackForTesting(
        FireOsIec61937AudioOutputProvider.PackerKind.AC3,
        /* multichannelCarrierFailed= */ false,
        /* allowStereoIecFallback= */ true);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_AC3)
                    .setSampleRate(48_000)
                    .setChannelCount(6)
                    .build())
            .build();

    AudioOutputProvider.OutputConfig outputConfig = provider.getOutputConfig(formatConfig);

    assertThat(outputConfig.encoding).isEqualTo(C.ENCODING_AC3);
    assertThat(outputConfig.sampleRate).isEqualTo(48_000);
    assertThat(outputConfig.channelMask).isEqualTo(AudioFormat.CHANNEL_OUT_STEREO);
  }

  @Test
  public void getFormatSupport_strictKodiModeKeepsAdvertisingIecAfterRecoverableFallback() {
    FakeAudioOutputProvider passthroughProvider =
        new FakeAudioOutputProvider(
            DIRECT_SUPPORT,
            createOutputConfig(C.ENCODING_AC3, 48_000, AudioFormat.CHANNEL_OUT_STEREO));
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(passthroughProvider, new FakeAudioOutputProvider());
    seedKodiProbeMatrix(provider, /* stereo48= */ true, /* stereo192= */ true, /* multi192= */ true);
    provider.requestFallbackForTesting(
        FireOsIec61937AudioOutputProvider.PackerKind.AC3,
        /* multichannelCarrierFailed= */ false,
        /* allowStereoIecFallback= */ true);
    AudioOutputProvider.FormatConfig formatConfig =
        new AudioOutputProvider.FormatConfig.Builder(
                new Format.Builder()
                    .setSampleMimeType(MimeTypes.AUDIO_AC3)
                    .setSampleRate(48_000)
                    .setChannelCount(6)
                    .build())
            .build();

    AudioOutputProvider.FormatSupport formatSupport = provider.getFormatSupport(formatConfig);

    assertThat(formatSupport.supportLevel)
        .isEqualTo(AudioOutputProvider.FORMAT_SUPPORTED_DIRECTLY);
  }

  @Test
  public void prepareEncodedPacket_eAc3MergesImmediateDependentFrame() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    byte[] mainFrame = createEAc3Syncframe(Ac3Util.SyncFrameInfo.STREAM_TYPE_TYPE0);
    byte[] dependentFrame = createEAc3Syncframe(Ac3Util.SyncFrameInfo.STREAM_TYPE_TYPE1);
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_E_AC3)
            .setSampleRate(48_000)
            .setChannelCount(6)
            .build();

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(format, ByteBuffer.wrap(join(mainFrame, dependentFrame)), 2);

    assertThat(preparedPacket).isNotNull();
    assertThat(preparedPacket.metadata.normalizedAccessUnitCount).isEqualTo(1);
    assertThat(preparedPacket.metadata.totalFrames).isEqualTo(1_536);
    int[] payloadSizes =
        FireOsIec61937AudioOutputProvider.getNormalizedPayloadSizesForTesting(preparedPacket);
    int[] sampleCounts =
        FireOsIec61937AudioOutputProvider.getNormalizedSampleCountsForTesting(preparedPacket);
    assertThat(payloadSizes).hasLength(1);
    assertThat(payloadSizes[0]).isEqualTo(mainFrame.length + dependentFrame.length);
    assertThat(sampleCounts).hasLength(1);
    assertThat(sampleCounts[0]).isEqualTo(1_536);
  }

  @Test
  public void prepareEncodedPacket_eAc3UsesParsedSyncframeMetadata() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    byte[] mainFrame = createEAc3Syncframe(Ac3Util.SyncFrameInfo.STREAM_TYPE_TYPE0);
    Ac3Util.SyncFrameInfo syncFrameInfo =
        Ac3Util.parseAc3SyncframeInfo(new ParsableBitArray(mainFrame));
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_E_AC3)
            .setSampleRate(44_100)
            .setChannelCount(2)
            .build();

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(format, ByteBuffer.wrap(mainFrame), 1);

    assertThat(preparedPacket).isNotNull();
    assertThat(preparedPacket.metadata.streamInfo.inputSampleRateHz)
        .isEqualTo(syncFrameInfo.sampleRate);
    assertThat(preparedPacket.metadata.streamInfo.inputChannelCount)
        .isEqualTo(syncFrameInfo.channelCount);
    assertThat(preparedPacket.metadata.streamInfo.inputSampleRateHz).isNotEqualTo(format.sampleRate);
  }

  @Test
  public void prepareEncodedPacket_invalidAc3ReturnsNull() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_AC3)
            .setSampleRate(48_000)
            .setChannelCount(6)
            .build();

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(format, ByteBuffer.wrap(new byte[] {0x00, 0x11, 0x22, 0x33}), 1);

    assertThat(preparedPacket).isNull();
  }

  @Test
  public void prepareEncodedPacket_invalidDtsReturnsNull() {
    FireOsIec61937AudioOutputProvider provider =
        new FireOsIec61937AudioOutputProvider(
            new FakeAudioOutputProvider(), new FakeAudioOutputProvider());
    Format format =
        new Format.Builder()
            .setSampleMimeType(MimeTypes.AUDIO_DTS_HD)
            .setSampleRate(48_000)
            .setChannelCount(6)
            .build();

    FireOsIec61937AudioOutputProvider.PreparedEncodedPacket preparedPacket =
        provider.prepareEncodedPacket(
            format,
            ByteBuffer.wrap(new byte[] {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}),
            1);

    assertThat(preparedPacket).isNull();
  }

  private static void seedKodiProbeMatrix(
      FireOsIec61937AudioOutputProvider provider,
      boolean stereo48,
      boolean stereo192,
      boolean multi192) {
    provider.setIecSupportForTesting(48_000, AudioFormat.CHANNEL_OUT_STEREO, stereo48);
    provider.setIecSupportForTesting(192_000, AudioFormat.CHANNEL_OUT_STEREO, stereo192);
    provider.setIecSupportForTesting(192_000, AudioFormat.CHANNEL_OUT_7POINT1_SURROUND, multi192);
    provider.setEncodedSupportForTesting(
        48_000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_AC3, false);
    provider.setEncodedSupportForTesting(
        48_000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_E_AC3, false);
  }

  private static AudioOutputProvider.OutputConfig createOutputConfig(
      int encoding, int sampleRate, int channelMask) {
    return new AudioOutputProvider.OutputConfig.Builder()
        .setEncoding(encoding)
        .setSampleRate(sampleRate)
        .setChannelMask(channelMask)
        .setBufferSize(32_768)
        .setAudioAttributes(AudioAttributes.DEFAULT)
        .build();
  }

  private static byte[] createTrueHdSyncframe(int frameSizeBytes, int frameTime) {
    byte[] frame = new byte[frameSizeBytes];
    System.arraycopy(TRUEHD_SYNCFRAME_HEADER, 0, frame, 0, TRUEHD_SYNCFRAME_HEADER.length);
    int frameSizeWords = frameSizeBytes / 2;
    frame[0] =
        (byte)
            ((TRUEHD_SYNCFRAME_HEADER[0] & 0xF0) | ((frameSizeWords >>> 8) & 0x0F));
    frame[1] = (byte) (frameSizeWords & 0xFF);
    frame[2] = (byte) ((frameTime >>> 8) & 0xFF);
    frame[3] = (byte) (frameTime & 0xFF);
    FireOsIec61937AudioOutputProvider.writeTrueHdMajorSyncCrcForTesting(frame);
    return frame;
  }

  private static byte[] createTrueHdSubframe(int frameSizeBytes, int frameTime) {
    byte[] frame = new byte[frameSizeBytes];
    int frameSizeWords = frameSizeBytes / 2;
    int lowNibble = (frameSizeWords >>> 8) & 0x0F;
    int byte1 = frameSizeWords & 0xFF;
    frame[1] = (byte) byte1;
    frame[2] = (byte) ((frameTime >>> 8) & 0xFF);
    frame[3] = (byte) (frameTime & 0xFF);
    for (int highNibble = 0; highNibble < 16; highNibble++) {
      int firstByte = (highNibble << 4) | lowNibble;
      int check = firstByte ^ byte1 ^ (frame[2] & 0xFF) ^ (frame[3] & 0xFF);
      if ((((check >> 4) ^ check) & 0x0F) == 0x0F) {
        frame[0] = (byte) firstByte;
        return frame;
      }
    }
    throw new IllegalStateException("Unable to construct a valid TrueHD subframe parity header");
  }

  private static byte[] createEAc3Syncframe(@Ac3Util.SyncFrameInfo.StreamType int streamType) {
    byte[] frame = new byte[2_560];
    System.arraycopy(E_AC3_SYNCFRAME_PREFIX, 0, frame, 0, E_AC3_SYNCFRAME_PREFIX.length);
    frame[2] = (byte) ((frame[2] & 0x3F) | (streamType << 6));
    return frame;
  }

  private static byte[] join(byte[] first, byte[] second) {
    byte[] joined = new byte[first.length + second.length];
    System.arraycopy(first, 0, joined, 0, first.length);
    System.arraycopy(second, 0, joined, first.length, second.length);
    return joined;
  }

  private static final class FakeAudioOutputProvider implements AudioOutputProvider {

    private final AudioOutputProvider.FormatSupport formatSupport;
    private final AudioOutputProvider.OutputConfig outputConfig;

    public FakeAudioOutputProvider() {
      this(AudioOutputProvider.FormatSupport.UNSUPPORTED, createOutputConfig(C.ENCODING_PCM_16BIT, 48_000, AudioFormat.CHANNEL_OUT_STEREO));
    }

    public FakeAudioOutputProvider(
        AudioOutputProvider.FormatSupport formatSupport,
        AudioOutputProvider.OutputConfig outputConfig) {
      this.formatSupport = formatSupport;
      this.outputConfig = outputConfig;
    }

    @Override
    public AudioOutputProvider.FormatSupport getFormatSupport(
        AudioOutputProvider.FormatConfig formatConfig) {
      return formatSupport;
    }

    @Override
    public AudioOutputProvider.OutputConfig getOutputConfig(
        AudioOutputProvider.FormatConfig formatConfig) {
      return outputConfig;
    }

    @Override
    public AudioOutput getAudioOutput(AudioOutputProvider.OutputConfig config)
        throws AudioOutputProvider.InitializationException {
      throw new AudioOutputProvider.InitializationException();
    }

    @Override
    public void addListener(AudioOutputProvider.Listener listener) {}

    @Override
    public void removeListener(AudioOutputProvider.Listener listener) {}

    @Override
    public void release() {}
  }
}
