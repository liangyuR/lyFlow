#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lyflow::barcode {
struct Options {
  std::string formats = "Any";
  bool tryHarder = true, tryRotate = true, tryInvert = true, tryDownscale = true;
  int limit = 129;
};
struct Decoded {
  bool valid = false, inverted = false, mirrored = false, hasECI = false;
  std::string format, text, contentType, error;
  std::vector<std::uint8_t> bytes;
  std::array<std::array<double, 2>, 4> corners{};
  int orientation = 0, lineCount = 0;
};
std::vector<Decoded> read(const std::uint8_t* gray, int width, int height, int rowStride,
                          const Options& options);
} // namespace lyflow::barcode
