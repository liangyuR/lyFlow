#pragma once

#include <array>
#include <string>

#include "lyflow/data.h"

namespace lyflow::kernel::metrology {

enum class DirectionPolicy { Normalize, DeclaredUnit };

struct SignedPointLine {
  double distance = 0;
  std::array<double, 2> foot{0, 0};
};

bool signedPointLine(const std::array<double, 2>& point, const std::array<double, 2>& linePoint,
                     const std::array<double, 2>& lineDir,
                     const std::array<double, 2>& positiveNormal, DirectionPolicy policy,
                     SignedPointLine& output, std::string& why);

struct SampledLine {
  std::array<double, 2> center{0, 0}, direction{1, 0};
  double minimum = 0, maximum = 0, rms = 0;
};

struct SampledGapFlushOptions {
  double referenceX = 0, maxResidual = 0.001, maxExtrapolation = 0.05;
};

struct SampledGapFlush {
  double gap = 0, flush = 0;
};

bool sampledGapFlush(const SampledLine& left, const SampledLine& right,
                     const SampledGapFlushOptions& options, SampledGapFlush& output,
                     std::string& why);

bool calibratedDistance(const Record* calibration, const Point2D* a, const Point2D* b,
                        const std::string& imageFrame, Measurement& out, std::string& why,
                        std::string& port);

class ResidualAccumulator {
public:
  // One residual norm per observation, in caller order; weights are not normalized here.
  void add(double distance, double weight) noexcept;
  double weightedRmse() const noexcept;
  double maxError() const noexcept;

private:
  double squaredSum_ = 0;
  double weightSum_ = 0;
  double maximum_ = 0;
};

} // namespace lyflow::kernel::metrology
