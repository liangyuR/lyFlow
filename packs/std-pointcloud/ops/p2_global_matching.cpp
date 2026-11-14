#include <Eigen/Eigenvalues>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>

#include "algo/p2_fit.h"
#include "algo/p2_matching.h"
#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {
using namespace p1;
using Json = nlohmann::json;
namespace fit3 = algo::p2;
constexpr std::size_t kMaxCloud = 131072;
constexpr std::size_t kMaxChecks = 64U * 1024U * 1024U;
constexpr std::size_t kMaxOutputPoints = 4U * 1024U * 1024U;
constexpr const char* kTemplate = "cloud.Template3D.v1";

struct Failure {
  Status status;
};

void require(bool condition, const std::string& message, const std::string& port = "cloud",
             const std::string& code = "bad_input") {
  if (!condition)
    throw Failure{error(message, port, code)};
}

void check(ExecContext& ctx) {
  require(!ctx.cancelled(), "三维匹配任务已取消", "cloud", "cancelled");
}

void success(const Status& status) {
  if (!status.ok)
    throw Failure{status};
}

struct Work {
  ExecContext& ctx;
  std::size_t used = 0;
  void add(std::size_t n = 1) {
    require(n <= kMaxChecks - used, "几何距离检查超过64Mi次预算", "cloud", "resource_limit");
    used += n;
    check(ctx);
  }
};

const PointCloud& cloud(const Inputs& in, const char* port, std::size_t minimum = 0) {
  const auto* c = in.get(port).asCloud();
  require(c != nullptr, "需要原生PointCloud", port);
  require(c->pointCount() <= kMaxCloud, "输入点云超过131072点预算", port, "resource_limit");
  success(validate(*c, minimum, port));
  return *c;
}

double scalar(const ParamView& p, const char* key, double lo, double hi) {
  const double n = p.number(key);
  require(std::isfinite(n) && n >= lo && n <= hi, "数值参数超出声明范围", key, "bad_param");
  return n;
}

int countParam(const ParamView& p, const char* key, int lo, int hi) {
  const auto n = p.integer(key);
  require(n >= lo && n <= hi, "整数参数超出声明范围", key, "bad_param");
  return static_cast<int>(n);
}

std::string frame(const ParamView& p, const char* key) {
  const auto value = p.text(key);
  require(frameValid(value), "参考帧必须非空且不超过1024字节", key, "bad_param");
  return value;
}

Param boolean(const char* name, const char* label, bool def) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Bool;
  p.def = Value::boolean(def);
  return p;
}

void bounded(const Record& record) {
  std::string why;
  require(values::boundedJson(record.data, why), "输出元信息超过Record预算：" + why, "cloud",
          "resource_limit");
}

std::vector<std::int32_t> allIndices(const PointCloud& c) {
  std::vector<std::int32_t> indices(c.pointCount());
  std::iota(indices.begin(), indices.end(), 0);
  return indices;
}

std::vector<std::int32_t> readIndices(const Data& value, const PointCloud& c, const char* port,
                                      bool allowDuplicates = false) {
  const auto* indices = value.asIndices();
  require(indices && indices->sourceCloudId == c.id, "索引来源必须绑定输入点云", port);
  std::set<std::int32_t> seen;
  for (const auto i : indices->values)
    require(i >= 0 && static_cast<std::size_t>(i) < c.pointCount() &&
                (allowDuplicates || seen.insert(i).second),
            "索引越界或重复", port);
  return indices->values;
}

std::vector<std::int32_t> sampled(const PointCloud& c, std::vector<std::int32_t> indices,
                                  double distance, std::size_t maximum, Work& work) {
  auto less = [&](std::int32_t a, std::int32_t b) {
    const auto pa = point(c, a), pb = point(c, b);
    return std::tie(pa.x(), pa.y(), pa.z(), a) < std::tie(pb.x(), pb.y(), pb.z(), b);
  };
  std::sort(indices.begin(), indices.end(), less);
  if (indices.empty())
    return indices;
  if (distance > 0) {
    std::map<std::array<std::int64_t, 3>, std::int32_t> cells;
    const Vec3 origin = point(c, indices.front());
    work.add(indices.size());
    for (const auto i : indices) {
      const Vec3 q = (point(c, i) - origin) / distance;
      std::array<std::int64_t, 3> cell{};
      for (int axis = 0; axis < 3; ++axis) {
        require(std::isfinite(q[axis]) && std::abs(q[axis]) < 9e18, "采样栅格超出安全整数范围");
        cell[axis] = static_cast<std::int64_t>(std::floor(q[axis]));
      }
      cells.emplace(cell, i);
    }
    indices.clear();
    for (const auto& item : cells)
      indices.push_back(item.second);
    std::sort(indices.begin(), indices.end(), less);
  }
  if (indices.size() <= maximum)
    return indices;
  std::vector<double> nearest(indices.size(), std::numeric_limits<double>::infinity());
  std::vector<bool> used(indices.size(), false);
  std::vector<std::int32_t> chosen;
  std::size_t next = 0;
  for (std::size_t count = 0; count < maximum; ++count) {
    chosen.push_back(indices[next]);
    used[next] = true;
    const Vec3 anchor = point(c, indices[next]);
    work.add(indices.size());
    double farthest = -1;
    for (std::size_t i = 0; i < indices.size(); ++i) {
      nearest[i] = std::min(nearest[i], (point(c, indices[i]) - anchor).squaredNorm());
      if (!used[i] && nearest[i] > farthest) {
        farthest = nearest[i];
        next = i;
      }
    }
  }
  std::sort(chosen.begin(), chosen.end(), less);
  return chosen;
}

struct Dimensions {
  Vec3 center, size;
  double diameter = 0;
};

Dimensions dimensions(const PointCloud& c) {
  Vec3 lo = point(c, 0), hi = lo, mean = Vec3::Zero();
  for (std::size_t i = 0; i < c.pointCount(); ++i) {
    const auto q = point(c, i);
    lo = lo.cwiseMin(q);
    hi = hi.cwiseMax(q);
    mean += q;
  }
  mean /= static_cast<double>(c.pointCount());
  Mat3 covariance = Mat3::Zero();
  for (std::size_t i = 0; i < c.pointCount(); ++i) {
    const Vec3 d = point(c, i) - mean;
    covariance += d * d.transpose();
  }
  Eigen::SelfAdjointEigenSolver<Mat3> eigen(covariance);
  require(eigen.info() == Eigen::Success && eigen.eigenvalues().allFinite() &&
              eigen.eigenvalues()[2] > 0 &&
              eigen.eigenvalues()[0] > eigen.eigenvalues()[2] * 1e-10 && (hi - lo).minCoeff() > 0,
          "三维模板必须具有非零三维体积；平面、共线和重合点不能建立可定向模板", "cloud",
          "degenerate_geometry");
  return {(lo + hi) / 2, hi - lo, (hi - lo).norm()};
}

Status makeTemplate(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const auto& source = cloud(in, "cloud", 4);
    const auto sourceFrame = frame(p, "sourceFrame"), modelFrame = frame(p, "modelFrame");
    const double distance = scalar(p, "sampleDistance", 0, 1e6);
    const auto maximum = countParam(p, "maxModelPoints", 4, 8192);
    const auto d = dimensions(source);
    Work work{ctx};
    const auto indices = sampled(source, allIndices(source), distance, maximum, work);
    require(indices.size() >= 4, "采样后模型点不足", "cloud", "degenerate_geometry");
    PointCloud model = source.select(indices);
    Mat4 translation = Mat4::Identity();
    for (int axis = 0; axis < 3; ++axis)
      translation(axis, 3) = static_cast<float>(-d.center[axis]);
    Transform native;
    require(transform(translation, native), "模型居中变换无法用原生float32表示");
    PointCloud centered;
    require(transformed(model, translation, centered, ctx), "居中模型超出原生范围或法线无效");
    dimensions(centered);
    Record metadata{kTemplate,
                    {{"modelCloudId", std::to_string(centered.id)},
                     {"pointCount", centered.pointCount()},
                     {"sourceFrame", sourceFrame},
                     {"modelFrame", modelFrame},
                     {"unit", "m"},
                     {"sourceCenter", array(d.center)},
                     {"size", array(d.size)},
                     {"diameter", d.diameter},
                     {"sourceToModel", matrixJson(translation)},
                     {"sampleDistance", distance},
                     {"maxModelPoints", maximum},
                     {"method", "voxel-farthest-v1"}}};
    bounded(metadata);
    check(ctx);
    out.set("model", Data::cloud(std::move(centered)));
    out.set("template", Data::record(std::move(metadata)));
    return Status::Ok();
  } catch (const Failure& e) {
    return e.status;
  }
}

struct TemplateInfo {
  Vec3 size;
  double diameter = 0;
  std::string modelFrame;
};

TemplateInfo readTemplate(const Data& input, const PointCloud& model) {
  const auto* record = input.asRecord();
  require(record && record->type == kTemplate, "需要cloud.Template3D.v1", "template");
  std::string why;
  require(values::boundedJson(record->data, why), "模板元信息无效：" + why, "template");
  const auto& d = record->data;
  const std::set<std::string> fields{"modelCloudId",   "pointCount",     "sourceFrame",
                                     "modelFrame",     "unit",           "sourceCenter",
                                     "size",           "diameter",       "sourceToModel",
                                     "sampleDistance", "maxModelPoints", "method"};
  require(d.is_object() && d.size() == fields.size(), "模板v1必须完整声明12个固定字段", "template");
  for (auto field = d.begin(); field != d.end(); ++field)
    require(fields.count(field.key()) != 0, "模板v1存在未知字段", "template");
  require(d.contains("modelCloudId") && d["modelCloudId"].is_string() &&
              d["modelCloudId"] == std::to_string(model.id) && d.contains("pointCount") &&
              cloud_groups::nonnegativeInteger(d["pointCount"]) &&
              d["pointCount"].get<std::size_t>() == model.pointCount() && d.contains("unit") &&
              d["unit"].is_string() && d["unit"] == "m" && d.contains("modelFrame") &&
              d["modelFrame"].is_string() && frameValid(d["modelFrame"].get<std::string>()) &&
              d.contains("sourceFrame") && d["sourceFrame"].is_string() &&
              frameValid(d["sourceFrame"].get<std::string>()) && d.contains("size") &&
              poses::numericArray(d["size"], 3, true) && d.contains("diameter") &&
              values::safeNumber(d["diameter"]) && d["diameter"].get<double>() > 0,
          "模板身份、点数、单位、帧或尺寸不匹配", "template");
  require(
      d.contains("sourceCenter") && poses::numericArray(d["sourceCenter"], 3) &&
          d.contains("sourceToModel") && poses::numericArray(d["sourceToModel"], 16) &&
          d.contains("sampleDistance") && values::safeNumber(d["sampleDistance"]) &&
          d["sampleDistance"].get<double>() >= 0 && d["sampleDistance"].get<double>() <= 1e6 &&
          d.contains("maxModelPoints") && cloud_groups::nonnegativeInteger(d["maxModelPoints"]) &&
          d["maxModelPoints"].get<std::size_t>() >= std::max<std::size_t>(4, model.pointCount()) &&
          d["maxModelPoints"].get<std::size_t>() <= 8192 && d.contains("method") &&
          d["method"].is_string() && d["method"] == "voxel-farthest-v1",
      "模板中心、变换、采样配置或方法非法", "template");
  const auto size = d["size"].get<std::array<double, 3>>();
  const auto center = d["sourceCenter"].get<std::array<double, 3>>();
  const double nativeMaximum = std::numeric_limits<float>::max();
  const Vec3 sizeVector(size[0], size[1], size[2]);
  require(sizeVector.maxCoeff() <= 2 * nativeMaximum &&
              std::abs(d["diameter"].get<double>() - sizeVector.norm()) <=
                  sizeVector.norm() * 1e-10,
          "模板直径必须与原AABB尺寸一致", "template");
  Transform native;
  for (int i = 0; i < 16; ++i) {
    const double value = d["sourceToModel"][i].get<double>();
    require(std::abs(value) <= nativeMaximum &&
                static_cast<double>(static_cast<float>(value)) == value,
            "模板变换必须能精确交付为原生float32", "template");
    native.m[i] = static_cast<float>(value);
  }
  Mat4 sourceToModel;
  require(rigid(native, sourceToModel) &&
              (sourceToModel.topLeftCorner<3, 3>() - Mat3::Identity()).norm() == 0 &&
              (sourceToModel.row(3) - Eigen::RowVector4d(0, 0, 0, 1)).norm() == 0,
          "voxel-farthest-v1只允许无旋转的右手刚体居中变换", "template");
  Vec3 residual;
  for (int axis = 0; axis < 3; ++axis) {
    require(std::abs(center[axis]) <= nativeMaximum &&
                sourceToModel(axis, 3) == static_cast<double>(static_cast<float>(-center[axis])),
            "模板sourceToModel与sourceCenter不一致", "template");
    residual[axis] = center[axis] + sourceToModel(axis, 3);
  }
  for (std::size_t i = 0; i < model.pointCount(); ++i)
    for (int axis = 0; axis < 3; ++axis)
      require(std::abs(point(model, i)[axis] - residual[axis]) <=
                  size[axis] / 2 + 8 * std::numeric_limits<float>::epsilon() * size[axis] +
                      2 * static_cast<double>(std::numeric_limits<float>::denorm_min()),
              "模型点超出模板声明的源AABB范围", "template");
  dimensions(model);
  return {Vec3(size[0], size[1], size[2]), d["diameter"].get<double>(),
          d["modelFrame"].get<std::string>()};
}

std::uint64_t mixed(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

PointCloud coarseCloud(const PointCloud& source, const std::vector<std::int32_t>& indices,
                       double voxelSize, std::size_t limit, Work& work) {
  auto selected = sampled(source, indices, voxelSize, source.pointCount(), work);
  if (selected.size() > limit) {
    std::sort(selected.begin(), selected.end(), [&](std::int32_t a, std::int32_t b) {
      const auto pa = point(source, a), pb = point(source, b);
      auto key = [&](const Vec3& q) {
        std::uint64_t hash = 0;
        for (int axis = 0; axis < 3; ++axis) {
          const double cell = std::floor(q[axis] / voxelSize);
          require(std::isfinite(cell) && std::abs(cell) < 9e18, "粗采样坐标超出安全栅格范围",
                  "scene");
          hash = mixed(hash ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(cell)));
        }
        return hash;
      };
      const auto ka = key(pa), kb = key(pb);
      if (ka != kb)
        return ka < kb;
      return std::tie(pa.x(), pa.y(), pa.z(), a) < std::tie(pb.x(), pb.y(), pb.z(), b);
    });
    selected.resize(limit);
  }
  return source.select(selected);
}

int axisParam(const ParamView& p, const char* name) {
  const auto value = p.choice(name);
  require(value == "none" || value == "x" || value == "y" || value == "z", "模型轴必须为none/x/y/z",
          name, "bad_param");
  return value == "none" ? -1 : value == "x" ? 0 : value == "y" ? 1 : 2;
}

double rotationDistance(const Mat3& a, const Mat3& b) {
  return std::acos(std::clamp(((a.transpose() * b).trace() - 1) / 2, -1.0, 1.0));
}

struct GlobalSettings {
  fit3::GlobalBackendOptions backend;
  fit3::GicpBackendOptions gicp;
  fit3::CloudFitOptions quality;
  int backendPoints = 8192, maxInstances = 16, maxPasses = 32, symmetryAxis = -1, folds = 1;
  int directionAxis = -1;
  Vec3 targetDirection = Vec3::UnitZ();
  double directionTolerance = poses::kPi, uniqueRatio = 0.5, nmsDistance = 0.005, nmsAngle = 0.1;
  double ambiguityRatio = 0.98;
  bool refine = true;
};

GlobalSettings globalSettings(const ParamView& p) {
  GlobalSettings s;
  s.backend.voxelSize = scalar(p, "voxelSize", 1e-6, 1);
  s.backend.normalRadiusFactor = scalar(p, "normalRadiusFactor", 1, 10);
  s.backend.featureRadiusFactor = scalar(p, "featureRadiusFactor", 1, 20);
  require(s.backend.featureRadiusFactor > s.backend.normalRadiusFactor, "特征半径必须大于法线半径",
          "featureRadiusFactor", "bad_param");
  s.backend.maxCorrespondences = countParam(p, "maxCorrespondences", 16, 2048);
  s.backend.ratioTest = p.flag("useRatioTest");
  s.backendPoints = countParam(p, "maxBackendPoints", 32, 8192);
  s.maxInstances = countParam(p, "maxInstances", 1, 64);
  s.maxPasses = countParam(p, "maxPasses", 1, 128);
  s.uniqueRatio = scalar(p, "minUniqueSceneRatio", 0, 1);
  s.nmsDistance = scalar(p, "nmsDistance", 0, 1e6);
  s.nmsAngle = scalar(p, "nmsAngle", 0, poses::kPi);
  s.ambiguityRatio = scalar(p, "ambiguityScoreRatio", 0, 1);
  s.symmetryAxis = axisParam(p, "symmetryAxis");
  s.folds = countParam(p, "symmetryFolds", 1, 64);
  require(s.symmetryAxis >= 0 || s.folds == 1, "无对称轴时折数必须为1", "symmetryFolds",
          "bad_param");
  s.directionAxis = axisParam(p, "directionAxis");
  const auto direction = p.vec3("targetDirection");
  s.targetDirection = Vec3(direction[0], direction[1], direction[2]);
  require(s.targetDirection.allFinite() && s.targetDirection.norm() > 0, "目标方向必须有限且非零",
          "targetDirection", "bad_param");
  s.targetDirection.normalize();
  s.directionTolerance = scalar(p, "directionTolerance", 0, poses::kPi);
  require(p.choice("stopAfter") == "coarse" || p.choice("stopAfter") == "refined",
          "停止阶段必须为coarse/refined", "stopAfter", "bad_param");
  s.refine = p.choice("stopAfter") == "refined";
  s.quality.maxCorrespondenceDistance = scalar(p, "maxCorrespondenceDistance", 1e-9, 1e6);
  s.quality.maxRmse = scalar(p, "maxRmse", 1e-9, 1e6);
  s.quality.minInlierRatio = scalar(p, "minInlierRatio", 1e-9, 1);
  s.quality.translationTolerance = scalar(p, "translationTolerance", 1e-12, 1);
  s.quality.rotationTolerance = scalar(p, "rotationTolerance", 1e-12, 1);
  s.quality.maxIterations = countParam(p, "maxIterations", 1, 1000);
  s.gicp = {s.backend.voxelSize, s.quality.maxCorrespondenceDistance,
            s.quality.translationTolerance, s.quality.rotationTolerance, s.quality.maxIterations};
  return s;
}

bool volumetricSupport(const PointCloud& model, const fit3::CloudFitResult& fit) {
  Vec3 mean = Vec3::Zero();
  std::size_t count = 0;
  for (std::size_t i = 0; i < fit.targetIndices.size(); ++i)
    if (fit.targetIndices[i] >= 0) {
      mean += point(model, i);
      ++count;
    }
  if (count < 4)
    return false;
  mean /= static_cast<double>(count);
  Mat3 covariance = Mat3::Zero();
  for (std::size_t i = 0; i < fit.targetIndices.size(); ++i)
    if (fit.targetIndices[i] >= 0) {
      const Vec3 delta = point(model, i) - mean;
      covariance += delta * delta.transpose();
    }
  Eigen::SelfAdjointEigenSolver<Mat3> eigen(covariance);
  return eigen.info() == Eigen::Success && eigen.eigenvalues()[2] > 0 &&
         eigen.eigenvalues()[0] > eigen.eigenvalues()[2] * 1e-10;
}

struct GlobalMatch {
  fit3::CloudFitResult fit;
  fit3::GlobalBackendResult coarse;
  std::vector<std::int32_t> support;
  std::string roi;
  double score = 0;
  bool ambiguous = false;
  int equivalent = 1;
  Json symmetryScores;
};

bool qualityAccepted(const PointCloud& model, const fit3::CloudFitResult& result,
                     const GlobalSettings& s) {
  return result.accepted && result.matchedCount > 0 &&
         static_cast<double>(result.targetSupport.size()) / result.matchedCount >= s.uniqueRatio &&
         volumetricSupport(model, result);
}

bool directionAccepted(const fit3::CloudFitResult& result, const GlobalSettings& s) {
  return s.directionAxis < 0 || std::acos(std::clamp(result.pose.topLeftCorner<3, 3>()
                                                         .col(s.directionAxis)
                                                         .normalized()
                                                         .dot(s.targetDirection),
                                                     -1.0, 1.0)) <= s.directionTolerance;
}

Status globalMatch(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const auto& model = cloud(in, "model", 4);
    const auto& scene = cloud(in, "scene");
    require(model.pointCount() <= 8192, "模板超过8192点预算", "model", "resource_limit");
    const auto metadata = readTemplate(in.get("template"), model);
    const auto sceneFrame = frame(p, "sceneFrame");
    const auto s = globalSettings(p);
    const auto selected =
        in.has("sceneIndices")
            ? readIndices(in.get("sceneIndices"), scene, "sceneIndices", in.has("sceneGroups"))
            : allIndices(scene);
    std::vector<cloud_groups::Group> rois;
    if (in.has("sceneGroups")) {
      require(in.has("sceneIndices"), "sceneGroups必须配套sceneIndices", "sceneGroups");
      const auto* groups = in.get("sceneGroups").asRecord();
      std::string why, roiFrame;
      require(groups &&
                  cloud_groups::parse(*groups, scene, *in.get("sceneIndices").asIndices(), rois,
                                      roiFrame, why) &&
                  roiFrame == sceneFrame,
              "ROI分组绑定或帧不匹配：" + why, "sceneGroups");
    } else if (!selected.empty()) {
      rois.push_back({in.has("sceneIndices") ? "roi" : "scene", 0, selected.size()});
    }
    require(rois.size() <= 64, "ROI超过64组预算", "sceneGroups", "resource_limit");
    Work work{ctx};
    fit3::FitBudget queries;
    const auto coarseModel =
        coarseCloud(model, allIndices(model), s.backend.voxelSize, s.backendPoints, work);
    std::vector<GlobalMatch> matches;
    std::size_t attempts = 0, rejected = 0, directionRejectedCount = 0;
    bool limitReached = false;
    Json roiResults = Json::array();
    for (const auto& roi : rois) {
      std::vector<std::int32_t> active(selected.begin() + roi.offset,
                                       selected.begin() + roi.offset + roi.count);
      std::sort(active.begin(), active.end());
      active.erase(std::unique(active.begin(), active.end()), active.end());
      std::string termination = "insufficient_points";
      while (active.size() >= 4) {
        check(ctx);
        if (matches.size() >= static_cast<std::size_t>(s.maxInstances)) {
          limitReached = true;
          termination = "max_instances";
          break;
        }
        require(attempts < static_cast<std::size_t>(s.maxPasses),
                "全局匹配阶段调用超过maxPasses预算", "scene", "resource_limit");
        ++attempts;
        const auto coarseScene =
            coarseCloud(scene, active, s.backend.voxelSize, s.backendPoints, work);
        if (coarseModel.pointCount() < 4 || coarseScene.pointCount() < 4) {
          termination = "insufficient_sampled_points";
          break;
        }
        fit3::GlobalBackendResult coarse;
        success(fit3::kissGlobalCandidate(coarseModel, coarseScene, s.backend, ctx, coarse));
        if (!coarse.valid) {
          termination = "no_feature_match";
          break;
        }
        Mat4 candidatePose = coarse.pose;
        int iterations = 0;
        if (s.refine) {
          fit3::GicpBackendResult fine;
          success(
              fit3::refineSmallGicp(coarseModel, coarseScene, candidatePose, s.gicp, ctx, fine));
          if (!fine.converged) {
            ++rejected;
            termination = "refinement_not_converged";
            break;
          }
          candidatePose = fine.pose;
          iterations = fine.iterations;
        }
        PointCloud nativeScene = scene.select(active);
        fit3::TargetIndex index;
        success(fit3::prepareTarget(nativeScene, index, ctx));
        GlobalMatch best;
        best.symmetryScores = Json::array();
        std::vector<fit3::CloudFitResult> variants;
        std::vector<double> scores, allowedScores;
        for (int fold = 0; fold < s.folds; ++fold) {
          Mat4 symmetry = Mat4::Identity();
          if (s.symmetryAxis >= 0)
            symmetry.topLeftCorner<3, 3>() =
                Eigen::AngleAxisd(2 * poses::kPi * fold / s.folds, Vec3::Unit(s.symmetryAxis))
                    .toRotationMatrix();
          fit3::CloudFitResult evaluated;
          success(fit3::evaluateCloudFit(model, index, candidatePose * symmetry, s.quality, ctx,
                                         queries, evaluated));
          const double score =
              qualityAccepted(model, evaluated, s)
                  ? evaluated.inlierRatio / (1 + evaluated.rmse / s.quality.maxRmse)
                  : 0;
          scores.push_back(score);
          allowedScores.push_back(directionAccepted(evaluated, s) ? score : 0);
          best.symmetryScores.push_back(score);
          variants.push_back(std::move(evaluated));
        }
        const auto geometricBest = std::max_element(scores.begin(), scores.end()) - scores.begin();
        if (scores[geometricBest] <= 0) {
          ++rejected;
          termination = "quality_rejected";
          break;
        }
        const auto allowedBest =
            std::max_element(allowedScores.begin(), allowedScores.end()) - allowedScores.begin();
        const bool directionRejected = allowedScores[allowedBest] <= 0;
        const auto chosen = directionRejected ? geometricBest : allowedBest;
        if (directionRejected)
          ++directionRejectedCount;
        best.fit = std::move(variants[chosen]);
        best.fit.iterations = iterations;
        best.score = scores[chosen];
        best.coarse = coarse;
        best.roi = roi.id;
        best.equivalent =
            static_cast<int>(std::count_if(scores.begin(), scores.end(), [&](double score) {
              return score > 0 && score >= best.score * s.ambiguityRatio;
            }));
        best.ambiguous = best.equivalent > 1;
        for (const auto i : best.fit.targetSupport)
          best.support.push_back(active.at(i));
        std::sort(best.support.begin(), best.support.end());
        bool duplicate = false;
        if (!directionRejected)
          for (auto& previous : matches) {
            std::vector<std::int32_t> common;
            work.add(best.support.size() + previous.support.size());
            std::set_intersection(best.support.begin(), best.support.end(),
                                  previous.support.begin(), previous.support.end(),
                                  std::back_inserter(common));
            const auto smaller = std::min(best.support.size(), previous.support.size());
            const double overlap = smaller ? static_cast<double>(common.size()) / smaller : 0;
            const double distance =
                (best.fit.pose.topRightCorner<3, 1>() - previous.fit.pose.topRightCorner<3, 1>())
                    .norm();
            if (overlap >= 0.6 ||
                (distance <= s.nmsDistance &&
                 rotationDistance(best.fit.pose.topLeftCorner<3, 3>(),
                                  previous.fit.pose.topLeftCorner<3, 3>()) <= s.nmsAngle)) {
              duplicate = true;
              previous.ambiguous = previous.ambiguous || best.ambiguous;
              break;
            }
          }
        fit3::TargetIndex alignedIndex;
        success(fit3::prepareTarget(best.fit.aligned, alignedIndex, ctx));
        auto removal = s.quality;
        removal.minInlierRatio = 1e-9;
        removal.maxRmse = removal.maxCorrespondenceDistance;
        fit3::CloudFitResult nearModel;
        success(fit3::evaluateCloudFit(nativeScene, alignedIndex, Mat4::Identity(), removal, ctx,
                                       queries, nearModel));
        require(nearModel.targetIndices.size() == active.size(), "移除阶段索引数量异常", "scene",
                "backend_error");
        std::vector<std::int32_t> remainder;
        for (std::size_t i = 0; i < active.size(); ++i)
          if (nearModel.targetIndices[i] < 0)
            remainder.push_back(active[i]);
        require(remainder.size() < active.size(), "有效匹配没有可移除的场景支持", "scene",
                "backend_error");
        active = std::move(remainder);
        if (!duplicate && !directionRejected)
          matches.push_back(std::move(best));
        termination = "scene_exhausted";
      }
      roiResults.push_back(
          {{"id", roi.id}, {"remainingCount", active.size()}, {"termination", termination}});
      if (limitReached)
        break;
    }
    std::sort(matches.begin(), matches.end(), [](const GlobalMatch& a, const GlobalMatch& b) {
      for (int axis = 0; axis < 3; ++axis)
        if (a.fit.pose(axis, 3) != b.fit.pose(axis, 3))
          return a.fit.pose(axis, 3) < b.fit.pose(axis, 3);
      return a.score > b.score;
    });
    Json poseItems = Json::array(), items = Json::array();
    Indices indices;
    indices.sourceCloudId = scene.id;
    std::vector<cloud_groups::Group> groups;
    for (std::size_t i = 0; i < matches.size(); ++i) {
      const auto& match = matches[i];
      const auto id = "match-" + std::to_string(i);
      poses::Matrix3 rotation{};
      for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
          rotation[row * 3 + column] = match.fit.pose(row, column);
      const auto& matrix = match.fit.pose;
      auto pose = poses::make3D(id, sceneFrame, {matrix(0, 3), matrix(1, 3), matrix(2, 3)},
                                poses::quaternion(rotation),
                                {metadata.size.x(), metadata.size.y(), metadata.size.z()});
      pose.data["confidence"] = match.score;
      pose.data["pickable"] = false;
      pose.data["attributes"] = {{"modelFrame", metadata.modelFrame},
                                 {"roiId", match.roi},
                                 {"ambiguous", match.ambiguous},
                                 {"equivalentOrientations", match.equivalent},
                                 {"rmse", match.fit.rmse},
                                 {"inlierRatio", match.fit.inlierRatio}};
      poseItems.push_back(std::move(pose.data));
      groups.push_back({id, indices.values.size(), match.support.size()});
      indices.values.insert(indices.values.end(), match.support.begin(), match.support.end());
      items.push_back(
          {{"id", id},
           {"roiId", match.roi},
           {"score", match.score},
           {"rmse", match.fit.rmse},
           {"inlierRatio", match.fit.inlierRatio},
           {"matchedCount", match.fit.matchedCount},
           {"sceneSupport", match.support.size()},
           {"uniqueSceneRatio", static_cast<double>(match.support.size()) / match.fit.matchedCount},
           {"converged", match.fit.converged},
           {"iterations", match.fit.iterations},
           {"ambiguous", match.ambiguous},
           {"symmetryScores", match.symmetryScores},
           {"modelToScene", matrixJson(match.fit.pose)},
           {"backendUnitScale", match.coarse.unitScale},
           {"modelFeatures", match.coarse.modelFeatures},
           {"sceneFeatures", match.coarse.sceneFeatures},
           {"neighborPairsBound", match.coarse.neighborPairsBound},
           {"estimatedMemoryBytes", match.coarse.estimatedMemoryBytes},
           {"initialCorrespondences", match.coarse.initialCorrespondences},
           {"finalCorrespondences", match.coarse.finalCorrespondences},
           {"rotationInliers", match.coarse.rotationInliers},
           {"translationInliers", match.coarse.translationInliers}});
    }
    auto instances = poses::make3DList(std::move(poseItems), sceneFrame);
    auto grouped = cloud_groups::make(scene, groups, sceneFrame);
    Record quality{"cloud.GlobalMatches.v1",
                   {{"modelFrame", metadata.modelFrame},
                    {"sceneFrame", sceneFrame},
                    {"unit", "m"},
                    {"direction", "centered_model_to_scene"},
                    {"backend", "kiss-matcher-max-core-gnc"},
                    {"refinement", s.refine ? "small_gicp-gicp-serial" : "not_run"},
                    {"deterministic", false},
                    {"attempts", attempts},
                    {"rejectedCandidates", rejected + directionRejectedCount},
                    {"geometryRejectedCandidates", rejected},
                    {"directionRejectedCandidates", directionRejectedCount},
                    {"maxInstancesReached", limitReached},
                    {"distanceChecks", work.used},
                    {"nearestQueries", queries.queries},
                    {"rois", std::move(roiResults)},
                    {"effective",
                     {{"voxelSize", s.backend.voxelSize},
                      {"normalRadiusFactor", s.backend.normalRadiusFactor},
                      {"featureRadiusFactor", s.backend.featureRadiusFactor},
                      {"maxCorrespondences", s.backend.maxCorrespondences},
                      {"maxBackendPoints", s.backendPoints},
                      {"maxIterations", s.quality.maxIterations},
                      {"maxCorrespondenceDistance", s.quality.maxCorrespondenceDistance},
                      {"maxRmse", s.quality.maxRmse},
                      {"minInlierRatio", s.quality.minInlierRatio},
                      {"minUniqueSceneRatio", s.uniqueRatio},
                      {"symmetryAxis", p.choice("symmetryAxis")},
                      {"symmetryFolds", s.folds}}},
                    {"items", std::move(items)}}};
    std::string why;
    require(poses::validate(instances, why), "输出位姿集合无效：" + why, "scene");
    bounded(grouped);
    bounded(quality);
    check(ctx);
    out.set("instances", Data::record(std::move(instances)));
    out.set("indices", Data::indices(std::move(indices)));
    out.set("groups", Data::record(std::move(grouped)));
    out.set("quality", Data::record(std::move(quality)));
    return Status::Ok();
  } catch (const Failure& e) {
    return e.status;
  }
}

Status extractEdges(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const auto mode = p.choice("mode");
    require(mode == "organized" || mode == "unorganized", "未知点云布局模式", "mode", "bad_param");
    const auto* raw = in.get("cloud").asCloud();
    require(raw && raw->channelsConsistent(), "需要通道一致的原生PointCloud", "cloud");
    require(raw->pointCount() <= kMaxCloud, "输入点云超过131072点预算", "cloud", "resource_limit");
    const auto& source = *raw;
    if (mode == "unorganized")
      success(validate(source));
    const auto frameName = frame(p, "frame");
    const int width = countParam(p, "organizedWidth", 0, 131072);
    const int k = countParam(p, "kSearch", 3, 128);
    const int minimum = countParam(p, "minNeighbors", 3, 128);
    const int searchNeighbors = countParam(p, "searchNeighbors", 1, 128);
    const double radius = scalar(p, "neighborRadius", 1e-9, 1e6);
    const double depth = scalar(p, "depthThreshold", 1e-9, 1e6);
    const double boundaryAngle = scalar(p, "boundaryAngle", 0, 2 * poses::kPi);
    const double curvatureAngle = scalar(p, "curvatureAngle", 0, poses::kPi / 2);
    require(minimum <= k, "minNeighbors不能大于kSearch", "minNeighbors", "bad_param");
    const bool boundary = p.flag("nanBoundary"), nearSide = p.flag("occluding"),
               farSide = p.flag("occluded"), curvature = p.flag("highCurvature");
    require(mode == "organized" || (!nearSide && !farSide),
            "无组织点云没有像素深度邻接，不能启用occluding/occluded", "mode", "bad_param");
    if (mode == "organized")
      require(width >= 2 && source.pointCount() % width == 0 && source.pointCount() / width >= 2,
              "organizedWidth必须显式给出且形成至少2×2的完整布局", "organizedWidth", "bad_param");
    auto valid = in.has("validIndices")
                     ? readIndices(in.get("validIndices"), source, "validIndices")
                     : allIndices(source);
    if (mode == "organized" && !in.has("validIndices"))
      valid.erase(std::remove_if(valid.begin(), valid.end(),
                                 [&](std::int32_t i) { return !point(source, i).allFinite(); }),
                  valid.end());
    for (const auto i : valid) {
      require(point(source, i).allFinite(), "有效点索引不能选中非有限XYZ", "validIndices");
      if (source.hasIntensity())
        require(std::isfinite(source.intensity[i]), "有效点强度必须有限", "cloud");
      if (source.hasNormals())
        for (int axis = 0; axis < 3; ++axis)
          require(std::isfinite(source.normals[3 * i + axis]), "有效点法线必须有限", "cloud");
    }
    std::vector<bool> validMap(source.pointCount(), false);
    for (const auto i : valid)
      validMap[i] = true;
    std::vector<Vec3> normals(source.pointCount(), Vec3::Zero());
    std::vector<std::uint8_t> labels(source.pointCount(), 0);
    std::size_t degenerate = 0;
    Work work{ctx};
    auto suppliedNormal = [&](std::size_t i) {
      if (!source.hasNormals())
        return false;
      const Vec3 n(source.normals[3 * i], source.normals[3 * i + 1], source.normals[3 * i + 2]);
      if (!(n.norm() > 0))
        return false;
      normals[i] = n.normalized();
      return true;
    };
    if (mode == "organized") {
      const int height = static_cast<int>(source.pointCount() / width);
      for (const auto index : valid) {
        work.add(4 * searchNeighbors);
        const int x = index % width, y = index / width;
        const Vec3 q = point(source, index);
        require(q.z() > 0, "组织深度边缘的有效点必须具有正Z深度", "cloud");
        for (const auto direction : {std::array<int, 2>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
          bool missing = false;
          for (int step = 1; step <= searchNeighbors; ++step) {
            const int nx = x + direction[0] * step, ny = y + direction[1] * step;
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
              missing = true;
              break;
            }
            const auto neighbor = static_cast<std::size_t>(ny * width + nx);
            if (!validMap[neighbor]) {
              missing = true;
              continue;
            }
            const double delta = point(source, neighbor).z() - q.z();
            if (nearSide && delta > depth)
              labels[index] |= 2;
            if (farSide && delta < -depth)
              labels[index] |= 4;
            break;
          }
          if (boundary && missing)
            labels[index] |= 1;
        }
        if (curvature && !suppliedNormal(index)) {
          if (x > 0 && x + 1 < width && y > 0 && y + 1 < height && validMap[index - 1] &&
              validMap[index + 1] && validMap[index - width] && validMap[index + width]) {
            const Vec3 dx = point(source, index + 1) - point(source, index - 1);
            const Vec3 dy = point(source, index + width) - point(source, index - width);
            const Vec3 normal = dx.cross(dy);
            if (normal.norm() > 0)
              normals[index] = normal.normalized();
          }
          if (normals[index].norm() == 0)
            ++degenerate;
        }
      }
      if (curvature)
        for (const auto index : valid) {
          work.add(4);
          const int x = index % width, y = index / width;
          for (const auto direction : {std::array<int, 2>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            const int nx = x + direction[0], ny = y + direction[1];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height)
              continue;
            const auto neighbor = static_cast<std::size_t>(ny * width + nx);
            if (validMap[neighbor] && normals[index].norm() > 0 && normals[neighbor].norm() > 0 &&
                std::acos(std::clamp(std::abs(normals[index].dot(normals[neighbor])), 0.0, 1.0)) >
                    curvatureAngle)
              labels[index] |= 8;
          }
        }
    } else if (!valid.empty()) {
      auto spatial = source.select(valid);
      spatial.normals.clear();
      fit3::TargetIndex index;
      success(fit3::prepareTarget(spatial, index, ctx));
      auto neighbors = [&](std::size_t local) {
        work.add(k);
        std::vector<int> found;
        std::vector<float> distances;
        const int amount = std::min<int>(k, static_cast<int>(valid.size()));
        index.tree.nearestKSearch(index.points->at(local), amount, found, distances);
        std::vector<std::size_t> result;
        for (std::size_t j = 0; j < found.size(); ++j)
          if (distances[j] <= radius * radius && static_cast<std::size_t>(found[j]) != local)
            result.push_back(valid[found[j]]);
        return result;
      };
      for (std::size_t local = 0; local < valid.size(); ++local) {
        const auto i = valid[local];
        if (suppliedNormal(i))
          continue;
        const auto nearPoints = neighbors(local);
        if (nearPoints.size() < static_cast<std::size_t>(minimum)) {
          ++degenerate;
          continue;
        }
        Vec3 mean = Vec3::Zero();
        for (const auto j : nearPoints)
          mean += point(source, j);
        mean /= static_cast<double>(nearPoints.size());
        Mat3 covariance = Mat3::Zero();
        for (const auto j : nearPoints) {
          const Vec3 d = point(source, j) - mean;
          covariance += d * d.transpose();
        }
        Eigen::SelfAdjointEigenSolver<Mat3> eigen(covariance);
        if (eigen.info() != Eigen::Success || eigen.eigenvalues()[2] <= 0 ||
            eigen.eigenvalues()[1] <= eigen.eigenvalues()[2] * 1e-10) {
          ++degenerate;
          continue;
        }
        normals[i] = eigen.eigenvectors().col(0);
      }
      for (std::size_t local = 0; local < valid.size(); ++local) {
        const auto i = valid[local];
        if (!(normals[i].norm() > 0))
          continue;
        const auto nearPoints = neighbors(local);
        if (nearPoints.size() < static_cast<std::size_t>(minimum))
          continue;
        const Vec3 n = normals[i], tangent = n.unitOrthogonal(), second = n.cross(tangent);
        std::vector<double> angles;
        for (const auto j : nearPoints) {
          const Vec3 d = point(source, j) - point(source, i);
          const double x = d.dot(tangent), y = d.dot(second);
          if (x * x + y * y > radius * radius * 1e-12)
            angles.push_back(std::atan2(y, x));
          if (curvature && normals[j].norm() > 0 &&
              std::acos(std::clamp(std::abs(n.dot(normals[j])), 0.0, 1.0)) > curvatureAngle)
            labels[i] |= 8;
        }
        if (boundary && angles.size() >= static_cast<std::size_t>(minimum)) {
          std::sort(angles.begin(), angles.end());
          double gap = angles.front() + 2 * poses::kPi - angles.back();
          for (std::size_t j = 1; j < angles.size(); ++j)
            gap = std::max(gap, angles[j] - angles[j - 1]);
          if (gap > boundaryAngle)
            labels[i] |= 1;
        }
      }
    }
    std::array<Indices, 4> kinds;
    Indices selected;
    selected.sourceCloudId = source.id;
    for (auto& kind : kinds)
      kind.sourceCloudId = source.id;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      if (labels[i])
        selected.values.push_back(static_cast<std::int32_t>(i));
      for (int bit = 0; bit < 4; ++bit)
        if (labels[i] & (1 << bit))
          kinds[bit].values.push_back(static_cast<std::int32_t>(i));
    }
    Record quality{"cloud.Edges3D.v1",
                   {{"sourceCloudId", std::to_string(source.id)},
                    {"frame", frameName},
                    {"unit", "m"},
                    {"mode", mode},
                    {"validCount", valid.size()},
                    {"count", selected.values.size()},
                    {"boundaryCount", kinds[0].values.size()},
                    {"occludingCount", kinds[1].values.size()},
                    {"occludedCount", kinds[2].values.size()},
                    {"curvatureCount", kinds[3].values.size()},
                    {"degenerateNeighborhoods", degenerate},
                    {"boundaryDefinition", mode == "organized" ? "invalid-or-layout-border"
                                                               : "tangent-neighbor-angle-gap"},
                    {"distanceChecks", work.used}}};
    bounded(quality);
    PointCloud result = source.select(selected.values);
    check(ctx);
    out.set("cloud", Data::cloud(std::move(result)));
    out.set("indices", Data::indices(std::move(selected)));
    for (int bit = 0; bit < 4; ++bit)
      out.set(std::array<const char*, 4>{"boundaryIndices", "occludingIndices", "occludedIndices",
                                         "curvatureIndices"}[bit],
              Data::indices(std::move(kinds[bit])));
    out.set("quality", Data::record(std::move(quality)));
    return Status::Ok();
  } catch (const Failure& e) {
    return e.status;
  }
}

Mat4 poseMatrix(const Json& pose) {
  const auto q = pose["rotation"].get<poses::Quaternion>();
  const auto rotation = poses::rotationMatrix(q);
  Mat4 result = Mat4::Identity();
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column)
      result(row, column) = rotation[row * 3 + column];
    result(row, 3) = pose["position"][row].get<double>();
  }
  return result;
}

void appendCloud(PointCloud& output, const PointCloud& input) {
  output.xyz.insert(output.xyz.end(), input.xyz.begin(), input.xyz.end());
  output.intensity.insert(output.intensity.end(), input.intensity.begin(), input.intensity.end());
  output.rgb.insert(output.rgb.end(), input.rgb.begin(), input.rgb.end());
  output.normals.insert(output.normals.end(), input.normals.begin(), input.normals.end());
}

Status sampleViews(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const auto& model = cloud(in, "model");
    const auto cameraFrame = frame(p, "cameraFrame"), modelFrame = frame(p, "modelFrame");
    const double resolution = scalar(p, "angularResolution", 1e-5, poses::kPi / 2);
    const double tolerance = scalar(p, "depthTolerance", 0, 1e6);
    const double nearPlane = scalar(p, "nearPlane", 1e-9, 1e6);
    const bool transformPose = p.flag("transformByPose"), cull = p.flag("cullBackfaces");
    Record viewPoses;
    std::string why;
    require(poses::readList(in.get("poses"), 3, viewPoses, why) &&
                viewPoses.data["frame"] == cameraFrame,
            "poses必须是同cameraFrame米制model→camera位姿：" + why, "poses");
    const auto& views = viewPoses.data["items"];
    require(views.size() <= 128, "采样视图超过128项预算", "poses", "resource_limit");
    require(model.pointCount() == 0 || views.size() <= kMaxOutputPoints / model.pointCount(),
            "视图×模型点数超过4Mi点预算", "poses", "resource_limit");
    require(!cull || model.hasNormals(), "背面剔除需要模型法线", "model");
    if (model.hasNormals())
      for (std::size_t i = 0; i < model.pointCount(); ++i)
        require(
            Vec3(model.normals[3 * i], model.normals[3 * i + 1], model.normals[3 * i + 2]).norm() >
                0,
            "模型法线必须非零", "model");
    struct Ray {
      std::int64_t x, y;
      double depth;
      std::int32_t index;
    };
    Work work{ctx};
    PointCloud combined;
    Indices sourceIndices;
    sourceIndices.sourceCloudId = model.id;
    std::vector<cloud_groups::Group> groups;
    Json items = Json::array();
    for (const auto& pose : views) {
      const Mat4 matrix = poseMatrix(pose);
      const Mat3 rotation = matrix.topLeftCorner<3, 3>();
      const Vec3 translation = matrix.topRightCorner<3, 1>();
      std::vector<Ray> rays;
      work.add(model.pointCount());
      for (std::size_t i = 0; i < model.pointCount(); ++i) {
        const Vec3 q = rotation * point(model, i) + translation;
        require(q.allFinite(), "视图坐标运算溢出", "poses");
        if (q.z() <= nearPlane)
          continue;
        if (cull) {
          const Vec3 normal = rotation * Vec3(model.normals[3 * i], model.normals[3 * i + 1],
                                              model.normals[3 * i + 2]);
          if (normal.dot(-q) <= 0)
            continue;
        }
        rays.push_back(
            {static_cast<std::int64_t>(std::floor(std::atan2(q.x(), q.z()) / resolution)),
             static_cast<std::int64_t>(std::floor(std::atan2(q.y(), q.z()) / resolution)), q.z(),
             static_cast<std::int32_t>(i)});
      }
      std::sort(rays.begin(), rays.end(), [](const Ray& a, const Ray& b) {
        return std::tie(a.x, a.y, a.depth, a.index) < std::tie(b.x, b.y, b.depth, b.index);
      });
      std::vector<std::int32_t> visible;
      std::size_t bins = 0;
      for (std::size_t begin = 0; begin < rays.size();) {
        ++bins;
        std::size_t end = begin;
        while (end < rays.size() && rays[end].x == rays[begin].x && rays[end].y == rays[begin].y) {
          if (rays[end].depth - rays[begin].depth <= tolerance)
            visible.push_back(rays[end].index);
          ++end;
        }
        begin = end;
      }
      std::sort(visible.begin(), visible.end());
      const auto id = pose["id"].get<std::string>();
      groups.push_back({id, sourceIndices.values.size(), visible.size()});
      sourceIndices.values.insert(sourceIndices.values.end(), visible.begin(), visible.end());
      PointCloud selected = model.select(visible), transformedCloud;
      if (transformPose) {
        require(transformed(selected, matrix, transformedCloud, ctx),
                "可见面变换无法交付为原生点云", "poses");
        appendCloud(combined, transformedCloud);
      } else {
        appendCloud(combined, selected);
      }
      items.push_back({{"id", id},
                       {"projectedCount", rays.size()},
                       {"bins", bins},
                       {"visibleCount", visible.size()},
                       {"occludedCount", rays.size() - visible.size()}});
    }
    auto sourceGroups = cloud_groups::make(model, groups, modelFrame);
    auto outputGroups =
        cloud_groups::make(combined, groups, transformPose ? cameraFrame : modelFrame);
    Indices cloudIndices;
    cloudIndices.sourceCloudId = combined.id;
    cloudIndices.values = allIndices(combined);
    Record quality{"cloud.ViewSampling.v1",
                   {{"sourceCloudId", std::to_string(model.id)},
                    {"cloudId", std::to_string(combined.id)},
                    {"modelFrame", modelFrame},
                    {"cameraFrame", cameraFrame},
                    {"unit", "m"},
                    {"method", "angular-depth-buffer-v1"},
                    {"angularResolution", resolution},
                    {"depthTolerance", tolerance},
                    {"transformByPose", transformPose},
                    {"items", std::move(items)}}};
    bounded(sourceGroups);
    bounded(outputGroups);
    bounded(quality);
    check(ctx);
    out.set("cloud", Data::cloud(std::move(combined)));
    out.set("indices", Data::indices(std::move(sourceIndices)));
    out.set("groups", Data::record(std::move(sourceGroups)));
    out.set("cloudIndices", Data::indices(std::move(cloudIndices)));
    out.set("cloudGroups", Data::record(std::move(outputGroups)));
    out.set("quality", Data::record(std::move(quality)));
    return Status::Ok();
  } catch (const Failure& e) {
    return e.status;
  }
}

} // namespace

void registerP2GlobalMatching(Registry& registry) {
  auto op = descriptor("cloud.template_3d", "三维表面模板构建",
                       "3D surface model template CAD centered point cloud sampling");
  op.inputs = {
      {"cloud", "PointCloud", "源模型点云", "由io.load_cloud读取PCD/PLY或STL表面采样。", true}};
  op.outputs = {{"model", "PointCloud", "居中模型", "原AABB中心居中，保留可选通道。", true},
                {"template", "Record", "模板描述", "cloud.Template3D.v1，绑定model身份。", true}};
  op.params = {text("sourceFrame", "原模型参考帧", "source"),
               text("modelFrame", "模型参考帧", "model"),
               number("sampleDistance", "采样栅格尺寸", 0.001, 0, 1e6, "m"),
               integer("maxModelPoints", "模型采样点数上限", 4096, 4, 8192)};
  op.doc =
      "三维非共面模型的原生点云模板。按栅格及确定性最远点采样，原AABB中心居中，不推断可抓取姿态；保存/加载组合io.save_pcd和io.load_cloud后重新构建模板绑定。";
  op.compute = &makeTemplate;
  registry.addOperator(std::move(op));

#if defined(LYFLOW_WITH_KISS_MATCHER)
  op = descriptor(
      "register.global_3d", "三维表面模板全局匹配",
      "KISS-Matcher small_gicp global 3D CAD surface model multiple instances symmetry registration");
  op.capabilities = {true, false, false};
  op.inputs = {
      {"model", "PointCloud", "居中模型", "cloud.template_3d的原生模型点云。", true},
      {"template", "Record", "模板描述", "与model身份一致的cloud.Template3D.v1。", true},
      {"scene", "PointCloud", "场景点云", "原生米制点云。", true},
      {"sceneIndices", "Indices", "场景ROI索引", "可选scene身份；显式空索引不会退全云。", false},
      {"sceneGroups", "Record", "场景ROI分组", "可选cloud.Groups.v1，必须配套sceneIndices。",
       false}};
  op.outputs = {{"instances", "Record", "匹配实例", "vision.Poses3D.v1，居中model→scene。", true},
                {"indices", "Indices", "场景支持索引", "绑定原scene，每实例一组。", true},
                {"groups", "Record", "实例分组", "绑定原scene的cloud.Groups.v1。", true},
                {"quality", "Record", "匹配质量",
                 "cloud.GlobalMatches.v1，含实际参数、米制质量和对称假设。", true}};
  op.params = {text("sceneFrame", "场景参考帧", "scene"),
               number("voxelSize", "匹配分辨率", 0.001, 1e-6, 1, "m"),
               number("normalRadiusFactor", "法线半径倍率", 3, 1, 10),
               number("featureRadiusFactor", "特征半径倍率", 5, 1, 20),
               integer("maxCorrespondences", "兼容图对应点预算", 512, 16, 2048),
               integer("maxBackendPoints", "每侧粗采样点上限", 8192, 32, 8192),
               boolean("useRatioTest", "描述子比值测试", false),
               integer("maxInstances", "请求实例数量上限", 16, 1, 64),
               integer("maxPasses", "全局阶段调用预算", 32, 1, 128),
               choice("stopAfter", "停止阶段", "refined",
                      {{"coarse", "全局候选质量门控"}, {"refined", "GICP精配准"}}),
               number("maxCorrespondenceDistance", "支持对应距离上限", 0.003, 1e-9, 1e6, "m"),
               number("maxRmse", "最大交付RMSE", 0.001, 1e-9, 1e6, "m"),
               number("minInlierRatio", "模型最少覆盖比例", 0.5, 1e-9, 1),
               number("minUniqueSceneRatio", "独立场景支持比例", 0.5, 0, 1),
               integer("maxIterations", "GICP迭代上限", 50, 1, 1000),
               number("translationTolerance", "平移收敛阈值", 1e-7, 1e-12, 1, "m"),
               number("rotationTolerance", "旋转收敛阈值", 1e-7, 1e-12, 1, "rad"),
               number("nmsDistance", "重复中心距离", 0.005, 0, 1e6, "m"),
               number("nmsAngle", "重复旋转角", 0.1, 0, poses::kPi, "rad"),
               choice("symmetryAxis", "模型离散对称轴", "none",
                      {{"none", "不声明对称"}, {"x", "X轴"}, {"y", "Y轴"}, {"z", "Z轴"}}),
               integer("symmetryFolds", "离散对称折数", 1, 1, 64),
               number("ambiguityScoreRatio", "对称歧义分数比例", 0.98, 0, 1),
               choice("directionAxis", "模型朝向约束轴", "none",
                      {{"none", "不限制"}, {"x", "X轴"}, {"y", "Y轴"}, {"z", "Z轴"}}),
               vector("targetDirection", "场景目标方向", {0, 0, 1}),
               number("directionTolerance", "朝向容差", poses::kPi, 0, poses::kPi, "rad")};
  op.doc =
      "KISS-Matcher特征/兼容图max-core/GNC提供任意SE(3)候选，small_gicp精化；米制居中归一化支持小件，最终原生float变换后重新计NN、RMSE、模型覆盖、独立场景支持和非共面支持。按真实支持移除搜索多实例；空ROI/无匹配输出空集合。对称规则显式，pickable=false；非确定性不缓存。库阶段不可抢占，取消在阶段边界响应，点数/对应数/调用预算明确。";
  op.compute = &globalMatch;
  registry.addOperator(std::move(op));
#endif

  op = descriptor(
      "cloud.edges_3d", "三维点云边缘提取",
      "3D cloud organized depth occluding occluded invalid boundary curvature edge unorganized tangent");
  op.inputs = {{"cloud", "PointCloud", "输入点云", "原生米制XYZ与可选通道。", true},
               {"validIndices", "Indices", "有效点", "可选原输入身份，空索引为空布局。", false}};
  op.outputs = {
      {"cloud", "PointCloud", "边缘点云", "保留原可选通道。", true},
      {"indices", "Indices", "边缘索引", "原输入身份，四种边缘的去重并集。", true},
      {"boundaryIndices", "Indices", "边界索引", "组织布局空洞/外边界；无组织切平面角缺口。", true},
      {"occludingIndices", "Indices", "遮挡近侧", "正Z深度差的近侧，限organized。", true},
      {"occludedIndices", "Indices", "遮挡远侧", "正Z深度差的远侧，限organized。", true},
      {"curvatureIndices", "Indices", "高曲率索引", "局部无向法线角度突变。", true},
      {"quality", "Record", "提取质量", "cloud.Edges3D.v1。", true}};
  op.params = {text("frame", "点云参考帧", "camera"),
               choice("mode", "布局模式", "unorganized",
                      {{"unorganized", "无组织邻域"}, {"organized", "组织深度布局"}}),
               integer("organizedWidth", "组织布局宽度", 0, 0, 131072),
               integer("kSearch", "近邻点数上限", 24, 3, 128),
               integer("minNeighbors", "最少有效近邻", 6, 3, 128),
               number("neighborRadius", "近邻半径", 0.02, 1e-9, 1e6, "m"),
               integer("searchNeighbors", "布局缺失搜索半径", 10, 1, 128),
               number("depthThreshold", "深度不连续门限", 0.002, 1e-9, 1e6, "m"),
               boolean("nanBoundary", "提取边界", true),
               boolean("occluding", "提取遮挡近侧", false),
               boolean("occluded", "提取遮挡远侧", false),
               boolean("highCurvature", "提取高曲率", false),
               number("boundaryAngle", "边界角缺口门限", 2, 0, 2 * poses::kPi, "rad"),
               number("curvatureAngle", "法线角门限", poses::kPi / 6, 0, poses::kPi / 2, "rad")};
  op.doc =
      "不推测原生无组织PointCloud的图像布局。organized必须显式宽高和正Z深度，validIndices可表达孔洞；unorganized以局部PCA切平面邻点最大角缺口找边界，不支持像素遮挡分类。退化邻域计入质量，不捏造法线。";
  op.compute = &extractEdges;
  registry.addOperator(std::move(op));

  op = descriptor(
      "cloud.view_sample", "点云可见面采样",
      "view sampling visibility depth buffer occlusion perspective multi pose model camera");
  op.inputs = {{"model", "PointCloud", "模型点云", "米制模型局部坐标。", true},
               {"poses", "Record", "模型到相机位姿",
                "vision.Pose3D.v1或Poses3D.v1，frame=cameraFrame。", true}};
  op.outputs = {
      {"cloud", "PointCloud", "可见面拼接云", "按视图顺序拼接；可保留模型坐标或变换到相机。", true},
      {"indices", "Indices", "原模型索引", "每视图可见点的原model索引，可以跨组重复。", true},
      {"groups", "Record", "原模型视图分组", "绑定原model的cloud.Groups.v1。", true},
      {"cloudIndices", "Indices", "拼接云索引", "绑定输出cloud，身份索引。", true},
      {"cloudGroups", "Record", "拼接云视图分组", "绑定输出cloud，可直接用于分组下游。", true},
      {"quality", "Record", "可见性质量", "cloud.ViewSampling.v1，记录离散角分辨率。", true}};
  op.params = {text("modelFrame", "模型参考帧", "model"),
               text("cameraFrame", "相机参考帧", "camera"),
               number("angularResolution", "角度分箱分辨率", 0.01, 1e-5, poses::kPi / 2, "rad"),
               number("depthTolerance", "可见深度容差", 0.002, 0, 1e6, "m"),
               number("nearPlane", "近裁面", 0.0001, 1e-9, 1e6, "m"),
               boolean("cullBackfaces", "按法线剔除背面", false),
               boolean("transformByPose", "输出到相机坐标", false)};
  op.doc =
      "按model→camera位姿投影，用离散atan2角度深度缓冲保留每箱最近深度层；包含真实前后遮挡而非仅法线朝向。分辨率和容差决定采样近似，不声称连续mesh精确可见性。原模型索引与输出拼接云分组分别绑定，不混用身份。";
  op.compute = &sampleViews;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::ops
