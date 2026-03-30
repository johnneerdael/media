#include "high_bit_depth_downconversion.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

bool ExpectPlaneEquals(const std::vector<uint8_t>& actual,
                       const std::vector<uint8_t>& expected) {
  if (actual == expected) {
    return true;
  }
  std::cerr << "Expected:";
  for (uint8_t value : expected) {
    std::cerr << ' ' << static_cast<int>(value);
  }
  std::cerr << "\nActual:";
  for (uint8_t value : actual) {
    std::cerr << ' ' << static_cast<int>(value);
  }
  std::cerr << '\n';
  return false;
}

int Test10BitRoundAndClamp() {
  const uint16_t source[] = {
      0, 2, 3, 1023,
      512, 513, 514, 515,
  };
  std::vector<uint8_t> destination(8, 0);
  dav1d_jni::DownconvertHighBitDepthPlaneTo8Bit(
      reinterpret_cast<const uint8_t*>(source),
      /* source_stride_bytes= */ 4 * static_cast<int>(sizeof(uint16_t)),
      destination.data(),
      /* destination_stride_bytes= */ 4,
      /* width= */ 4,
      /* height= */ 2,
      /* bit_depth= */ 10);
  return ExpectPlaneEquals(destination, {0, 1, 1, 255, 128, 128, 129, 129})
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}

int Test12BitStrideHandling() {
  const uint16_t source[] = {
      0, 16, 32, 48, 999,
      64, 80, 96, 112, 999,
  };
  std::vector<uint8_t> destination(8, 0);
  dav1d_jni::DownconvertHighBitDepthPlaneTo8Bit(
      reinterpret_cast<const uint8_t*>(source),
      /* source_stride_bytes= */ 5 * static_cast<int>(sizeof(uint16_t)),
      destination.data(),
      /* destination_stride_bytes= */ 4,
      /* width= */ 4,
      /* height= */ 2,
      /* bit_depth= */ 12);
  return ExpectPlaneEquals(destination, {0, 1, 2, 3, 4, 5, 6, 7}) ? EXIT_SUCCESS
                                                                   : EXIT_FAILURE;
}

}  // namespace

int main() {
  if (Test10BitRoundAndClamp() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  if (Test12BitStrideHandling() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
