package androidx.media3.exoplayer.audio;

import java.util.ArrayDeque;

/**
 * 1:1 Java port of Kodi's CAEBitstreamPacker (AEBitstreamPacker.cpp).
 */
/* package */ final class AeBitstreamPacker {

  private static final int EAC3_MAX_BURST_PAYLOAD_SIZE = 24576 - AePackIec61937.IEC61937_DATA_OFFSET; // EAC3_FRAME_SIZE * 4 - header

  private final ArrayDeque<byte[]> outputQueue;
  private final byte[] packedBuffer;
  
  private byte[] eac3Buffer;
  private int eac3Size;
  private int eac3FramesCount;
  private int eac3FramesPerBurst;
  
  private byte[] dtsHdBuffer;

  private int dataSize;
  private int pauseDuration;
  
  // Audio format type matching constants
  public static final int STREAM_TYPE_AC3 = 1;
  public static final int STREAM_TYPE_EAC3 = 2;
  public static final int STREAM_TYPE_DTS_512 = 3;
  public static final int STREAM_TYPE_DTS_1024 = 4;
  public static final int STREAM_TYPE_DTS_2048 = 5;
  public static final int STREAM_TYPE_DTSHD = 6;
  public static final int STREAM_TYPE_TRUEHD = 7;

  public AeBitstreamPacker() {
    outputQueue = new ArrayDeque<>();
    packedBuffer = new byte[AePackIec61937.MAX_IEC61937_PACKET];
    eac3Buffer = new byte[EAC3_MAX_BURST_PAYLOAD_SIZE];
    dtsHdBuffer = new byte[0];
    eac3Size = 0;
    eac3FramesCount = 0;
    eac3FramesPerBurst = 6; // Standard 6 blocks for EAC3
    dataSize = 0;
    pauseDuration = 0;
  }

  public void reset() {
    eac3Size = 0;
    eac3FramesCount = 0;
    dataSize = 0;
    pauseDuration = 0;
    outputQueue.clear();
  }

  public void pack(int streamType, byte[] data, int dataOffset, int size, int dtsPeriod, boolean littleEndian) {
    dataSize = 0;
    pauseDuration = 0;

    if (streamType == STREAM_TYPE_TRUEHD) {
      if (size >= AePackIec61937.IEC61937_DATA_OFFSET) {
        dataSize =
            AePackIec61937.packTrueHd(
                data,
                dataOffset + AePackIec61937.IEC61937_DATA_OFFSET,
                size - AePackIec61937.IEC61937_DATA_OFFSET,
                packedBuffer,
                /* littleEndian= */ false);
      }
    } else {
      switch (streamType) {
        case STREAM_TYPE_AC3:
          dataSize = AePackIec61937.packAc3(data, dataOffset, size, packedBuffer, littleEndian);
          break;
        case STREAM_TYPE_DTS_512:
          dataSize = AePackIec61937.packDts512(data, dataOffset, size, packedBuffer, littleEndian);
          break;
        case STREAM_TYPE_DTS_1024:
          dataSize = AePackIec61937.packDts1024(data, dataOffset, size, packedBuffer, littleEndian);
          break;
        case STREAM_TYPE_DTS_2048:
          dataSize = AePackIec61937.packDts2048(data, dataOffset, size, packedBuffer, littleEndian);
          break;
        case STREAM_TYPE_DTSHD:
          packDtsHd(data, dataOffset, size, dtsPeriod, littleEndian);
          break;
        case STREAM_TYPE_EAC3:
          packEac3(data, dataOffset, size, littleEndian);
          break;
      }
      
    }

    if (dataSize > 0) {
      byte[] output = new byte[dataSize];
      System.arraycopy(packedBuffer, 0, output, 0, dataSize);
      outputQueue.addLast(output);
    }
  }

  private void packDtsHd(byte[] data, int dataOffset, int size, int dtsPeriod, boolean littleEndian) {
    if (dtsHdBuffer.length < size + 12) {
      dtsHdBuffer = new byte[size + 12];
    }
    dtsHdBuffer[0] = 0x01; dtsHdBuffer[1] = 0x00; dtsHdBuffer[2] = 0x00; dtsHdBuffer[3] = 0x00;
    dtsHdBuffer[4] = 0x00; dtsHdBuffer[5] = 0x00; dtsHdBuffer[6] = 0x00; dtsHdBuffer[7] = 0x00;
    dtsHdBuffer[8] = (byte) 0xfe; dtsHdBuffer[9] = (byte) 0xfe;
    dtsHdBuffer[10] = (byte) (size >> 8); dtsHdBuffer[11] = (byte) (size & 0xff);
    System.arraycopy(data, dataOffset, dtsHdBuffer, 12, size);
    dataSize = AePackIec61937.packDtsHd(dtsHdBuffer, 0, size + 12, packedBuffer, dtsPeriod, littleEndian);
  }

  private void packEac3(byte[] data, int dataOffset, int size, boolean littleEndian) {
    if (size >= 5) {
      int blocks = (data[dataOffset + 4] & 0xC0) >> 6;
      int frames = blocks == 0x03 ? 6 : blocks == 0x02 ? 3 : blocks == 0x01 ? 2 : 1;
      int newFramesPerBurst = 6 / frames;
      
      if (eac3FramesPerBurst != 0 && eac3FramesPerBurst != newFramesPerBurst) {
        // switched streams, discard partial burst
        eac3Size = 0;
      }
      eac3FramesPerBurst = newFramesPerBurst;
    }
    if (eac3FramesPerBurst == 1) {
      dataSize = AePackIec61937.packEac3(data, dataOffset, size, packedBuffer, littleEndian);
    } else {
      int newsize = eac3Size + size;
      boolean overrun = newsize > EAC3_MAX_BURST_PAYLOAD_SIZE;

      if (!overrun) {
        System.arraycopy(data, dataOffset, eac3Buffer, eac3Size, size);
        eac3Size = newsize;
        eac3FramesCount++;
      }

      if (eac3FramesCount >= eac3FramesPerBurst || overrun) {
        dataSize = AePackIec61937.packEac3(eac3Buffer, 0, eac3Size, packedBuffer, littleEndian);
        eac3Size = 0;
        eac3FramesCount = 0;
      } else {
        dataSize = 0;
      }
    }
  }

  public boolean packPause(int streamType, int millis, int channelsCount, int outputRateHz, int inputSampleRateHz) {
    if (pauseDuration == millis) {
      return false; // Skip redundant
    }

    int repPeriod = 0;
    int frameSize = channelsCount * 2;

    switch (streamType) {
      case STREAM_TYPE_TRUEHD:
      case STREAM_TYPE_EAC3:
        dataSize = AePackIec61937.packPause(packedBuffer, millis, frameSize, outputRateHz, 4, inputSampleRateHz);
        break;
      case STREAM_TYPE_AC3:
      case STREAM_TYPE_DTS_512:
      case STREAM_TYPE_DTS_1024:
      case STREAM_TYPE_DTS_2048:
      case STREAM_TYPE_DTSHD:
        dataSize = AePackIec61937.packPause(packedBuffer, millis, frameSize, outputRateHz, 3, inputSampleRateHz);
        break;
    }

    if (dataSize > 0) {
      byte[] output = new byte[dataSize];
      System.arraycopy(packedBuffer, 0, output, 0, dataSize);
      outputQueue.addLast(output);
      pauseDuration = millis;
      return true;
    }
    return false;
  }
  
  public byte[] getOutputFrame() {
      return outputQueue.pollFirst();
  }
  
  public void clearOutputFrames() {
      outputQueue.clear();
  }
}
