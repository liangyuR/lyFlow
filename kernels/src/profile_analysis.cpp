#include "lyflow/kernel/profile_analysis.h"

#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <limits>

namespace lyflow::kernel::profile_analysis {
using Vec2 = Eigen::Vector2d;
using Mat4 = Eigen::Matrix4d;

bool validateCloud(const CloudView& cloud, std::size_t minimum, std::string& why) {
  const auto n = cloud.pointCount();
  if (cloud.xyz.size() != n * 3 || (!cloud.intensity.empty() && cloud.intensity.size() != n) ||
      (!cloud.normals.empty() && cloud.normals.size() != n * 3) ||
      (!cloud.rgb.empty() && cloud.rgb.size() != n * 3) || n < minimum ||
      n > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    why = "点云通道不一致、点数不足或超过原生索引范围";
    return false;
  }
  for (float value : cloud.xyz)
    if (!std::isfinite(value)) {
      why = "点云坐标必须全部有限；请先显式移除无效点";
      return false;
    }
  for (float value : cloud.intensity)
    if (!std::isfinite(value)) {
      why = "点云强度必须有限";
      return false;
    }
  for (float value : cloud.normals)
    if (!std::isfinite(value)) {
      why = "点云法线分量必须有限";
      return false;
    }
  why.clear();
  return true;
}

bool fitLine(const CloudView& cloud, int x, int y, double minimum, double maximum, LineFit& output,
             const CancelCheck& cancelled) {
  if (x < 0 || x > 2 || y < 0 || y > 2 || x == y || std::isnan(minimum) || std::isnan(maximum) ||
      minimum > maximum)
    return false;
  std::string why;
  if (!validateCloud(cloud, 3, why))
    return false;
  struct WorkingLine {
    Vec2 center = Vec2::Zero(), direction = Vec2::UnitX();
    double minimum = 0, maximum = 0, rms = 0;
    std::size_t count = 0;
  } result;
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    if ((i & 8191) == 0 && (cancelled && cancelled()))
      return false;
    const Vec2 value(cloud.xyz[i * 3 + x], cloud.xyz[i * 3 + y]);
    if (value.x() < minimum || value.x() > maximum)
      continue;
    if (!result.count)
      result.minimum = result.maximum = value.x();
    result.minimum = std::min(result.minimum, value.x());
    result.maximum = std::max(result.maximum, value.x());
    result.center += value;
    ++result.count;
  }
  if (result.count < 3)
    return false;
  result.center /= static_cast<double>(result.count);
  Eigen::Matrix2d covariance = Eigen::Matrix2d::Zero();
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    const Vec2 value(cloud.xyz[i * 3 + x], cloud.xyz[i * 3 + y]);
    if (value.x() >= minimum && value.x() <= maximum) {
      const Vec2 delta = value - result.center;
      covariance += delta * delta.transpose();
    }
  }
  Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> solver(covariance);
  if (solver.info() != Eigen::Success || solver.eigenvalues()[1] <= 0 ||
      solver.eigenvalues()[1] - solver.eigenvalues()[0] <= solver.eigenvalues()[1] * 1e-8)
    return false;
  result.direction = solver.eigenvectors().col(1);
  if (result.direction.x() < 0)
    result.direction *= -1;
  result.center = result.center.cast<float>().cast<double>();
  result.direction = result.direction.cast<float>().cast<double>();
  double squared = 0;
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    if ((i & 8191) == 0 && (cancelled && cancelled()))
      return false;
    const Vec2 value(cloud.xyz[i * 3 + x], cloud.xyz[i * 3 + y]);
    if (value.x() >= minimum && value.x() <= maximum) {
      const Vec2 delta = value - result.center;
      const double cross = delta.x() * result.direction.y() - delta.y() * result.direction.x();
      squared += cross * cross / result.direction.squaredNorm();
    }
  }
  result.rms = std::sqrt(squared / result.count);
  if (!std::isfinite(result.rms))
    return false;
  LineFit fit;
  fit.center = {result.center.x(), result.center.y()};
  fit.direction = {result.direction.x(), result.direction.y()};
  fit.minimum = result.minimum;
  fit.maximum = result.maximum;
  fit.rms = result.rms;
  fit.count = result.count;
  output = fit;
  return true;
}

Line2D nativeLine(const LineFit& profile) {
  Line2D line;
  line.unit = Unit2D::Meter;
  for (int i = 0; i < 2; ++i) {
    line.point[i] = static_cast<float>(profile.center[i]);
    line.dir[i] = static_cast<float>(profile.direction[i]);
  }
  return line;
}

namespace {
Mat4 eigenMatrix(const Matrix4& source) {
  Mat4 matrix;
  for (int i = 0; i < 16; ++i)
    matrix(i / 4, i % 4) = source[i];
  return matrix;
}
Matrix4 arrayMatrix(const Mat4& source) {
  Matrix4 matrix;
  for (int i = 0; i < 16; ++i)
    matrix[i] = source(i / 4, i % 4);
  return matrix;
}
}

bool rigidMatrix(const Transform& transform, Matrix4& output) {
  Mat4 matrix;
  for (int i = 0; i < 16; ++i)
    matrix(i / 4, i % 4) = transform.m[i];
  const Eigen::Matrix3d rotation = matrix.topLeftCorner<3, 3>();
  if (!matrix.allFinite() || (matrix.row(3) - Eigen::RowVector4d(0, 0, 0, 1)).norm() > 1e-6 ||
      (rotation.transpose() * rotation - Eigen::Matrix3d::Identity()).norm() > 1e-5 ||
      std::abs(rotation.determinant() - 1) > 1e-5)
    return false;
  output = arrayMatrix(matrix);
  return true;
}

Matrix4 identityMatrix() { return arrayMatrix(Mat4::Identity()); }
Matrix4 compose(const Matrix4& outer, const Matrix4& inner) {
  return arrayMatrix(eigenMatrix(outer) * eigenMatrix(inner));
}

bool transformCloud(const CloudView& input, const Matrix4& transform, CloudChannels& output,
                    const CancelCheck& cancelled, const Progress& progress) {
  std::string why;
  if (!validateCloud(input, 0, why))
    return false;
  const Mat4 matrix = eigenMatrix(transform);
  CloudChannels result;
  result.xyz.resize(input.xyz.size());
  result.intensity = input.intensity;
  result.rgb = input.rgb;
  result.normals.resize(input.normals.size());
  for (std::size_t i = 0; i < input.pointCount(); ++i) {
    if ((i & 8191) == 0) {
      if (cancelled && cancelled())
        return false;
      if (progress)
        progress(static_cast<float>(static_cast<double>(i) / input.pointCount()));
    }
    const Eigen::Vector3d point(input.xyz[i * 3], input.xyz[i * 3 + 1], input.xyz[i * 3 + 2]);
    const Eigen::Vector3d moved =
        matrix.topLeftCorner<3, 3>() * point + matrix.topRightCorner<3, 1>();
    if (!moved.allFinite() || moved.cwiseAbs().maxCoeff() > std::numeric_limits<float>::max())
      return false;
    for (int j = 0; j < 3; ++j)
      result.xyz[i * 3 + j] = static_cast<float>(moved[j]);
    if (input.hasNormals()) {
      const Eigen::Vector3d normal(input.normals[i * 3], input.normals[i * 3 + 1],
                                   input.normals[i * 3 + 2]);
      Eigen::Vector3d rotated = matrix.topLeftCorner<3, 3>() * normal;
      if (!rotated.allFinite() || !(rotated.norm() > 0))
        return false;
      rotated.normalize();
      for (int j = 0; j < 3; ++j)
        result.normals[i * 3 + j] = static_cast<float>(rotated[j]);
    }
  }
  output = std::move(result);
  return true;
}
} // namespace lyflow::kernel::profile_analysis
