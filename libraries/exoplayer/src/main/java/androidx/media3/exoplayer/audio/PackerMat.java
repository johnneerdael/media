package androidx.media3.exoplayer.audio;

import java.util.ArrayDeque;
import java.util.Arrays;

/**
 * 1:1 Java port of Kodi's CPackerMAT (PackerMAT.cpp).
 */
/* package */ final class PackerMat {

  private static final int FORMAT_MAJOR_SYNC = 0xF8726FBA;
  private static final int BURST_HEADER_SIZE = 8;
  private static final int MAT_BUFFER_SIZE = 61440;
  private static final int MAT_BUFFER_LIMIT = MAT_BUFFER_SIZE - 24;
  private static final int MAT_POS_MIDDLE = 30708 + BURST_HEADER_SIZE;

  private static final byte[] MAT_START_CODE = new byte[] {
      0x07, (byte)0x9E, 0x00, 0x03, (byte)0x84, 0x01, 0x01, 0x01,
      (byte)0x80, 0x00, 0x56, (byte)0xA5, 0x3B, (byte)0xF4, (byte)0x81, (byte)0x83,
      0x49, (byte)0x80, 0x77, (byte)0xE0
  };

  private static final byte[] MAT_MIDDLE_CODE = new byte[] {
      (byte)0xC3, (byte)0xC1, 0x42, 0x49, 0x3B, (byte)0xFA, (byte)0x82, (byte)0x83,
      0x49, (byte)0x80, 0x77, (byte)0xE0
  };

  private static final byte[] MAT_END_CODE = new byte[] {
      (byte)0xC3, (byte)0xC2, (byte)0xC0, (byte)0xC4, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, (byte)0x97, 0x11,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };

  private static final class MatState {
    public boolean init = false;
    public int ratebits = 0;
    public int prevFrametime = 0;
    public boolean prevFrametimeValid = false;
    public int matFramesize = 0;
    public int prevMatFramesize = 0;
    public int padding = 0;
  }

  private MatState state = new MatState();
  private int bufferCount = 0;
  private byte[] buffer = new byte[MAT_BUFFER_SIZE];
  private final ArrayDeque<byte[]> outputQueue = new ArrayDeque<>();

  public PackerMat() {
  }

  public void reset() {
    state = new MatState();
    state.init = true;
    bufferCount = 0;
    outputQueue.clear();
  }

  private static int readBigEndianInt(byte[] data, int offset) {
    return ((data[offset] & 0xFF) << 24) |
           ((data[offset + 1] & 0xFF) << 16) |
           ((data[offset + 2] & 0xFF) << 8) |
           (data[offset + 3] & 0xFF);
  }

  private static int readBigEndianShort(byte[] data, int offset) {
    return ((data[offset] & 0xFF) << 8) | (data[offset + 1] & 0xFF);
  }

  private static int align(int size, int alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
  }

  public boolean packTrueHd(byte[] data, int offset, int size) {
    if (size < 10) {
      return false;
    }

    if (readBigEndianInt(data, offset + 4) == FORMAT_MAJOR_SYNC) {
      state.ratebits = (data[offset + 8] & 0xFF) >> 4;
    } else if (!state.prevFrametimeValid) {
      return false;
    }

    int frameTime = readBigEndianShort(data, offset + 2) & 0xFFFF;
    int spaceSize = 0;

    if (state.prevFrametimeValid) {
      spaceSize = ((frameTime - state.prevFrametime) & 0xFFFF) * (64 >> (state.ratebits & 7));
    }

    if (spaceSize < state.prevMatFramesize) {
      spaceSize = align(state.prevMatFramesize, (64 >> (state.ratebits & 7)));
    }

    state.padding += (spaceSize - state.prevMatFramesize);

    if (state.padding > MAT_BUFFER_SIZE * 5) {
      state = new MatState();
      state.init = true;
      bufferCount = 0;
      outputQueue.clear();
      return false;
    }

    state.prevFrametime = frameTime;
    state.prevFrametimeValid = true;

    if (bufferCount == 0) {
      writeHeader();
      if (!state.init) {
        state.init = true;
        state.matFramesize = 0;
      }
    }

    while (state.padding > 0) {
      writePadding();
      if (state.padding == 0 && bufferCount != MAT_BUFFER_SIZE) {
        break;
      }
      if (bufferCount == MAT_BUFFER_SIZE) {
        flushPacket();
        writeHeader();
      }
    }

    int remaining = fillDataBuffer(data, offset, size, false);
    if (remaining != 0 || bufferCount == MAT_BUFFER_SIZE) {
      flushPacket();
      if (remaining > 0) {
        writeHeader();
        int written = size - remaining;
        remaining = fillDataBuffer(data, offset + written, remaining, false);
        if (remaining != 0) {
            throw new IllegalStateException("Remaining TrueHD data after MAT flush");
        }
      }
    }

    state.prevMatFramesize = state.matFramesize;
    state.matFramesize = 0;

    return !outputQueue.isEmpty();
  }

  public byte[] getOutputFrame() {
    return outputQueue.pollFirst();
  }

  private void writeHeader() {
    Arrays.fill(buffer, (byte)0);
    System.arraycopy(MAT_START_CODE, 0, buffer, BURST_HEADER_SIZE, MAT_START_CODE.length);
    int size = BURST_HEADER_SIZE + MAT_START_CODE.length;
    bufferCount = size;
    state.matFramesize += size;

    if (state.padding > 0) {
      if (state.padding > size) {
        state.padding -= size;
        state.matFramesize = 0;
      } else {
        state.matFramesize = size - state.padding;
        state.padding = 0;
      }
    }
  }

  private void writePadding() {
    if (state.padding == 0) {
      return;
    }
    int remaining = fillDataBuffer(null, 0, state.padding, true);
    if (remaining >= 0) {
      state.padding = remaining;
      state.matFramesize = 0;
    } else {
      state.padding = 0;
      state.matFramesize = -remaining;
    }
  }

  private void appendData(byte[] data, int offset, int size, boolean isPadding) {
    if (size <= 0) {
      return;
    }
    if (!isPadding) {
      System.arraycopy(data, offset, buffer, bufferCount, size);
    } else {
      // It's padding, buffer is already zeroed
    }
    bufferCount += size;
    state.matFramesize += size;
  }

  private int fillDataBuffer(byte[] data, int offset, int size, boolean isPadding) {
    if (bufferCount >= MAT_BUFFER_LIMIT) {
      return size;
    }
    int remaining = size;
    if (bufferCount <= MAT_POS_MIDDLE && bufferCount + size > MAT_POS_MIDDLE) {
      int size1 = MAT_POS_MIDDLE - bufferCount;
      appendData(data, offset, size1, isPadding);
      remaining -= size1;
      appendData(MAT_MIDDLE_CODE, 0, MAT_MIDDLE_CODE.length, false);
      if (isPadding) {
        remaining -= MAT_MIDDLE_CODE.length;
      }
      if (remaining > 0) {
        remaining = fillDataBuffer(data, isPadding ? offset : offset + size1, remaining, isPadding);
      }
      return remaining;
    }

    if (bufferCount + size >= MAT_BUFFER_LIMIT) {
      int size1 = MAT_BUFFER_LIMIT - bufferCount;
      appendData(data, offset, size1, isPadding);
      remaining -= size1;
      appendData(MAT_END_CODE, 0, MAT_END_CODE.length, false);
      if (isPadding) {
        remaining -= MAT_END_CODE.length;
      }
      return remaining;
    }

    appendData(data, offset, size, isPadding);
    return 0;
  }

  private void flushPacket() {
    if (bufferCount == 0) {
      return;
    }
    if (bufferCount != MAT_BUFFER_SIZE) {
        throw new IllegalStateException("Incomplete MAT packet");
    }
    byte[] outputFrame = new byte[MAT_BUFFER_SIZE];
    System.arraycopy(buffer, 0, outputFrame, 0, MAT_BUFFER_SIZE);
    outputQueue.addLast(outputFrame);
    bufferCount = 0;
    Arrays.fill(buffer, (byte)0);
  }
}
