#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <set>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap::profile_scene {
enum class CircleSource { Merged, Primary, Secondary };
struct CircleTraceSide {
  CircleSource source = CircleSource::Merged;
  std::vector<std::int32_t> slots;
};
struct CirclePairTrace {
  std::array<CircleTraceSide, 2> sides;
};
struct CirclePairInputs {
  Data merged;
  Data primary;
  Data secondary;
  Data mergedProvenance;
  Data primaryProvenance;
  Data secondaryProvenance;
  std::array<Data, 2> boxes;
  std::array<Data, 2> boxContexts;
  Data refLine;
  Data refLineContext;
  Data refLineRight;
  Data refLineRightContext;
};
struct CirclePairResult {
  std::array<Data, 2> circles;
  std::array<Data, 2> contexts;
  std::array<Data, 2> profiles;
  std::array<Data, 2> provenances;
  std::array<Data, 2> inliers;
  Record quality;
};
std::vector<Param> circlePairParams();
std::vector<Issue> validateCirclePairParams(const ParamView&, const std::set<std::string>&);
Status fitProfileGapCircles(const CirclePairInputs& inputs, const ParamView& parameters,
                            CirclePairResult& out, ExecContext& ctx);
void registerProfileGapCircles(Registry& registry);
namespace detail {
Status fitCirclePairClouds(const Inputs& inputs, const ParamView& parameters, Outputs& outputs,
                           ExecContext& ctx, CirclePairTrace* trace = nullptr);
}
}
