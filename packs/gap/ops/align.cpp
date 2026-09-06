// 模板对齐与模板选择。ICP 本身复用 Alignment::registerCloud2DICPOutcome
// （它是 protected，所以这里派生一个只为了把它露出来的子类）。
#include <cmath>
#include <limits>

#include "gap_detection/Alignment.hpp"
#include "gap_detection/GapUtils.hpp"
#include "gap_ops.h"

namespace lyflow::packs::gap {
namespace {

namespace dom = ::detection::domain;

/// 只为了把 protected 的 registerCloud2DICPOutcome 露出来。run() 永远不会被调到。
class PackAlignment final : public ::detection::Alignment {
 public:
  int64_t run(const GapCloud&, const GapCloud&) override { return 0; }
  using ::detection::Alignment::registerCloud2DICPOutcome;
};

// ---- Layer 1 信赖域。原实现在 GapDetection.cpp 的匿名命名空间里，没有导出。

struct Se2Delta {
  Eigen::Vector2f translation = Eigen::Vector2f::Zero();
  float rotationRad = 0.0F;
};

Se2Delta decomposeDelta(const Eigen::Matrix3f& reference, const Eigen::Matrix3f& candidate) {
  const Eigen::Matrix3f delta = candidate * reference.inverse();
  Se2Delta result;
  result.translation = delta.block<2, 1>(0, 2);
  result.rotationRad = std::atan2(delta(1, 0), delta(0, 0));
  return result;
}

Eigen::Matrix3f composeDelta(const Eigen::Matrix3f& reference, const Eigen::Vector2f& translation,
                             float rotationRad) {
  Eigen::Matrix3f delta = Eigen::Matrix3f::Identity();
  const float cosine = std::cos(rotationRad);
  const float sine = std::sin(rotationRad);
  delta(0, 0) = cosine;
  delta(0, 1) = -sine;
  delta(1, 0) = sine;
  delta(1, 1) = cosine;
  delta.block<2, 1>(0, 2) = translation;
  return delta * reference;
}

Eigen::Matrix3f applyTrustRegion(const Eigen::Matrix3f& reference, const Eigen::Matrix3f& transform,
                                 double maxTranslationMm, double maxRotationDeg,
                                 double* rawTranslationMm, double* rawRotationDeg, bool* clamped) {
  const auto delta = decomposeDelta(reference, transform);
  const double translationMm = static_cast<double>(delta.translation.norm()) * kScale;
  const double rotationDeg = static_cast<double>(delta.rotationRad) * 180.0 / M_PI;
  *rawTranslationMm = translationMm;
  *rawRotationDeg = rotationDeg;
  *clamped = false;
  const bool translationExceeds = translationMm > maxTranslationMm;
  const bool rotationExceeds = std::fabs(rotationDeg) > maxRotationDeg;
  if (!translationExceeds && !rotationExceeds) return transform;

  *clamped = true;
  Eigen::Vector2f clampedTranslation = delta.translation;
  if (translationExceeds && translationMm > 0) {
    clampedTranslation *= static_cast<float>(maxTranslationMm / translationMm);
  }
  double clampedRotationDeg = rotationDeg;
  if (rotationExceeds) clampedRotationDeg = std::copysign(maxRotationDeg, rotationDeg);
  return composeDelta(reference, clampedTranslation,
                      static_cast<float>(clampedRotationDeg * M_PI / 180.0));
}

// ---------------------------------------------------------------------------

dom::AlignmentConfiguration configFromParams(const ParamView& p) {
  dom::AlignmentConfiguration c;
  c.method = "ICP";
  c.save_result = false;
  c.using_camera = "Both";
  c.icp.max_matching_distance = p.number("maxMatchingDist");
  c.icp.max_fitness_distance = p.number("maxFitnessDist");
  c.icp.max_iteration_count = static_cast<int>(p.integer("maxIterations"));
  c.icp.minimum_score = static_cast<int>(p.integer("minScore"));
  c.icp.neighbor_count = static_cast<int>(p.integer("normalKnn"));
  c.icp.bidirectional = p.flag("bidirection");
  c.robustness.degenerate_ratio = p.number("degenerateRatio");
  c.robustness.global_coarse = p.flag("globalCoarse");
  c.robustness.trust_region.max_translation_mm = p.number("trustTranslation");
  c.robustness.trust_region.max_rotation_deg = p.number("trustRotation");
  return c;
}

dom::InitialPoseMode poseModeFromParams(const ParamView& p) {
  const std::string mode = p.choice("initialPoseMode");
  if (mode == "centroid") return dom::InitialPoseMode::kCentroidTranslation;
  if (mode == "pca") return dom::InitialPoseMode::kPcaRigidCentroid;
  return dom::InitialPoseMode::kEndpointMidpointTranslation;
}

struct SideOutcome {
  ::detection::AlignmentOutcome outcome;
  double trustTranslationMm = std::numeric_limits<double>::quiet_NaN();
  double trustRotationDeg = std::numeric_limits<double>::quiet_NaN();
  bool trustClamped = false;
};

nlohmann::json sideJson(const SideOutcome& s) {
  return nlohmann::json{
      {"transform", transformToJson(s.outcome.transform)},
      {"score", s.outcome.score},
      {"success", s.outcome.success},
      {"bidirectional", s.outcome.bidirectional},
      {"degenerateRatio", std::isfinite(s.outcome.degenerate_eigenvalue_ratio)
                              ? nlohmann::json(s.outcome.degenerate_eigenvalue_ratio)
                              : nlohmann::json()},
      {"degenerateLocked", s.outcome.degenerate_locked},
      {"trustTranslationMm", std::isfinite(s.trustTranslationMm)
                                 ? nlohmann::json(s.trustTranslationMm)
                                 : nlohmann::json()},
      {"trustRotationDeg",
       std::isfinite(s.trustRotationDeg) ? nlohmann::json(s.trustRotationDeg) : nlohmann::json()},
      {"trustClamped", s.trustClamped},
  };
}

Status alignTemplate(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext& ctx) {
  const GapCloud target = toPcl(*inputs.get("cloud").asCloud());
  const GapCloud tplLeft = toPcl(*inputs.get("tplLeft").asCloud());
  const GapCloud tplRight = toPcl(*inputs.get("tplRight").asCloud());
  if (target.empty() || tplLeft.empty() || tplRight.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "点云或模板是空的", {}, "cloud");
  }

  PackAlignment aligner;
  aligner.setConfiguration(configFromParams(params));
  const double trustTranslation = params.number("trustTranslation");
  const double trustRotation = params.number("trustRotation");
  const bool globalCoarse = params.flag("globalCoarse");
  const bool successGuide = params.flag("successGuide");
  const bool segRoi = params.flag("segRoi");

  const auto evaluateSide = [&](const GapCloud& source, const Eigen::Matrix3f& initialPose,
                                const Eigen::Matrix3f& trustReference) {
    SideOutcome side;
    side.outcome = aligner.registerCloud2DICPOutcome(source, target, initialPose);
    if (side.outcome.success) {
      side.outcome.transform =
          applyTrustRegion(trustReference, side.outcome.transform, trustTranslation, trustRotation,
                           &side.trustTranslationMm, &side.trustRotationDeg, &side.trustClamped);
    }
    return side;
  };

  const auto initial = ::detection::utils::computeInitialPose(tplLeft, tplRight, target, target,
                                                              poseModeFromParams(params));
  Eigen::Matrix3f sideInitialPose = initial.transform;
  bool globalSucceeded = false;
  double globalScore = std::numeric_limits<double>::quiet_NaN();
  if (globalCoarse) {
    GapCloud mergedTemplate(tplLeft);
    mergedTemplate += tplRight;
    // seg_mode ROI 下 left_cloud_ 与 right_cloud_ 是同一片云，原算法把它加了两遍。
    // 照做（G8）—— 这会让全局粗配的目标点数翻倍。
    GapCloud mergedTarget(target);
    mergedTarget += target;
    PackAlignment globalAligner;
    globalAligner.setConfiguration(configFromParams(params));
    const auto global =
        globalAligner.registerCloud2DICPOutcome(mergedTemplate, mergedTarget, initial.transform);
    globalScore = global.score;
    if (global.success) {
      sideInitialPose = global.transform;
      globalSucceeded = true;
    }
  }
  const Eigen::Matrix3f trustReference = sideInitialPose;

  SideOutcome left = evaluateSide(tplLeft, sideInitialPose, trustReference);
  SideOutcome right;
  bool mirrored = false;
  if (segRoi && left.outcome.success && ::detection::utils::cloudsIdentical2D(tplLeft, tplRight)) {
    // 模板两侧内容相同 + 同一片目标云 = 同一个配准问题，直接复用左边的结果。
    right = left;
    mirrored = true;
  } else if (!successGuide) {
    if (left.outcome.success) right = evaluateSide(tplRight, sideInitialPose, trustReference);
  } else if (left.outcome.success) {
    right = evaluateSide(tplRight, sideInitialPose, trustReference);
    if (!right.outcome.success) {
      right = evaluateSide(tplRight, left.outcome.transform, trustReference);
    }
  } else {
    right = evaluateSide(tplRight, Eigen::Matrix3f::Identity(), trustReference);
    if (right.outcome.success) {
      left = evaluateSide(tplLeft, right.outcome.transform, trustReference);
    }
  }

  lyflow::Record record;
  record.type = "GapAlignment";
  record.data = nlohmann::json{
      {"templateId", params.text("templateId")},
      {"order", params.integer("order")},
      {"minScore", params.integer("minScore")},
      {"globalCoarse", globalCoarse},
      {"globalSucceeded", globalSucceeded},
      {"globalScore", std::isfinite(globalScore) ? nlohmann::json(globalScore) : nlohmann::json()},
      {"mirrored", mirrored},
      {"left", sideJson(left)},
      {"right", sideJson(right)},
  };
  static const char* kKeys[4] = {"flushBase", "gapLeft", "flushRef", "gapRight"};
  static const char* kParams[4] = {"roiFlushBase", "roiGapLeft", "roiFlushRef", "roiGapRight"};
  nlohmann::json rois = nlohmann::json::object();
  for (int i = 0; i < 4; ++i) {
    const std::array<float, 4> v = params.vec4(kParams[i]);
    rois[kKeys[i]] = {v[0], v[1], v[2], v[3]};
  }
  record.data["rois"] = rois;

  ctx.log(LogLevel::Info, "ICP " + params.text("templateId") + ": left=" +
                              std::to_string(left.outcome.score) +
                              " right=" + std::to_string(right.outcome.score) +
                              (mirrored ? " (mirrored)" : ""));
  outputs.set("alignment", Data::record(std::move(record)));
  return Status::Ok();
}

Status selectAlignment(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                       ExecContext& ctx) {
  std::vector<const lyflow::Record*> candidates;
  for (const char* port : {"a", "b", "c", "d"}) {
    if (!inputs.has(port)) continue;
    const lyflow::Record* rec = inputs.get(port).asRecord();
    if (!rec || rec->type != "GapAlignment") {
      return Status::Error(Phase::Execute, "bad_input", "输入不是 GapAlignment", {}, port);
    }
    candidates.push_back(rec);
  }
  if (candidates.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "一个候选都没有", {}, "a");
  }

  std::vector<::detection::utils::TemplateIcpScore> scores;
  scores.reserve(candidates.size());
  for (const auto* rec : candidates) {
    ::detection::utils::TemplateIcpScore s;
    s.id = rec->data.value("templateId", std::string());
    s.left = rec->data["left"].value("score", 0.0);
    s.right = rec->data["right"].value("score", 0.0);
    s.configuration_order = static_cast<std::size_t>(rec->data.value("order", 0));
    scores.push_back(std::move(s));
  }
  const double minScore = static_cast<double>(params.integer("minScore"));
  const auto picked = ::detection::utils::selectBestTemplateByIcp(scores, minScore);
  if (!picked) {
    double worst = scores.front().left;
    if (std::isfinite(scores.front().right)) worst = std::min(worst, scores.front().right);
    return Status::Error(Phase::Execute, "icp_score_low",
                         "所有候选模板的 ICP 分都低于 " + std::to_string(minScore) +
                             "（最好的一份是 " + std::to_string(worst) + "）");
  }
  ctx.log(LogLevel::Info, "选中模板 " + scores[*picked].id);
  outputs.set("alignment", Data::record(*candidates[*picked]));
  return Status::Ok();
}

Param intParam(const char* name, const char* label, std::int64_t def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  return p;
}

Param numParam(const char* name, const char* label, double def, const char* unit,
               const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = unit;
  return p;
}

Param boolParam(const char* name, const char* label, bool def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Bool;
  p.label = label;
  p.doc = doc;
  p.def = Value::boolean(def);
  return p;
}

Param roiParam(const char* name, const char* label) {
  Param p;
  p.name = name;
  p.type = ParamType::Vec4f;
  p.label = label;
  p.doc = "模板自带的业务 ROI（毫米）。候选没写就填配置里的那一份。";
  p.def = Value::vec({0.0, 0.0, 0.0, 0.0});
  p.unit = "mm";
  p.group = "ROI";
  p.componentLabels = {"X Min", "Y Min", "X Max", "Y Max"};
  return p;
}

}  // namespace

void registerAlignTemplate(Registry& r) {
  OperatorDesc op;
  op.id = "gap.align_template";
  op.version = "1.0.0";
  op.label = "Align Template";
  op.category = "Gap/Align";
  op.keywords = {"icp", "template", "align", "配准", "模板"};
  op.doc =
      "把一对模板配到当前样本上（复刻 evaluate_pair）：全局粗配 → 左右两侧 ICP、"
      "信赖域钳制、退化方向锁定；模板两侧内容相同时右侧复用左侧的结果。";
  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "合并并滤波之后的测量帧点云。", true},
      Port{"tplLeft", "PointCloud", "Template Left", "左模板。", true},
      Port{"tplRight", "PointCloud", "Template Right", "右模板。", true},
  };
  op.outputs = {Port{"alignment", "Record", "Alignment", "GapAlignment。", true}};

  Param templateId;
  templateId.name = "templateId";
  templateId.type = ParamType::String;
  templateId.label = "Template Id";
  templateId.def = Value::text("primary");

  Param poseMode;
  poseMode.name = "initialPoseMode";
  poseMode.type = ParamType::Enum;
  poseMode.label = "Initial Pose";
  poseMode.def = Value::text("endpoint_midpoint");
  poseMode.advanced = true;
  poseMode.options = {EnumOption{"endpoint_midpoint", "Endpoint Midpoint", ""},
                      EnumOption{"centroid", "Centroid", ""},
                      EnumOption{"pca", "PCA Rigid Centroid", ""}};

  op.params = {
      templateId,
      poseMode,
      intParam("order", "Config Order", 0, "配置里的候选顺序，模板选择平局时用它。"),
      numParam("maxMatchingDist", "Max Matching Dist", 10.0, "mm", "ICP 的最大匹配距离。"),
      numParam("maxFitnessDist", "Max Fitness Dist", 0.5, "mm", "算 fitness 用的距离。"),
      intParam("maxIterations", "Max Iterations", 1000, "ICP 最大迭代次数。"),
      intParam("normalKnn", "Normal KNN", 10, "估法向用几个邻居（num_neighbor）。"),
      intParam("minScore", "Min Score", 60, "fitness × 100 的下限。"),
      boolParam("bidirection", "Bidirection", false, "分数不够时试一次反向配准。"),
      boolParam("globalCoarse", "Global Coarse", true, "先把两模板合起来粗配一次。"),
      boolParam("successGuide", "Success Guide", false, "一侧失败时用另一侧的结果重试。"),
      boolParam("segRoi", "Seg Mode ROI", true,
                "seg_mode=ROI：左右两侧是同一片合并云，模板相同则右侧复用左侧。"),
      numParam("trustTranslation", "Trust Translation", 3.0, "mm", "信赖域的平移上限。"),
      numParam("trustRotation", "Trust Rotation", 2.0, "deg", "信赖域的旋转上限。"),
      numParam("degenerateRatio", "Degenerate Ratio", 1.0e-3, "", "退化方向锁定的特征值比阈值。"),
      roiParam("roiFlushBase", "Flush Base ROI"),
      roiParam("roiGapLeft", "Gap Left ROI"),
      roiParam("roiFlushRef", "Flush Ref ROI"),
      roiParam("roiGapRight", "Gap Right ROI"),
  };
  op.capabilities = {false, false, true};
  op.compute = &alignTemplate;
  r.addOperator(std::move(op));
}

void registerSelectAlignment(Registry& r) {
  OperatorDesc op;
  op.id = "gap.select_alignment";
  op.version = "1.0.0";
  op.label = "Select Alignment";
  op.category = "Gap/Align";
  op.keywords = {"template", "select", "best icp", "模板选择"};
  op.doc =
      "在候选模板里挑一个：先过滤左右都 ≥ minScore 的，"
      "再按 min(l,r) 降、mean 降、配置顺序升、id 升排序取第一个（§3.4）。";
  op.inputs = {
      Port{"a", "Record", "A", "第一个候选。", true},
      Port{"b", "Record", "B", "第二个候选。", false},
      Port{"c", "Record", "C", "第三个候选。", false},
      Port{"d", "Record", "D", "第四个候选。", false},
  };
  op.outputs = {Port{"alignment", "Record", "Alignment", "选中的那一份。", true}};
  op.params = {intParam("minScore", "Min Score", 60, "低于它的候选一律不要。")};
  op.capabilities = {false, true, true};
  op.compute = &selectAlignment;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
