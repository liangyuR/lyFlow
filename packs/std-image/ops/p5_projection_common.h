#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#include "lyflow/calibration.h"
#include "lyflow/operator.h"
#include "lyflow_cv/compat.h"

namespace lyflow::ops::projection5 {

inline constexpr const char* kType = "vision.DepthMapping.v1";
inline constexpr std::size_t kMaxPixels = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxCloudPoints = 1u << 24;
inline constexpr std::uint8_t kObserved = 1;
inline constexpr std::uint8_t kInterpolated = 2;
inline constexpr std::uint8_t kFiltered = 4;
inline constexpr std::uint8_t kCollision = 8;

struct Grid {
  int width = 0, height = 0, fullWidth = 0, fullHeight = 0, scale = 1;
  std::string imageFrame, gridFrame, cloudFrame, projection;
  cv::Matx44d gridToCloud = cv::Matx44d::eye();
  calibration::Camera camera;
  cv::Vec2d origin{0, 0}, resolution{1, 1};
};

inline Status failure(const std::string& message, const char* port = "mapping",
                      const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}

inline bool valid(std::uint8_t value) { return (value & (kObserved | kInterpolated)) != 0; }

struct Cancelled {};

inline std::string hash(const void* values, std::size_t bytes, ExecContext* ctx = nullptr) {
  std::uint64_t value = 14695981039346656037ull;
  const auto* input = static_cast<const std::uint8_t*>(values);
  for (std::size_t i = 0; i < bytes; ++i) {
    if ((i & 65535) == 0 && ctx && ctx->cancelled())
      throw Cancelled{};
    value ^= input[i];
    value *= 1099511628211ull;
  }
  if (ctx && ctx->cancelled())
    throw Cancelled{};
  std::ostringstream result;
  result << std::hex << std::setfill('0') << std::setw(16) << value;
  return result.str();
}

inline std::string hash(const Tensor& value, ExecContext* ctx = nullptr) {
  return hash(value.data.data(), value.data.size() * sizeof(float), ctx);
}

inline std::string hash(const Image& value, ExecContext* ctx = nullptr) {
  return hash(value.pixels.get(), value.byteSize(), ctx);
}

inline bool rigid(const nlohmann::json& data, cv::Matx44d& matrix) {
  if (!data.is_array() || data.size() != 16)
    return false;
  for (int i = 0; i < 16; ++i) {
    if (!values::safeNumber(data[i]))
      return false;
    matrix.val[i] = data[i].get<double>();
  }
  for (int i = 0; i < 4; ++i)
    if (std::abs(matrix(3, i) - (i == 3 ? 1.0 : 0.0)) > 1e-6)
      return false;
  for (int i = 0; i < 3; ++i)
    if (std::abs(matrix(i, 3)) > std::numeric_limits<float>::max())
      return false;
  cv::Matx33d rotation;
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x)
      rotation(y, x) = matrix(y, x);
  return cv::norm(rotation.t() * rotation - cv::Matx33d::eye()) <= 1e-5 &&
         std::abs(cv::determinant(rotation) - 1) <= 1e-5;
}

inline cv::Matx44d inverse(const cv::Matx44d& matrix) {
  auto result = cv::Matx44d::eye();
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column)
      result(row, column) = matrix(column, row);
    result(row, 3) = 0;
    for (int k = 0; k < 3; ++k)
      result(row, 3) -= result(row, k) * matrix(k, 3);
  }
  return result;
}

inline nlohmann::json cameraData(const calibration::Camera& camera) {
  return {{"imageSize", {camera.width, camera.height}},
          {"cameraMatrix", camera.matrix},
          {"distortion", camera.distortion},
          {"frame", camera.frame},
          {"unit", "m"}};
}

inline Record rebind(const Record& previous, const PointCloud& cloud, const Image& depth,
                     const Image& status, const Tensor& indexMap, ExecContext& ctx,
                     const Tensor* sourceIndexMap = nullptr) {
  Record result = previous;
  if (result.data.contains("sourceCloudId") &&
      result.data["sourceCloudId"] != std::to_string(cloud.id)) {
    result.data["parentCloudId"] = result.data["sourceCloudId"];
    result.data["parentPointCount"] = result.data["sourcePointCount"];
    result.data["parentCloudFrame"] = result.data["cloudFrame"];
  }
  result.data.erase("sourceIndexHash");
  if (sourceIndexMap)
    result.data["sourceIndexHash"] = hash(*sourceIndexMap, &ctx);
  result.data["sourceCloudId"] = std::to_string(cloud.id);
  result.data["sourcePointCount"] = cloud.pointCount();
  result.data["indexHash"] = hash(indexMap, &ctx);
  result.data["depthHash"] = hash(depth, &ctx);
  result.data["statusHash"] = hash(status, &ctx);
  return result;
}

inline Record make(const Grid& grid, const PointCloud& cloud, const Image& depth,
                   const Image& status, const Tensor& indexMap, ExecContext& ctx) {
  nlohmann::json matrix = nlohmann::json::array();
  for (double value : grid.gridToCloud.val)
    matrix.push_back(value);
  Record result{kType,
                {{"width", grid.width},
                 {"height", grid.height},
                 {"fullWidth", grid.fullWidth},
                 {"fullHeight", grid.fullHeight},
                 {"scale", grid.scale},
                 {"imageFrame", grid.imageFrame},
                 {"gridFrame", grid.gridFrame},
                 {"cloudFrame", grid.cloudFrame},
                 {"unit", "m"},
                 {"projection", grid.projection},
                 {"gridToCloud", std::move(matrix)}}};
  if (grid.projection == "perspective")
    result.data["camera"] = cameraData(grid.camera);
  else {
    result.data["origin"] = {grid.origin[0], grid.origin[1]};
    result.data["resolution"] = {grid.resolution[0], grid.resolution[1]};
  }
  return rebind(result, cloud, depth, status, indexMap, ctx);
}

struct RayBudget {
  std::size_t maximumEvaluations = 64 * 1024 * 1024;
  std::size_t evaluations = 0;
  bool exhausted = false;
  ExecContext* context = nullptr;
};

inline bool normalizedRay(const calibration::Camera& camera, double x, double y, cv::Vec3d& ray,
                          RayBudget* budget = nullptr) {
  if (!std::isfinite(x) || !std::isfinite(y) || !(camera.matrix[0] > 0 && camera.matrix[4] > 0))
    return false;
  if (budget && budget->context && budget->context->cancelled())
    throw Cancelled{};
  double ux = (x - camera.matrix[2]) / camera.matrix[0],
         uy = (y - camera.matrix[5]) / camera.matrix[4];
  if (!std::isfinite(ux) || !std::isfinite(uy))
    return false;
  if (std::all_of(camera.distortion.begin(), camera.distortion.end(),
                  [](double value) { return value == 0; })) {
    ray = {ux, uy, 1};
    return true;
  }
  const cv::Matx33d k(camera.matrix.data());
  const cv::Mat distortion(camera.distortion);
  auto evaluate = [&](double a, double b, cv::Point2d& projected, cv::Mat& jacobian) {
    if (budget) {
      if (budget->context && budget->context->cancelled())
        throw Cancelled{};
      if (budget->evaluations >= budget->maximumEvaluations) {
        budget->exhausted = true;
        return false;
      }
      ++budget->evaluations;
    }
    if (!std::isfinite(a) || !std::isfinite(b))
      return false;
    const std::vector<cv::Point3d> object{{a, b, 1}};
    std::vector<cv::Point2d> pixels;
    cv::projectPoints(object, cv::Vec3d::all(0), cv::Vec3d::all(0), k, distortion, pixels,
                      jacobian);
    if (pixels.size() != 1 || !std::isfinite(pixels[0].x) || !std::isfinite(pixels[0].y) ||
        jacobian.rows != 2 || jacobian.cols < 6)
      return false;
    projected = pixels[0];
    return true;
  };
  cv::Point2d projected;
  cv::Mat jacobian;
  if (!evaluate(ux, uy, projected, jacobian))
    return false;
  for (int iteration = 0; iteration < 32; ++iteration) {
    const double rx = projected.x - x, ry = projected.y - y;
    const double error = std::hypot(rx, ry);
    if (error <= 1e-7) {
      ray = {ux, uy, 1};
      return true;
    }
    const double a = jacobian.at<double>(0, 3), b = jacobian.at<double>(0, 4),
                 c = jacobian.at<double>(1, 3), d = jacobian.at<double>(1, 4);
    const double scale = std::max({std::abs(a), std::abs(b), std::abs(c), std::abs(d)});
    if (!(scale > 0) || !std::isfinite(scale))
      return false;
    const double determinant = (a / scale) * (d / scale) - (b / scale) * (c / scale);
    if (!(determinant > 1e-12) || !std::isfinite(determinant))
      return false;
    const double dx = ((d / scale) * (rx / scale) - (b / scale) * (ry / scale)) / determinant;
    const double dy = (-(c / scale) * (rx / scale) + (a / scale) * (ry / scale)) / determinant;
    if (!std::isfinite(dx) || !std::isfinite(dy))
      return false;
    bool accepted = false;
    for (int search = 0; search < 12; ++search) {
      const double weight = std::ldexp(1.0, -search), nx = ux - weight * dx, ny = uy - weight * dy;
      cv::Point2d candidate;
      cv::Mat candidateJacobian;
      if (!evaluate(nx, ny, candidate, candidateJacobian)) {
        if (budget && budget->exhausted)
          return false;
        continue;
      }
      if (std::hypot(candidate.x - x, candidate.y - y) < error) {
        ux = nx;
        uy = ny;
        projected = candidate;
        jacobian = std::move(candidateJacobian);
        accepted = true;
        break;
      }
    }
    if (!accepted)
      return false;
  }
  return false;
}

inline bool backProjectFull(const Grid& grid, double x, double y, double z, cv::Vec3d& output,
                            RayBudget* budget = nullptr) {
  cv::Vec3d local;
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    return false;
  if (grid.projection == "perspective") {
    if (!(z > 0))
      return false;
    cv::Vec3d normalized;
    if (!normalizedRay(grid.camera, x, y, normalized, budget))
      return false;
    local = normalized * z;
  } else {
    local = {grid.origin[0] + x * grid.resolution[0], grid.origin[1] + y * grid.resolution[1], z};
  }
  const cv::Vec4d point = grid.gridToCloud * cv::Vec4d(local[0], local[1], local[2], 1);
  output = {point[0], point[1], point[2]};
  for (double value : output.val)
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
      return false;
  return true;
}

inline double fullCoordinate(double value, int scale, int extent) {
  const int remainder = extent % scale;
  if (!remainder)
    return (value + .5) * scale - .5;
  const int last = extent / scale;
  if (last == 0)
    return value * extent + (extent - 1) * .5;
  if (value <= last - 1)
    return (value + .5) * scale - .5;
  const double previous = (last - .5) * scale - .5;
  const double center = last * scale + (remainder - 1) * .5;
  if (value <= last)
    return previous + (value - last + 1) * (center - previous);
  return center + (value - last) * remainder;
}

inline double fullX(const Grid& grid, double value) {
  return fullCoordinate(value, grid.scale, grid.fullWidth);
}
inline double fullY(const Grid& grid, double value) {
  return fullCoordinate(value, grid.scale, grid.fullHeight);
}

inline double fullBoundaryX(const Grid& grid, double value) {
  return std::clamp((value + .5) * grid.scale - .5, -.5, grid.fullWidth - .5);
}
inline double fullBoundaryY(const Grid& grid, double value) {
  return std::clamp((value + .5) * grid.scale - .5, -.5, grid.fullHeight - .5);
}

inline bool backProject(const Grid& grid, double x, double y, double z, cv::Vec3d& output,
                        RayBudget* budget = nullptr) {
  return backProjectFull(grid, fullX(grid, x), fullY(grid, y), z, output, budget);
}

inline bool backProjectBoundary(const Grid& grid, double x, double y, double z, cv::Vec3d& output,
                                RayBudget* budget = nullptr) {
  return backProjectFull(grid, fullBoundaryX(grid, x), fullBoundaryY(grid, y), z, output, budget);
}

inline Status parse(const PointCloud& cloud, const Image& depth, const Image& status,
                    const Tensor& indexMap, const Record& mapping, Grid& grid, ExecContext& ctx) {
  try {
    std::string why;
    const auto& d = mapping.data;
    if (mapping.type != kType || !values::boundedJson(d, why) || !d.is_object())
      return failure("需要有界 vision.DepthMapping.v1：" + why);
    auto integer = [&](const char* name, int low, int high, int& destination) {
      if (!d.contains(name) || !d[name].is_number_integer())
        return false;
      const double value = d[name].get<double>();
      if (value < low || value > high)
        return false;
      destination = static_cast<int>(value);
      return true;
    };
    auto text = [&](const char* name, std::string& destination) {
      if (!d.contains(name) || !d[name].is_string())
        return false;
      destination = d[name].get<std::string>();
      return !destination.empty() && destination.size() <= 256;
    };
    if (!integer("width", 1, 32768, grid.width) || !integer("height", 1, 32768, grid.height) ||
        !integer("fullWidth", 1, 32768, grid.fullWidth) ||
        !integer("fullHeight", 1, 32768, grid.fullHeight) ||
        !integer("scale", 1, 32768, grid.scale) || (grid.scale & (grid.scale - 1)) != 0 ||
        !text("imageFrame", grid.imageFrame) || !text("gridFrame", grid.gridFrame) ||
        !text("cloudFrame", grid.cloudFrame) || !text("projection", grid.projection) ||
        d.value("unit", "") != "m" || !d.contains("gridToCloud") ||
        !rigid(d["gridToCloud"], grid.gridToCloud))
      return failure("映射尺寸、帧、单位或刚体变换无效");
    if ((grid.fullWidth + grid.scale - 1) / grid.scale != grid.width ||
        (grid.fullHeight + grid.scale - 1) / grid.scale != grid.height ||
        !cloud.channelsConsistent() || cloud.pointCount() > kMaxCloudPoints ||
        !d.contains("sourceCloudId") || d["sourceCloudId"] != std::to_string(cloud.id) ||
        !d.contains("sourcePointCount") || !d["sourcePointCount"].is_number_integer() ||
        d["sourcePointCount"].get<double>() < 0 ||
        d["sourcePointCount"].get<std::size_t>() != cloud.pointCount())
      return failure("映射来源点云、分辨率或可选通道不匹配");
    if (grid.projection == "perspective") {
      if (!d.contains("camera") ||
          !calibration::parseCamera({calibration::kCameraType, d["camera"]}, grid.camera, why) ||
          grid.camera.width != grid.fullWidth || grid.camera.height != grid.fullHeight ||
          grid.camera.frame != grid.gridFrame)
        return failure("相机映射标定不匹配：" + why);
    } else if (grid.projection == "orthographic") {
      for (const char* name : {"origin", "resolution"}) {
        if (!d.contains(name) || !d[name].is_array() || d[name].size() != 2)
          return failure("正交映射需要origin/resolution二维向量");
        for (int i = 0; i < 2; ++i) {
          if (!values::safeNumber(d[name][i]))
            return failure("正交映射栅格值必须有限");
          (name[0] == 'o' ? grid.origin : grid.resolution)[i] = d[name][i].get<double>();
        }
      }
      if (!(grid.resolution[0] > 0 && grid.resolution[1] > 0))
        return failure("正交像素分辨率须为正米数");
    } else {
      return failure("未知投影模型");
    }
    if (!depth.consistent() || depth.depth != PixelDepth::F32 || depth.channels != 1 ||
        depth.width != grid.width || depth.height != grid.height || depth.scale != grid.scale ||
        !status.consistent() || status.depth != PixelDepth::U8 || status.channels != 1 ||
        status.width != grid.width || status.height != grid.height || status.scale != grid.scale ||
        !indexMap.consistent() ||
        indexMap.shape != std::vector<std::int64_t>{grid.height, grid.width})
      return failure("深度、状态图、原生索引图必须与映射尺寸/scale一致");
    if (depth.pixelCount() > kMaxPixels)
      return failure("映射超过8Mi像素预算", "mapping", "resource_limit");
    if (d.value("indexHash", "") != hash(indexMap, &ctx) ||
        d.value("depthHash", "") != hash(depth, &ctx) ||
        d.value("statusHash", "") != hash(status, &ctx))
      return failure("深度、状态或索引载荷不属于该映射");
    const auto cloudToGrid = inverse(grid.gridToCloud);
    for (std::size_t i = 0; i < indexMap.data.size(); ++i) {
      if ((i & 16383) == 0 && ctx.cancelled())
        return failure("深度映射校验已取消", "mapping", "cancelled");
      const auto state = status.pixels.get()[i];
      const float index = indexMap.data[i];
      float value;
      std::memcpy(&value, depth.pixels.get() + i * sizeof(float), sizeof(float));
      if ((state & ~15) || ((state & kObserved) && (state & kInterpolated)) ||
          (!valid(state) && state != 0) || !std::isfinite(value) || !std::isfinite(index))
        return failure("深度状态位或数值无效");
      if (!valid(state)) {
        if (index != -1 || value != 0)
          return failure("无效像素必须深度0、索引-1");
      } else {
        if (index < 0 || index != std::floor(index) || index >= cloud.pointCount() ||
            (grid.projection == "perspective" && !(value > 0)))
          return failure("有效像素必须关联合法有限源点及物理深度");
        const auto source = static_cast<std::size_t>(index);
        for (int axis = 0; axis < 3; ++axis)
          if (!std::isfinite(cloud.xyz[3 * source + axis]) ||
              (cloud.hasNormals() && !std::isfinite(cloud.normals[3 * source + axis])))
            return failure("映射引用的点坐标/法线必须有限");
        if (cloud.hasIntensity() && !std::isfinite(cloud.intensity[source]))
          return failure("映射引用的强度必须有限");
        const auto physical =
            cloudToGrid * cv::Vec4d(cloud.xyz[3 * source], cloud.xyz[3 * source + 1],
                                    cloud.xyz[3 * source + 2], 1);
        if (!std::isfinite(physical[2]) ||
            std::abs(physical[2] - value) > 1e-5 * std::max(1.0, std::abs(physical[2])))
          return failure("深度值必须与索引引用点的物理Z一致");
      }
    }
    return Status::Ok();
  } catch (const Cancelled&) {
    return failure("深度映射校验已取消", "mapping", "cancelled");
  } catch (const std::exception& error) {
    return failure(std::string("深度映射字段类型无效：") + error.what());
  }
}

} // namespace lyflow::ops::projection5
