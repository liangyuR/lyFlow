#pragma once
#include <array>
#include <initializer_list>
#include <string>
#include <utility>
#include "lyflow/pose.h"
namespace lyflow::kernel {
using V3 = std::array<double, 3>;
using M4 = std::array<double, 16>;
using Json = nlohmann::json;
bool finite(V3 value);
V3 sub(V3 a, V3 b);
V3 add(V3 a, V3 b);
V3 mul(V3 value, double scale);
V3 cross(V3 a, V3 b);
double dot(V3 a, V3 b);
bool normalize(V3& value);
double sum3(double a, double b, double c);
double productSum(std::initializer_list<std::pair<double, double>> terms);
V3 apply(const M4& matrix, V3 point, bool direction = false);
bool affine(const Transform* transform, M4& matrix, std::string& why);
bool rigid(const Transform* transform, M4& matrix, poses::Quaternion& rotation, std::string& why);
bool similarity2D(const Transform* transform, M4& matrix, double& scale, bool proper,
                  std::string& why);
bool nativeFloat(double value);
V3 jvector(const Json& value);
int axis(const std::string& value);
V3 basis(int index);
V3 rotated(const poses::Quaternion& rotation, V3 vector);
poses::Quaternion turn(int axis, double radians);
poses::Quaternion inverse(poses::Quaternion rotation);
double angular(poses::Quaternion a, poses::Quaternion b);
bool validNumber(double value, double bound = 1e12);
bool bounded(const Record& record);
bool transformPoses3D(Record& record, const Transform* transform, const std::string& target,
                      std::string& why, bool* invalidResult = nullptr);
Transform legacyEulerDegrees(const std::array<float, 3>& translation,
                             const std::array<float, 3>& rotation);
}
