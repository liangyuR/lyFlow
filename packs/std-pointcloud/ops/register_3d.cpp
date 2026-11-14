#include <Eigen/SVD>
#include <pcl/search/kdtree.h>

#include "lyflow_pcl/adapter.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {
using namespace p1;
const double kSearchSpan = std::sqrt(static_cast<double>(std::numeric_limits<float>::max())) / 4;

struct Pair {
  Vec3 source, target, normal;
};

bool fit(const std::vector<Pair>& pairs, Mat4& result) {
  if (pairs.size() < 3)
    return false;
  Vec3 a = Vec3::Zero(), b = Vec3::Zero();
  for (const auto& pair : pairs) {
    a += pair.source;
    b += pair.target;
  }
  a /= static_cast<double>(pairs.size());
  b /= static_cast<double>(pairs.size());
  Mat3 covariance = Mat3::Zero();
  for (const auto& pair : pairs)
    covariance += (pair.source - a) * (pair.target - b).transpose();
  Eigen::JacobiSVD<Mat3> svd(covariance, Eigen::ComputeFullU | Eigen::ComputeFullV);
  if (!svd.singularValues().allFinite() || svd.singularValues()[0] <= 0 ||
      svd.singularValues()[1] <= svd.singularValues()[0] * 1e-10)
    return false;
  Mat3 parity = Mat3::Identity();
  parity(2, 2) = (svd.matrixV() * svd.matrixU().transpose()).determinant() < 0 ? -1 : 1;
  const Mat3 rotation = svd.matrixV() * parity * svd.matrixU().transpose();
  result = Mat4::Identity();
  result.topLeftCorner<3, 3>() = rotation;
  result.topRightCorner<3, 1>() = b - rotation * a;
  return result.allFinite();
}

bool planeStep(const std::vector<Pair>& pairs, Mat4& step) {
  if (pairs.size() < 6)
    return false;
  Eigen::Matrix<double, 6, 6> h = Eigen::Matrix<double, 6, 6>::Zero();
  Eigen::Matrix<double, 6, 1> g = Eigen::Matrix<double, 6, 1>::Zero();
  Vec3 center = Vec3::Zero();
  for (const auto& pair : pairs)
    center += pair.source;
  center /= static_cast<double>(pairs.size());
  for (const auto& pair : pairs) {
    Eigen::Matrix<double, 6, 1> j;
    j.head<3>() = (pair.source - center).cross(pair.normal);
    j.tail<3>() = pair.normal;
    h += j * j.transpose();
    g += j * pair.normal.dot(pair.target - pair.source);
  }
  Eigen::SelfAdjointEigenSolver<Eigen::Matrix<double, 6, 6>> eigen(h);
  if (eigen.info() != Eigen::Success || eigen.eigenvalues()[5] <= 0 ||
      eigen.eigenvalues()[0] <= eigen.eigenvalues()[5] * 1e-10)
    return false;
  const Eigen::Matrix<double, 6, 1> update = h.ldlt().solve(g);
  if (!update.allFinite() || update.head<3>().norm() > 0.5)
    return false;
  const double angle = update.head<3>().norm();
  const Mat3 rotation = angle > 0
                            ? Eigen::AngleAxisd(angle, update.head<3>() / angle).toRotationMatrix()
                            : Mat3::Identity();
  step = Mat4::Identity();
  step.topLeftCorner<3, 3>() = rotation;
  step.topRightCorner<3, 1>() = center - rotation * center + update.tail<3>();
  return true;
}

std::vector<Pair> match(const PointCloud& source, const PointCloud& target,
                        const pcl::search::KdTree<pcl::PointXYZ>& tree, const Mat4& pose,
                        double maximum, bool normals, ExecContext& ctx) {
  std::vector<Pair> pairs;
  pairs.reserve(source.pointCount());
  std::vector<int> indices(1);
  std::vector<float> distances(1);
  Ticker ticker(ctx, source.pointCount());
  for (std::size_t i = 0; i < source.pointCount(); ++i) {
    if (ticker.tick(i))
      return {};
    const Vec3 p = pose.topLeftCorner<3, 3>() * point(source, i) + pose.topRightCorner<3, 1>();
    if (!p.allFinite() || p.cwiseAbs().maxCoeff() > std::numeric_limits<float>::max() ||
        (p - point(target, 0)).cwiseAbs().maxCoeff() > kSearchSpan)
      continue;
    const pcl::PointXYZ query(static_cast<float>(p.x()), static_cast<float>(p.y()),
                              static_cast<float>(p.z()));
    if (tree.nearestKSearch(query, 1, indices, distances) != 1)
      continue;
    const auto index = static_cast<std::size_t>(indices[0]);
    const Vec3 q = point(target, index);
    if ((p - q).norm() > maximum)
      continue;
    Vec3 normal = Vec3::Zero();
    if (normals) {
      normal = Vec3(target.normals[index * 3], target.normals[index * 3 + 1],
                    target.normals[index * 3 + 2]);
      normal.normalize();
    }
    pairs.push_back({p, q, normal});
  }
  return pairs;
}

double rmse(const std::vector<Pair>& pairs) {
  double sum = 0;
  for (const auto& pair : pairs)
    sum += (pair.source - pair.target).squaredNorm();
  return pairs.empty() ? std::numeric_limits<double>::infinity()
                       : std::sqrt(sum / static_cast<double>(pairs.size()));
}

Status inputs(const Inputs& in, const ParamView& p, const PointCloud*& source,
              const PointCloud*& target, Mat4& initial) {
  source = in.get("source").asCloud();
  target = in.get("target").asCloud();
  if (!source || !target)
    return error("source和target必须是PointCloud");
  Status s = validate(*source, 3, "source");
  if (!s.ok)
    return s;
  s = validate(*target, 3, "target");
  if (!s.ok)
    return s;
  if (!frameValid(p.text("sourceFrame")) || !frameValid(p.text("targetFrame")))
    return error("源/目标帧必须显式且有效", "sourceFrame", "bad_param");
  initial = Mat4::Identity();
  if (in.has("initial")) {
    const auto* transform = in.get("initial").asTransform();
    if (!transform || !rigid(*transform, initial))
      return error("initial必须为source到target的有限右手刚体变换", "initial");
  }
  return Status::Ok();
}

bool delivered(const PointCloud& source, const PointCloud& target, const Mat4& matrix,
               const pcl::search::KdTree<pcl::PointXYZ>* tree, const ParamView& p, ExecContext& ctx,
               Transform& pose, PointCloud& aligned, std::vector<Pair>& pairs) {
  Mat4 rounded;
  if (!transform(matrix, pose) || !rigid(pose, rounded) ||
      !transformed(source, rounded, aligned, ctx))
    return false;
  Ticker ticker(ctx, source.pointCount());
  for (std::size_t i = 0; i < source.pointCount(); ++i) {
    if (ticker.tick(i))
      return false;
    const float x = source.xyz[i * 3], y = source.xyz[i * 3 + 1], z = source.xyz[i * 3 + 2];
    for (int axis = 0; axis < 3; ++axis) {
      const float* row = pose.m + axis * 4;
      const float value = row[0] * x + row[1] * y + row[2] * z + row[3];
      if (!std::isfinite(value))
        return false;
      aligned.xyz[i * 3 + axis] = value;
    }
  }
  if (tree) {
    pairs = match(aligned, target, *tree, Mat4::Identity(), p.number("maxCorrespondenceDistance"),
                  false, ctx);
  } else {
    pairs.clear();
    pairs.reserve(source.pointCount());
    for (std::size_t i = 0; i < source.pointCount(); ++i)
      pairs.push_back({point(aligned, i), point(target, i), Vec3::Zero()});
  }
  return !ctx.cancelled();
}

Status emit(const PointCloud& source, const PointCloud& target, const Mat4& matrix,
            const pcl::search::KdTree<pcl::PointXYZ>* tree, const ParamView& p, Outputs& out,
            ExecContext& ctx, int iterations, const std::string& mode, bool converged) {
  Transform pose;
  PointCloud aligned;
  std::vector<Pair> pairs;
  if (!delivered(source, target, matrix, tree, p, ctx, pose, aligned, pairs)) {
    if (ctx.cancelled())
      return Status::Ok();
    return error("配准输出超出原生坐标范围或法线无效");
  }
  const double residual = rmse(pairs);
  const double ratio = static_cast<double>(pairs.size()) / source.pointCount();
  if (!converged || !std::isfinite(residual) || residual > p.number("maxRmse") ||
      ratio < p.number("minInlierRatio"))
    return error("配准未收敛或原生变换未通过RMSE/匹配比例门限", "source", "registration_rejected");
  Record quality{"cloud.Registration.v1",
                 {{"sourceFrame", p.text("sourceFrame")},
                  {"targetFrame", p.text("targetFrame")},
                  {"direction", "source_to_target"},
                  {"mode", mode},
                  {"unit", "m"},
                  {"accepted", true},
                  {"converged", converged},
                  {"iterations", iterations},
                  {"matchedCount", pairs.size()},
                  {"inlierRatio", ratio},
                  {"rmse", residual}}};
  out.set("transform", Data::transform(pose));
  out.set("aligned", Data::cloud(std::move(aligned)));
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}

struct Solution {
  Mat4 pose = Mat4::Identity();
  bool converged = false;
  int iterations = 0;
};

Solution icp(const PointCloud& source, const PointCloud& target,
             const pcl::search::KdTree<pcl::PointXYZ>& tree, Mat4 pose, const ParamView& p,
             ExecContext& ctx) {
  Solution solution;
  const bool normals = p.choice("mode") == "point_to_plane";
  for (int i = 0; i < p.integer("maxIterations"); ++i) {
    if (ctx.cancelled())
      return solution;
    auto pairs =
        match(source, target, tree, pose, p.number("maxCorrespondenceDistance"), normals, ctx);
    Mat4 step;
    if (!(normals ? planeStep(pairs, step) : fit(pairs, step)))
      return solution;
    pose = step * pose;
    solution.iterations = i + 1;
    const double translation = step.topRightCorner<3, 1>().norm();
    const double angle = Eigen::AngleAxisd(step.topLeftCorner<3, 3>()).angle();
    if (translation <= p.number("translationTolerance") && angle <= p.number("rotationTolerance")) {
      solution.converged = true;
      break;
    }
  }
  solution.pose = pose;
  return solution;
}

bool positiveParameters(const ParamView& p) {
  for (const char* key :
       {"maxRmse", "maxCorrespondenceDistance", "translationTolerance", "rotationTolerance"})
    if (!std::isfinite(p.number(key)) || p.number(key) <= 0)
      return false;
  return std::isfinite(p.number("minInlierRatio")) && p.number("minInlierRatio") > 0 &&
         p.number("minInlierRatio") <= 1 && p.integer("maxIterations") >= 1 &&
         p.integer("maxIterations") <= 10000;
}

} // namespace

void registerRegistration3D(Registry& r) {
  OperatorDesc op = descriptor("register.rigid_fit", "对应点三维刚体拟合",
                               "3D corresponding points rigid best fit Kabsch");
  op.inputs = {{"source", "PointCloud", "源点", "按点序一一对应，至少3个非共线点。", true},
               {"target", "PointCloud", "目标点", "与源点等长。", true}};
  op.outputs = {{"transform", "Transform", "变换", "source→target。", true},
                {"aligned", "PointCloud", "变换后的源云", "保留并旋转可选法线。", true},
                {"quality", "Record", "配准质量", "cloud.Registration.v1。", true}};
  op.params = {text("sourceFrame", "源参考帧", "source"),
               text("targetFrame", "目标参考帧", "target"),
               number("maxRmse", "最大RMSE", 0.01, 1e-12, 1e6, "m"),
               number("minInlierRatio", "最小匹配比例", 1, 1e-9, 1)};
  op.doc = "double中心化SVD拟合对应点，修正反射；拒绝共线/重合退化。与混合点线面约束拟合分开。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const PointCloud *source, *target;
    Mat4 matrix;
    Status s = inputs(in, p, source, target, matrix);
    if (!s.ok)
      return s;
    if (source->pointCount() != target->pointCount())
      return error("对应点数量不一致", "target");
    if (!std::isfinite(p.number("maxRmse")) || p.number("maxRmse") <= 0 ||
        !std::isfinite(p.number("minInlierRatio")) || p.number("minInlierRatio") <= 0 ||
        p.number("minInlierRatio") > 1)
      return error("maxRmse必须为正有限数", "maxRmse", "bad_param");
    std::vector<Pair> pairs;
    for (std::size_t i = 0; i < source->pointCount(); ++i)
      pairs.push_back({point(*source, i), point(*target, i), Vec3::Zero()});
    if (!fit(pairs, matrix))
      return error("对应点共线、重合或数值退化", "source", "degenerate_geometry");
    return emit(*source, *target, matrix, nullptr, p, out, ctx, 1, "corresponding_points", true);
  };
  r.addOperator(op);
  for (bool search : {false, true}) {
    op.id = search ? "register.local_search_3d" : "register.icp_3d";
    op.label = search ? "三维局部多初值配准" : "三维迭代最近点配准";
    op.keywords = {"SE3 ICP 3D registration point to plane local search", "三维配准", "局部搜索"};
    op.inputs.push_back({"initial", "Transform", "初始变换", "可选source→target初值。", false});
    op.params = {text("sourceFrame", "源参考帧", "source"),
                 text("targetFrame", "目标参考帧", "target"),
                 choice("mode", "残差模型", "point_to_point",
                        {{"point_to_point", "点到点"}, {"point_to_plane", "点到面"}}),
                 number("maxCorrespondenceDistance", "对应距离上限", 0.1, 1e-12, 1e6, "m"),
                 number("maxRmse", "最大RMSE", 0.01, 1e-12, 1e6, "m"),
                 number("minInlierRatio", "最小匹配比例", 0.7, 1e-9, 1),
                 integer("maxIterations", "迭代上限", 100, 1, 10000),
                 number("translationTolerance", "平移收敛阈值", 1e-7, 1e-12, 1, "m"),
                 number("rotationTolerance", "旋转收敛阈值", 1e-7, 1e-12, 1, "rad")};
    if (search) {
      op.params.push_back(vector("searchTranslation", "目标帧平移搜索半径", {0.02, 0.02, 0.02}));
      op.params.push_back(number("searchAngle", "Z轴局部旋转半径", 0.1, 0, 0.5, "rad"));
    }
    op.doc =
        "局部SE(3)配准，source→target；点面模式必须有目标法线且六自由度可观测。未收敛/匹配比例低/RMSE高均拒绝。局部搜索使用至多9个显式初值，不是全局匹配。";
    op.compute = [search](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
      const PointCloud *source, *target;
      Mat4 initial;
      Status s = inputs(in, p, source, target, initial);
      if (!s.ok)
        return s;
      if (!positiveParameters(p))
        return error("迭代或质量门限参数非法", "maxIterations", "bad_param");
      if (p.choice("mode") != "point_to_point" && p.choice("mode") != "point_to_plane")
        return error("未知残差模型", "mode", "bad_param");
      const bool normals = p.choice("mode") == "point_to_plane";
      if (normals && !target->hasNormals())
        return error("点面ICP需要目标法线", "target");
      if (normals)
        for (std::size_t i = 0; i < target->pointCount(); ++i) {
          const Vec3 n(target->normals[i * 3], target->normals[i * 3 + 1],
                       target->normals[i * 3 + 2]);
          if (!n.allFinite() || n.norm() <= 0)
            return error("目标法线必须有限且非零", "target");
        }
      for (std::size_t i = 0; i < target->pointCount(); ++i)
        if ((point(*target, i) - point(*target, 0)).cwiseAbs().maxCoeff() > kSearchSpan)
          return error("目标云跨度超过float近邻搜索的安全平方距离范围", "target");
      auto pclTarget = adapter::toPcl(*target);
      pcl::search::KdTree<pcl::PointXYZ> tree;
      tree.setInputCloud(pclTarget);
      std::vector<Mat4> starts{initial};
      if (search) {
        const auto radius = p.vec3("searchTranslation");
        if (!std::isfinite(p.number("searchAngle")) || p.number("searchAngle") < 0 ||
            p.number("searchAngle") > 0.5)
          return error("局部角度超出[0,0.5]rad", "searchAngle", "bad_param");
        for (int axis = 0; axis < 3; ++axis) {
          if (!std::isfinite(radius[axis]) || radius[axis] < 0)
            return error("平移搜索半径必须非负有限", "searchTranslation", "bad_param");
          for (double sign : {-1.0, 1.0}) {
            Mat4 candidate = initial;
            candidate(axis, 3) += sign * radius[axis];
            starts.push_back(candidate);
          }
        }
        for (double sign : {-1.0, 1.0}) {
          Mat4 rotation = Mat4::Identity();
          rotation.topLeftCorner<3, 3>() =
              Eigen::AngleAxisd(sign * p.number("searchAngle"), Vec3::UnitZ()).toRotationMatrix();
          starts.push_back(rotation * initial);
        }
      }
      Solution best;
      double bestRmse = std::numeric_limits<double>::infinity();
      for (const auto& start : starts) {
        Solution candidate = icp(*source, *target, tree, start, p, ctx);
        if (ctx.cancelled())
          return Status::Ok();
        if (!candidate.converged)
          continue;
        Transform nativePose;
        PointCloud aligned;
        std::vector<Pair> nativePairs;
        if (!delivered(*source, *target, candidate.pose, &tree, p, ctx, nativePose, aligned,
                       nativePairs)) {
          if (ctx.cancelled())
            return Status::Ok();
          continue;
        }
        const double residual = rmse(nativePairs);
        const double ratio = static_cast<double>(nativePairs.size()) / source->pointCount();
        if (ratio >= p.number("minInlierRatio") && residual < bestRmse) {
          bestRmse = residual;
          best = std::move(candidate);
        }
      }
      return emit(*source, *target, best.pose, &tree, p, out, ctx, best.iterations,
                  p.choice("mode"), best.converged);
    };
    r.addOperator(op);
    op.inputs.pop_back();
  }
}

} // namespace lyflow::ops
