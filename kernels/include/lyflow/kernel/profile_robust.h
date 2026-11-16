#pragma once
#include <array>
#include <cstddef>
#include <vector>

#include "lyflow/kernel/profile.h"

namespace lyflow::kernel::profile_robust {

struct Point {
  float x = 0, y = 0, z = 0;
};

// Borrowed read-only coordinates in one consistent unit. Meter inputs use meter thresholds/radii.
// Callers explicitly map the analysis plane to XY (normally z=0). The legacy line backend retains
// its XYZ arithmetic; circles use XY. No coordinate is silently dropped or normalized here.
// Slots and order are retained; returned indices address these slots, including around NaN gaps.
struct Samples {
  const Point* data = nullptr;
  std::size_t size = 0;
  bool dense = false;
};

struct Line2DFitOptions {
  float distThresh = 0.0001F;
  int maxIterations = 1000;
  bool optimize = true;
};

struct Circle2DFitOptions {
  float distThresh = 0.00003F;
  double minRadius = 0.0005;
  double maxRadius = 0.01;
  int maxIterations = 10000;
  bool optimizeCoefficients = true;
  double radiusPrior = 0.0;
  double radiusPriorSigma = 0.0;
};

enum class Circle2DRadiusOutcome { Free, Prior, ClampedMin, ClampedMax, Unrefined };

struct Circle2DFitReport {
  Circle2DRadiusOutcome radius = Circle2DRadiusOutcome::Free;
  double unclampedRadius = 0.0;
};

struct LineFit {
  std::array<float, 6> coefficients{};
  std::vector<int> inliers;
};

struct CircleFit {
  std::array<float, 3> coefficients{};
  std::vector<int> inliers;
  Circle2DFitReport report;
};

using CancelCheck = profile::CancelCheck;
using Cancelled = profile::Cancelled;

// Hypotheses use threshold/3, dual-trace competition and the original threshold for final inliers.
// False results and cancellation leave the caller's output unchanged. PCL calls are checked for
// cancellation on return; copying and local refinement also check periodically.
bool fitLine(Samples samples, LineFit& out, const Line2DFitOptions& options = {},
             const CancelCheck& cancelled = {});
bool fitAxisLine(Samples samples, bool vertical, LineFit& out, const Line2DFitOptions& options = {},
                 const CancelCheck& cancelled = {});
bool fitCircle(Samples samples, CircleFit& out, const Circle2DFitOptions& options = {},
               const CancelCheck& cancelled = {});
bool fitCircleFixedRadius(Samples samples, double radius, CircleFit& out,
                          const Circle2DFitOptions& options = {},
                          const CancelCheck& cancelled = {});

} // namespace lyflow::kernel::profile_robust
