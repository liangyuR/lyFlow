#pragma once

#include <array>
#include <string>

#include "lyflow/data.h"

namespace lyflow::kernel::profile_dimensions {

enum class FlushSign { CanonicalAxis, PositiveNormal };

struct FlushOptions {
  bool signedDistance = true;
  FlushSign sign = FlushSign::CanonicalAxis;
  std::array<float, 2> positiveNormal{0, -1};
  double gain = 1, offsetMm = 0;
};

struct FlushResult {
  double valueMm = 0;
  Point2D foot;
  Line2D segment, baseLine;
};

enum class CircleGapDefinition { CenterLine, DatumTangents };

struct CircleGapOptions {
  CircleGapDefinition definition = CircleGapDefinition::CenterLine;
  double offsetMm = 0;
};

struct CircleGapResult {
  double valueMm = 0;
  bool valid = false, crossed = false;
  Line2D segment;
};

struct PointOffsetOptions {
  bool absDx = false, absDy = false;
  double gainDx = 1, gainDy = 1, dxOffsetMm = 0, dyOffsetMm = 0;
};

struct PointOffsetResult {
  double dxMm = 0, dyMm = 0, distanceMm = 0;
  Line2D segment;
};

bool flushPointLine(const Line2D& base, const Point2D& reference, const FlushOptions& options,
                    FlushResult& output, std::string& why);
bool circleGap(const Circle2D& left, const Circle2D& right, const Line2D* base,
               const CircleGapOptions& options, CircleGapResult& output, std::string& why);
bool pointOffset(const Point2D& a, const Point2D& b, const PointOffsetOptions& options,
                 PointOffsetResult& output, std::string& why);

} // namespace lyflow::kernel::profile_dimensions
