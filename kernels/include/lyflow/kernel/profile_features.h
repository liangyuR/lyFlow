#pragma once
#include <array>
#include <cstddef>
#include <vector>

#include "lyflow/kernel/profile_robust.h"

namespace lyflow::kernel::profile_features {

enum class LineType { Free, Vertical, Horizontal };
enum class DirectionMode { Free, Band, Fixed };
enum class Endpoints { RoiIntersection, InlierEnds };

struct Bounds {
  std::array<float, 2> min{};
  std::array<float, 2> max{};
};

struct LineOptions {
  profile_robust::Line2DFitOptions fit;
  LineType type = LineType::Free;
  std::size_t segmentPoints = 0;
  std::size_t minInliers = 2;
  DirectionMode directionMode = DirectionMode::Free;
  bool hasReference = false;
  std::array<float, 2> referenceDirection{1, 0};
  double nominalDeg = 0;
  double toleranceDeg = 12;
  Endpoints endpoints = Endpoints::RoiIntersection;
  Bounds roi;
  std::array<double, 2> toward{};
};

struct LineQuality {
  std::size_t pointCount = 0;
  std::size_t inlierCount = 0;
  double inlierRatio = 0;
  std::array<double, 2> linePointMm{};
  std::array<double, 2> lineDirection{};
  double rmsResidualMm = 0;
  double maxResidualMm = 0;
};

struct LineFeature {
  std::array<float, 6> coefficients{};
  std::array<float, 2> point{};
  std::array<float, 2> direction{1, 0};
  bool hasSegment = false;
  std::array<float, 2> start{};
  std::array<float, 2> end{};
  int innerSlot = -1;
  // Sorted original input slots. Fixed-direction fallback can retain repeated sample weights.
  std::vector<int> inliers;
  bool segmentApplied = false;
  bool segmentSkipped = false;
  bool roiIntersectionMissing = false;
  bool directionPinned = false;
  std::size_t initialInlierCount = 0;
  LineQuality quality;
};

enum class LineFailureStage {
  None,
  InsufficientSamples,
  InitialFit,
  SegmentRefit,
  EmptyInliers,
  FixedFit,
  MinInliers,
  InvalidResult
};

struct LineFailure {
  LineFailureStage stage = LineFailureStage::None;
  std::size_t inlierCount = 0;
};

// Inputs, ROI, toward and thresholds are meters in the explicitly selected XY analysis plane.
// Nonfinite slots are excluded through an internal view; input order and output slot identity remain.
// Quality retains the legacy millimeter arithmetic. False/cancellation leave out unchanged.
bool fitLine(profile_robust::Samples samples, const LineOptions& options, LineFeature& out,
             LineFailure* failure = nullptr, const profile_robust::CancelCheck& cancelled = {});

} // namespace lyflow::kernel::profile_features
