#pragma once
#include <vector>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::profile_contract {
struct RobustLineInputs {
  Data profile;
  Data box;
  Data boxContext;
  Data toward;
  Data towardContext;
  Data refLine;
  Data refLineContext;
};
struct RobustLineResult {
  Data profile;
  Data line;
  Data lineContext;
  Data innerEnd;
  Data pointContext;
  Data inliers;
  Data weightedInliers;
  Data indexMap;
  Data quality;
};
struct RobustCircleInputs {
  Data profile;
};
struct RobustCircleResult {
  Data profile;
  Data circle;
  Data circleContext;
  Data inliers;
  Data weightedInliers;
  Data indexMap;
  Data quality;
};
enum class NearestReference { Point, Line };
struct NearestInputs {
  Data profile;
  Data reference;
  Data referenceContext;
};
struct NearestResult {
  Data profile;
  Data point;
  Data pointContext;
  Data selection;
};
std::vector<Param> robustLineParams();
std::vector<Param> robustCircleParams();
Status fitRobustLine(const RobustLineInputs& inputs, const ParamView& parameters,
                     RobustLineResult& out, ExecContext& ctx);
Status fitRobustCircle(const RobustCircleInputs& inputs, const ParamView& parameters,
                       RobustCircleResult& out, ExecContext& ctx);
Status nearestSample(const NearestInputs& inputs, NearestReference reference, NearestResult& out,
                     ExecContext& ctx);
}
