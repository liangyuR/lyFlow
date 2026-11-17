#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>
#include <array>

#include "p1_common.h"
#include "../algo/p2_fit.h"
#include "lyflow/pose.h"
#include "lyflow/kernel/constraints.h"
#include "lyflow/kernel/metrology.h"

namespace lyflow::ops {
namespace {
using namespace p1;
using Json = nlohmann::json;
using V6 = Eigen::Matrix<double, 6, 1>;
using M6 = Eigen::Matrix<double, 6, 6>;
using J36 = Eigen::Matrix<double, 3, 6>;
constexpr std::size_t kMaxOutputPoints = 4U * 1024U * 1024U;

struct Constraint {
  std::string id;
  std::string type;
  Vec3 source, target, direction;
  double weight = 1;
  double offset = 0;
  int datum = -1;
  Mat3 projection = Mat3::Identity();
};

Status constraints(const Inputs& in, const ParamView& p, std::vector<Constraint>& result) {
  const auto* record = in.get("constraints").asRecord();
  if (!record || record->type != kernel::constraints::kRecordType)
    return error("需要有界cloud.Constraints3D.v1约束记录：", "constraints");
  std::vector<kernel::constraints::Constraint> parsed;
  kernel::constraints::Error issue;
  if (!kernel::constraints::parse3D(record->data, p.text("sourceFrame"), p.text("targetFrame"),
                                    p.choice("mode"), parsed, issue))
    return error(issue.message, issue.field, issue.code);
  std::vector<Constraint> converted;
  converted.reserve(parsed.size());
  for (auto& value : parsed) {
    Constraint c;
    c.id = std::move(value.id);
    c.type = std::move(value.type);
    c.source = Eigen::Map<const Vec3>(value.source.data());
    c.target = Eigen::Map<const Vec3>(value.target.data());
    c.direction = Eigen::Map<const Vec3>(value.direction.data());
    c.weight = value.weight;
    c.offset = value.offset;
    c.datum = value.datum;
    if (c.type == "line")
      c.projection = Mat3::Identity() - c.direction * c.direction.transpose();
    else if (c.type == "plane" || c.type == "axis")
      c.projection = c.direction * c.direction.transpose();
    converted.push_back(std::move(c));
  }
  result = std::move(converted);
  return Status::Ok();
}

Mat3 skew(const Vec3& v) {
  Mat3 result;
  result << 0, -v.z(), v.y(), v.z(), 0, -v.x(), -v.y(), v.x(), 0;
  return result;
}

Vec3 residual(const Constraint& c, const Vec3& point) {
  Vec3 result = c.projection * (point - c.target);
  if (c.type == "plane" || c.type == "axis")
    result -= c.direction * c.offset;
  return result;
}

double cost(const std::vector<Constraint>& cs, const Mat4& pose, double huber) {
  double sum = 0;
  for (const auto& c : cs) {
    const double norm =
        residual(c, pose.topLeftCorner<3, 3>() * c.source + pose.topRightCorner<3, 1>()).norm();
    sum += c.weight * (huber > 0 && norm > huber ? 2 * huber * norm - huber * huber : norm * norm);
  }
  return sum;
}

struct ConstraintQuality {
  double rmse = 0;
  double maxError = 0;
  Json items = Json::array();
};

Vec3 deliveredPoint(const Transform& matrix, const Vec3& source) {
  const Vec3 rounded = source.cast<float>().cast<double>();
  Vec3 result;
  for (int i = 0; i < 3; ++i) {
    const float* row = matrix.m + 4 * i;
    const float x = static_cast<float>(rounded[0]), y = static_cast<float>(rounded[1]),
                z = static_cast<float>(rounded[2]);
    result[i] = row[0] * x + row[1] * y + row[2] * z + row[3];
  }
  return result;
}

ConstraintQuality measured(const std::vector<Constraint>& cs, const Transform& pose,
                           const Transform& before) {
  ConstraintQuality q;
  kernel::metrology::ResidualAccumulator residuals;
  for (const auto& c : cs) {
    const Vec3 afterResidual = residual(c, deliveredPoint(pose, c.source));
    const Vec3 beforeResidual = residual(c, deliveredPoint(before, c.source));
    const double distance = afterResidual.norm();
    residuals.add(distance, c.weight);
    Json item{{"id", c.id},
              {"type", c.type},
              {"weight", c.weight},
              {"before", beforeResidual.norm()},
              {"after", distance}};
    if (c.type == "plane" || c.type == "axis") {
      item["signedBefore"] = c.direction.dot(beforeResidual);
      item["signedAfter"] = c.direction.dot(afterResidual);
    }
    q.items.push_back(std::move(item));
  }
  q.rmse = residuals.weightedRmse();
  q.maxError = residuals.maxError();
  return q;
}

Status constrainedFit(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  if (!frameValid(p.text("sourceFrame")) || !frameValid(p.text("targetFrame")))
    return error("源/目标帧必须显式有效", "sourceFrame", "bad_param");
  for (const char* key : {"maxRmse", "maxError", "translationTolerance", "rotationTolerance"})
    if (!std::isfinite(p.number(key)) || p.number(key) <= 0 || p.number(key) > 1e6)
      return error("质量和收敛门限须为(0,1e6]有限数", key, "bad_param");
  if (p.integer("maxIterations") < 1 || p.integer("maxIterations") > 1000 ||
      !std::isfinite(p.number("huberDelta")) || p.number("huberDelta") < 0 ||
      p.number("huberDelta") > 1e6)
    return error("迭代次数或Huber门限非法", "maxIterations", "bad_param");
  std::vector<Constraint> cs;
  Status status = constraints(in, p, cs);
  if (!status.ok)
    return status;
  Mat4 pose = Mat4::Identity();
  Transform before;
  if (in.has("initial")) {
    const auto* value = in.get("initial").asTransform();
    if (!value || !rigid(*value, pose))
      return error("initial必须是source→target刚体变换", "initial");
  }
  if (!transform(pose, before))
    return error("初始变换超出float32范围", "initial");
  std::size_t operations = 0;
  int iterations = 0;
  bool converged = false;
  double conditionRatio = 0;
  for (int iteration = 0; iteration < p.integer("maxIterations"); ++iteration) {
    if (ctx.cancelled())
      return error("约束拟合已取消", "constraints", "cancelled");
    if (cs.size() > (16U * 1024U * 1024U - operations) / 20)
      return error("约束拟合累计残差预算超限", "constraints", "resource_limit");
    operations += cs.size() * 20;
    Vec3 center = Vec3::Zero();
    double sumWeight = 0;
    for (const auto& c : cs) {
      center += c.weight * (pose.topLeftCorner<3, 3>() * c.source + pose.topRightCorner<3, 1>());
      sumWeight += c.weight;
    }
    center /= sumWeight;
    double scale = 0;
    for (const auto& c : cs)
      scale +=
          c.weight * (pose.topLeftCorner<3, 3>() * c.source + pose.topRightCorner<3, 1>() - center)
                         .squaredNorm();
    scale = std::sqrt(scale / sumWeight);
    if (!(scale > 1e-12) || !std::isfinite(scale))
      return error("约束源点重合或尺度退化", "constraints", "degenerate_geometry");
    M6 h = M6::Zero();
    V6 g = V6::Zero();
    for (const auto& c : cs) {
      const Vec3 x = pose.topLeftCorner<3, 3>() * c.source + pose.topRightCorner<3, 1>();
      const Vec3 r = residual(c, x);
      J36 j;
      j.leftCols<3>() = -c.projection * skew(x - center) / scale;
      j.rightCols<3>() = c.projection;
      const double w = c.weight * (p.number("huberDelta") > 0 && r.norm() > p.number("huberDelta")
                                       ? p.number("huberDelta") / r.norm()
                                       : 1);
      h += w * j.transpose() * j;
      g += w * j.transpose() * r;
    }
    Eigen::SelfAdjointEigenSolver<M6> eigen(h);
    if (eigen.info() != Eigen::Success || !eigen.eigenvalues().allFinite() ||
        !(eigen.eigenvalues()[5] > 0))
      return error("约束正规方程无有效尺度", "constraints", "degenerate_geometry");
    conditionRatio = eigen.eigenvalues()[0] / eigen.eigenvalues()[5];
    if (conditionRatio <= 1e-10)
      return error("混合约束未观测全部六自由度或严重病态", "constraints", "degenerate_geometry");
    const V6 delta = -h.ldlt().solve(g);
    if (!delta.allFinite())
      return error("约束更新数值失败", "constraints", "degenerate_geometry");
    Vec3 omega = delta.head<3>() / scale;
    Vec3 displacement = delta.tail<3>();
    const double currentCost = cost(cs, pose, p.number("huberDelta"));
    Mat4 candidate = pose;
    bool improved = false;
    double fraction = std::min(1.0, omega.norm() > 0 ? 0.5 / omega.norm() : 1.0);
    for (int line = 0; line < 16; ++line, fraction *= 0.5) {
      const double angle = omega.norm() * fraction;
      const Mat3 rotation = angle > 0
                                ? Eigen::AngleAxisd(angle, omega.normalized()).toRotationMatrix()
                                : Mat3::Identity();
      Mat4 update = Mat4::Identity();
      update.topLeftCorner<3, 3>() = rotation;
      update.topRightCorner<3, 1>() = center - rotation * center + fraction * displacement;
      candidate = update * pose;
      if (cost(cs, candidate, p.number("huberDelta")) <= currentCost + 1e-24) {
        improved = true;
        break;
      }
    }
    if (!improved)
      return error("约束拟合线搜索未改善实际目标函数", "constraints", "registration_rejected");
    pose = candidate;
    iterations = iteration + 1;
    if (omega.norm() * fraction <= p.number("rotationTolerance") &&
        displacement.norm() * fraction <= p.number("translationTolerance")) {
      converged = true;
      break;
    }
  }
  if (!converged)
    return error("约束拟合未收敛", "constraints", "registration_rejected");
  Transform native;
  Mat4 rounded;
  if (!transform(pose, native) || !rigid(native, rounded))
    return error("约束变换无法作为float32右手刚体交付", "constraints", "registration_rejected");
  Vec3 deliveredCenter = Vec3::Zero();
  double deliveredWeight = 0, deliveredScale = 0;
  for (const auto& c : cs) {
    deliveredCenter += c.weight * deliveredPoint(native, c.source);
    deliveredWeight += c.weight;
  }
  deliveredCenter /= deliveredWeight;
  for (const auto& c : cs)
    deliveredScale += c.weight * (deliveredPoint(native, c.source) - deliveredCenter).squaredNorm();
  deliveredScale = std::sqrt(deliveredScale / deliveredWeight);
  if (!(deliveredScale > 1e-12) || !std::isfinite(deliveredScale))
    return error("交付float32支持点尺度退化", "constraints", "degenerate_geometry");
  M6 deliveredH = M6::Zero();
  for (const auto& c : cs) {
    J36 j;
    j.leftCols<3>() =
        -c.projection * skew(deliveredPoint(native, c.source) - deliveredCenter) / deliveredScale;
    j.rightCols<3>() = c.projection;
    deliveredH += c.weight * j.transpose() * j;
  }
  Eigen::SelfAdjointEigenSolver<M6> deliveredEigen(deliveredH);
  if (deliveredEigen.info() != Eigen::Success || !(deliveredEigen.eigenvalues()[5] > 0) ||
      deliveredEigen.eigenvalues()[0] <= deliveredEigen.eigenvalues()[5] * 1e-10)
    return error("交付float32支持点未观测全部六自由度", "constraints", "degenerate_geometry");
  conditionRatio = deliveredEigen.eigenvalues()[0] / deliveredEigen.eigenvalues()[5];
  const ConstraintQuality quality = measured(cs, native, before);
  if (!std::isfinite(quality.rmse) || quality.rmse > p.number("maxRmse") ||
      quality.maxError > p.number("maxError"))
    return error("交付float32输出未通过约束RMSE或最大误差门限", "constraints",
                 "registration_rejected");
  PointCloud aligned;
  if (in.has("cloud")) {
    const auto* cloud = in.get("cloud").asCloud();
    if (!cloud || !validate(*cloud).ok)
      return error("cloud必须是通道一致的有限点云", "cloud");
    if (cloud->pointCount() > kMaxOutputPoints)
      return error("对齐输出超过4194304点", "cloud", "resource_limit");
    if (!transformed(*cloud, rounded, aligned, ctx)) {
      if (ctx.cancelled())
        return error("对齐点云已取消", "cloud", "cancelled");
      return error("对齐点云溢出或法线无效", "cloud");
    }
    for (std::size_t i = 0; i < cloud->pointCount(); ++i) {
      if ((i & 255U) == 0 && ctx.cancelled())
        return error("对齐点云已取消", "cloud", "cancelled");
      const Vec3 value = deliveredPoint(native, point(*cloud, i));
      if (!value.allFinite())
        return error("交付点云float32溢出", "cloud");
      for (int axis = 0; axis < 3; ++axis)
        aligned.xyz[3 * i + axis] = static_cast<float>(value[axis]);
    }
  }
  Record q{"cloud.ConstrainedFit.v1",
           {{"sourceFrame", p.text("sourceFrame")},
            {"targetFrame", p.text("targetFrame")},
            {"unit", "m"},
            {"mode", p.choice("mode")},
            {"accepted", true},
            {"converged", true},
            {"iterations", iterations},
            {"conditionRatio", conditionRatio},
            {"weightedRmse", quality.rmse},
            {"maxError", quality.maxError},
            {"items", quality.items}}};
  std::string why;
  if (!values::boundedJson(q.data, why))
    return error("质量记录超过限制：" + why, "constraints", "resource_limit");
  out.set("transform", Data::transform(native));
  out.set("aligned", Data::cloud(std::move(aligned)));
  out.set("quality", Data::record(std::move(q)));
  return Status::Ok();
}

struct SearchInitial {
  Mat4 matrix = Mat4::Identity();
  Json item;
};

Mat4 poseMatrix(const Json& item) {
  const auto q = item["rotation"].get<poses::Quaternion>();
  const auto r = poses::rotationMatrix(q);
  Mat4 result = Mat4::Identity();
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column)
      result(row, column) = r[row * 3 + column];
    result(row, 3) = item["position"][row].get<double>();
  }
  return result;
}

Status initials(const Inputs& in, const ParamView& p, const PointCloud& source,
                std::vector<SearchInitial>& result) {
  if (in.has("initial") && in.has("poses"))
    return error("initial和poses互斥，不隐式覆盖初值", "poses");
  if (in.has("poses")) {
    Record list;
    std::string why;
    if (!poses::read3DList(in.get("poses"), list, why) ||
        list.data["frame"] != p.text("targetFrame"))
      return error("poses必须是目标帧米制Pose3D集合：" + why, "poses");
    if (list.data["items"].empty())
      return error("初值工件集合不能为空", "poses");
    for (const auto& item : list.data["items"])
      result.push_back({poseMatrix(item), item});
  } else {
    SearchInitial value;
    if (in.has("initial")) {
      const auto* initial = in.get("initial").asTransform();
      if (!initial || !rigid(*initial, value.matrix))
        return error("initial必须为source→target刚体变换", "initial");
    }
    Vec3 minimum = point(source, 0), maximum = minimum;
    for (std::size_t i = 1; i < source.pointCount(); ++i) {
      minimum = minimum.cwiseMin(point(source, i));
      maximum = maximum.cwiseMax(point(source, i));
    }
    const Vec3 size = (maximum - minimum).cwiseMax(1e-9);
    value.item = poses::make3D(p.text("id"), p.text("targetFrame"), {0, 0, 0}, {0, 0, 0, 1},
                               {size.x(), size.y(), size.z()})
                     .data;
    result.push_back(std::move(value));
  }
  return Status::Ok();
}

Mat4 offsetMatrix(const Mat4& initial, const V6& offset, bool local) {
  Mat4 delta = Mat4::Identity();
  delta.topLeftCorner<3, 3>() =
      (Eigen::AngleAxisd(offset[5], Vec3::UnitZ()) * Eigen::AngleAxisd(offset[4], Vec3::UnitY()) *
       Eigen::AngleAxisd(offset[3], Vec3::UnitX()))
          .toRotationMatrix();
  delta.topRightCorner<3, 1>() = offset.head<3>();
  if (local)
    return initial * delta;
  Mat4 result = initial;
  result.topLeftCorner<3, 3>() = delta.topLeftCorner<3, 3>() * initial.topLeftCorner<3, 3>();
  result.topRightCorner<3, 1>() += offset.head<3>();
  return result;
}

bool better(const algo::p2::CloudFitResult& candidate, const algo::p2::CloudFitResult& best) {
  const bool valid = candidate.converged && candidate.observable && candidate.matchedCount >= 3 &&
                     std::isfinite(candidate.rmse);
  const bool bestValid =
      best.converged && best.observable && best.matchedCount >= 3 && std::isfinite(best.rmse);
  return valid && (!bestValid || candidate.inlierRatio > best.inlierRatio ||
                   (candidate.inlierRatio == best.inlierRatio && candidate.rmse < best.rmse));
}

Status poseSearch(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto* source = in.get("source").asCloud();
  const auto* target = in.get("target").asCloud();
  if (source->pointCount() > 1024U * 1024U)
    return error("模型云超过1048576点", "source", "resource_limit");
  Status status = validate(*source, 3, "source");
  if (!status.ok)
    return status;
  if (!frameValid(p.text("sourceFrame")) || !frameValid(p.text("targetFrame")) ||
      p.text("id").empty() || p.text("id").size() > 1024)
    return error("源/目标帧和实例id必须显式有效", "sourceFrame", "bad_param");
  if (p.integer("steps") % 2 == 0)
    return error("steps必须为奇数", "steps", "bad_param");
  const auto& translation = p.raw().at("translationRadius").vecValue();
  const auto& rotation = p.raw().at("rotationRadius").vecValue();
  V6 radius;
  std::size_t count = 1;
  for (int axis = 0; axis < 6; ++axis) {
    radius[axis] = axis < 3 ? translation[axis] : rotation[axis - 3];
    if (radius[axis] < 0 || radius[axis] > (axis < 3 ? 1e6 : poses::kPi))
      return error("平移半径须为[0,1e6]m、旋转半径为[0,pi]rad", "translationRadius", "bad_param");
    if (radius[axis] > 0)
      count *= static_cast<std::size_t>(p.integer("steps"));
  }
  if (p.integer("maxIterations") < 1 || p.integer("maxIterations") > 1000)
    return error("ICP迭代次数必须为1..1000", "maxIterations", "bad_param");
  algo::p2::CloudFitOptions options;
  options.maxCorrespondenceDistance = p.number("maxCorrespondenceDistance");
  options.maxRmse = p.number("maxRmse");
  options.minInlierRatio = p.number("minInlierRatio");
  options.maxIterations = static_cast<int>(p.integer("maxIterations"));
  options.translationTolerance = p.number("translationTolerance");
  options.rotationTolerance = p.number("rotationTolerance");
  options.pointToPlane = p.choice("refinement") == "point_to_plane";
  status = algo::p2::validateFitOptions(options);
  if (!status.ok)
    return status;
  std::vector<SearchInitial> starts;
  status = initials(in, p, *source, starts);
  if (!status.ok)
    return status;
  const auto levels =
      p.choice("searchMode") == "grid" ? 1U : static_cast<std::size_t>(p.integer("levels"));
  if (count > static_cast<std::size_t>(p.integer("maxEvaluations")) / levels / starts.size() ||
      starts.size() > kMaxOutputPoints / source->pointCount())
    return error("完整Cartesian网格/批次/层级超过候选或输出点预算，不截断轴组合", "source",
                 "resource_limit");
  algo::p2::TargetIndex index;
  status = algo::p2::prepareTarget(*target, index, ctx);
  if (!status.ok)
    return status;
  algo::p2::FitBudget budget;
  Json posesOut = Json::array(), qualityItems = Json::array();
  PointCloud aligned;
  std::vector<cloud_groups::Group> groups;
  Indices indices;
  for (const auto& start : starts) {
    algo::p2::CloudFitResult best, acceptedBest;
    V6 center = V6::Zero(), winning = V6::Zero(), acceptedWinning = V6::Zero(),
       currentRadius = radius;
    std::size_t evaluations = 0;
    Json candidates = Json::array();
    for (std::size_t level = 0; level < levels; ++level) {
      algo::p2::CloudFitResult levelBest;
      V6 levelWinning = center;
      for (std::size_t candidateIndex = 0; candidateIndex < count; ++candidateIndex) {
        if (ctx.cancelled())
          return error("工件局部搜索已取消", "poses", "cancelled");
        std::size_t code = candidateIndex;
        V6 offset = center;
        for (int axis = 0; axis < 6; ++axis) {
          if (radius[axis] == 0)
            continue;
          const auto digit = code % static_cast<std::size_t>(p.integer("steps"));
          code /= static_cast<std::size_t>(p.integer("steps"));
          const double delta =
              -currentRadius[axis] + 2 * currentRadius[axis] * static_cast<double>(digit) /
                                         static_cast<double>(p.integer("steps") - 1);
          offset[axis] = std::clamp(center[axis] + delta, -radius[axis], radius[axis]);
        }
        const Mat4 candidatePose =
            offsetMatrix(start.matrix, offset, p.choice("composition") == "source");
        algo::p2::CloudFitResult candidate;
        status = p.choice("refinement") == "none"
                     ? algo::p2::evaluateCloudFit(*source, index, candidatePose, options, ctx,
                                                  budget, candidate)
                     : algo::p2::refineCloudFit(*source, index, candidatePose, options, ctx, budget,
                                                candidate);
        if (!status.ok)
          return status;
        ++evaluations;
        Json candidateQuality{
            {"level", level},
            {"index", candidateIndex},
            {"offset", std::vector<double>(offset.data(), offset.data() + 6)},
            {"accepted", candidate.accepted},
            {"converged", candidate.converged},
            {"observable", candidate.observable},
            {"matchedCount", candidate.matchedCount},
            {"inlierRatio", candidate.inlierRatio},
            {"rmse", std::isfinite(candidate.rmse) ? Json(candidate.rmse) : Json(nullptr)}};
        candidates.push_back(std::move(candidateQuality));
        if (candidate.accepted && better(candidate, acceptedBest)) {
          acceptedWinning = offset;
          acceptedBest = candidate;
        }
        if (better(candidate, levelBest)) {
          levelWinning = offset;
          levelBest = std::move(candidate);
        }
      }
      if (!levelBest.converged || levelBest.matchedCount < 3 || !std::isfinite(levelBest.rmse))
        return error("工件" + start.item["id"].get<std::string>() + "的层级" +
                         std::to_string(level) + "没有有限匹配候选用于后续细分",
                     "poses", "registration_rejected");
      if (better(levelBest, best)) {
        winning = levelWinning;
        best = std::move(levelBest);
      }
      center = winning;
      currentRadius *= 1.0 / static_cast<double>(p.integer("steps") - 1);
    }
    if (!acceptedBest.accepted)
      return error("工件" + start.item["id"].get<std::string>() + "最终未通过交付float32质量门限",
                   "poses", "registration_rejected");
    best = std::move(acceptedBest);
    winning = acceptedWinning;
    Json item = start.item;
    poses::Matrix3 matrix{};
    for (int row = 0; row < 3; ++row)
      for (int column = 0; column < 3; ++column)
        matrix[row * 3 + column] = best.pose(row, column);
    item["rotation"] = poses::quaternion(matrix);
    item["position"] = std::array<double, 3>{best.pose(0, 3), best.pose(1, 3), best.pose(2, 3)};
    item["confidence"] = best.inlierRatio / (1 + best.rmse / options.maxRmse);
    item["pickable"] = false;
    posesOut.push_back(std::move(item));
    const std::size_t begin = aligned.pointCount();
    aligned.xyz.insert(aligned.xyz.end(), best.aligned.xyz.begin(), best.aligned.xyz.end());
    aligned.intensity.insert(aligned.intensity.end(), best.aligned.intensity.begin(),
                             best.aligned.intensity.end());
    aligned.rgb.insert(aligned.rgb.end(), best.aligned.rgb.begin(), best.aligned.rgb.end());
    aligned.normals.insert(aligned.normals.end(), best.aligned.normals.begin(),
                           best.aligned.normals.end());
    for (std::size_t i = 0; i < source->pointCount(); ++i)
      indices.values.push_back(static_cast<std::int32_t>(begin + i));
    groups.push_back({start.item["id"].get<std::string>(), begin, source->pointCount()});
    qualityItems.push_back(
        {{"id", start.item["id"]},
         {"accepted", true},
         {"converged", best.converged},
         {"iterations", best.iterations},
         {"evaluated", evaluations},
         {"matchedCount", best.matchedCount},
         {"inlierRatio", best.inlierRatio},
         {"rmse", best.rmse},
         {"transform", matrixJson(best.pose)},
         {"winningOffset", std::vector<double>(winning.data(), winning.data() + 6)},
         {"candidates", std::move(candidates)}});
  }
  Record result = poses::make3DList(std::move(posesOut), p.text("targetFrame"));
  Record quality{"cloud.PoseSearch.v1",
                 {{"sourceFrame", p.text("sourceFrame")},
                  {"targetFrame", p.text("targetFrame")},
                  {"unit", "m"},
                  {"searchMode", p.choice("searchMode")},
                  {"composition", p.choice("composition")},
                  {"refinement", p.choice("refinement")},
                  {"queries", budget.queries},
                  {"items", std::move(qualityItems)}}};
  std::string why;
  if (!poses::validate(result, why) || !values::boundedJson(quality.data, why, true))
    return error("搜索输出记录超过契约限制：" + why, "poses", "resource_limit");
  indices.sourceCloudId = aligned.id;
  Record grouping = cloud_groups::make(aligned, groups, p.text("targetFrame"));
  out.set("poses", Data::record(std::move(result)));
  out.set("groups", Data::record(std::move(grouping)));
  out.set("indices", Data::indices(std::move(indices)));
  out.set("aligned", Data::cloud(std::move(aligned)));
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}
} // namespace

void registerAdvancedFit3D(Registry& r) {
  OperatorDesc op =
      descriptor("register.constrained_fit_3d", "三维加权几何约束拟合",
                 "weighted mixed point line plane datum 321 constrained rigid fitting");
  op.inputs = {{"constraints", "Record", "几何约束",
                "cloud.Constraints3D.v1，source→target米制约束。", true},
               {"initial", "Transform", "初值", "可选source→target刚体变换。", false},
               {"cloud", "PointCloud", "源点云", "可选；输出保通道的对齐点云。", false}};
  op.outputs = {{"transform", "Transform", "变换", "交付float32 source→target。", true},
                {"aligned", "PointCloud", "对齐点云", "未接cloud时为空云。", true},
                {"quality", "Record", "前后约束质量", "cloud.ConstrainedFit.v1。", true}};
  op.params = {text("sourceFrame", "源参考帧", "source"),
               text("targetFrame", "目标参考帧", "target"),
               choice("mode", "约束模型", "mixed",
                      {{"mixed", "混合点线面"}, {"weighted_321", "加权3-2-1基准"}}),
               number("huberDelta", "Huber转折残差", 0, 0, 1e6, "m"),
               number("maxRmse", "最大加权RMSE", 0.001, 1e-12, 1e6, "m"),
               number("maxError", "最大单约束误差", 0.01, 1e-12, 1e6, "m"),
               integer("maxIterations", "迭代上限", 100, 1, 1000),
               number("translationTolerance", "平移收敛阈值", 1e-8, 1e-12, 1, "m"),
               number("rotationTolerance", "旋转收敛阈值", 1e-8, 1e-12, 1, "rad")};
  op.doc =
      "有界局部SE3加权点/点到线/点到面/轴向偏移约束；归一化可观性检查、Huber可选、输入只读。3-2-1要求正交datum及3/2/1及以上约束；拒绝不观测、未收敛或交付float32质量不合格。";
  op.compute = constrainedFit;
  r.addOperator(op);
  op = descriptor("register.pose_search_3d", "三维工件六轴局部搜索",
                  "six axis Cartesian grid pyramid hierarchical local pose search batch objects");
  op.inputs = {{"source", "PointCloud", "模型云", "原生有限通道，所有工件共享模型。", true},
               {"target", "PointCloud", "场景云", "目标帧；点面精化需有效法线。", true},
               {"initial", "Transform", "初值", "可选；与poses互斥。", false},
               {"poses", "Record", "工件初值", "目标帧Pose3D或集合；保留ID/属性。", false}};
  op.outputs = {{"poses", "Record", "搜索工件", "目标帧vision.Poses3D.v1。", true},
                {"aligned", "PointCloud", "批次对齐云", "保强度/RGB/旋转法线。", true},
                {"indices", "Indices", "分组索引", "绑定aligned。", true},
                {"groups", "Record", "工件分组", "cloud.Groups.v1，以工件ID绑定。", true},
                {"quality", "Record", "候选质量", "cloud.PoseSearch.v1，胜者及逐候选。", true}};
  op.params = {
      text("sourceFrame", "源参考帧", "source"),
      text("targetFrame", "目标参考帧", "target"),
      text("id", "单初值实例标识", "object-1"),
      choice("searchMode", "采样方式", "grid",
             {{"grid", "完整网格"}, {"hierarchical", "分级网格"}}),
      choice("composition", "搜索坐标系", "target",
             {{"target", "目标帧"}, {"source", "模型局部帧"}}),
      vector("translationRadius", "XYZ平移搜索半径(m)", {0.02, 0.02, 0.02}),
      vector("rotationRadius", "XYZ旋转搜索半径(rad)", {0, 0, 0.1}),
      integer("steps", "每有效轴采样数(奇数)", 3, 3, 9),
      integer("levels", "分级层数", 3, 1, 8),
      integer("maxEvaluations", "总候选预算", 4096, 1, 4096),
      choice(
          "refinement", "候选精化", "none",
          {{"none", "仅网格"}, {"point_to_point", "点到点ICP"}, {"point_to_plane", "点到面ICP"}}),
      number("maxCorrespondenceDistance", "对应距离上限", 0.1, 1e-12, 1e6, "m"),
      number("maxRmse", "最大RMSE", 0.01, 1e-12, 1e6, "m"),
      number("minInlierRatio", "最小模型覆盖比例", 0.7, 1e-9, 1),
      integer("maxIterations", "ICP迭代上限", 100, 1, 1000),
      number("translationTolerance", "平移收敛阈值", 1e-7, 1e-12, 1, "m"),
      number("rotationTolerance", "旋转收敛阈值", 1e-7, 1e-12, 1, "rad")};
  op.doc =
      "XYZ平移与Rz*Ry*Rx旋转的全部Cartesian组合；分级网格围绕胜者按采样间距缩小且不越初始搜索半径。可选ICP可离开网格边界，需显式启用。按真实float32模型覆盖比例优先、RMSE次优稳定择优；任一对象失败使整批诊断，不伪造初值为成功。";
  op.compute = poseSearch;
  r.addOperator(op);
}
} // namespace lyflow::ops
