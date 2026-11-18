#pragma once

#include <array>

#include "lyflow/data.h"

namespace lyflow::ops {

inline std::array<float, 3> transformPoint(const Transform& t, const float* p) {
  return {t.m[0] * p[0] + t.m[1] * p[1] + t.m[2] * p[2] + t.m[3],
          t.m[4] * p[0] + t.m[5] * p[1] + t.m[6] * p[2] + t.m[7],
          t.m[8] * p[0] + t.m[9] * p[1] + t.m[10] * p[2] + t.m[11]};
}

} // namespace lyflow::ops
