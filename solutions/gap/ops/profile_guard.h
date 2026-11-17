#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap::profile_scene {
struct CameraGuardOptions {
  std::string onDisagree = "record";
  double maxDeltaMm = .5;
  double sampleStepMm = .5;
  double halfWindowMm = .25;
  std::int64_t minSamples = 20;
  std::size_t maxSamples = 1000000;
};
struct GuardResult {
  Data primary;
  Data secondary;
  Record quality;
};
Status guardProfiles(const Data& primary, const Data& secondary, const CameraGuardOptions& options,
                     GuardResult& out, ExecContext& ctx, const Data* box = nullptr,
                     const Record* boxContext = nullptr);
void registerProfileCameraGuard(Registry& registry);
}
