#include "gap_ml/RoiFeatures.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace gap::ml {

namespace {

void requireSlotCount(std::size_t actual, const char* field) {
  if (actual != kProfileSlots) {
    throw std::runtime_error(std::string("gap_ml::buildChannels: ") + field +
                             " must have exactly " + std::to_string(kProfileSlots) +
                             " slots, got " + std::to_string(actual));
  }
}

double sanitizeScalar(double value) { return std::isfinite(value) ? value : 0.0; }

// Zero out invalid slots regardless of whether they hold 0 or NaN (ml_handoff/model/features.py
// _sanitize): invalid slots are forced to 0 *before* the NaN/Inf scrub, so a non-finite value at a
// valid slot is what actually gets scrubbed, never masked away by validity alone.
std::vector<double> sanitize(const std::vector<double>& a, const std::vector<std::uint8_t>& valid) {
  std::vector<double> out(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    out[i] = valid[i] ? sanitizeScalar(a[i]) : 0.0;
  }
  return out;
}

// a[i] - a[i-1]; the first slot has no predecessor and is defined as 0.
std::vector<double> edgePadDiff(const std::vector<double>& a) {
  std::vector<double> d(a.size());
  d[0] = 0.0;
  for (std::size_t i = 1; i < a.size(); ++i) d[i] = a[i] - a[i - 1];
  return d;
}

// a[i+1] - a[i-1], with edges holding the one-sided difference (a[1]-a[0] / a[n-1]-a[n-2]).
std::vector<double> centralDiff(const std::vector<double>& a) {
  const std::size_t n = a.size();
  std::vector<double> central(n);
  if (n == 0) return central;
  if (n == 1) {
    central[0] = 0.0;
    return central;
  }
  central[0] = a[1] - a[0];
  for (std::size_t i = 1; i + 1 < n; ++i) central[i] = a[i + 1] - a[i - 1];
  central[n - 1] = a[n - 1] - a[n - 2];
  return central;
}

}  // namespace

std::vector<float> buildChannels(const ProfileRow& row) {
  requireSlotCount(row.x_mm.size(), "x_mm");
  requireSlotCount(row.z_mm.size(), "z_mm");
  requireSlotCount(row.intensity.size(), "intensity");
  requireSlotCount(row.valid.size(), "valid");

  const std::size_t n = kProfileSlots;
  const auto& valid = row.valid;

  // x and z are sanitized once, up front -- every derived quantity (including the z mean/std
  // below) is computed from these sanitized arrays, never from the raw input.
  const std::vector<double> x = sanitize(row.x_mm, valid);
  const std::vector<double> z = sanitize(row.z_mm, valid);

  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (valid[i]) {
      sum += z[i];
      ++count;
    }
  }
  double z_mean = 0.0;
  double z_std = 0.0;
  if (count > 0) {
    z_mean = sum / static_cast<double>(count);
    double squared_deviation_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (valid[i]) {
        const double deviation = z[i] - z_mean;
        squared_deviation_sum += deviation * deviation;
      }
    }
    // Population standard deviation (divide by N, matching np.ndarray.std()'s default ddof=0) --
    // *not* the N-1 sample standard deviation.
    z_std = std::sqrt(squared_deviation_sum / static_cast<double>(count));
  }
  z_std = z_std > 1e-6 ? z_std : 1.0;

  std::vector<double> z_norm(n);
  for (std::size_t i = 0; i < n; ++i) z_norm[i] = (z[i] - z_mean) / z_std;
  z_norm = sanitize(z_norm, valid);

  const std::vector<double> x_diff = sanitize(edgePadDiff(x), valid);

  const std::vector<double> dz = centralDiff(z);
  const std::vector<double> dx = centralDiff(x);
  std::vector<double> normal_angle(n);
  for (std::size_t i = 0; i < n; ++i) normal_angle[i] = std::atan2(dz[i], dx[i]);
  normal_angle = sanitize(normal_angle, valid);

  std::vector<double> curvature(n, 0.0);
  for (std::size_t i = 1; i + 1 < n; ++i) curvature[i] = z[i + 1] - 2.0 * z[i] + z[i - 1];
  curvature = sanitize(curvature, valid);

  std::vector<double> intensity_norm(n);
  for (std::size_t i = 0; i < n; ++i) intensity_norm[i] = row.intensity[i] / 255.0;
  intensity_norm = sanitize(intensity_norm, valid);

  std::vector<float> channels(kNumChannels * n);
  auto writeChannel = [&](std::size_t channel_index, const std::vector<double>& values) {
    const std::size_t base = channel_index * n;
    for (std::size_t i = 0; i < n; ++i) channels[base + i] = static_cast<float>(values[i]);
  };
  writeChannel(0, z_norm);
  writeChannel(1, x_diff);
  writeChannel(2, normal_angle);
  writeChannel(3, curvature);
  writeChannel(4, intensity_norm);
  for (std::size_t i = 0; i < n; ++i) channels[5 * n + i] = valid[i] ? 1.0F : 0.0F;

  return channels;
}

}  // namespace gap::ml
