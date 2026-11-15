#pragma once

#include <array>
#include <set>

#include "lyflow_cv/compat.h"
#include "p1_common.h"
#include "p5_projection_common.h"
#include "lyflow/cloud_groups.h"
#include "lyflow/pose.h"

namespace lyflow::ops::inspection5 {
using Json = nlohmann::json;
struct Failure {
  Status status;
};
inline void need(bool condition, const std::string& why, const char* port = "image") {
  if (!condition)
    throw Failure{img::badInput(why, port)};
}
inline void poll(ExecContext& ctx) {
  if (ctx.cancelled())
    throw Failure{Status::Error(Phase::Execute, "cancelled", "任务已取消")};
}
inline void charge(std::uint64_t& work, std::uint64_t count, ExecContext& ctx) {
  poll(ctx);
  if (count > 100000000 || work > 100000000 - count)
    throw Failure{Status::Error(Phase::Execute, "resource_limit", "累计计算超过100M工作量预算")};
  work += count;
}
inline void rayResult(bool success, const projection5::RayBudget& budget, const char* message,
                      const char* port = "mapping") {
  if (!success && budget.exhausted)
    throw Failure{Status::Error(Phase::Execute, "resource_limit", "逆畸变累计正投影预算耗尽")};
  need(success, message, port);
}
inline double scalar(const ParamView& p, const char* name, double lo, double hi) {
  const double v = p.number(name);
  if (!std::isfinite(v) || v < lo || v > hi)
    throw Failure{img::badParam(std::string("数值超出范围：") + name, name)};
  return v;
}
inline int integer(const ParamView& p, const char* name, int lo, int hi) {
  const auto v = p.integer(name);
  if (v < lo || v > hi)
    throw Failure{img::badParam(std::string("整数超出范围：") + name, name)};
  return static_cast<int>(v);
}
inline Param number(const char* name, const char* label, double value, double min, double max,
                    const char* unit = "") {
  auto p = img::floatParam(name, label, value, "");
  p.min = min;
  p.max = max;
  p.unit = unit;
  return p;
}
inline void bounded(const Record& r) {
  std::string why;
  if (!values::boundedJson(r.data, why, true))
    throw Failure{Status::Error(Phase::Execute, "resource_limit", why)};
}
inline Image native(cv::Mat pixels) {
  Image image;
  need(cvx::fromMat(std::move(pixels), image), "原生图像转换失败");
  return image;
}
inline std::array<double, 3> vec(const cv::Vec3d& p) { return {p[0], p[1], p[2]}; }
inline poses::Quaternion rotation(const cv::Vec3d& n) {
  const cv::Vec3d ref = std::abs(n[0]) < .8 ? cv::Vec3d(1, 0, 0) : cv::Vec3d(0, 1, 0);
  auto x = ref - n * n.dot(ref);
  x /= cv::norm(x);
  const auto y = n.cross(x);
  return poses::quaternion({x[0], y[0], n[0], x[1], y[1], n[1], x[2], y[2], n[2]});
}
inline Status caught(const std::exception& e) { return img::badInput(e.what()); }
struct Depth {
  const PointCloud* cloud = nullptr;
  const Image* depth = nullptr;
  const Image* state = nullptr;
  const Tensor* indices = nullptr;
  const Record* mapping = nullptr;
  projection5::Grid grid;
  Depth(const Inputs& in, ExecContext& ctx) {
    cloud = in.get("cloud").asCloud();
    depth = in.get("depth").asImage();
    state = in.get("status").asImage();
    indices = in.get("indexMap").asTensor();
    mapping = in.get("mapping").asRecord();
    need(cloud && depth && state && indices && mapping,
         "需要完整cloud/depth/status/indexMap/mapping", "mapping");
    const auto s = projection5::parse(*cloud, *depth, *state, *indices, *mapping, grid, ctx);
    if (!s.ok)
      throw Failure{s};
    need(grid.scale == 1, "物理检测/填洞/截面需要全分辨率映射", "mapping");
    if (depth->pixelCount() > 2097152 || cloud->pointCount() > 2097152)
      throw Failure{
          Status::Error(Phase::Execute, "resource_limit", "深度检测超过2Mi像素/源点预算")};
  }
  std::uint8_t status(int x, int y) const {
    return state->pixels.get()[static_cast<std::size_t>(y) * grid.width + x];
  }
  float z(int x, int y) const {
    return reinterpret_cast<const float*>(
        depth->pixels.get())[static_cast<std::size_t>(y) * grid.width + x];
  }
  int index(int x, int y) const {
    return static_cast<int>(indices->data[static_cast<std::size_t>(y) * grid.width + x]);
  }
  bool selected(int x, int y, bool synthetic) const {
    return synthetic ? projection5::valid(status(x, y))
                     : (status(x, y) & projection5::kObserved) != 0;
  }
  cv::Vec3d point(int x, int y) const {
    const auto i = static_cast<std::size_t>(index(x, y));
    return {cloud->xyz[3 * i], cloud->xyz[3 * i + 1], cloud->xyz[3 * i + 2]};
  }
};
struct Base {
  cv::Vec3d normal{0, 0, 1};
  double offset = 0;
  Base(const Inputs& in, const ParamView& p, const Depth& view) {
    const auto* plane = in.get("plane").asPlane();
    need(plane && p.text("planeFrame") == view.grid.cloudFrame, "基准平面必须与源云同帧", "plane");
    normal = {plane->normal[0], plane->normal[1], plane->normal[2]};
    const double length = cv::norm(normal);
    need(std::isfinite(length) && length > 1e-12 && std::isfinite(plane->d), "平面系数退化",
         "plane");
    normal /= length;
    offset = plane->d / length;
  }
  double height(const cv::Vec3d& point) const { return normal.dot(point) + offset; }
  cv::Vec3d project(const cv::Vec3d& point) const { return point - normal * height(point); }
  cv::Vec3d onRay(const projection5::Grid& grid, double x, double y,
                  projection5::RayBudget* budget = nullptr) const {
    cv::Vec3d a, b;
    const bool ok = projection5::backProject(grid, x, y, 1, a, budget) &&
                    projection5::backProject(grid, x, y, 2, b, budget);
    if (budget)
      rayResult(ok, *budget, "反投影无法满足真实正投影误差或超出范围");
    else
      need(ok, "反投影超出范围", "mapping");
    const auto direction = b - a;
    const double denominator = normal.dot(direction);
    need(std::abs(denominator) > 1e-12, "基准平面与投影视线平行", "plane");
    const double t = -height(a) / denominator;
    need(grid.projection != "perspective" || 1 + t > 0, "透视基准平面交点位于相机后方", "plane");
    const auto point = a + direction * t;
    need(std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]),
         "基准平面交点非有限", "plane");
    return point;
  }
};
inline cv::Mat selection(const Inputs& in, const Depth& view) {
  cv::Mat mask(view.grid.height, view.grid.width, CV_8U, cv::Scalar(255));
  if (in.has("roi")) {
    const auto* box = in.get("roi").asBox2D();
    need(box && box->unit == Unit2D::Pixel, "ROI需要原图像素Box2D", "roi");
    for (int i = 0; i < 2; ++i)
      need(std::isfinite(box->min[i]) && std::isfinite(box->max[i]) && box->min[i] < box->max[i],
           "ROI范围非法", "roi");
    for (int y = 0; y < mask.rows; ++y)
      for (int x = 0; x < mask.cols; ++x)
        if (x < box->min[0] || x >= box->max[0] || y < box->min[1] || y >= box->max[1])
          mask.at<unsigned char>(y, x) = 0;
    need(cv::countNonZero(mask) > 0, "ROI与图像没有有效交集", "roi");
  }
  if (in.has("mask")) {
    const auto* supplied = in.get("mask").asImage();
    need(supplied && supplied->consistent() && supplied->channels == 1 &&
             supplied->depth == PixelDepth::U8 && supplied->scale == 1 &&
             supplied->width == mask.cols && supplied->height == mask.rows,
         "区域mask需要同尺寸全分辨率u8单通道", "mask");
    cv::bitwise_and(mask, cvx::view(*supplied) > 0, mask);
    need(cv::countNonZero(mask) > 0, "区域mask为空", "mask");
  }
  return mask;
}
}
