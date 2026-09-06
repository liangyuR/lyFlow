#pragma once

#include "gap_core/MeasurementTypes.hpp"

namespace gap::core {

class MeasurementEngine {
 public:
  MeasurementResult measure(const MeasurementRequest& request) const noexcept;
};

}  // namespace gap::core
