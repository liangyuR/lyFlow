#include "lyflow/kernel/rigid.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace lyflow::kernel {
bool finite(V3 v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
V3 sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(V3 a, V3 b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool normalize(V3& v) {
  const double length = std::hypot(v[0], v[1], v[2]);
  if (!std::isfinite(length) || length == 0)
    return false;
  for (double& c : v)
    c /= length;
  return true;
}
double sum3(double a, double b, double c) {
  double sum = 0, error = 0;
  for (double value : {a, b, c}) {
    const double next = sum + value;
    error += std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
    sum = next;
  }
  return sum + error;
}
double productSum(std::initializer_list<std::pair<double, double>> terms) {
  double sum = 0, error = 0;
  const auto add = [&](double value) {
    const double next = sum + value;
    error += std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
    sum = next;
  };
  for (const auto& [a, b] : terms) {
    const double product = a * b;
    add(product);
    add(std::fma(a, b, -product));
  }
  return sum + error;
}
V3 apply(const M4& m, V3 p, bool direction) {
  V3 result{};
  for (int i = 0; i < 3; ++i)
    result[i] = productSum({{m[i * 4], p[0]},
                            {m[i * 4 + 1], p[1]},
                            {m[i * 4 + 2], p[2]},
                            {direction ? 0 : m[i * 4 + 3], 1}});
  return result;
}
bool affine(const Transform* transform, M4& m, std::string& why) {
  if (!transform) {
    why = "需要原生 Transform";
    return false;
  }
  for (int i = 0; i < 16; ++i) {
    m[i] = transform->m[i];
    if (!std::isfinite(m[i])) {
      why = "变换矩阵必须有限";
      return false;
    }
  }
  if (m[12] != 0 || m[13] != 0 || m[14] != 0 || m[15] != 1) {
    why = "变换末行必须是 [0,0,0,1]";
    return false;
  }
  why.clear();
  return true;
}
bool rigid(const Transform* transform, M4& m, poses::Quaternion& q, std::string& why) {
  if (!affine(transform, m, why))
    return false;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double product = 0;
      for (int k = 0; k < 3; ++k)
        product += m[k * 4 + i] * m[k * 4 + j];
      if (std::abs(product - (i == j ? 1 : 0)) > 1e-5) {
        why = "位姿仅接受刚体变换，旋转不得含尺度或剪切";
        return false;
      }
    }
  const V3 x{m[0], m[4], m[8]}, y{m[1], m[5], m[9]}, z{m[2], m[6], m[10]};
  if (dot(cross(x, y), z) < 0) {
    why = "旋转必须为右手正交矩阵，不接受镜像";
    return false;
  }
  q = poses::quaternion({m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]});
  why.clear();
  return true;
}
bool similarity2D(const Transform* transform, M4& m, double& scale, bool proper, std::string& why) {
  if (!affine(transform, m, why))
    return false;
  const double a = std::hypot(m[0], m[4]), b = std::hypot(m[1], m[5]);
  if (a == 0 || b == 0 || std::abs(a - b) > 1e-6 * std::max(a, b) ||
      std::abs((m[0] / a) * (m[1] / b) + (m[4] / a) * (m[5] / b)) > 1e-6 || m[2] != 0 ||
      m[6] != 0 || m[8] != 0 || m[9] != 0 || m[10] != 1 || m[11] != 0 ||
      (proper && m[0] * m[5] - m[1] * m[4] < 0)) {
    why = "二维变换必须为非退化相似变换，保留 z 平面；工件位姿不接受镜像";
    return false;
  }
  scale = a;
  why.clear();
  return true;
}
bool nativeFloat(double v) {
  return std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max();
}
V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
V3 jvector(const Json& j) { return j.get<V3>(); }
int axis(const std::string& s) { return s == "X" ? 0 : s == "Y" ? 1 : s == "Z" ? 2 : -1; }
V3 basis(int i) {
  if (i < 0 || i > 2) {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    return {nan, nan, nan};
  }
  V3 a{};
  a[i] = 1;
  return a;
}
V3 rotated(const poses::Quaternion& q, V3 v) {
  const auto r = poses::rotationMatrix(q);
  return {productSum({{r[0], v[0]}, {r[1], v[1]}, {r[2], v[2]}}),
          productSum({{r[3], v[0]}, {r[4], v[1]}, {r[5], v[2]}}),
          productSum({{r[6], v[0]}, {r[7], v[1]}, {r[8], v[2]}})};
}
poses::Quaternion turn(int axis, double angle) {
  if (axis < 0 || axis > 2) {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    return {nan, nan, nan, nan};
  }
  poses::Quaternion q{0, 0, 0, std::cos(angle / 2)};
  q[axis] = std::sin(angle / 2);
  return q;
}
poses::Quaternion inverse(poses::Quaternion q) {
  for (int i = 0; i < 3; ++i)
    q[i] = -q[i];
  return q;
}
double angular(poses::Quaternion a, poses::Quaternion b) {
  auto q = poses::multiply(inverse(a), b);
  poses::normalizeQuaternion(q);
  return 2 * std::atan2(std::hypot(q[0], q[1], q[2]), std::abs(q[3]));
}
bool validNumber(double v, double bound) { return std::isfinite(v) && std::abs(v) <= bound; }
bool bounded(const Record& r) {
  std::string why;
  return values::boundedJson(r.data, why, true);
}

bool transformPoses3D(Record& record, const Transform* transform, const std::string& target,
                      std::string& why, bool* invalidResult) {
  if (invalidResult)
    *invalidResult = false;
  if (record.type != poses::kPoses3DType) {
    why = "需要相应维度的位姿或位姿集合";
    return false;
  }
  if (!poses::validate(record, why))
    return false;
  if (!transform) {
    if (record.data["frame"] == target) {
      why.clear();
      return true;
    }
    why = "跨帧必须显式提供 source→target Transform";
    return false;
  }
  M4 m;
  poses::Quaternion q;
  if (!rigid(transform, m, q, why))
    return false;
  Record candidate = record;
  candidate.data["frame"] = target;
  for (auto& item : candidate.data["items"]) {
    item["frame"] = target;
    item["position"] = kernel::apply(m, jvector(item["position"]));
    item["rotation"] = poses::multiply(q, item["rotation"].get<poses::Quaternion>());
  }
  if (!poses::validate(candidate, why)) {
    if (invalidResult)
      *invalidResult = true;
    return false;
  }
  record = std::move(candidate);
  why.clear();
  return true;
}
Transform legacyEulerDegrees(const std::array<float, 3>& t, const std::array<float, 3>& deg) {
  const double toRad = 3.14159265358979323846 / 180.0;
  const double rx = deg[0] * toRad, ry = deg[1] * toRad, rz = deg[2] * toRad;
  const double cx = std::cos(rx), sx = std::sin(rx);
  const double cy = std::cos(ry), sy = std::sin(ry);
  const double cz = std::cos(rz), sz = std::sin(rz);

  // 内旋 X→Y→Z，即 R = Rz * Ry * Rx。这是 ROS / PCL 一系的惯例，
  // 换一种约定会让同一组欧拉角在不同工具里得到不同结果，必须写死并写清楚。
  const double m00 = cz * cy;
  const double m01 = cz * sy * sx - sz * cx;
  const double m02 = cz * sy * cx + sz * sx;
  const double m10 = sz * cy;
  const double m11 = sz * sy * sx + cz * cx;
  const double m12 = sz * sy * cx - cz * sx;
  const double m20 = -sy;
  const double m21 = cy * sx;
  const double m22 = cy * cx;

  Transform out;
  out.m[0] = static_cast<float>(m00);
  out.m[1] = static_cast<float>(m01);
  out.m[2] = static_cast<float>(m02);
  out.m[3] = t[0];
  out.m[4] = static_cast<float>(m10);
  out.m[5] = static_cast<float>(m11);
  out.m[6] = static_cast<float>(m12);
  out.m[7] = t[1];
  out.m[8] = static_cast<float>(m20);
  out.m[9] = static_cast<float>(m21);
  out.m[10] = static_cast<float>(m22);
  out.m[11] = t[2];
  out.m[12] = 0;
  out.m[13] = 0;
  out.m[14] = 0;
  out.m[15] = 1;

  return out;
}
}
