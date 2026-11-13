#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>

#include "algo/icp2d.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

/// SE(2) 的 3x3 ↔ LyFlow 的 4x4 行主序。z 轴原样保留。
Transform toTransform(const Eigen::Matrix3f& m) {
  Transform t;
  t.m[0] = m(0, 0);
  t.m[1] = m(0, 1);
  t.m[2] = 0;
  t.m[3] = m(0, 2);
  t.m[4] = m(1, 0);
  t.m[5] = m(1, 1);
  t.m[6] = 0;
  t.m[7] = m(1, 2);
  t.m[8] = 0;
  t.m[9] = 0;
  t.m[10] = 1;
  t.m[11] = 0;
  t.m[12] = 0;
  t.m[13] = 0;
  t.m[14] = 0;
  t.m[15] = 1;
  return t;
}

Eigen::Matrix3f fromTransform(const Transform& t) {
  Eigen::Matrix3f m = Eigen::Matrix3f::Identity();
  m(0, 0) = t.m[0];
  m(0, 1) = t.m[1];
  m(0, 2) = t.m[3];
  m(1, 0) = t.m[4];
  m(1, 1) = t.m[5];
  m(1, 2) = t.m[7];
  return m;
}

std_pc::Cloud2D sampleXY(const std_pc::Cloud2D& input, double voxelSize) {
  struct CellHash {
    std::size_t operator()(const std::pair<double, double>& cell) const {
      const auto first = std::hash<double>{}(cell.first);
      return first ^ (std::hash<double>{}(cell.second) + 0x9e3779b9u + (first << 6) + (first >> 2));
    }
  };
  std_pc::Cloud2D sampled;
  sampled.reserve(input.size());
  std::unordered_set<std::pair<double, double>, CellHash> cells;
  for (const auto& point : input) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
      continue;
    const auto cell = std::make_pair(std::floor(static_cast<double>(point.x) / voxelSize),
                                     std::floor(static_cast<double>(point.y) / voxelSize));
    if (!std::isfinite(cell.first) || !std::isfinite(cell.second) || cells.insert(cell).second)
      sampled.push_back(point);
  }
  return sampled;
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const bool robust = params.choice("robustLoss") != "none";
  const auto levels = params.integer("multiScaleLevels");
  if (robust && !(params.number("robustScale") > 0.0))
    issues.push_back(
        Issue::error("bad_param", "启用鲁棒损失时 robustScale 必须为正", "robustScale"));
  if (levels > 1 && !(params.number("coarseVoxelSize") > 0.0))
    issues.push_back(
        Issue::error("bad_param", "启用多尺度时 coarseVoxelSize 必须为正", "coarseVoxelSize"));
  if (levels > 1 && levels <= 6) {
    const double factor = std::ldexp(1.0, static_cast<int>(levels) - 1);
    if (!std::isfinite(params.number("maxMatchingDist") * factor))
      issues.push_back(Issue::error("bad_param", "粗尺度配对距离溢出", "maxMatchingDist"));
    if (robust && !std::isfinite(params.number("robustScale") * factor))
      issues.push_back(Issue::error("bad_param", "粗尺度鲁棒阈值溢出", "robustScale"));
  }
  return issues;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const PointCloud& source = *inputs.get("source").asCloud();
  const PointCloud& target = *inputs.get("target").asCloud();
  if (source.pointCount() == 0 || target.pointCount() == 0) {
    return Status::Error(Phase::Execute, "bad_input", "源云或目标云是空的", {},
                         source.pointCount() == 0 ? "source" : "target");
  }

  std_pc::Icp2DOptions options;
  options.maxMatchingDistance = params.number("maxMatchingDist");
  options.fitnessDistance = params.number("fitnessDist");
  options.maxIterationCount = static_cast<int>(params.integer("maxIterations"));
  options.normalKnn = static_cast<int>(params.integer("normalKnn"));
  options.smoothLength = static_cast<int>(params.integer("smoothLength"));
  options.minDiffRotErr = params.number("minDiffRot");
  options.minDiffTransErr = params.number("minDiffTrans");
  const std::string& loss = params.choice("robustLoss");
  if (loss == "huber")
    options.robustLoss = std_pc::Icp2DRobustLoss::Huber;
  else if (loss == "tukey")
    options.robustLoss = std_pc::Icp2DRobustLoss::Tukey;
  options.robustScale = params.number("robustScale");

  Eigen::Matrix3f init = Eigen::Matrix3f::Identity();
  if (inputs.has("init")) {
    const Transform* t = inputs.get("init").asTransform();
    if (t != nullptr)
      init = fromTransform(*t);
  }
  if (!init.allFinite())
    return Status::Error(Phase::Execute, "bad_input", "初始位姿含非有限值", {}, "init");

  const int levels = static_cast<int>(params.integer("multiScaleLevels"));
  std_pc::Cloud2D sourceXY;
  std_pc::Cloud2D targetXY;
  if (levels > 1) {
    sourceXY = std_pc::toCloud2D(source);
    targetXY = std_pc::toCloud2D(target);
  }
  const bool qualityEvaluated = params.flag("reportQuality") || params.flag("qualityGate");
  std_pc::Icp2DResult result;
  std_pc::Icp2DQuality quality;
  nlohmann::json stages = nlohmann::json::array();
  std::int64_t totalIterations = 0;
  for (int level = levels - 1; level >= 0; --level) {
    const double factor = std::ldexp(1.0, level);
    const double voxel =
        level == 0 ? 0.0 : std::ldexp(params.number("coarseVoxelSize"), level - levels + 1);
    std_pc::Cloud2D sampledSource;
    std_pc::Cloud2D sampledTarget;
    if (level > 0) {
      sampledSource = sampleXY(sourceXY, voxel);
      sampledTarget = sampleXY(targetXY, voxel);
    }
    auto stageOptions = options;
    stageOptions.maxMatchingDistance *= factor;
    if (options.robustLoss != std_pc::Icp2DRobustLoss::None)
      stageOptions.robustScale *= factor;
    std_pc::Icp2D icp(stageOptions);
    if (levels == 1) {
      icp.setSource(std_pc::toCloud2D(source));
      icp.setTarget(std_pc::toCloud2D(target));
    } else {
      icp.setSource(level == 0 ? sourceXY : sampledSource);
      icp.setTarget(level == 0 ? targetXY : sampledTarget);
    }
    nlohmann::json stage = {{"scaleFactor", factor},
                            {"voxelSize", voxel},
                            {"maxMatchingDist", stageOptions.maxMatchingDistance},
                            {"sourceCount", icp.sourcePointCount()},
                            {"targetCount", icp.targetPointCount()}};
    if (level > 0 && (icp.sourcePointCount() < 3 || icp.targetPointCount() < 3)) {
      stage["skipped"] = "too_few_points";
      stages.push_back(std::move(stage));
      continue;
    }
    if (icp.sourcePointCount() == 0 || icp.targetPointCount() == 0)
      return Status::Error(Phase::Execute, "bad_input", "源云或目标云没有有限 XY 点", {},
                           icp.sourcePointCount() == 0 ? "source" : "target");
    result = icp.align(init);
    if (!result.transform.allFinite())
      return Status::Error(Phase::Execute, "registration_failed", "ICP 产生了非有限位姿", {},
                           "source");
    init = result.transform;
    totalIterations += result.iterations;
    stage["iterations"] = result.iterations;
    stage["fitness"] = result.fitness;
    stage["converged"] = result.converged;
    stages.push_back(std::move(stage));
    if (level == 0 && qualityEvaluated)
      quality = icp.evaluatePose(result.transform);
  }

  if (params.flag("qualityGate")) {
    std::ostringstream rejected;
    if (params.flag("requireConverged") && !result.converged)
      rejected << " converged=false;";
    if (quality.effectiveMatchedCount < static_cast<std::size_t>(params.integer("minMatches")))
      rejected << " effectiveMatches=" << quality.effectiveMatchedCount << " < "
               << params.integer("minMatches") << ';';
    if (quality.inlierRatio < params.number("minInlierRatio"))
      rejected << " inlierRatio=" << quality.inlierRatio << " < " << params.number("minInlierRatio")
               << ';';
    if (!std::isfinite(quality.planeRmse) || quality.planeRmse > params.number("maxPlaneRmse"))
      rejected << " planeRmse=" << quality.planeRmse
               << " m, limit=" << params.number("maxPlaneRmse") << " m;";
    if (!rejected.str().empty())
      return Status::Error(Phase::Execute, "registration_rejected",
                           "ICP 质量门拒绝：" + rejected.str(), {}, "source");
  }

  Record record;
  record.type = "Icp2DResult";
  record.data["fitness"] = result.fitness;
  record.data["iterations"] = result.iterations;
  record.data["converged"] = result.converged;
  record.data["totalIterations"] = totalIterations;
  record.data["levels"] = std::move(stages);
  record.data["robustLoss"] = loss;
  record.data["qualityEvaluated"] = qualityEvaluated;
  record.data["qualityGateEnabled"] = params.flag("qualityGate");
  if (qualityEvaluated) {
    record.data["sourceCount"] = quality.sourceCount;
    record.data["targetCount"] = quality.targetCount;
    record.data["matchedCount"] = quality.matchedCount;
    record.data["normalMatchedCount"] = quality.normalMatchedCount;
    record.data["effectiveMatchedCount"] = quality.effectiveMatchedCount;
    record.data["inlierRatio"] = quality.inlierRatio;
    record.data["weightSum"] = quality.weightSum;
    for (const auto& metric : {std::make_pair("pointRmse", quality.pointRmse),
                               std::make_pair("planeRmse", quality.planeRmse),
                               std::make_pair("weightedPlaneRmse", quality.weightedPlaneRmse)}) {
      record.data[metric.first] =
          std::isfinite(metric.second) ? nlohmann::json(metric.second) : nlohmann::json(nullptr);
    }
  }
  outputs.set("transform", Data::transform(toTransform(result.transform)));
  outputs.set("result", Data::record(std::move(record)));

  ctx.log(LogLevel::Info, "icp fitness=" + std::to_string(result.fitness) +
                              " iterations=" + std::to_string(result.iterations) +
                              (result.converged ? " converged" : " not converged"));
  return Status::Ok();
}

Param numParam(const char* name, const char* label, double def, const char* unit, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = unit;
  p.min = 0.0;
  return p;
}

Param intParam(const char* name, const char* label, std::int64_t def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  p.min = 1.0;
  p.max = static_cast<double>(std::numeric_limits<int>::max());
  return p;
}

Param flagParam(const char* name, const char* label, bool def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Bool;
  p.label = label;
  p.doc = doc;
  p.def = Value::boolean(def);
  return p;
}

} // namespace

void registerRegisterIcp2D(Registry& r) {
  OperatorDesc op;
  op.id = "register.icp_2d";
  op.version = "1.1.0";
  op.label = "二维 ICP 配准";
  op.category = "配准/ICP";
  op.keywords = {"2D ICP registration",
                 "iterative closest point",
                 "point to plane ICP",
                 "二维点云配准",
                 "点到平面",
                 "刚体对齐",
                 "icp",
                 "register",
                 "align",
                 "2d",
                 "配准",
                 "对齐"};
  op.doc = "XY 平面上的 point-to-plane ICP。源云的法线由 knn 邻域现估，"
           "收敛判据是最近 Smooth Length 步的平均旋转/平移增量都低于阈值。\n"
           "可选 Huber/Tukey 鲁棒损失与最多 6 层的粗细配准，默认关闭。\n"
           "Report Quality 或 Quality Gate 打开时，在最终位姿重新计算质量；质量门拒绝返回 "
           "registration_rejected。输出的 Transform 是源 → 目标的 4x4（z 轴恒等）。";
  op.inputs = {
      Port{"source", "PointCloud", "Source", "待配准的点云。", true},
      Port{"target", "PointCloud", "Target", "参考点云。", true},
      Port{"init", "Transform", "Init", "可选：初始位姿。不接就是单位阵。", false},
  };
  op.outputs = {
      Port{"transform", "Transform", "Transform", "源 → 目标的变换。", true},
      Port{"result", "Record", "Result", "Icp2DResult：收敛、各尺度记录及按需评估的配准质量。",
           true},
  };

  Param loss;
  loss.name = "robustLoss";
  loss.type = ParamType::Enum;
  loss.label = "Robust Loss";
  loss.doc = "none 保留最小二乘；Huber 降低大残差的权重；Tukey 拒绝超过 robustScale 的平面残差。";
  loss.def = Value::text("none");
  loss.options = {{"none", "None", ""}, {"huber", "Huber", ""}, {"tukey", "Tukey", ""}};
  Param levels =
      intParam("multiScaleLevels", "Multi Scale Levels", 1,
               "1 保留原始单尺度；每个粗层将配对距离与鲁棒阈值放宽 2 倍，最后跑原始全量点。");
  levels.max = 6;
  Param ratio = numParam("minInlierRatio", "Min Inlier Ratio", 0.5, "",
                         "质量门要求最终位姿的几何匹配数 / 有限源点数不低于它。");
  ratio.max = 1.0;
  op.params = {
      numParam("maxMatchingDist", "Max Matching Dist", 0.001, "m",
               "配对距离上限，超过就不算一对。"),
      numParam("fitnessDist", "Fitness Dist", 0.01, "m", "算 fitness 时的距离阈值。"),
      intParam("maxIterations", "Max Iterations", 1000, "最大迭代次数。"),
      intParam("normalKnn", "Normal KNN", 10, "估源云法线的邻域大小。"),
      intParam("smoothLength", "Smooth Length", 4, "收敛判据的滑动窗长度。"),
      numParam("minDiffRot", "Min Diff Rot", 1e-3, "rad", "平均旋转增量低于它就算收敛。"),
      numParam("minDiffTrans", "Min Diff Trans", 1e-4, "m", "平均平移增量低于它就算收敛。"),
      loss,
      numParam("robustScale", "Robust Scale", 0.001, "m",
               "平面残差的鲁棒阈值；启用损失时必须为正。"),
      levels,
      numParam("coarseVoxelSize", "Coarse Voxel Size", 0.002, "m",
               "最粗层的 XY 网格大小，每格保留首点；中间层逐次减半，最终层不抽稀。"),
      flagParam("reportQuality", "Report Quality", false,
                "在最终位姿额外做一次近邻扫描以报告质量。"),
      flagParam("qualityGate", "Quality Gate", false,
                "自动评估最终质量，不合格返回 registration_rejected，不产出变换与结果。"),
      intParam("minMatches", "Min Matches", 3,
               "质量门要求的有效法线且鲁棒权重大于零的最少匹配数。"),
      ratio,
      numParam("maxPlaneRmse", "Max Plane RMSE", 0.001, "m",
               "质量门允许的未加权平面残差 RMSE 上限。"),
      flagParam("requireConverged", "Require Converged", true,
                "质量门要求最终全量层达到收敛条件。"),
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;

  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
