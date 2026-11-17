#include "profile_migration.h"
#include "profile_template.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

#include "algo/cloud2d.h"
#include "gap_detection/Alignment.hpp"
#include "gap_detection/GapUtils.hpp"
#include "lyflow/hash.h"
#include "lyflow/profile.h"
#include "lyflow_profile/provenance.h"
#include "profile_scan.h"

namespace lyflow::packs::gap::profile_template {
namespace {
using Json = nlohmann::json;
namespace dom = ::detection::domain;
using GapCloud = ::PointCloud;
constexpr double kScale = 1000.0;
constexpr double kPi = 3.14159265358979323846;
Status failure(const std::string& message, const char* port = "templates",
               const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}
Status cancelled() { return failure("模板配准已取消", "", "cancelled"); }
std::vector<double> matrixJson(const Eigen::Matrix3f& matrix) {
  std::vector<double> out(9);
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
      out[static_cast<std::size_t>(row * 3 + column)] = matrix(row, column);
  return out;
}
GapCloud nativeCloud(const PointCloud& cloud) {
  auto result = std_pc::toCloud2D(cloud);
  result.is_dense = false;
  return result;
}
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
  const double rotationDeg = static_cast<double>(delta.rotationRad) * 180.0 / kPi;
  *rawTranslationMm = translationMm;
  *rawRotationDeg = rotationDeg;
  *clamped = false;
  const bool translationExceeds = translationMm > maxTranslationMm;
  const bool rotationExceeds = std::fabs(rotationDeg) > maxRotationDeg;
  if (!translationExceeds && !rotationExceeds)
    return transform;

  *clamped = true;
  Eigen::Vector2f clampedTranslation = delta.translation;
  if (translationExceeds && translationMm > 0) {
    clampedTranslation *= static_cast<float>(maxTranslationMm / translationMm);
  }
  double clampedRotationDeg = rotationDeg;
  if (rotationExceeds)
    clampedRotationDeg = std::copysign(maxRotationDeg, rotationDeg);
  return composeDelta(reference, clampedTranslation,
                      static_cast<float>(clampedRotationDeg * kPi / 180.0));
}

// ---------------------------------------------------------------------------

dom::AlignmentConfiguration configFromOptions(const AlignmentOptions& p) {
  dom::AlignmentConfiguration c;
  c.method = "ICP";
  c.save_result = false;
  c.using_camera = "Both";
  c.icp.max_matching_distance = p.maxMatchingDist;
  c.icp.max_fitness_distance = p.maxFitnessDist;
  c.icp.max_iteration_count = static_cast<int>(p.maxIterations);
  c.icp.minimum_score = static_cast<int>(p.minScore);
  c.icp.neighbor_count = static_cast<int>(p.normalKnn);
  c.icp.bidirectional = p.bidirection;
  c.robustness.degenerate_ratio = p.degenerateRatio;
  c.robustness.global_coarse = p.globalCoarse;
  c.robustness.trust_region.max_translation_mm = p.trustTranslation;
  c.robustness.trust_region.max_rotation_deg = p.trustRotation;
  return c;
}

dom::InitialPoseMode poseModeFromOptions(const AlignmentOptions& p) {
  const std::string mode = p.initialPoseMode;
  if (mode == "centroid")
    return dom::InitialPoseMode::kCentroidTranslation;
  if (mode == "pca")
    return dom::InitialPoseMode::kPcaRigidCentroid;
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
      {"transform", matrixJson(s.outcome.transform)},
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

} // namespace

Status alignClouds(const PointCloud& targetCloud, const PointCloud& leftCloud,
                   const PointCloud& rightCloud, const AlignmentOptions& options, Record& result,
                   ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  const GapCloud target = nativeCloud(targetCloud);
  const GapCloud tplLeft = nativeCloud(leftCloud);
  const GapCloud tplRight = nativeCloud(rightCloud);
  if (target.empty() || tplLeft.empty() || tplRight.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "点云或模板是空的", {}, "cloud");
  }

  PackAlignment aligner;
  aligner.setConfiguration(configFromOptions(options));
  const double trustTranslation = options.trustTranslation;
  const double trustRotation = options.trustRotation;
  const bool globalCoarse = options.globalCoarse;
  const bool successGuide = options.successGuide;
  const bool segRoi = options.segRoi;

  const auto evaluateSide = [&](const GapCloud& source, const Eigen::Matrix3f& initialPose,
                                const Eigen::Matrix3f& trustReference) {
    SideOutcome side;
    if (ctx.cancelled())
      return side;
    side.outcome = aligner.registerCloud2DICPOutcome(source, target, initialPose);
    if (side.outcome.success) {
      side.outcome.transform =
          applyTrustRegion(trustReference, side.outcome.transform, trustTranslation, trustRotation,
                           &side.trustTranslationMm, &side.trustRotationDeg, &side.trustClamped);
    }
    return side;
  };

  const auto initial = ::detection::utils::computeInitialPose(tplLeft, tplRight, target, target,
                                                              poseModeFromOptions(options));
  Eigen::Matrix3f sideInitialPose = initial.transform;
  bool globalSucceeded = false;
  double globalScore = std::numeric_limits<double>::quiet_NaN();
  if (globalCoarse) {
    GapCloud mergedTemplate(tplLeft);
    mergedTemplate += tplRight;
    // seg_mode ROI 下左右两片目标云是同一片，这里把它加了两遍：
    // 全局粗配的目标点数因此翻倍，与模板那一侧（左右各一片）的点数量级对齐。
    GapCloud mergedTarget(target);
    mergedTarget += target;
    PackAlignment globalAligner;
    globalAligner.setConfiguration(configFromOptions(options));
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
    if (left.outcome.success)
      right = evaluateSide(tplRight, sideInitialPose, trustReference);
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

  if (ctx.cancelled())
    return cancelled();
  lyflow::Record record;
  record.type = "GapAlignment";
  record.data = nlohmann::json{
      {"templateId", options.templateId},
      {"order", options.order},
      {"minScore", options.minScore},
      {"globalCoarse", globalCoarse},
      {"globalSucceeded", globalSucceeded},
      {"globalScore", std::isfinite(globalScore) ? nlohmann::json(globalScore) : nlohmann::json()},
      {"mirrored", mirrored},
      {"left", sideJson(left)},
      {"right", sideJson(right)},
  };
  static const char* kKeys[4] = {"flushBase", "gapLeft", "flushRef", "gapRight"};
  nlohmann::json rois = nlohmann::json::object();
  for (int i = 0; i < 4; ++i) {
    const std::array<float, 4> v = options.roisMm[static_cast<std::size_t>(i)];
    rois[kKeys[i]] = {v[0], v[1], v[2], v[3]};
  }
  record.data["rois"] = rois;
  // 这个槽的四框从哪来（docs/result-bundle-plan.md (b)）：候选自带 rois = template，
  // 只回放配置里的全局框 = config —— 与原算法的 quality.roi_source 同一个判据
  record.data["roisFrom"] = options.roisFrom;

  ctx.log(LogLevel::Info,
          "ICP " + options.templateId + ": left=" + std::to_string(left.outcome.score) +
              " right=" + std::to_string(right.outcome.score) + (mirrored ? " (mirrored)" : ""));
  if (ctx.cancelled())
    return cancelled();
  result = std::move(record);
  return Status::Ok();
}

namespace {

Param intParam(const char* name, const char* label, std::int64_t def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  return p;
}

Param numParam(const char* name, const char* label, double def, const char* unit, const char* doc) {
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
  // 框在模板坐标系里，底图就是喂进来的左右模板（m8b 验收遗留的那一条）：模板是输入端口不是文件，
  // 所以底图取自 tplLeft / tplRight 上游那一次运行的结果
  p.semantic = "roi";
  p.roiBackdrop.inputPorts = {"tplLeft", "tplRight"};
  return p;
}

} // namespace

std::vector<Param> alignmentParameters() {
  Param templateId;
  templateId.name = "templateId";
  templateId.type = ParamType::String;
  templateId.label = "Template Id";
  templateId.def = Value::text("primary");

  Param roisFrom;
  roisFrom.name = "roisFrom";
  roisFrom.type = ParamType::Enum;
  roisFrom.label = "ROIs From";
  roisFrom.doc =
      "这个槽的四个框从哪来，进 GapAlignment.roisFrom，结果汇总的 roi_source 取它："
      "template = 候选自带 rois，config = 候选没带、回放配置里的全局框。导入器按配置写。";
  roisFrom.def = Value::text("template");
  roisFrom.advanced = true;
  roisFrom.options = {EnumOption{"template", "Template", "候选自带 rois。"},
                      EnumOption{"config", "Config", "候选没带，用配置里的全局框。"}};

  Param poseMode;
  poseMode.name = "initialPoseMode";
  poseMode.type = ParamType::Enum;
  poseMode.label = "Initial Pose";
  poseMode.def = Value::text("endpoint_midpoint");
  poseMode.advanced = true;
  poseMode.options = {EnumOption{"endpoint_midpoint", "Endpoint Midpoint", ""},
                      EnumOption{"centroid", "Centroid", ""},
                      EnumOption{"pca", "PCA Rigid Centroid", ""}};

  return {
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
      roisFrom,
  };
}

std::vector<Param> selectionParameters() {
  return {intParam("minScore", "Min Score", 60, "低于它的候选一律不要。")};
}

AlignmentOptions alignmentOptions(const ParamView& p) {
  AlignmentOptions out;
  out.templateId = p.text("templateId");
  out.initialPoseMode = p.choice("initialPoseMode");
  out.order = p.integer("order");
  out.maxMatchingDist = p.number("maxMatchingDist");
  out.maxFitnessDist = p.number("maxFitnessDist");
  out.maxIterations = static_cast<int>(p.integer("maxIterations"));
  out.normalKnn = static_cast<int>(p.integer("normalKnn"));
  out.minScore = static_cast<int>(p.integer("minScore"));
  out.bidirection = p.flag("bidirection");
  out.globalCoarse = p.flag("globalCoarse");
  out.successGuide = p.flag("successGuide");
  out.segRoi = p.flag("segRoi");
  out.trustTranslation = p.number("trustTranslation");
  out.trustRotation = p.number("trustRotation");
  out.degenerateRatio = p.number("degenerateRatio");
  constexpr std::array<const char*, 4> names{"roiFlushBase", "roiGapLeft", "roiFlushRef",
                                             "roiGapRight"};
  for (std::size_t i = 0; i < names.size(); ++i)
    out.roisMm[i] = p.vec4(names[i]);
  out.roisFrom = p.choice("roisFrom");
  return out;
}
TemplateLoadOptions loadOptions(const ParamView& p) {
  TemplateLoadOptions out;
  out.dir = p.path("dir");
  out.left = p.text("left");
  out.right = p.text("right");
  out.layout = p.choice("layout");
  out.unit = p.choice("unit");
  out.fileFrame = p.text("fileFrame");
  out.templateFrame = p.text("templateFrame");
  out.handedness = p.choice("handedness");
  out.leftSourceId = p.text("leftSourceId");
  out.rightSourceId = p.text("rightSourceId");
  return out;
}
namespace {
bool fail(std::string& why, const char* message) {
  why = message;
  return false;
}
bool keys(const Json& data, std::initializer_list<const char*> names) {
  if (!data.is_object() || data.size() != names.size())
    return false;
  for (const auto* name : names)
    if (!data.contains(name))
      return false;
  return true;
}
bool exactBundle(const Bundle* bundle, const char* kind, std::initializer_list<const char*> names) {
  if (!bundle || bundle->kind != kind || bundle->fields.size() != names.size())
    return false;
  for (const auto* name : names)
    if (!bundle->field(name))
      return false;
  return true;
}
bool metricProfile(const Data& data, const Profile*& profile, std::string& why) {
  profile = data.asProfile();
  return profile && profile->descriptor.unit == "m" && profile->descriptor.plane == "XY"
             ? true
             : fail(why, "需要明确米制XY原生Profile");
}
std::string bindingHash(Json value) {
  value.erase("bindingHash");
  HashBuilder hash;
  hash.add("gap-template-contract/v2");
  hash.add(value.dump());
  return hash.hex();
}
std::string recordHash(const Record& record) {
  HashBuilder hash;
  hash.add(record.type);
  hash.add(record.data.dump());
  return hash.hex();
}
std::string fileStamp(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error)
    return {};
  const auto time = std::filesystem::last_write_time(path, error);
  return error ? "" : std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
}
bool activeCloud(const Data& data, PointCloud& result, std::string& why, ExecContext& context) {
  const Profile* profile = nullptr;
  if (!metricProfile(data, profile, why))
    return false;
  std::vector<std::int32_t> selected;
  if (profile->pointCount() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    return fail(why, "配准原槽超过int32索引预算");
  for (std::size_t i = 0; i < profile->pointCount(); ++i) {
    if ((i & 8191U) == 0 && context.cancelled())
      return fail(why, "配准选槽已取消");
    if (profile->validity[i])
      selected.push_back(static_cast<std::int32_t>(i));
  }
  if (selected.empty())
    return fail(why, "点云或模板没有有效观测");
  result = profile->cloud->select(selected);
  return true;
}
bool matrix(const Json& values, Eigen::Matrix3f& result, std::string& why) {
  if (!values.is_array() || values.size() != 9)
    return fail(why, "配准矩阵必须为9个有限数值");
  Eigen::Matrix3f candidate;
  for (int i = 0; i < 9; ++i) {
    if (!values[i].is_number())
      return fail(why, "配准矩阵字段不是数值");
    const auto v = values[i].get<double>();
    if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max())
      return fail(why, "配准矩阵不可表示");
    candidate(i / 3, i % 3) = static_cast<float>(v);
  }
  const auto rotation = candidate.block<2, 2>(0, 0).eval();
  if (std::abs(candidate(2, 0)) > 1e-6F || std::abs(candidate(2, 1)) > 1e-6F ||
      std::abs(candidate(2, 2) - 1) > 1e-6F ||
      !((rotation.transpose() * rotation).isApprox(Eigen::Matrix2f::Identity(), 1e-3F)) ||
      std::abs(rotation.determinant() - 1) > 1e-3F)
    return fail(why, "模板配准只接受有限SE2刚体矩阵");
  result = candidate;
  return true;
}
bool transformed(const Data& original, const Eigen::Matrix3f& pose, const std::string& frame,
                 Data& result, std::string& why, ExecContext* context = nullptr) {
  const Profile* input = nullptr;
  if (!metricProfile(original, input, why))
    return false;
  Profile output = *input;
  PointCloud cloud = *input->cloud;
  PointCloud identity;
  cloud.id = identity.id;
  for (std::size_t i = 0; i < input->pointCount(); ++i) {
    if ((i & 8191U) == 0 && context && context->cancelled())
      return fail(why, "模板变换已取消");
    if (!input->validity[i])
      continue;
    const Eigen::Vector3f moved = pose * Eigen::Vector3f(cloud.xyz[3 * i], cloud.xyz[3 * i + 1], 1);
    if (!moved.allFinite())
      return fail(why, "模板变换坐标超出float32范围");
    cloud.xyz[3 * i] = moved.x();
    cloud.xyz[3 * i + 1] = moved.y();
    if (cloud.hasNormals()) {
      const Eigen::Vector2f normal =
          pose.block<2, 2>(0, 0) * Eigen::Vector2f(cloud.normals[3 * i], cloud.normals[3 * i + 1]);
      if (!normal.allFinite() || !std::isfinite(cloud.normals[3 * i + 2]))
        return fail(why, "模板法线必须有限且可变换");
      cloud.normals[3 * i] = normal.x();
      cloud.normals[3 * i + 1] = normal.y();
    }
  }
  output.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  output.descriptor.frame = frame;
  result = Data::profile(std::move(output));
  return true;
}
bool sideValid(const Json& value, std::string& why) {
  Eigen::Matrix3f transform;
  if (!keys(value,
            {"transform", "score", "success", "bidirectional", "degenerateRatio",
             "degenerateLocked", "trustTranslationMm", "trustRotationDeg", "trustClamped"}) ||
      !matrix(value["transform"], transform, why) || !value["success"].is_boolean() ||
      !value["bidirectional"].is_boolean() || !value["degenerateLocked"].is_boolean() ||
      !value["trustClamped"].is_boolean())
    return fail(why, "配准侧结果或刚体矩阵非法");
  if ((value["score"].is_null() && value["success"] != false) ||
      (!value["score"].is_null() &&
       (!value["score"].is_number() || !std::isfinite(value["score"].get<double>()))))
    return fail(why, "配准侧分数必须为有限数值；未执行且失败的一侧可明确null");
  for (const auto* name : {"degenerateRatio", "trustTranslationMm", "trustRotationDeg"})
    if (!value[name].is_null() &&
        (!value[name].is_number() || !std::isfinite(value[name].get<double>())))
      return fail(why, "配准质量必须为有限数值或明确null");
  return true;
}

bool rois(const Json& value, std::array<std::array<float, 4>, 4>& result, std::string& why) {
  constexpr std::array<const char*, 4> names{"flushBase", "gapLeft", "flushRef", "gapRight"};
  if (!keys(value, {"flushBase", "gapLeft", "flushRef", "gapRight"}))
    return fail(why, "配准记录缺少明确四框");
  for (std::size_t i = 0; i < 4; ++i) {
    const auto& bounds = value[names[i]];
    if (!bounds.is_array() || bounds.size() != 4)
      return fail(why, "模板框必须为四个毫米边界");
    for (int j = 0; j < 4; ++j) {
      if (!bounds[j].is_number() || !std::isfinite(bounds[j].get<double>()) ||
          std::abs(bounds[j].get<double>()) > std::numeric_limits<float>::max())
        return fail(why, "模板框边界不可表示");
      result[i][j] = bounds[j].get<float>();
    }
    if (result[i][0] > result[i][2] || result[i][1] > result[i][3])
      return fail(why, "模板框范围倒置");
  }
  return true;
}
} // namespace
std::string templateFingerprint(const TemplateLoadOptions& options) {
  if (options.dir.empty())
    return {};
  return fileStamp(options.dir / options.left) + "|" + fileStamp(options.dir / options.right);
}
bool makeTemplatePair(const Data& left, const Data& right, const Json& origins, Data& result,
                      std::string& why) {
  try {
    const Profile *a = nullptr, *b = nullptr;
    if (!metricProfile(left, a, why) || !metricProfile(right, b, why) ||
        !profile_contract::sameSpace(a->descriptor, b->descriptor) ||
        std::find(a->validity.begin(), a->validity.end(), 1) == a->validity.end() ||
        std::find(b->validity.begin(), b->validity.end(), 1) == b->validity.end())
      return fail(why, "模板需要同空间且包含有效观测的米制XY Profile");
    if (!keys(origins, {"left", "right"}) || origins.dump().size() > 32768)
      return fail(why, "模板需要有界的实际双路来源");
    for (const auto& item : {std::make_pair("left", a), std::make_pair("right", b)}) {
      const auto& origin = origins[item.first];
      if (!keys(origin, {"kind", "sourceId", "details"}) ||
          (origin["kind"] != "file" && origin["kind"] != "injected_profile" &&
           origin["kind"] != "profile_declaration") ||
          origin["sourceId"] != item.second->descriptor.sourceId || !origin["details"].is_object())
        return fail(why, "模板来源不得用逻辑角色改写物理源身份");
    }
    Json info{{"schemaVersion", 2},
              {"templateFrame", a->descriptor.frame},
              {"contexts", {profile_contract::context(left), profile_contract::context(right)}},
              {"origins", origins}};
    info["bindingHash"] = bindingHash(info);
    Bundle bundle(kTemplatePairKind);
    bundle.set("left", left)
        .set("right", right)
        .set("info", Data::record({kTemplateInfoType, info}));
    result = Data::bundle(std::move(bundle));
    why.clear();
    return true;
  } catch (const Json::exception&) {
    return fail(why, "模板来源或元数据类型非法");
  }
}
bool readTemplatePair(const Data& data, TemplateView& result, std::string& why) {
  try {
    const auto* bundle = data.asBundle();
    if (!exactBundle(bundle, kTemplatePairKind, {"left", "right", "info"}))
      return fail(why, "需要完整gap.TemplatePair.v2");
    const auto* info = bundle->field("info")->asRecord();
    if (!info || info->type != kTemplateInfoType ||
        !keys(info->data,
              {"schemaVersion", "templateFrame", "contexts", "origins", "bindingHash"}) ||
        !info->data["schemaVersion"].is_number_integer() || info->data["schemaVersion"] != 2 ||
        !info->data["contexts"].is_array() || info->data["contexts"].size() != 2)
      return fail(why, "模板版本或来源字段非法");
    Data expected;
    if (!makeTemplatePair(*bundle->field("left"), *bundle->field("right"), info->data["origins"],
                          expected, why) ||
        expected.asBundle()->field("info")->asRecord()->data != info->data)
      return fail(why, "模板原槽或通道与绑定信息不匹配");
    result = {bundle->field("left"), bundle->field("right"), info};
    why.clear();
    return true;
  } catch (const Json::exception&) {
    return fail(why, "模板信息类型或编码非法");
  }
}
Status loadTemplatePair(const TemplateLoadOptions& options, Data& result, ExecContext& context) {
  if (options.dir.empty())
    return failure("模板目录未设置", "", "bad_param");
  profile_scene::ProfilePairOptions loader;
  loader.source = "files";
  loader.primaryFile = options.dir / options.left;
  loader.secondaryFile = options.dir / options.right;
  loader.layout = options.layout;
  loader.unit = options.unit;
  loader.fileFrame = options.fileFrame;
  loader.sensorFrame = options.templateFrame;
  loader.handedness = options.handedness;
  loader.primarySourceId = options.leftSourceId;
  loader.secondarySourceId = options.rightSourceId;
  profile_scene::LoadedProfilePair loaded;
  auto status = profile_scene::loadProfilePair(loader, loaded, context);
  if (!status.ok)
    return status;
  std::array<Data, 2> metric;
  Json origins = Json::object();
  std::string why;
  std::size_t index = 0;
  for (const auto& item : {std::make_tuple("left", "primary", &loaded.primary),
                           std::make_tuple("right", "secondary", &loaded.secondary)}) {
    Profile axes, converted, fileProfile;
    const auto stop = [&context] { return context.cancelled(); };
    if (!remapProfileAxes(*std::get<2>(item)->asProfile(), "XZY", {1, 1, 1}, options.templateFrame,
                          axes, why, stop) ||
        !convertProfileUnit(axes, "m", converted, why, stop) ||
        !remapProfileAxes(*std::get<2>(item)->asProfile(),
                          options.layout == "profile" ? "XZY" : "XYZ", {1, 1, 1}, options.fileFrame,
                          fileProfile, why, stop))
      return context.cancelled() ? cancelled() : failure(why);
    metric[index++] = Data::profile(std::move(converted));
    auto origin = loaded.sourceInfo.data["origins"][std::get<1>(item)];
    origin["details"]["fileContext"] =
        profile_contract::context(Data::profile(std::move(fileProfile)));
    origins[std::get<0>(item)] = std::move(origin);
  }
  Data candidate;
  if (!makeTemplatePair(metric[0], metric[1], origins, candidate, why))
    return failure(why, "templates", "io");
  if (context.cancelled())
    return cancelled();
  result = std::move(candidate);
  return Status::Ok();
}

namespace {
bool alignmentInfo(const Json& info, std::string& why) {
  if (!keys(info, {"templateId", "order", "minScore", "globalCoarse", "globalSucceeded",
                   "globalScore", "mirrored", "left", "right", "rois", "roisFrom", "schemaVersion",
                   "targetContext", "templateContexts", "alignedContexts", "templateInfo",
                   "targetProvenanceHash", "invalidSlots", "bindingHash"}) ||
      !info["schemaVersion"].is_number_integer() || info["schemaVersion"] != 2 ||
      !info["templateId"].is_string() || info["templateId"].get_ref<const std::string&>().empty() ||
      info["templateId"].get_ref<const std::string&>().size() > 1024 ||
      !info["order"].is_number_integer() || info["order"] < 0 ||
      !info["minScore"].is_number_integer() || info["minScore"] < 0 || info["minScore"] > 100 ||
      !info["globalCoarse"].is_boolean() || !info["globalSucceeded"].is_boolean() ||
      !info["mirrored"].is_boolean() ||
      (info["roisFrom"] != "template" && info["roisFrom"] != "config") ||
      info["invalidSlots"] != "preserved_si_untransformed_not_observations" ||
      !info["templateContexts"].is_array() || info["templateContexts"].size() != 2 ||
      !info["alignedContexts"].is_array() || info["alignedContexts"].size() != 2 ||
      !info["targetProvenanceHash"].is_string() || info["bindingHash"] != bindingHash(info) ||
      !sideValid(info["left"], why) || !sideValid(info["right"], why))
    return fail(why, "需要有界且完整绑定的版本2模板配准信息");
  if (!info["globalScore"].is_null() &&
      (!info["globalScore"].is_number() || !std::isfinite(info["globalScore"].get<double>())))
    return fail(why, "全局ICP分必须为有限数值或明确null");
  std::array<std::array<float, 4>, 4> bounds;
  return rois(info["rois"], bounds, why) && info.dump().size() <= 131072;
}
bool optionsValid(const AlignmentOptions& options, std::string& why) {
  for (const auto v : {options.maxMatchingDist, options.maxFitnessDist, options.trustTranslation,
                       options.trustRotation, options.degenerateRatio})
    if (v < 0)
      return fail(why, "配准距离、信赖域和退化阈值必须为非负数");
  if (options.maxMatchingDist <= 0 || options.maxFitnessDist <= 0 || options.maxIterations < 1 ||
      options.normalKnn < 1 || options.minScore < 0 || options.minScore > 100 ||
      options.order < 0 || options.templateId.empty() || options.templateId.size() > 1024)
    return fail(why, "模板配准选项非法");
  for (const auto& box : options.roisMm)
    if (box[0] > box[2] || box[1] > box[3])
      return fail(why, "模板框范围倒置");
  return true;
}
}
bool readAlignment(const Data& data, AlignmentView& result, std::string& why) {
  try {
    const auto* bundle = data.asBundle();
    if (!exactBundle(bundle, kAlignmentKind,
                     {"target", "targetProvenance", "templateLeft", "templateRight", "alignedLeft",
                      "alignedRight", "info"}))
      return fail(why, "需要完整gap.Alignment.v2原生Bundle");
    const auto* info = bundle->field("info")->asRecord();
    const auto* provenance = bundle->field("targetProvenance")->asRecord();
    if (!info || info->type != kAlignmentInfoType || !provenance || !alignmentInfo(info->data, why))
      return fail(why, "模板配准来源字段非法");
    const auto& v = info->data;
    const Profile *target = nullptr, *left = nullptr, *right = nullptr;
    if (!metricProfile(*bundle->field("target"), target, why) ||
        !metricProfile(*bundle->field("templateLeft"), left, why) ||
        !metricProfile(*bundle->field("templateRight"), right, why) ||
        !profile_contract::matches(*bundle->field("target"), v["targetContext"], why) ||
        !profile_contract::matches(*bundle->field("templateLeft"), v["templateContexts"][0], why) ||
        !profile_contract::matches(*bundle->field("templateRight"), v["templateContexts"][1],
                                   why) ||
        left->descriptor.handedness != target->descriptor.handedness ||
        right->descriptor.handedness != target->descriptor.handedness ||
        v["targetProvenanceHash"] != recordHash(*provenance))
      return fail(why, "配准当前载荷与原始模板或扫描来源不匹配");
    Json sources;
    if (!profile_contract::profileSources(*bundle->field("target"), provenance, sources, why))
      return false;
    Bundle templates(kTemplatePairKind);
    templates.set("left", *bundle->field("templateLeft"))
        .set("right", *bundle->field("templateRight"))
        .set("info", Data::record({kTemplateInfoType, v["templateInfo"]}));
    TemplateView templateView;
    if (!readTemplatePair(Data::bundle(std::move(templates)), templateView, why))
      return false;
    std::size_t index = 0;
    for (const auto& pair : {std::make_tuple("left", "templateLeft", "alignedLeft"),
                             std::make_tuple("right", "templateRight", "alignedRight")}) {
      Eigen::Matrix3f pose;
      Data moved;
      const auto* aligned = bundle->field(std::get<2>(pair));
      const Profile* alignedProfile = nullptr;
      if (!matrix(v[std::get<0>(pair)]["transform"], pose, why) ||
          !transformed(*bundle->field(std::get<1>(pair)), pose, target->descriptor.frame, moved,
                       why) ||
          !metricProfile(*aligned, alignedProfile, why) ||
          !profile_contract::sameSpace(target->descriptor, alignedProfile->descriptor) ||
          !profile_contract::matches(*aligned, v["alignedContexts"][index], why) ||
          profile_contract::context(moved) != v["alignedContexts"][index])
        return fail(why, "配准后模板必须实际应用刚体矩阵并保留原槽与通道");
      ++index;
    }
    result = {bundle->field("target"),
              provenance,
              bundle->field("templateLeft"),
              bundle->field("templateRight"),
              bundle->field("alignedLeft"),
              bundle->field("alignedRight"),
              info};
    why.clear();
    return true;
  } catch (const Json::exception&) {
    return fail(why, "配准信息字段类型或编码非法");
  }
}
Status align(const Data& scan, const Data& templates, const AlignmentOptions& options, Data& result,
             ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  std::string why;
  profile_scene::ScanView scanView;
  TemplateView templateView;
  if (!optionsValid(options, why))
    return failure(why, "", "bad_param");
  if (!profile_scene::readScanPair(scan, scanView, why))
    return failure(why, "scan");
  if (!readTemplatePair(templates, templateView, why))
    return failure(why);
  if (templateView.left->asProfile()->descriptor.handedness !=
      scanView.merged->asProfile()->descriptor.handedness)
    return failure("模板与量测轮廓手性不一致，必须显式转换");
  PointCloud target, left, right;
  if (!activeCloud(*scanView.merged, target, why, context) ||
      !activeCloud(*templateView.left, left, why, context) ||
      !activeCloud(*templateView.right, right, why, context))
    return context.cancelled() ? cancelled() : failure(why);
  Record legacy;
  const auto status = alignClouds(target, left, right, options, legacy, context);
  if (!status.ok)
    return status;
  std::array<Data, 2> aligned;
  for (std::size_t i = 0; i < 2; ++i) {
    Eigen::Matrix3f pose;
    if (!matrix(legacy.data[i == 0 ? "left" : "right"]["transform"], pose, why) ||
        !transformed(i == 0 ? *templateView.left : *templateView.right, pose,
                     scanView.merged->asProfile()->descriptor.frame, aligned[i], why, &context))
      return context.cancelled() ? cancelled() : failure(why, "alignment", "invalid_result");
  }
  auto info = legacy.data;
  for (const auto* side : {"left", "right"})
    if (info[side]["score"].is_number() && !std::isfinite(info[side]["score"].get<double>()))
      info[side]["score"] = nullptr;
  info["schemaVersion"] = 2;
  info["targetContext"] = profile_contract::context(*scanView.merged);
  info["templateContexts"] = {profile_contract::context(*templateView.left),
                              profile_contract::context(*templateView.right)};
  info["alignedContexts"] = {profile_contract::context(aligned[0]),
                             profile_contract::context(aligned[1])};
  info["templateInfo"] = templateView.info->data;
  info["targetProvenanceHash"] = recordHash(*scanView.mergedProvenance);
  info["invalidSlots"] = "preserved_si_untransformed_not_observations";
  info["bindingHash"] = bindingHash(info);
  Bundle candidate(kAlignmentKind);
  candidate.set("target", *scanView.merged)
      .set("targetProvenance", Data::record(*scanView.mergedProvenance))
      .set("templateLeft", *templateView.left)
      .set("templateRight", *templateView.right)
      .set("alignedLeft", aligned[0])
      .set("alignedRight", aligned[1])
      .set("info", Data::record({kAlignmentInfoType, info}));
  Data output = Data::bundle(std::move(candidate));
  AlignmentView checked;
  if (!readAlignment(output, checked, why))
    return failure(why, "alignment", "invalid_result");
  if (context.cancelled())
    return cancelled();
  result = std::move(output);
  return Status::Ok();
}
Status select(const std::vector<Data>& candidates, int minScore, Data& result,
              ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  if (candidates.empty() || candidates.size() > 4 || minScore < 0 || minScore > 100)
    return failure("需要1到4个候选和0到100的最低分", "a");
  std::vector<::detection::utils::TemplateIcpScore> scores;
  Json target;
  std::string why;
  for (const auto& candidate : candidates) {
    AlignmentView view;
    if (!readAlignment(candidate, view, why))
      return failure(why, "alignment");
    const auto& v = view.info->data;
    if (target.is_null())
      target = v["targetContext"];
    else if (target != v["targetContext"])
      return failure("配准候选不属于同一实际扫描载荷", "alignment");
    ::detection::utils::TemplateIcpScore score;
    score.id = v["templateId"].get<std::string>();
    score.left = v["left"]["score"].is_null() ? std::numeric_limits<double>::quiet_NaN()
                                              : v["left"]["score"].get<double>();
    score.right = v["right"]["score"].is_null() ? std::numeric_limits<double>::quiet_NaN()
                                                : v["right"]["score"].get<double>();
    score.configuration_order = v["order"].get<std::size_t>();
    scores.push_back(std::move(score));
    if (context.cancelled())
      return cancelled();
  }
  const auto selected = ::detection::utils::selectBestTemplateByIcp(scores, minScore);
  if (!selected)
    return failure("所有候选模板的ICP分都低于 " + std::to_string(minScore), "alignment",
                   "icp_score_low");
  context.log(LogLevel::Info, "选中模板 " + scores[*selected].id);
  if (context.cancelled())
    return cancelled();
  result = candidates[*selected];
  return Status::Ok();
}
Status transformBusinessRois(const Record& alignment, const std::string& datumSide,
                             std::array<Box2D, 4>& boxes, Box2D& seam, std::string& resolvedSide) {
  try {
    std::string why;
    if (alignment.type != "GapAlignment" && alignment.type != kAlignmentInfoType)
      return failure("输入不是模板配准结果", "alignment");
    std::array<std::array<float, 4>, 4> bounds;
    Eigen::Matrix3f left, right;
    const auto& v = alignment.data;
    if (!v.is_object() || !v.contains("rois") || !v.contains("left") || !v.contains("right") ||
        !rois(v["rois"], bounds, why) || !matrix(v["left"]["transform"], left, why) ||
        !matrix(v["right"]["transform"], right, why))
      return failure(why, "alignment");
    bool datumRight = datumSide == "right";
    if (datumSide == "auto") {
      const auto& original = v["rois"];
      const double datumX =
          .5 * (original["flushBase"][0].get<double>() + original["flushBase"][2].get<double>());
      const double seamX =
          .25 * (original["gapLeft"][0].get<double>() + original["gapLeft"][2].get<double>() +
                 original["gapRight"][0].get<double>() + original["gapRight"][2].get<double>());
      datumRight = datumX > seamX;
    }
    const Eigen::Matrix3f* transforms[4] = {datumRight ? &right : &left, &left,
                                            datumRight ? &left : &right, &right};
    std::array<Box2D, 4> moved;
    for (std::size_t i = 0; i < 4; ++i) {
      const auto& r = bounds[i];
      const float x0 = r[0] / 1000.0F, x1 = r[2] / 1000.0F, y0 = r[1] / 1000.0F,
                  y1 = r[3] / 1000.0F;
      const float halfW = .5F * (x1 - x0), halfH = .5F * (y1 - y0);
      const Eigen::Vector3f center =
          *transforms[i] * Eigen::Vector3f(.5F * (x0 + x1), .5F * (y0 + y1), 1.0F);
      moved[i].min[0] = center.x() - halfW;
      moved[i].min[1] = center.y() - halfH;
      moved[i].max[0] = center.x() + halfW;
      moved[i].max[1] = center.y() + halfH;
      if (!profile_contract::validGeometryV2(Data::box2d(moved[i]), why))
        return failure(why, "alignment", "invalid_result");
    }
    const double lx = .5 * (static_cast<double>(moved[1].min[0]) + moved[1].max[0]);
    const double ly = .5 * (static_cast<double>(moved[1].min[1]) + moved[1].max[1]);
    const double rx = .5 * (static_cast<double>(moved[3].min[0]) + moved[3].max[0]);
    const double ry = .5 * (static_cast<double>(moved[3].min[1]) + moved[3].max[1]);
    Box2D seamBox;
    seamBox.min[0] = seamBox.max[0] = static_cast<float>((lx + rx) * .5);
    seamBox.min[1] = seamBox.max[1] = static_cast<float>((ly + ry) * .5);
    if (!profile_contract::validGeometryV2(Data::box2d(seamBox), why))
      return failure(why, "alignment", "invalid_result");
    boxes = moved;
    seam = seamBox;
    resolvedSide = datumRight ? "right" : "left";
    return Status::Ok();
  } catch (const Json::exception&) {
    return failure("模板框或配准矩阵字段类型非法", "alignment");
  }
}
Status businessRois(const Data& alignment, const std::string& datumSide,
                    profile_scene::RoiFields& result, Data& seam, Data& seamContext,
                    ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  AlignmentView view;
  std::string why, resolved;
  if (!readAlignment(alignment, view, why))
    return failure(why, "alignment");
  std::array<Box2D, 4> oldBoxes;
  Box2D seamBox;
  const auto status = transformBusinessRois(*view.info, datumSide, oldBoxes, seamBox, resolved);
  if (!status.ok)
    return status;
  Json sourceContexts;
  if (!profile_contract::profileSources(*view.target, view.targetProvenance, sourceContexts, why))
    return failure(why, "alignment");
  const bool right = resolved == "right";
  const Data* roles[4] = {right ? view.alignedRight : view.alignedLeft,
                          right ? view.alignedLeft : view.alignedRight, view.alignedLeft,
                          view.alignedRight};
  constexpr std::array<std::size_t, 4> mapping{0, 2, 1, 3};
  profile_scene::RoiFields candidate;
  for (std::size_t i = 0; i < 4; ++i) {
    auto sources = sourceContexts;
    sources.push_back(profile_contract::context(*roles[i]));
    candidate.boxes[i] = Data::box2d(oldBoxes[mapping[i]]);
    ProfileDescriptor sourceSpace;
    Json unique = Json::array();
    for (const auto& source : sources)
      if (std::find(unique.begin(), unique.end(), source) == unique.end())
        unique.push_back(source);
    if (!profile_contract::geometrySources(unique, sourceSpace, why))
      return failure(why, "alignment");
    const auto info = profile_contract::geometryContextV2FromSources(candidate.boxes[i], unique);
    if (info.is_null())
      return failure("模板角色框来源超过预算或空间不一致", "alignment");
    candidate.contexts[i] = Data::record({"profile.Geometry.v2", info});
  }
  auto seamSources = sourceContexts;
  seamSources.push_back(profile_contract::context(*view.alignedLeft));
  seamSources.push_back(profile_contract::context(*view.alignedRight));
  Data candidateSeam = Data::box2d(seamBox);
  ProfileDescriptor seamSpace;
  Json uniqueSeam = Json::array();
  for (const auto& source : seamSources)
    if (std::find(uniqueSeam.begin(), uniqueSeam.end(), source) == uniqueSeam.end())
      uniqueSeam.push_back(source);
  if (!profile_contract::geometrySources(uniqueSeam, seamSpace, why))
    return failure(why, "alignment");
  const auto candidateSeamInfo =
      profile_contract::geometryContextV2FromSources(candidateSeam, uniqueSeam);
  if (candidateSeamInfo.is_null())
    return failure("缝方向框来源超过预算或空间不一致", "alignment");
  Data candidateSeamContext = Data::record({"profile.Geometry.v2", candidateSeamInfo});
  const auto source = view.info->data["roisFrom"];
  candidate.info = {profile_scene::kRoiInfoType,
                    {{"schemaVersion", 2},
                     {"datumSide", resolved},
                     {"source", source},
                     {"roisFrom", source},
                     {"alignment", view.info->data},
                     {"overallMm", nullptr},
                     {"cropStatus", nullptr},
                     {"processing",
                      {{"method", "template"},
                       {"status", "complete"},
                       {"details", {{"templateId", view.info->data["templateId"]}}}}}}};
  Data checked;
  if (!profile_scene::makeRoiSet(candidate, checked, why))
    return failure(why, "alignment", "invalid_result");
  if (context.cancelled())
    return cancelled();
  result = std::move(candidate);
  seam = std::move(candidateSeam);
  seamContext = std::move(candidateSeamContext);
  return Status::Ok();
}

namespace {
Param textParameter(const char* name, const char* label, const char* value) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(value);
  return p;
}
Param choiceParameter(const char* name, const char* label, const char* value,
                      std::vector<EnumOption> options) {
  auto p = textParameter(name, label, value);
  p.type = ParamType::Enum;
  p.options = std::move(options);
  return p;
}
}
std::vector<Param> loadParameters() {
  auto dir = textParameter("dir", "模板目录", "");
  dir.type = ParamType::Path;
  dir.mode = "dir";
  return {dir,
          textParameter("left", "左模板文件", "f1_left.pcd"),
          textParameter("right", "右模板文件", "f1_right.pcd"),
          choiceParameter("layout", "文件坐标布局", "profile",
                          {{"profile", "轮廓XY", ""}, {"sensor", "传感器XZ", ""}}),
          choiceParameter("unit", "文件长度单位", "m", {{"m", "米", ""}, {"mm", "毫米", ""}}),
          textParameter("fileFrame", "文件坐标系", "template-file"),
          textParameter("templateFrame", "模板坐标系", "template-frame"),
          choiceParameter("handedness", "模板手性", "reflected",
                          {{"right", "右手", ""}, {"reflected", "反射手性", ""}}),
          textParameter("leftSourceId", "左模板来源标识", ""),
          textParameter("rightSourceId", "右模板来源标识", "")};
}
std::vector<Param> businessRoiParameters() {
  return {choiceParameter(
      "datumSide", "基准侧", "auto",
      {{"auto", "自动", "按原模板业务框位置判断。"}, {"left", "左侧", ""}, {"right", "右侧", ""}})};
}
void registerProfileTemplateOps(Registry& registry) {
  registry.addBundle({kTemplatePairKind,
                      "原生模板对",
                      "双路原槽模板和明确文件或宿主来源。",
                      "",
                      {{"left", "Profile", "左模板XY米制完整原槽。"},
                       {"right", "Profile", "右模板XY米制完整原槽。"},
                       {"info", "Record", "gap.TemplateInfo.v2文件及原始模板上下文。"}}});
  registry.addBundle({kAlignmentKind,
                      "原生模板配准",
                      "实际扫描、原模板、已变换模板和来源绑定。",
                      "",
                      {{"target", "Profile", "实际量测帧扫描轮廓。"},
                       {"targetProvenance", "Record", "扫描采样与引用来源。"},
                       {"templateLeft", "Profile", "原模板帧左轮廓。"},
                       {"templateRight", "Profile", "原模板帧右轮廓。"},
                       {"alignedLeft", "Profile", "实际变换后的量测帧左模板。"},
                       {"alignedRight", "Profile", "实际变换后的量测帧右模板。"},
                       {"info", "Record", "gap.AlignmentInfo.v2矩阵、候选与内容绑定。"}}});
  OperatorDesc load;
  load.id = "gap.load_template";
  load.version = "2.0.0";
  declareGapGraphUpgrade(load);
  load.label = "模板轮廓读取";
  load.category = "间隙/输入";
  load.keywords = {"template", "模板", "PCD", "profile"};
  load.doc = "读取保留有效掩码、原槽索引及全部支持通道的模板对，显式转换为模板XY米制坐标。";
  load.outputs = {
      {"templates", "Bundle<gap.TemplatePair.v2>", "模板对", "完整模板来源契约。", true},
      {"left", "Profile", "左模板", "原生左模板视图。", true},
      {"right", "Profile", "右模板", "原生右模板视图。", true}};
  load.params = loadParameters();
  load.capabilities = {true, false, true};
  load.externalKey =
      +[](const ParamView& params) { return templateFingerprint(loadOptions(params)); };
  load.compute = [](const Inputs&, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
    Data templates;
    auto status = loadTemplatePair(loadOptions(params), templates, ctx);
    if (!status.ok)
      return status;
    const auto* bundle = templates.asBundle();
    outputs.set("left", *bundle->field("left"));
    outputs.set("right", *bundle->field("right"));
    outputs.set("templates", std::move(templates));
    return Status::Ok();
  };
  registry.addOperator(std::move(load));
  OperatorDesc operation;
  operation.id = "gap.align_template";
  operation.version = "2.0.0";
  declareGapGraphUpgrade(operation);
  operation.label = "模板配准";
  operation.category = "间隙/配准";
  operation.keywords = {"template registration", "2D ICP", "模板对齐", "二维刚体配准"};
  operation.doc =
      "复用全局粗配、左右ICP、双向重试、退化锁定与信赖域策略；实际变换模板原槽并生成量测坐标来源。";
  operation.inputs = {
      {"scan", "Bundle<gap.ScanPair.v2>", "扫描轮廓对", "实际扫描来源。", true},
      {"templates", "Bundle<gap.TemplatePair.v2>", "模板对", "明确模板来源。", true}};
  operation.outputs = {{"alignment", "Bundle<gap.Alignment.v2>", "配准结果",
                        "矩阵、原模板和实际已变换轮廓。", true}};
  operation.params = alignmentParameters();
  for (auto& parameter : operation.params)
    if (parameter.semantic == "roi")
      parameter.roiBackdrop.inputPorts = {"templates.left", "templates.right"};
  operation.capabilities = {true, false, true};
  operation.compute = [](const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
    Data value;
    auto status =
        align(inputs.get("scan"), inputs.get("templates"), alignmentOptions(params), value, ctx);
    if (status.ok)
      outputs.set("alignment", std::move(value));
    return status;
  };
  registry.addOperator(std::move(operation));
  OperatorDesc selection;
  selection.id = "gap.select_alignment";
  selection.version = "2.0.0";
  declareGapGraphUpgrade(selection);
  selection.label = "配准结果选择";
  selection.category = "间隙/配准";
  selection.keywords = {"alignment selection", "best ICP", "模板优选", "配准评分"};
  selection.doc =
      "只接受同一实际扫描的1到4个候选，按最低侧分、均值、配置顺序及模板标识确定最优；全部低分时明确失败。";
  selection.inputs = {{"a", "Bundle<gap.Alignment.v2>", "候选A", "", true},
                      {"b", "Bundle<gap.Alignment.v2>", "候选B", "", false},
                      {"c", "Bundle<gap.Alignment.v2>", "候选C", "", false},
                      {"d", "Bundle<gap.Alignment.v2>", "候选D", "", false}};
  selection.outputs = {{"alignment", "Bundle<gap.Alignment.v2>", "选中配准", "", true}};
  selection.params = selectionParameters();
  selection.capabilities = {true, true, true};
  selection.compute = [](const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
    std::vector<Data> candidates;
    for (const auto* port : {"a", "b", "c", "d"})
      if (inputs.has(port))
        candidates.push_back(inputs.get(port));
    Data value;
    auto status = select(candidates, static_cast<int>(params.integer("minScore")), value, ctx);
    if (status.ok)
      outputs.set("alignment", std::move(value));
    return status;
  };
  registry.addOperator(std::move(selection));
  OperatorDesc roi;
  roi.id = "gap.business_rois";
  roi.version = "2.0.0";
  declareGapGraphUpgrade(roi);
  roi.label = "业务区域定位";
  roi.category = "间隙/配准";
  roi.keywords = {"business ROI", "模板框", "基准面", "间隙"};
  roi.doc =
      "按左右配准矩阵移动业务框中心并保持原宽高，各框同时携带扫描和实际已变换模板的量测坐标来源。";
  roi.inputs = {{"alignment", "Bundle<gap.Alignment.v2>", "配准结果", "", true}};
  roi.outputs = {{"rois", "Bundle<gap.RoiSet.v2>", "业务区域集", "", true}};
  for (const auto* name : {"flushBase", "gapLeft", "flushRef", "gapRight", "seam"}) {
    roi.outputs.push_back({name, "Box2D", name, "米制业务区域。", true});
    roi.outputs.push_back({std::string(name) + "Context", "Record", std::string(name) + "来源",
                           "profile.Geometry.v2。", true});
  }
  roi.params = businessRoiParameters();
  roi.capabilities = {true, true, true};
  roi.compute = [](const Inputs& inputs, const ParamView& params, Outputs& outputs,
                   ExecContext& ctx) {
    profile_scene::RoiFields fields;
    Data seam, seamContext;
    auto status = businessRois(inputs.get("alignment"), params.choice("datumSide"), fields, seam,
                               seamContext, ctx);
    if (!status.ok)
      return status;
    Data bundle;
    std::string why;
    if (!profile_scene::makeRoiSet(fields, bundle, why))
      return failure(why, "rois", "invalid_result");
    constexpr std::array<std::size_t, 4> mapping{0, 2, 1, 3};
    constexpr std::array<const char*, 4> names{"flushBase", "gapLeft", "flushRef", "gapRight"};
    for (std::size_t i = 0; i < 4; ++i) {
      outputs.set(names[i], fields.boxes[mapping[i]]);
      outputs.set(std::string(names[i]) + "Context", fields.contexts[mapping[i]]);
    }
    outputs.set("seam", std::move(seam));
    outputs.set("seamContext", std::move(seamContext));
    outputs.set("rois", std::move(bundle));
    return Status::Ok();
  };
  registry.addOperator(std::move(roi));
}
} // namespace lyflow::packs::gap::profile_template
