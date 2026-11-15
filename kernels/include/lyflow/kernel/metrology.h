#pragma once

#include <string>

#include "lyflow/data.h"

namespace lyflow::kernel::metrology {

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
