package androidx.media3.exoplayer.audio;

import java.util.Arrays;

/**
 * 1:1 Java port of Kodi's CAEPackIEC61937 (AEPackIEC61937.cpp).
 * Handles stateless IEC61937 packing math and bitstream formatting.
 */
/* package */ final class AePackIec61937 {

  public static final int IEC61937_PREAMBLE1 = 0xF872;
  public static final int IEC61937_PREAMBLE2 = 0x4E1F;
  public static final int IEC61937_DATA_OFFSET = 8;
  public static final int MAX_IEC61937_PACKET = 65536;

  public static final int IEC61937_TYPE_AC3 = 0x01;
  public static final int IEC61937_TYPE_EAC3 = 0x15;
  public static final int IEC61937_TYPE_DTS1 = 0x0B;
  public static final int IEC61937_TYPE_DTS2 = 0x0C;
  public static final int IEC61937_TYPE_DTS3 = 0x0D;
  public static final int IEC61937_TYPE_DTSHD = 0x11;
  public static final int IEC61937_TYPE_TRUEHD = 0x16;

  public static final int AC3_FRAME_SIZE_BYTES = 6144;
  public static final int EAC3_FRAME_SIZE_BYTES = 24576;
  public static final int DTS1_FRAME_SIZE_BYTES = 2048;
  public static final int DTS2_FRAME_SIZE_BYTES = 4096;
  public static final int DTS3_FRAME_SIZE_BYTES = 8192;
  public static final int TRUEHD_FRAME_SIZE_BYTES = 61440;

  private AePackIec61937() {}

  public static void swapEndian(byte[] dest, int destOffset, byte[] src, int srcOffset, int sizeBytes) {
    int pairs = sizeBytes & ~1;
    for (int i = 0; i < pairs; i += 2) {
      dest[destOffset + i] = src[srcOffset + i + 1];
      dest[destOffset + i + 1] = src[srcOffset + i];
    }
    if ((sizeBytes & 1) != 0) {
      dest[destOffset + pairs] = 0;
      dest[destOffset + pairs + 1] = src[srcOffset + sizeBytes - 1];
    }
  }

  private static void writePreamble(byte[] dest, int type, int length) {
    dest[0] = (byte) (IEC61937_PREAMBLE1 & 0xFF);
    dest[1] = (byte) ((IEC61937_PREAMBLE1 >>> 8) & 0xFF);
    dest[2] = (byte) (IEC61937_PREAMBLE2 & 0xFF);
    dest[3] = (byte) ((IEC61937_PREAMBLE2 >>> 8) & 0xFF);
    dest[4] = (byte) (type & 0xFF);
    dest[5] = (byte) ((type >>> 8) & 0xFF);
    dest[6] = (byte) (length & 0xFF);
    dest[7] = (byte) ((length >>> 8) & 0xFF);
  }

  public static int packAc3(byte[] data, int dataOffset, int size, byte[] dest, boolean littleEndian) {
    if (size > AC3_FRAME_SIZE_BYTES) {
        throw new IllegalArgumentException();
    }
    int bitstreamMode = data[dataOffset + 5] & 0x7;
    int type = IEC61937_TYPE_AC3 | (bitstreamMode << 8);
    int length = size << 3;
    writePreamble(dest, type, length);

    if (littleEndian) {
      System.arraycopy(data, dataOffset, dest, IEC61937_DATA_OFFSET, size);
    } else {
      swapEndian(dest, IEC61937_DATA_OFFSET, data, dataOffset, size);
    }

    Arrays.fill(dest, IEC61937_DATA_OFFSET + size + (littleEndian ? 0 : (size & 1)), AC3_FRAME_SIZE_BYTES, (byte) 0);
    return AC3_FRAME_SIZE_BYTES;
  }

  public static int packEac3(byte[] data, int dataOffset, int size, byte[] dest, boolean littleEndian) {
    if (size > EAC3_FRAME_SIZE_BYTES) {
        throw new IllegalArgumentException();
    }
    writePreamble(dest, IEC61937_TYPE_EAC3, size);

    if (littleEndian) {
      System.arraycopy(data, dataOffset, dest, IEC61937_DATA_OFFSET, size);
    } else {
      swapEndian(dest, IEC61937_DATA_OFFSET, data, dataOffset, size);
    }

    Arrays.fill(dest, IEC61937_DATA_OFFSET + size + (littleEndian ? 0 : (size & 1)), EAC3_FRAME_SIZE_BYTES, (byte) 0);
    return EAC3_FRAME_SIZE_BYTES;
  }

  public static int packDts512(byte[] data, int dataOffset, int size, byte[] dest, boolean littleEndian) {
    return packDts(data, dataOffset, size, dest, littleEndian, DTS1_FRAME_SIZE_BYTES, IEC61937_TYPE_DTS1);
  }

  public static int packDts1024(byte[] data, int dataOffset, int size, byte[] dest, boolean littleEndian) {
    return packDts(data, dataOffset, size, dest, littleEndian, DTS2_FRAME_SIZE_BYTES, IEC61937_TYPE_DTS2);
  }

  public static int packDts2048(byte[] data, int dataOffset, int size, byte[] dest, boolean littleEndian) {
    return packDts(data, dataOffset, size, dest, littleEndian, DTS3_FRAME_SIZE_BYTES, IEC61937_TYPE_DTS3);
  }

  public static int packTrueHd(byte[] data, int dataOffset, int size, byte[] dest, boolean littleEndian) {
    if (size == 0) {
      return TRUEHD_FRAME_SIZE_BYTES;
    }
    if (size > TRUEHD_FRAME_SIZE_BYTES) {
        throw new IllegalArgumentException();
    }
    writePreamble(dest, IEC61937_TYPE_TRUEHD, 61424);

    if (littleEndian) {
      System.arraycopy(data, dataOffset, dest, IEC61937_DATA_OFFSET, size);
    } else {
      swapEndian(dest, IEC61937_DATA_OFFSET, data, dataOffset, size);
    }

    Arrays.fill(dest, IEC61937_DATA_OFFSET + size + (littleEndian ? 0 : (size & 1)), TRUEHD_FRAME_SIZE_BYTES, (byte) 0);
    return TRUEHD_FRAME_SIZE_BYTES;
  }

  public static int packDtsHd(byte[] data, int dataOffset, int size, byte[] dest, int period, boolean littleEndian) {
    int subtype;
    switch (period) {
      case 512: subtype = 0; break;
      case 1024: subtype = 1; break;
      case 2048: subtype = 2; break;
      case 4096: subtype = 3; break;
      case 8192: subtype = 4; break;
      case 16384: subtype = 5; break;
      default: return 0;
    }

    int type = IEC61937_TYPE_DTSHD | (subtype << 8);
    // Align so that (length_code & 0xf) == 0x8.
    int length = ((size + 0x17) & ~0x0f) - 0x08;
    writePreamble(dest, type, length);

    if (littleEndian) {
      System.arraycopy(data, dataOffset, dest, IEC61937_DATA_OFFSET, size);
    } else {
      swapEndian(dest, IEC61937_DATA_OFFSET, data, dataOffset, size);
    }

    int burstSize = period << 2;
    Arrays.fill(dest, IEC61937_DATA_OFFSET + size + (littleEndian ? 0 : (size & 1)), burstSize, (byte) 0);
    return burstSize;
  }

  private static int packDts(byte[] data, int dataOffset, int size, byte[] dest, boolean littleEndian, int frameSize, int type) {
    if (size > frameSize) {
        throw new IllegalArgumentException();
    }
    boolean byteSwapNeeded = !littleEndian; // Assuming target is LE
    int dataToOffset;

    if (size == frameSize) {
      dataToOffset = 0;
    } else if (size <= frameSize - IEC61937_DATA_OFFSET) {
      writePreamble(dest, type, size << 3);
      dataToOffset = IEC61937_DATA_OFFSET;
    } else {
      return 0;
    }

    if (!byteSwapNeeded) {
      System.arraycopy(data, dataOffset, dest, dataToOffset, size);
    } else {
      swapEndian(dest, dataToOffset, data, dataOffset, size);
    }

    if (size != frameSize) {
      Arrays.fill(dest, dataToOffset + size + (!byteSwapNeeded ? 0 : (size & 1)), frameSize, (byte) 0);
    }

    return frameSize;
  }

  public static int packPause(byte[] dest, int millis, int frameSize, int sampleRate, int repPeriod, int encodedRate) {
    int periodInBytes = repPeriod * frameSize;
    double periodInTime = (double) repPeriod / sampleRate * 1000;
    int periodsNeeded = (int) (millis / periodInTime);
    int maxPeriods = MAX_IEC61937_PACKET / periodInBytes;
    if (periodsNeeded > maxPeriods) {
      periodsNeeded = maxPeriods;
    }
    int gap = encodedRate * millis / 1000;

    writePreamble(dest, 3, 32);
    Arrays.fill(dest, IEC61937_DATA_OFFSET, periodInBytes, (byte) 0);

    for (int i = 1; i < periodsNeeded; i++) {
      System.arraycopy(dest, 0, dest, i * periodInBytes, periodInBytes);
    }

    dest[IEC61937_DATA_OFFSET] = (byte) (gap & 0xFF);
    dest[IEC61937_DATA_OFFSET + 1] = (byte) ((gap >>> 8) & 0xFF);

    return periodsNeeded * periodInBytes;
  }
}
