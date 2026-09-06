#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gap::ml {

// One laser-profile row, sensor millimetres. Invalid slots may hold either 0 or NaN (both
// conventions occur upstream, see model/features.py) -- buildChannels sanitizes both via `valid`.
// Every field must have exactly kProfileSlots entries.
struct ProfileRow {
  std::vector<double> x_mm;
  std::vector<double> z_mm;
  std::vector<double> intensity;
  std::vector<std::uint8_t> valid;
};

inline constexpr std::size_t kProfileSlots = 1280;
inline constexpr std::size_t kNumChannels = 6;

// C++ port of ml_handoff/model/features.py build_channels (see
// (contract: docs/model_roi_port_contract.md) section 1 for the exact semantics: population std,
// _sanitize ordering, one-sided edge derivatives). Returns kNumChannels * kProfileSlots float32
// values in row-major [channel][slot] order: z_norm, x_diff, normal_angle, curvature,
// intensity_norm, valid. Throws std::runtime_error if any field of `row` is not exactly
// kProfileSlots long.
std::vector<float> buildChannels(const ProfileRow& row);

}  // namespace gap::ml
