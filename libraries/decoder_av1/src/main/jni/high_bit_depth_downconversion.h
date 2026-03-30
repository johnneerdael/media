#ifndef ANDROIDX_MEDIA3_DECODER_AV1_HIGH_BIT_DEPTH_DOWNCONVERSION_H_
#define ANDROIDX_MEDIA3_DECODER_AV1_HIGH_BIT_DEPTH_DOWNCONVERSION_H_

#include <algorithm>
#include <cstdint>

#ifdef __ARM_NEON__
#include <arm_neon.h>
#endif

namespace dav1d_jni {

inline uint8_t DownconvertHighBitDepthSampleTo8Bit(uint16_t sample,
                                                   int bit_depth) {
  const int shift = bit_depth - 8;
  const int rounding_offset = 1 << (shift - 1);
  const int converted =
      (static_cast<int>(sample) + rounding_offset) >> shift;
  return static_cast<uint8_t>(std::min(converted, 0xFF));
}

inline void DownconvertHighBitDepthRowTo8BitScalar(const uint16_t* source_row,
                                                   uint8_t* destination_row,
                                                   int width,
                                                   int bit_depth) {
  for (int x = 0; x < width; ++x) {
    destination_row[x] =
        DownconvertHighBitDepthSampleTo8Bit(source_row[x], bit_depth);
  }
}

#ifdef __ARM_NEON__
inline void Downconvert10BitRowTo8BitNeon(const uint16_t* source_row,
                                          uint8_t* destination_row,
                                          int width) {
  int x = 0;
  for (; x <= width - 16; x += 16) {
    vst1q_u8(destination_row + x,
             vcombine_u8(vrshrn_n_u16(vld1q_u16(source_row + x), 2),
                         vrshrn_n_u16(vld1q_u16(source_row + x + 8), 2)));
  }
  for (; x <= width - 8; x += 8) {
    vst1_u8(destination_row + x,
            vrshrn_n_u16(vld1q_u16(source_row + x), 2));
  }
  DownconvertHighBitDepthRowTo8BitScalar(source_row + x, destination_row + x,
                                         width - x, 10);
}

inline void Downconvert12BitRowTo8BitNeon(const uint16_t* source_row,
                                          uint8_t* destination_row,
                                          int width) {
  int x = 0;
  for (; x <= width - 16; x += 16) {
    vst1q_u8(destination_row + x,
             vcombine_u8(vrshrn_n_u16(vld1q_u16(source_row + x), 4),
                         vrshrn_n_u16(vld1q_u16(source_row + x + 8), 4)));
  }
  for (; x <= width - 8; x += 8) {
    vst1_u8(destination_row + x,
            vrshrn_n_u16(vld1q_u16(source_row + x), 4));
  }
  DownconvertHighBitDepthRowTo8BitScalar(source_row + x, destination_row + x,
                                         width - x, 12);
}
#endif

inline void DownconvertHighBitDepthPlaneTo8Bit(const uint8_t* source,
                                               int source_stride_bytes,
                                               uint8_t* destination,
                                               int destination_stride_bytes,
                                               int width,
                                               int height,
                                               int bit_depth) {
  for (int y = 0; y < height; ++y) {
    const uint16_t* source_row = reinterpret_cast<const uint16_t*>(
        source + y * source_stride_bytes);
    uint8_t* destination_row = destination + y * destination_stride_bytes;
#ifdef __ARM_NEON__
    if (bit_depth == 10) {
      Downconvert10BitRowTo8BitNeon(source_row, destination_row, width);
      continue;
    }
    if (bit_depth == 12) {
      Downconvert12BitRowTo8BitNeon(source_row, destination_row, width);
      continue;
    }
#endif
    DownconvertHighBitDepthRowTo8BitScalar(source_row, destination_row, width,
                                           bit_depth);
  }
}

}  // namespace dav1d_jni

#endif  // ANDROIDX_MEDIA3_DECODER_AV1_HIGH_BIT_DEPTH_DOWNCONVERSION_H_
