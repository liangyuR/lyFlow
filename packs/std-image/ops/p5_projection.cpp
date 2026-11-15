#include <numeric>
#include <set>

#include "lyflow/cloud_groups.h"
#include "lyflow/pose.h"
#include "p1_common.h"
#include "p5_projection_common.h"

namespace lyflow::ops {
namespace {
using Json = nlohmann::json;
using projection5::Grid;
namespace pr = projection5;
struct Failure {
  Status status;
};
void require(bool condition, const std::string& message, const char* port = "cloud") {
  if (!condition)
    throw Failure{pr::failure(message, port)};
}
void budget(bool condition, const char* message) {
  if (!condition)
    throw Failure{pr::failure(message, "", "resource_limit")};
}
void check(ExecContext& ctx) {
  if (ctx.cancelled())
    throw pr::Cancelled{};
}
double number(const ParamView& p, const char* name, double low, double high) {
  const double value = p.number(name);
  if (!std::isfinite(value) || value < low || value > high)
    throw Failure{img::badParam("参数超出有限范围", name)};
  return value;
}
int integer(const ParamView& p, const char* name, int low, int high) {
  const auto value = p.integer(name);
  if (value < low || value > high)
    throw Failure{img::badParam("整数参数超出范围", name)};
  return static_cast<int>(value);
}
std::string frame(const ParamView& p, const char* name) {
  auto value = p.text(name);
  require(!value.empty() && value.size() <= 256, "参考帧须为1..256字节", name);
  return value;
}
const PointCloud& cloudInput(const Inputs& in, ExecContext& ctx) {
  const auto* cloud = in.get("cloud").asCloud();
  require(cloud && cloud->channelsConsistent(), "点云与可选通道长度必须一致");
  budget(cloud->pointCount() <= 2 * 1024 * 1024, "输入点云超过2Mi点预算");
  budget(cloud->xyz.size() * sizeof(float) + cloud->normals.size() * sizeof(float) +
                 cloud->intensity.size() * sizeof(float) + cloud->rgb.size() <=
             128 * 1024 * 1024,
         "输入点云超过128MiB预算");
  for (std::size_t i = 0; i < cloud->pointCount(); ++i) {
    if ((i & 16383) == 0)
      check(ctx);
    bool finite = true;
    for (int axis = 0; axis < 3; ++axis)
      finite &= std::isfinite(cloud->xyz[3 * i + axis]);
    if (!finite)
      continue;
    if (cloud->hasIntensity())
      require(std::isfinite(cloud->intensity[i]), "有效点的强度必须有限");
    if (cloud->hasNormals())
      for (int axis = 0; axis < 3; ++axis)
        require(std::isfinite(cloud->normals[3 * i + axis]), "有效点的法线必须有限");
  }
  return *cloud;
}
cv::Matx44d transform(const Inputs& in, const char* port) {
  cv::Matx44d result = cv::Matx44d::eye();
  if (in.has(port)) {
    const auto* value = in.get(port).asTransform();
    require(value && pr::rigid(Json(value->m), result), "需要有限右手刚体变换", port);
  }
  return result;
}
cv::Vec3d point(const PointCloud& cloud, std::size_t i, const cv::Matx44d& matrix) {
  const auto p =
      matrix * cv::Vec4d(cloud.xyz[3 * i], cloud.xyz[3 * i + 1], cloud.xyz[3 * i + 2], 1);
  return {p[0], p[1], p[2]};
}
bool finite(const cv::Vec3d& p) {
  for (double value : p.val)
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
      return false;
  return true;
}
float* floats(Image& image) { return reinterpret_cast<float*>(image.mutablePixels()); }
const float* floats(const Image& image) {
  return reinterpret_cast<const float*>(image.pixels.get());
}
Image image(int width, int height, PixelDepth depth, int scale = 1, int channels = 1) {
  Image result = Image::allocate(width, height, channels, depth);
  result.scale = scale;
  return result;
}
struct Raster {
  Grid grid;
  Image depth, status;
  Tensor index;
  std::vector<float> pointState;
};
Raster raster(Grid grid, std::size_t sourceCount) {
  const auto size = static_cast<std::size_t>(grid.width) * grid.height;
  budget(grid.width > 0 && grid.height > 0 && grid.width <= 32768 && grid.height <= 32768 &&
             size <= pr::kMaxPixels,
         "栅格超过8Mi像素或32768边长预算");
  Raster result;
  result.grid = std::move(grid);
  result.depth = image(result.grid.width, result.grid.height, PixelDepth::F32, result.grid.scale);
  result.status = image(result.grid.width, result.grid.height, PixelDepth::U8, result.grid.scale);
  result.index.shape = {result.grid.height, result.grid.width};
  result.index.data.assign(size, -1);
  result.pointState.assign(sourceCount, 0);
  return result;
}
void offer(Raster& result, std::size_t pixel, std::size_t source, double z, bool nearest) {
  const float metric = static_cast<float>(z);
  auto* flags = result.status.mutablePixels();
  auto* depth = floats(result.depth);
  if (!flags[pixel]) {
    flags[pixel] = pr::kObserved;
    depth[pixel] = metric;
    result.index.data[pixel] = static_cast<float>(source);
    result.pointState[source] = 1;
    return;
  }
  flags[pixel] |= pr::kCollision;
  const auto previous = static_cast<std::size_t>(result.index.data[pixel]);
  if ((nearest && metric < depth[pixel]) || (!nearest && metric > depth[pixel])) {
    result.pointState[previous] = 2;
    depth[pixel] = metric;
    result.index.data[pixel] = static_cast<float>(source);
    result.pointState[source] = 1;
  } else {
    result.pointState[source] = 2;
  }
}
struct Projected {
  int x = -1, y = -1;
  double z = 0, u = 0, v = 0;
};
Raster cameraRaster(const PointCloud& cloud, const Grid& grid, const cv::Matx44d& cloudToCamera,
                    ExecContext& ctx, std::vector<Projected>* coordinates = nullptr) {
  auto result = raster(grid, cloud.pointCount());
  if (coordinates)
    coordinates->assign(cloud.pointCount(), {});
  const cv::Matx33d k(grid.camera.matrix.data());
  const cv::Mat distortion(grid.camera.distortion);
  for (std::size_t offset = 0; offset < cloud.pointCount(); offset += 4096) {
    check(ctx);
    std::vector<cv::Point3d> points;
    std::vector<std::size_t> ids;
    for (std::size_t i = offset; i < std::min(offset + 4096, cloud.pointCount()); ++i) {
      const auto p = point(cloud, i, cloudToCamera);
      if (!finite(p) || !(p[2] > 0))
        continue;
      points.emplace_back(p[0], p[1], p[2]);
      ids.push_back(i);
      result.pointState[i] = 3;
    }
    if (points.empty())
      continue;
    std::vector<cv::Point2d> pixels;
    cv::projectPoints(points, cv::Vec3d::all(0), cv::Vec3d::all(0), k, distortion, pixels);
    for (std::size_t j = 0; j < ids.size(); ++j) {
      const auto p = pixels[j];
      if (!(p.x > -.5 && p.y > -.5 && p.x < grid.fullWidth - .5 && p.y < grid.fullHeight - .5))
        continue;
      const int x = static_cast<int>(std::floor((p.x + .5) / grid.scale));
      const int y = static_cast<int>(std::floor((p.y + .5) / grid.scale));
      if (coordinates)
        (*coordinates)[ids[j]] = {x, y, points[j].z, p.x, p.y};
      offer(result, static_cast<std::size_t>(y) * grid.width + x, ids[j], points[j].z, true);
    }
  }
  return result;
}
Raster orthographicRaster(const PointCloud& cloud, const Grid& grid, double minHeight,
                          double maxHeight, ExecContext& ctx) {
  auto result = raster(grid, cloud.pointCount());
  const auto inverse = pr::inverse(grid.gridToCloud);
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    if ((i & 16383) == 0)
      check(ctx);
    const auto p = point(cloud, i, inverse);
    if (!finite(p))
      continue;
    result.pointState[i] = 3;
    if (p[2] < minHeight || p[2] > maxHeight)
      continue;
    const double x = (p[0] - grid.origin[0]) / grid.resolution[0];
    const double y = (p[1] - grid.origin[1]) / grid.resolution[1];
    if (!(x > -.5 && y > -.5 && x < grid.fullWidth - .5 && y < grid.fullHeight - .5))
      continue;
    const auto u = static_cast<std::size_t>(std::floor(x + .5));
    const auto v = static_cast<std::size_t>(std::floor(y + .5));
    offer(result, v * grid.width + u, i, p[2], false);
  }
  return result;
}
struct Compacted {
  PointCloud cloud;
  Tensor source;
};
Compacted compact(Raster& result, const PointCloud& cloud, const cv::Matx44d& matrix,
                  ExecContext& ctx) {
  std::vector<std::int32_t> ids;
  Tensor source = result.index;
  for (std::size_t i = 0; i < result.index.data.size(); ++i) {
    if ((i & 16383) == 0)
      check(ctx);
    if (source.data[i] < 0)
      continue;
    ids.push_back(static_cast<std::int32_t>(source.data[i]));
    result.index.data[i] = static_cast<float>(ids.size() - 1);
  }
  PointCloud output = cloud.select(ids);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if ((i & 16383) == 0)
      check(ctx);
    const auto p = point(cloud, static_cast<std::size_t>(ids[i]), matrix);
    require(finite(p), "转换后点超出有限float32范围");
    for (int a = 0; a < 3; ++a)
      output.xyz[3 * i + a] = static_cast<float>(p[a]);
    if (output.hasNormals()) {
      const auto id = static_cast<std::size_t>(ids[i]);
      const auto n = matrix * cv::Vec4d(cloud.normals[3 * id], cloud.normals[3 * id + 1],
                                        cloud.normals[3 * id + 2], 0);
      require(finite({n[0], n[1], n[2]}), "转换后法线超出有限float32范围");
      for (int a = 0; a < 3; ++a)
        output.normals[3 * i + a] = static_cast<float>(n[a]);
    }
  }
  return {std::move(output), std::move(source)};
}
Record lineage(const Grid& grid, const PointCloud& current, const PointCloud& parent,
               const Image& depth, const Image& status, const Tensor& indices, const Tensor& source,
               ExecContext& ctx) {
  auto record = pr::make(grid, current, depth, status, indices, ctx);
  record.data["parentCloudId"] = std::to_string(parent.id);
  record.data["parentPointCount"] = parent.pointCount();
  record.data["parentCloudFrame"] = grid.cloudFrame;
  record.data["sourceIndexHash"] = pr::hash(source, &ctx);
  return record;
}
void publish(Outputs& out, const Inputs& in, Raster&& result, Compacted&& cloud, Record&& mapping,
             Record&& quality) {
  out.set("sourceCloud", in.get("cloud"));
  out.set("cloud", Data::cloud(std::move(cloud.cloud)));
  out.set("depth", Data::image(std::move(result.depth)));
  out.set("status", Data::image(std::move(result.status)));
  out.set("indexMap", Data::tensor(std::move(result.index)).withTensorScale(result.grid.scale));
  out.set("sourceIndexMap",
          Data::tensor(std::move(cloud.source)).withTensorScale(result.grid.scale));
  out.set("mapping", Data::record(std::move(mapping)));
  out.set("quality", Data::record(std::move(quality)));
  Tensor states;
  states.shape = {static_cast<std::int64_t>(result.pointState.size())};
  states.data = std::move(result.pointState);
  out.set("pointState", Data::tensor(std::move(states)));
}
Grid cameraGrid(const Inputs& in, const ParamView& p, int width, int height, int scale) {
  const auto* calib = in.get("calib").asRecord();
  Grid grid;
  std::string why;
  require(calib && calibration::parseCamera(*calib, grid.camera, why), "需要有效相机标定：" + why,
          "calib");
  require(scale >= 1 && (scale & (scale - 1)) == 0, "图像scale须为2的幂", "image");
  budget(grid.camera.width <= 32768 && grid.camera.height <= 32768,
         "标定完整尺寸超过32768边长预算");
  require((grid.camera.width + scale - 1) / scale == width &&
              (grid.camera.height + scale - 1) / scale == height,
          "图像scale与完整标定分辨率不匹配", "calib");
  grid.width = width;
  grid.height = height;
  grid.scale = scale;
  grid.fullWidth = grid.camera.width;
  grid.fullHeight = grid.camera.height;
  grid.projection = "perspective";
  grid.gridFrame = grid.camera.frame;
  grid.cloudFrame = frame(p, "cloudFrame");
  grid.imageFrame = frame(p, "imageFrame");
  require(grid.cloudFrame == grid.camera.frame || in.has("cloudToCamera"),
          "跨三维帧必须提供cloudToCamera", "cloudToCamera");
  grid.gridToCloud = pr::inverse(transform(in, "cloudToCamera"));
  return grid;
}
Status projectCamera(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& cloud = cloudInput(in, ctx);
  const auto* rgb = in.get("image").asImage();
  require(rgb && rgb->consistent() && rgb->scale >= 1 && (rgb->channels == 3 || rgb->channels == 4),
          "需要RGB/RGBA图像", "image");
  budget(rgb->pixelCount() <= pr::kMaxPixels && rgb->byteSize() <= 128 * 1024 * 1024,
         "相机图像超过预算");
  const auto cloudBytes =
      (cloud.xyz.size() + cloud.normals.size() + cloud.intensity.size()) * sizeof(float) +
      cloud.rgb.size();
  budget(cloudBytes * 2 + rgb->byteSize() + rgb->pixelCount() * 13 + cloud.pointCount() * 11 +
                 16 * 1024 * 1024 <=
             512 * 1024 * 1024,
         "相机投影预计峰值超过512MiB");
  if (rgb->depth == PixelDepth::F32)
    for (int y = 0; y < rgb->height; ++y) {
      check(ctx);
      for (int x = 0; x < rgb->width; ++x)
        for (int c = 0; c < rgb->channels; ++c)
          require(std::isfinite(rgb->at(x, y, c)) && rgb->at(x, y, c) >= 0 && rgb->at(x, y, c) <= 1,
                  "浮点RGB各通道须有限0..1", "image");
    }
  auto grid = cameraGrid(in, p, rgb->width, rgb->height, rgb->scale);
  const auto matrix = transform(in, "cloudToCamera");
  auto result = cameraRaster(cloud, grid, matrix, ctx);
  auto compacted = compact(result, cloud, matrix, ctx);
  result.grid.gridToCloud = cv::Matx44d::eye();
  result.grid.cloudFrame = grid.camera.frame;
  if (p.choice("colorSource") == "image") {
    compacted.cloud.rgb.resize(compacted.cloud.pointCount() * 3);
    for (std::size_t pixel = 0; pixel < result.index.data.size(); ++pixel) {
      if ((pixel & 16383) == 0)
        check(ctx);
      if (result.index.data[pixel] < 0)
        continue;
      const auto id = static_cast<std::size_t>(result.index.data[pixel]);
      const int x = static_cast<int>(pixel % grid.width), y = static_cast<int>(pixel / grid.width);
      for (int c = 0; c < 3; ++c) {
        double value = rgb->at(x, y, c);
        if (rgb->depth == PixelDepth::F32)
          value *= 255;
        if (rgb->depth == PixelDepth::U16)
          value *= 255.0 / 65535;
        compacted.cloud.rgb[3 * id + c] =
            static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
      }
    }
  }
  auto mapping = lineage(result.grid, compacted.cloud, cloud, result.depth, result.status,
                         result.index, compacted.source, ctx);
  mapping.data["parentCloudFrame"] = grid.cloudFrame;
  Record quality{
      "vision.ProjectionQuality.v1",
      {{"sourceCloudId", std::to_string(cloud.id)},
       {"sourceCount", cloud.pointCount()},
       {"visibleCount", compacted.cloud.pointCount()},
       {"pointStateScope", "sourceCloud-parent-points"},
       {"pointStateCodes", {{"invalid", 0}, {"visible", 1}, {"occluded", 2}, {"outside", 3}}},
       {"method", "distorted-camera-nearest-zbuffer"},
       {"colorSource", p.choice("colorSource")}}};
  check(ctx);
  out.set("image", in.get("image"));
  publish(out, in, std::move(result), std::move(compacted), std::move(mapping), std::move(quality));
  return Status::Ok();
}

std::pair<Image, Image> angles(const PointCloud& cloud, const Raster& raster, ExecContext& ctx) {
  Image result = image(raster.grid.width, raster.grid.height, PixelDepth::F32, raster.grid.scale);
  Image valid = image(raster.grid.width, raster.grid.height, PixelDepth::U8, raster.grid.scale);
  const auto inverse = pr::inverse(raster.grid.gridToCloud);
  const int w = raster.grid.width, h = raster.grid.height;
  for (int y = 1; y < h - 1; ++y) {
    check(ctx);
    for (int x = 1; x < w - 1; ++x) {
      const auto at = [&](int u, int v) {
        return raster.index.data[static_cast<std::size_t>(v) * w + u];
      };
      if (at(x, y) < 0 || at(x - 1, y) < 0 || at(x + 1, y) < 0 || at(x, y - 1) < 0 ||
          at(x, y + 1) < 0)
        continue;
      const auto left = point(cloud, static_cast<std::size_t>(at(x - 1, y)), inverse);
      const auto right = point(cloud, static_cast<std::size_t>(at(x + 1, y)), inverse);
      const auto top = point(cloud, static_cast<std::size_t>(at(x, y - 1)), inverse);
      const auto bottom = point(cloud, static_cast<std::size_t>(at(x, y + 1)), inverse);
      auto normal = (right - left).cross(bottom - top);
      const double norm = cv::norm(normal);
      if (!(norm > 1e-15) || !std::isfinite(norm))
        continue;
      normal *= 1 / norm;
      if (normal[2] < 0)
        normal *= -1;
      const auto pixel = static_cast<std::size_t>(y) * w + x;
      floats(result)[pixel] = static_cast<float>(std::acos(std::clamp(normal[2], -1.0, 1.0)));
      valid.mutablePixels()[pixel] = 255;
    }
  }
  return {std::move(result), std::move(valid)};
}
Image colored(const cv::Mat& mono, const Image& valid, int scale) {
  cv::Mat color;
  cv::applyColorMap(mono, color, cv::COLORMAP_TURBO);
  cv::cvtColor(color, color, cv::COLOR_BGR2RGB);
  color.setTo(cv::Scalar::all(0), cvx::view(valid) == 0);
  Image result;
  require(cvx::fromMat(std::move(color), result), "无法创建可视化图像");
  result.scale = scale;
  return result;
}
Status orthographic(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx,
                    bool advanced) {
  const auto& cloud = cloudInput(in, ctx);
  Grid grid;
  grid.width = grid.fullWidth = integer(p, "width", 1, 32768);
  grid.height = grid.fullHeight = integer(p, "height", 1, 32768);
  grid.resolution = cv::Vec2d::all(number(p, "resolution", 1e-9, 1e6));
  grid.origin = {number(p, "originX", -1e6, 1e6), number(p, "originY", -1e6, 1e6)};
  grid.cloudFrame = frame(p, "cloudFrame");
  grid.gridFrame = frame(p, "gridFrame");
  grid.imageFrame = frame(p, "imageFrame");
  require(grid.cloudFrame == grid.gridFrame || in.has("gridToCloud"),
          "跨三维帧必须显式提供gridToCloud", "gridToCloud");
  grid.gridToCloud = transform(in, "gridToCloud");
  grid.projection = "orthographic";
  const double low = number(p, "minHeight", -1e6, 1e6), high = number(p, "maxHeight", -1e6, 1e6);
  require(low <= high, "minHeight不能大于maxHeight", "minHeight");
  const auto pixels = static_cast<std::size_t>(grid.width) * grid.height;
  Grid normalGrid = grid;
  const double step = advanced ? number(p, "normalResolution", 1e-9, 1e6) : grid.resolution[0];
  const double fineWidth = std::ceil(grid.width * grid.resolution[0] / step);
  const double fineHeight = std::ceil(grid.height * grid.resolution[1] / step);
  budget(fineWidth >= 1 && fineHeight >= 1 && fineWidth <= 32768 && fineHeight <= 32768 &&
             fineWidth * fineHeight <= pr::kMaxPixels,
         "法线采样栅格超过8Mi像素预算");
  normalGrid.width = normalGrid.fullWidth = static_cast<int>(fineWidth);
  normalGrid.height = normalGrid.fullHeight = static_cast<int>(fineHeight);
  normalGrid.origin = grid.origin + cv::Vec2d::all((step - grid.resolution[0]) * .5);
  normalGrid.resolution = cv::Vec2d::all(step);
  const auto finePixels = static_cast<std::size_t>(normalGrid.width) * normalGrid.height;
  const auto cloudBytes =
      (cloud.xyz.size() + cloud.normals.size() + cloud.intensity.size()) * sizeof(float) +
      cloud.rgb.size();
  budget(cloudBytes * 2 + pixels * 29 + finePixels * 14 + cloud.pointCount() * 12 +
                 16 * 1024 * 1024 <=
             512 * 1024 * 1024,
         "正交投影预计峰值超过512MiB");
  auto result = orthographicRaster(cloud, grid, low, high, ctx);
  auto fine = orthographicRaster(cloud, normalGrid, low, high, ctx);
  auto normals = angles(cloud, fine, ctx);
  Image angle = image(grid.width, grid.height, PixelDepth::F32);
  Image normalValid = image(grid.width, grid.height, PixelDepth::U8);
  cv::Mat gray(grid.height, grid.width, CV_8U, cv::Scalar(0));
  cv::Mat normalGray = gray.clone();
  double min = std::numeric_limits<double>::infinity(), max = -min;
  for (std::size_t i = 0; i < pixels; ++i)
    if (pr::valid(result.status.pixels.get()[i])) {
      min = std::min(min, static_cast<double>(floats(result.depth)[i]));
      max = std::max(max, static_cast<double>(floats(result.depth)[i]));
    }
  for (int y = 0; y < grid.height; ++y) {
    check(ctx);
    for (int x = 0; x < grid.width; ++x) {
      const auto pixel = static_cast<std::size_t>(y) * grid.width + x;
      if (!pr::valid(result.status.pixels.get()[pixel]))
        continue;
      const double z = floats(result.depth)[pixel];
      gray.at<std::uint8_t>(y, x) = static_cast<std::uint8_t>(
          min < max ? std::lround(30 + 225 * (z - min) / (max - min)) : 255);
      const double nx = (grid.origin[0] + x * grid.resolution[0] - normalGrid.origin[0]) / step;
      const double ny = (grid.origin[1] + y * grid.resolution[1] - normalGrid.origin[1]) / step;
      if (!(nx > -.5 && ny > -.5 && nx < normalGrid.width - .5 && ny < normalGrid.height - .5))
        continue;
      const auto fx = static_cast<std::size_t>(std::floor(nx + .5)),
                 fy = static_cast<std::size_t>(std::floor(ny + .5));
      const auto from = fy * normalGrid.width + fx;
      if (!normals.second.pixels.get()[from])
        continue;
      floats(angle)[pixel] = floats(normals.first)[from];
      normalValid.mutablePixels()[pixel] = 255;
      normalGray.at<std::uint8_t>(y, x) =
          static_cast<std::uint8_t>(std::lround(255 * floats(angle)[pixel] / poses::kPi));
    }
  }
  auto pseudo = colored(gray, result.status, 1);
  Image display;
  const auto mode = advanced ? p.choice("outputMode") : std::string("height");
  if (advanced && mode == "height")
    display = result.depth;
  else if (mode == "normal_angle")
    display = colored(normalGray, normalValid, 1);
  else if (mode == "pseudo_height")
    display = pseudo;
  else {
    cv::Mat rgb;
    cv::cvtColor(gray, rgb, cv::COLOR_GRAY2RGB);
    require(cvx::fromMat(std::move(rgb), display), "灰度可视化输出失败");
  }
  auto compacted = compact(result, cloud, cv::Matx44d::eye(), ctx);
  auto mapping = lineage(grid, compacted.cloud, cloud, result.depth, result.status, result.index,
                         compacted.source, ctx);
  Record quality{
      "vision.ProjectionQuality.v1",
      {{"sourceCloudId", std::to_string(cloud.id)},
       {"sourceCount", cloud.pointCount()},
       {"visibleCount", compacted.cloud.pointCount()},
       {"pointStateScope", "sourceCloud-parent-points"},
       {"pointStateCodes", {{"invalid", 0}, {"visible", 1}, {"occluded", 2}, {"outside", 3}}},
       {"resolution", grid.resolution[0]},
       {"normalResolution", step},
       {"outputMode", mode},
       {"method", "orthographic-observed-max-height;finite-difference-normal-angle"}}};
  check(ctx);
  out.set("image", Data::image(std::move(display)));
  out.set("pseudo", Data::image(std::move(pseudo)));
  out.set("normalAngle", Data::image(std::move(angle)));
  out.set("normalValid", Data::image(std::move(normalValid)));
  publish(out, in, std::move(result), std::move(compacted), std::move(mapping), std::move(quality));
  return Status::Ok();
}

void append(PointCloud& output, const PointCloud& source, std::int32_t reference,
            const cv::Vec3d& p) {
  require(finite(p), "重建点超出有限float32范围");
  output.push(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
  if (source.hasIntensity())
    output.intensity.push_back(
        reference >= 0 ? source.intensity[static_cast<std::size_t>(reference)] : 0);
  if (source.hasNormals())
    for (int c = 0; c < 3; ++c)
      output.normals.push_back(
          reference >= 0 ? source.normals[3 * static_cast<std::size_t>(reference) + c] : 0);
  if (source.hasRgb())
    for (int c = 0; c < 3; ++c)
      output.rgb.push_back(reference >= 0 ? source.rgb[3 * static_cast<std::size_t>(reference) + c]
                                          : 0);
}
Status depthFilter(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto* inputDepth = in.get("depth").asImage();
  require(inputDepth && inputDepth->consistent() && inputDepth->channels == 1 &&
              inputDepth->depth == PixelDepth::F32,
          "深度必须为F32单通道米制图", "depth");
  budget(inputDepth->pixelCount() <= pr::kMaxPixels, "深度超过8Mi像素预算");
  const auto count = inputDepth->pixelCount();
  PointCloud generated;
  const PointCloud* source = nullptr;
  Raster initial;
  Record metadata;
  pr::RayBudget rays;
  rays.context = &ctx;
  const bool mapped =
      in.has("cloud") || in.has("mapping") || in.has("status") || in.has("indexMap");
  if (mapped) {
    require(in.has("cloud") && in.has("mapping") && in.has("status") && in.has("indexMap"),
            "cloud/mapping/status/indexMap须成组输入", "mapping");
    source = &cloudInput(in, ctx);
    const auto* state = in.get("status").asImage();
    const auto* index = in.get("indexMap").asTensor();
    const auto* mapping = in.get("mapping").asRecord();
    require(state && index && mapping, "映射输入类型错误", "mapping");
    const auto checked =
        pr::parse(*source, *inputDepth, *state, *index, *mapping, initial.grid, ctx);
    if (!checked.ok)
      return checked;
    initial.depth = *inputDepth;
    initial.status = *state;
    initial.index = *index;
    metadata = *mapping;
  } else {
    require(in.has("calib"), "直接深度滤波必须提供相机标定，或完整映射组", "calib");
    const auto* calibration = in.get("calib").asRecord();
    std::string why;
    Grid grid;
    require(calibration && calibration::parseCamera(*calibration, grid.camera, why),
            "相机标定无效：" + why, "calib");
    grid.width = inputDepth->width;
    grid.height = inputDepth->height;
    grid.scale = inputDepth->scale;
    grid.fullWidth = grid.camera.width;
    grid.fullHeight = grid.camera.height;
    budget(grid.fullWidth <= 32768 && grid.fullHeight <= 32768, "相机完整尺寸超过32768预算");
    require(grid.scale >= 1 && (grid.scale & (grid.scale - 1)) == 0 &&
                (grid.fullWidth + grid.scale - 1) / grid.scale == grid.width &&
                (grid.fullHeight + grid.scale - 1) / grid.scale == grid.height,
            "深度scale与标定分辨率不匹配", "calib");
    grid.projection = "perspective";
    grid.imageFrame = frame(p, "imageFrame");
    grid.gridFrame = grid.cloudFrame = grid.camera.frame;
    std::size_t positiveDepth = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if ((i & 16383) == 0)
        check(ctx);
      const auto z = floats(*inputDepth)[i];
      positiveDepth += std::isfinite(z) && z > 0;
    }
    const std::size_t workingBytes = count * (p.choice("mode") == "eliminate" ? 49 : 37);
    budget(positiveDepth * 3 * sizeof(float) + workingBytes + 16 * 1024 * 1024 <= 512 * 1024 * 1024,
           "直接深度建云及滤波工作峰值超过512MiB");
    check(ctx);
    generated.reserve(positiveDepth);
    initial = raster(grid, 0);
    for (int y = 0; y < grid.height; ++y) {
      check(ctx);
      for (int x = 0; x < grid.width; ++x) {
        const auto i = static_cast<std::size_t>(y) * grid.width + x;
        const double z = floats(*inputDepth)[i];
        cv::Vec3d xyz;
        if (!(std::isfinite(z) && z > 0))
          continue;
        const bool recovered = pr::backProject(grid, x, y, z, xyz, &rays);
        budget(!rays.exhausted, "深度逆投影超过64Mi评估预算");
        require(recovered, "深度逆投影未收敛到标定像素或无有限解", "calib");
        initial.index.data[i] = static_cast<float>(generated.pointCount());
        generated.push(static_cast<float>(xyz[0]), static_cast<float>(xyz[1]),
                       static_cast<float>(xyz[2]));
        floats(initial.depth)[i] = static_cast<float>(z);
        initial.status.mutablePixels()[i] = pr::kObserved;
      }
    }
    source = &generated;
    metadata = pr::make(grid, *source, initial.depth, initial.status, initial.index, ctx);
  }
  const auto mode = p.choice("mode");
  const auto method = p.choice("fillMethod");
  const auto type = p.choice("eliminateType"), direction = p.choice("direction");
  const int kernel = integer(p, "kernelSize", 1, 31), iterations = integer(p, "iterations", 1, 10);
  require(kernel % 2 == 1, "核尺寸必须为奇数", "kernelSize");
  const double sigma = number(p, "sigma", 1e-9, 100), power = number(p, "idwPower", .01, 16);
  const double threshold = number(p, "threshold", 0, 1e6), jump = number(p, "maxDepthJump", 0, 1e6);
  const bool fill = p.flag("fillInvalid"), include = p.flag("includeInterpolated");
  std::size_t present = 0, support = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if ((i & 16383) == 0)
      check(ctx);
    const auto value = initial.status.pixels.get()[i];
    present += pr::valid(value);
    support += pr::valid(value) && (include || (value & pr::kObserved));
  }
  const std::size_t centers = !support                   ? 0
                              : mode == "fill"           ? count - present
                              : (mode == "mean" && fill) ? count
                                                         : present;
  const std::size_t work = static_cast<std::size_t>(kernel) * kernel * centers * iterations *
                           (mode == "eliminate" ? 4 : 1);
  budget(work <= 256 * 1024 * 1024, "深度邻域运算超过256Mi预算");
  const auto inputBytes =
      (source->xyz.size() + source->normals.size() + source->intensity.size()) * sizeof(float) +
      source->rgb.size();
  const std::size_t workingBytes = count * (mode == "eliminate" ? 49 : 37);
  budget(inputBytes + workingBytes + 16 * 1024 * 1024 <= 512 * 1024 * 1024,
         "深度滤波工作峰值超过512MiB");
  std::vector<float> current(floats(initial.depth), floats(initial.depth) + count), next = current;
  std::vector<std::uint8_t> state(initial.status.pixels.get(), initial.status.pixels.get() + count),
      nextState = state;
  const int w = initial.grid.width, h = initial.grid.height, radius = kernel / 2;
  const auto usable = [&](std::uint8_t value) {
    return pr::valid(value) && (include || (value & pr::kObserved));
  };
  auto extremum = [&](const std::vector<float>& input, bool maximum) {
    std::vector<float> output = input;
    for (int y = 0; y < h; ++y) {
      check(ctx);
      for (int x = 0; x < w; ++x) {
        const auto i = static_cast<std::size_t>(y) * w + x;
        if (!usable(state[i]))
          continue;
        float value = maximum ? -std::numeric_limits<float>::infinity()
                              : std::numeric_limits<float>::infinity();
        for (int dy = -radius; dy <= radius; ++dy)
          for (int dx = -radius; dx <= radius; ++dx) {
            if ((direction == "x" && dy) || (direction == "y" && dx))
              continue;
            const auto n = static_cast<std::size_t>(std::clamp(y + dy, 0, h - 1)) * w +
                           std::clamp(x + dx, 0, w - 1);
            if (!usable(state[n]))
              continue;
            value = maximum ? std::max(value, input[n]) : std::min(value, input[n]);
          }
        output[i] = value;
      }
    }
    return output;
  };
  std::vector<float> neighbors;
  neighbors.reserve(static_cast<std::size_t>(kernel) * kernel);
  for (int iteration = 0; iteration < iterations && support > 0; ++iteration) {
    next = current;
    nextState = state;
    if (mode == "eliminate") {
      std::vector<float> opened, closed;
      if (type != "depression")
        opened = extremum(extremum(current, false), true);
      if (type != "protrusion")
        closed = extremum(extremum(current, true), false);
      for (std::size_t i = 0; i < count; ++i) {
        if ((i & 16383) == 0)
          check(ctx);
        if (!usable(state[i]))
          continue;
        const double up = opened.empty() ? 0 : static_cast<double>(current[i]) - opened[i];
        const double down = closed.empty() ? 0 : static_cast<double>(closed[i]) - current[i];
        if ((up > 0 && up >= threshold) || (down > 0 && down >= threshold)) {
          next[i] = 0;
          nextState[i] = 0;
        }
      }
    } else {
      for (int y = 0; y < h; ++y) {
        check(ctx);
        for (int x = 0; x < w; ++x) {
          const auto i = static_cast<std::size_t>(y) * w + x;
          const bool has = pr::valid(state[i]);
          if ((mode == "fill" && has) || (!has && !(mode == "fill" || (mode == "mean" && fill))))
            continue;
          neighbors.clear();
          double weighted = 0, weights = 0, min = std::numeric_limits<double>::infinity(),
                 max = -min;
          for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx) {
              const auto n = static_cast<std::size_t>(std::clamp(y + dy, 0, h - 1)) * w +
                             std::clamp(x + dx, 0, w - 1);
              if (!usable(state[n]))
                continue;
              const double value = current[n];
              if (has && jump > 0 && std::abs(value - current[i]) > jump)
                continue;
              const double distance = static_cast<double>(dx * dx + dy * dy);
              if (!has && distance == 0)
                continue;
              double weight = 1;
              if (mode == "gaussian")
                weight = std::exp(-distance / (2 * sigma * sigma));
              else if (mode == "fill" && method == "idw")
                weight = std::pow(distance, -power / 2);
              if (!std::isfinite(weight) || weight <= 0)
                continue;
              neighbors.push_back(static_cast<float>(value));
              weighted += value * weight;
              weights += weight;
              min = std::min(min, value);
              max = std::max(max, value);
            }
          if (neighbors.empty() || (!has && jump > 0 && max - min > jump))
            continue;
          double value = weighted / weights;
          if (mode == "median") {
            std::sort(neighbors.begin(), neighbors.end());
            const auto n = neighbors.size();
            value = (static_cast<double>(neighbors[(n - 1) / 2]) + neighbors[n / 2]) / 2;
          } else if (mode == "fill" && method == "min")
            value = min;
          else if (mode == "fill" && method == "max")
            value = max;
          require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
                  "滤波结果超出有限float32范围", "depth");
          next[i] = static_cast<float>(value);
          if (!has)
            nextState[i] = pr::kInterpolated;
          else if (next[i] != current[i])
            nextState[i] |= pr::kFiltered;
        }
      }
    }
    current.swap(next);
    state.swap(nextState);
  }
  std::size_t validCount = 0, changed = 0, filled = 0, removed = 0;
  for (std::size_t i = 0; i < count; ++i)
    validCount += pr::valid(state[i]);
  const auto perPoint = 12 + (source->hasNormals() ? 12 : 0) + (source->hasIntensity() ? 4 : 0) +
                        (source->hasRgb() ? 3 : 0);
  budget(validCount <= pr::kMaxCloudPoints &&
             inputBytes + workingBytes + validCount * perPoint + 16 * 1024 * 1024 <=
                 512 * 1024 * 1024,
         "过滤点云重建预计峰值超过512MiB");
  PointCloud output;
  output.reserve(validCount);
  Raster result = raster(initial.grid, 0);
  Tensor sourceMap = initial.index;
  Image changedImage = image(w, h, PixelDepth::U8, initial.grid.scale);
  const auto inverse = pr::inverse(initial.grid.gridToCloud);
  for (int y = 0; y < h; ++y) {
    check(ctx);
    for (int x = 0; x < w; ++x) {
      const auto i = static_cast<std::size_t>(y) * w + x;
      if (current[i] != floats(initial.depth)[i] || state[i] != initial.status.pixels.get()[i]) {
        changedImage.mutablePixels()[i] = 255;
        ++changed;
      }
      if (!pr::valid(state[i])) {
        sourceMap.data[i] = -1;
        if (pr::valid(initial.status.pixels.get()[i]))
          ++removed;
        continue;
      }
      auto reference = static_cast<std::int32_t>(initial.index.data[i]);
      cv::Vec3d xyz;
      if (reference >= 0) {
        if (current[i] == floats(initial.depth)[i])
          xyz = point(*source, static_cast<std::size_t>(reference), cv::Matx44d::eye());
        else {
          auto local = point(*source, static_cast<std::size_t>(reference), inverse);
          if (initial.grid.projection == "perspective") {
            require(local[2] > 0, "原观测点不在相机前方", "cloud");
            local *= current[i] / local[2];
          } else
            local[2] = current[i];
          const auto p = initial.grid.gridToCloud * cv::Vec4d(local[0], local[1], local[2], 1);
          xyz = {p[0], p[1], p[2]};
        }
      } else {
        ++filled;
        const bool recovered = pr::backProject(initial.grid, x, y, current[i], xyz, &rays);
        budget(!rays.exhausted, "深度逆投影超过64Mi评估预算");
        require(recovered, "补值像素逆投影未收敛或无有限三维解", "depth");
        for (int dy = -radius; dy <= radius && reference < 0; ++dy)
          for (int dx = -radius; dx <= radius; ++dx) {
            const auto n = static_cast<std::size_t>(std::clamp(y + dy, 0, h - 1)) * w +
                           std::clamp(x + dx, 0, w - 1);
            if (initial.index.data[n] >= 0) {
              reference = static_cast<std::int32_t>(initial.index.data[n]);
              break;
            }
          }
      }
      result.index.data[i] = static_cast<float>(output.pointCount());
      floats(result.depth)[i] = current[i];
      result.status.mutablePixels()[i] = state[i];
      append(output, *source, reference, xyz);
    }
  }
  auto mapping =
      pr::rebind(metadata, output, result.depth, result.status, result.index, ctx, &sourceMap);
  Record quality{
      "vision.DepthFilterQuality.v1",
      {{"mode", mode},
       {"kernelSize", kernel},
       {"iterations", iterations},
       {"changedPixels", changed},
       {"filledPixels", filled},
       {"removedPixels", removed},
       {"pointCount", output.pointCount()},
       {"geometryPolicy", "unchangedXYZ-preserved;changed-depth-original-ray;filled-pixel-center"},
       {"syntheticChannels", "associated-neighborhood-values-not-new-observations"}}};
  check(ctx);
  out.set("sourceCloud", mapped ? in.get("cloud") : Data::cloud(std::move(generated)));
  out.set("cloud", Data::cloud(std::move(output)));
  out.set("depth", Data::image(std::move(result.depth)));
  out.set("status", Data::image(std::move(result.status)));
  out.set("indexMap", Data::tensor(std::move(result.index)).withTensorScale(initial.grid.scale));
  out.set("sourceIndexMap", Data::tensor(std::move(sourceMap)).withTensorScale(initial.grid.scale));
  out.set("mapping", Data::record(std::move(mapping)));
  out.set("changed", Data::image(std::move(changedImage)));
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}

struct Region {
  std::string id;
  double cx = 0, cy = 0, width = 0, height = 0, angle = 0;
  bool useMask = false;
  bool unavailable = false;
};
Status roiExtract(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto& cloud = cloudInput(in, ctx);
  const bool mapped =
      in.has("mapping") || in.has("depth") || in.has("status") || in.has("indexMap");
  Raster base;
  Record oldMapping;
  std::vector<Projected> projected;
  if (mapped) {
    require(in.has("mapping") && in.has("depth") && in.has("status") && in.has("indexMap"),
            "深度关联输入必须成组", "mapping");
    const auto* depth = in.get("depth").asImage();
    const auto* status = in.get("status").asImage();
    const auto* indices = in.get("indexMap").asTensor();
    const auto* mapping = in.get("mapping").asRecord();
    require(depth && status && indices && mapping, "深度关联输入类型错误", "mapping");
    const auto result = pr::parse(cloud, *depth, *status, *indices, *mapping, base.grid, ctx);
    if (!result.ok)
      return result;
    base.depth = *depth;
    base.status = *status;
    base.index = *indices;
    oldMapping = *mapping;
    require(p.choice("visibility") == "visible",
            "组织视图只能消费显式可见索引；全部投影点请使用calib路径", "visibility");
  } else {
    const auto* record = in.get("calib").asRecord();
    calibration::Camera camera;
    std::string why;
    require(record && calibration::parseCamera(*record, camera, why),
            "未组织云需要相机标定：" + why, "calib");
    auto grid = cameraGrid(in, p, camera.width, camera.height, 1);
    base = cameraRaster(cloud, grid, transform(in, "cloudToCamera"), ctx, &projected);
    oldMapping = pr::make(grid, cloud, base.depth, base.status, base.index, ctx);
  }
  const auto& grid = base.grid;
  require(frame(p, "imageFrame") == grid.imageFrame, "ROI像素参考帧必须与mapping一致",
          "imageFrame");
  require(!(in.has("instances") && in.has("mask")), "instances与单张mask互斥", "mask");
  require(in.has("instances") || in.has("mask"), "必须提供二维实例或mask，不默认提取全云",
          "instances");
  const double shrink = number(p, "shrink", 1e-6, 1), offsetX = number(p, "offsetX", -1e9, 1e9),
               offsetY = number(p, "offsetY", -1e9, 1e9);
  const bool usingMask = p.flag("usingMask");
  const bool usingOffset = p.flag("usingOffset");
  require(usingOffset || (offsetX == 0 && offsetY == 0), "非零ROI偏移必须显式开启usingOffset",
          "usingOffset");
  const bool translated = usingOffset && (offsetX != 0 || offsetY != 0);
  const Tensor* masks = nullptr;
  int maskWidth = 0, maskHeight = 0;
  const Image* single = nullptr;
  std::vector<Region> regions;
  if (in.has("instances")) {
    const auto* record = in.get("instances").asRecord();
    std::string why;
    require(record && record->type == poses::kPoses2DType && poses::validate(*record, why),
            "需要合法Poses2D：" + why, "instances");
    const auto& data = record->data;
    require(data["unit"] == "px" && data["frame"] == grid.imageFrame,
            "实例必须使用映射像素帧和px单位", "instances");
    budget(data["items"].size() <= 64, "ROI实例数超过64预算");
    require(in.has("masks") == in.has("maskInfo"), "masks与maskInfo必须成组", "maskInfo");
    const Record* maskInfo = nullptr;
    if (in.has("masks")) {
      masks = in.get("masks").asTensor();
      maskInfo = in.get("maskInfo").asRecord();
      require(masks && masks->consistent() && masks->shape.size() == 3 &&
                  in.get("masks").tensorScale() == 1 && maskInfo &&
                  maskInfo->type == "vision.Masks2D.v1" && values::boundedJson(maskInfo->data, why),
              "需要原生NHW掩膜及Masks2D绑定", "maskInfo");
      const auto& info = maskInfo->data;
      require(info.is_object() && info.size() == 6 && info.value("frame", "") == grid.imageFrame &&
                  info.value("unit", "") == "px" && info.value("layout", "") == "NHW" &&
                  info.contains("width") && info["width"].is_number_integer() &&
                  info.contains("height") && info["height"].is_number_integer(),
              "掩膜帧/布局/尺寸字段错误", "maskInfo");
      const auto mw = info["width"].get<std::int64_t>(), mh = info["height"].get<std::int64_t>();
      require(mw > 0 && mh > 0 && mw <= 32768 && mh <= 32768, "掩膜尺寸超出范围", "maskInfo");
      maskWidth = static_cast<int>(mw);
      maskHeight = static_cast<int>(mh);
      require(translated ? (offsetX >= 0 && offsetY >= 0 && offsetX + grid.fullWidth <= mw &&
                            offsetY + grid.fullHeight <= mh)
                         : (mw == grid.fullWidth && mh == grid.fullHeight),
              "掩膜尺寸须匹配完整栅格，或显式覆盖usingOffset裁剪窗口", "maskInfo");
      require(masks->shape ==
                      std::vector<std::int64_t>{static_cast<std::int64_t>(data["items"].size()), mh,
                                                mw} &&
                  info.contains("items") && info["items"].is_array() &&
                  info["items"].size() == data["items"].size(),
              "NHW层数/绑定项必须与实例一致", "masks");
      budget(masks->data.size() <= 32 * 1024 * 1024, "ROI掩膜超过32Mi float元素预算");
      for (std::size_t n = 0; n < info["items"].size(); ++n) {
        const auto& item = info["items"][n];
        require(item.is_object() && item.size() == 4 &&
                    item.value("id", "") == data["items"][n]["id"] && item.contains("index") &&
                    item["index"].is_number_integer() && item["index"] == n &&
                    item.contains("hasMask") && item["hasMask"].is_boolean() &&
                    item.contains("source") && item["source"].is_string() &&
                    !item["source"].get_ref<const std::string&>().empty() &&
                    item["source"].get_ref<const std::string&>().size() <= 256,
                "每个掩膜层必须与原实例ID/顺序绑定", "maskInfo");
        const bool present = item["hasMask"].get<bool>();
        require(present ? (item["source"] != "none") : (item["source"] == "none"),
                "hasMask与source矛盾", "maskInfo");
        const auto& pose = data["items"][n];
        if (pose.contains("attributes")) {
          const auto& attributes = pose["attributes"];
          if (attributes.contains("hasMask"))
            require(attributes["hasMask"] == present, "Pose与MaskInfo hasMask矛盾", "maskInfo");
          if (attributes.contains("maskIndex"))
            require(attributes["maskIndex"].is_number_integer() && attributes["maskIndex"] == n,
                    "Pose maskIndex与原生层顺序矛盾", "maskInfo");
        }
        const double width = pose["size"][0].get<double>(), height = pose["size"][1].get<double>();
        const double norm = std::max(width, height), cx = pose["position"][0].get<double>() / norm,
                     cy = pose["position"][1].get<double>() / norm;
        const double c = std::cos(pose["angle"].get<double>()),
                     s = std::sin(pose["angle"].get<double>());
        bool nonzero = false;
        const auto pixels = static_cast<std::size_t>(maskWidth) * maskHeight;
        for (std::size_t i = 0; i < pixels; ++i) {
          if ((i & 16383) == 0)
            check(ctx);
          const auto value = masks->data[n * pixels + i];
          require(value == 0 || value == 1, "掩膜必须是有限二值0/1", "masks");
          if (value == 0)
            continue;
          nonzero = true;
          const double dx = (i % maskWidth) / norm - cx, dy = (i / maskWidth) / norm - cy;
          require(present && std::abs(c * dx + s * dy) <= width / norm / 2 + 1e-12 &&
                      std::abs(-s * dx + c * dy) <= height / norm / 2 + 1e-12,
                  "掩膜支持越出Pose或hasMask=false", "masks");
        }
        require(nonzero == present, "hasMask与非零原生支持不一致", "masks");
      }
    }
    for (std::size_t n = 0; n < data["items"].size(); ++n) {
      const auto& item = data["items"][n];
      bool hasMask = masks && maskInfo->data["items"][n]["hasMask"].get<bool>();
      if (item.contains("attributes") && item["attributes"].contains("hasMask")) {
        require(item["attributes"]["hasMask"].is_boolean(), "hasMask必须为布尔值", "instances");
        hasMask = item["attributes"]["hasMask"].get<bool>();
        require(!usingMask || !hasMask || masks, "实例声明掩膜但缺失原生masks", "masks");
      }
      regions.push_back({item["id"].get<std::string>(), item["position"][0].get<double>(),
                         item["position"][1].get<double>(), item["size"][0].get<double>() * shrink,
                         item["size"][1].get<double>() * shrink, item["angle"].get<double>(),
                         usingMask && hasMask, usingMask && !hasMask});
    }
  } else {
    require(!in.has("masks") && !in.has("maskInfo"), "单mask路径不接受实例掩膜层", "masks");
    single = in.get("mask").asImage();
    require(single && single->consistent() && single->depth == PixelDepth::U8 &&
                single->channels == 1 && single->scale >= 1,
            "mask须为U8单通道", "mask");
    budget(single->pixelCount() <= pr::kMaxPixels, "mask超过8Mi像素预算");
    require(translated
                ? (offsetX >= 0 && offsetY >= 0 &&
                   offsetX + grid.fullWidth <= static_cast<double>(single->width) * single->scale &&
                   offsetY + grid.fullHeight <= static_cast<double>(single->height) * single->scale)
                : ((grid.fullWidth + single->scale - 1) / single->scale == single->width &&
                   (grid.fullHeight + single->scale - 1) / single->scale == single->height),
            "mask必须匹配完整栅格或显式裁剪窗口", "mask");
    const double width = translated ? static_cast<double>(single->width) * single->scale
                                    : grid.fullWidth,
                 height = translated ? static_cast<double>(single->height) * single->scale
                                     : grid.fullHeight;
    regions.push_back(
        {"mask", (width - 1) * .5, (height - 1) * .5, width * shrink, height * shrink, 0, true});
  }
  budget(regions.size() * cloud.pointCount() <= 64 * 1024 * 1024, "ROI点包含检查超过64Mi预算");
  std::size_t activePixels = 0;
  for (std::size_t i = 0; i < base.index.data.size(); ++i) {
    if ((i & 16383) == 0)
      check(ctx);
    activePixels += base.index.data[i] >= 0;
  }
  if (mapped || p.choice("visibility") == "visible")
    budget(regions.size() * activePixels <= 64 * 1024 * 1024, "ROI像素包含检查超过64Mi预算");
  const auto sourceBytes =
      (cloud.xyz.size() + cloud.normals.size() + cloud.intensity.size()) * sizeof(float) +
      cloud.rgb.size();
  const auto fixedMemory = sourceBytes * 2 + base.index.data.size() * (mapped ? 26 : 22) +
                           cloud.pointCount() * (mapped ? 18 : 54) +
                           (masks ? masks->data.size() * sizeof(float) : 0) +
                           (single ? single->byteSize() : 0) + 16 * 1024 * 1024;
  budget(fixedMemory <= 512 * 1024 * 1024, "ROI工作峰值超过512MiB");
  const int selected = integer(p, "selectedRoi", 0, 63);
  require(regions.empty() ? selected == 0 : static_cast<std::size_t>(selected) < regions.size(),
          "selectedRoi超出实例范围", "selectedRoi");
  auto contains = [&](std::size_t n, double x, double y) {
    const auto& r = regions[n];
    if (r.unavailable)
      return false;
    x += offsetX;
    y += offsetY;
    const double scale = std::max(r.width, r.height), dx = (x - r.cx) / scale,
                 dy = (y - r.cy) / scale;
    if (!std::isfinite(dx) || !std::isfinite(dy))
      return false;
    const double c = std::cos(r.angle), s = std::sin(r.angle), halfX = r.width / scale / 2,
                 halfY = r.height / scale / 2;
    const double u = std::fma(c, dx, s * dy), v = std::fma(-s, dx, c * dy);
    const double roundoffX = 8 * std::numeric_limits<double>::epsilon() *
                             (std::abs(c * dx) + std::abs(s * dy) + halfX),
                 roundoffY = 8 * std::numeric_limits<double>::epsilon() *
                             (std::abs(s * dx) + std::abs(c * dy) + halfY);
    if (!(std::abs(u) <= halfX + roundoffX && std::abs(v) <= halfY + roundoffY))
      return false;
    if (!r.useMask)
      return true;
    if (single) {
      const double px = (x + .5) / single->scale, py = (y + .5) / single->scale;
      if (!(px >= 0 && py >= 0 && px < single->width && py < single->height))
        return false;
      return single->at(static_cast<int>(std::floor(px)), static_cast<int>(std::floor(py)), 0) != 0;
    }
    if (!(x > -.5 && y > -.5 && x < maskWidth - .5 && y < maskHeight - .5))
      return false;
    const auto pixel = static_cast<std::size_t>(std::floor(y + .5)) * maskWidth +
                       static_cast<std::size_t>(std::floor(x + .5));
    return masks->data[n * static_cast<std::size_t>(maskWidth) * maskHeight + pixel] == 1;
  };
  std::vector<std::vector<std::int32_t>> groups(regions.size());
  std::vector<bool> unionMask(cloud.pointCount(), false);
  std::size_t entries = 0;
  if (mapped || p.choice("visibility") == "visible") {
    for (int y = 0; y < grid.height; ++y) {
      check(ctx);
      for (int x = 0; x < grid.width; ++x) {
        const auto pixel = static_cast<std::size_t>(y) * grid.width + x;
        if (base.index.data[pixel] < 0)
          continue;
        const auto id = static_cast<std::int32_t>(base.index.data[pixel]);
        for (std::size_t n = 0; n < regions.size(); ++n)
          if (contains(n, pr::fullX(grid, x), pr::fullY(grid, y))) {
            budget(entries < 16 * 1024 * 1024, "ROI分组索引超过16Mi元素预算");
            budget(fixedMemory + (entries + 1) * 12 <= 512 * 1024 * 1024, "ROI分组峰值超过512MiB");
            groups[n].push_back(id);
            unionMask[static_cast<std::size_t>(id)] = true;
            ++entries;
          }
      }
    }
  } else {
    for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
      if ((i & 16383) == 0)
        check(ctx);
      if (projected[i].x < 0)
        continue;
      for (std::size_t n = 0; n < regions.size(); ++n)
        if (contains(n, projected[i].u, projected[i].v)) {
          budget(entries < 16 * 1024 * 1024, "ROI分组索引超过16Mi元素预算");
          budget(fixedMemory + (entries + 1) * 12 <= 512 * 1024 * 1024, "ROI分组峰值超过512MiB");
          groups[n].push_back(static_cast<std::int32_t>(i));
          unionMask[i] = true;
          ++entries;
        }
    }
  }
  budget(entries <= 16 * 1024 * 1024, "ROI分组索引超过16Mi元素预算");
  std::vector<std::int32_t> selectedIds, remap(cloud.pointCount(), -1);
  for (std::size_t i = 0; i < unionMask.size(); ++i)
    if (unionMask[i]) {
      remap[i] = static_cast<std::int32_t>(selectedIds.size());
      selectedIds.push_back(static_cast<std::int32_t>(i));
    }
  PointCloud output = cloud.select(selectedIds);
  Indices indices;
  indices.sourceCloudId = output.id;
  indices.values.reserve(entries);
  std::vector<cloud_groups::Group> definitions;
  for (std::size_t n = 0; n < groups.size(); ++n) {
    auto& group = groups[n];
    std::sort(group.begin(), group.end());
    group.erase(std::unique(group.begin(), group.end()), group.end());
    definitions.push_back({regions[n].id, indices.values.size(), group.size()});
    for (auto id : group)
      indices.values.push_back(remap[static_cast<std::size_t>(id)]);
  }
  auto result = raster(grid, 0);
  Tensor sourceMap = result.index;
  if (!regions.empty())
    for (std::size_t i = 0; i < result.index.data.size(); ++i) {
      if ((i & 16383) == 0)
        check(ctx);
      const auto original = static_cast<std::int32_t>(base.index.data[i]);
      if (original < 0 ||
          !std::binary_search(groups[static_cast<std::size_t>(selected)].begin(),
                              groups[static_cast<std::size_t>(selected)].end(), original))
        continue;
      result.index.data[i] = static_cast<float>(remap[static_cast<std::size_t>(original)]);
      sourceMap.data[i] = static_cast<float>(original);
      floats(result.depth)[i] = floats(base.depth)[i];
      result.status.mutablePixels()[i] = base.status.pixels.get()[i];
    }
  auto mapping =
      pr::rebind(oldMapping, output, result.depth, result.status, result.index, ctx, &sourceMap);
  auto groupRecord = cloud_groups::make(output, definitions, grid.cloudFrame);
  Indices sourceIndices;
  sourceIndices.sourceCloudId = cloud.id;
  sourceIndices.values = selectedIds;
  Json unavailable = Json::array();
  for (const auto& region : regions)
    if (region.unavailable)
      unavailable.push_back(region.id);
  Record quality{"vision.CloudROIQuality.v1",
                 {{"instanceCount", regions.size()},
                  {"pointCount", output.pointCount()},
                  {"selectedRoi", selected},
                  {"visibility", p.choice("visibility")},
                  {"maskPolicy", usingMask ? "native-mask-required" : "explicit-pose-geometry"},
                  {"missingMaskRois", std::move(unavailable)},
                  {"organizedView", "depth/status/indexMap/mapping"},
                  {"offsetPixels", {offsetX, offsetY}},
                  {"sourceCloudId", std::to_string(cloud.id)}}};
  check(ctx);
  out.set("cloud", Data::cloud(std::move(output)));
  out.set("sourceCloud", in.get("cloud"));
  out.set("indices", Data::indices(std::move(indices)));
  out.set("sourceIndices", Data::indices(std::move(sourceIndices)));
  out.set("groups", Data::record(std::move(groupRecord)));
  out.set("mapping", Data::record(std::move(mapping)));
  out.set("depth", Data::image(std::move(result.depth)));
  out.set("status", Data::image(std::move(result.status)));
  out.set("indexMap", Data::tensor(std::move(result.index)).withTensorScale(grid.scale));
  out.set("sourceIndexMap", Data::tensor(std::move(sourceMap)).withTensorScale(grid.scale));
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}

template <class Function> Status guarded(Function&& fn) {
  try {
    return fn();
  } catch (const Failure& failure) {
    return failure.status;
  } catch (const pr::Cancelled&) {
    return pr::failure("深度处理已取消", "", "cancelled");
  } catch (const std::bad_alloc&) {
    return pr::failure("深度处理内存分配失败", "", "resource_limit");
  } catch (const std::exception& error) {
    return pr::failure(std::string("深度处理失败：") + error.what());
  }
}
Param metric(const char* name, const char* label, double value) {
  auto result = img::floatParam(name, label, value, "有限米制值");
  result.unit = "m";
  return result;
}
Port optionalImage(const char* name, const char* doc) {
  auto result = img::imageIn(name, doc);
  result.required = false;
  return result;
}
std::vector<Port> mappingOutputs(const char* quality) {
  return {Port{"cloud", "PointCloud", "结果点云", "当前indexMap所引用的完整保通道结果云", true},
          Port{"sourceCloud", "PointCloud", "源点云", "父云原始只读载荷", true},
          img::imageOut("depth", "F32米；invalid为0并由status区分"),
          img::imageOut("status", "observed1/interpolated2/filtered4/collision8"),
          Port{"indexMap", "Tensor", "栅格点索引", "HW原生float32整数；-1无效", true},
          Port{"sourceIndexMap", "Tensor", "父云索引", "HW原生float32；-1无直接父观测", true},
          image_p1::recordPort("mapping", pr::kType),
          image_p1::recordPort("quality", quality)};
}

} // namespace

void registerP5Projection(Registry& registry) {
  for (const bool advanced : {false, true}) {
    auto op = image_p1::descriptor(
        advanced ? "cloud.orthographic_map" : "cloud.height_map",
        advanced ? "点云正交多特征图" : "点云正交高度图",
        "orthographic signed height workspace normal angle zbuffer organized pixel point mapping");
    op.inputs = {Port{"cloud", "PointCloud", "输入点云", "米制XYZ与可选通道", true},
                 Port{"gridToCloud", "Transform", "栅格到源云", "跨三维帧时必填", false}};
    op.outputs = mappingOutputs("vision.ProjectionQuality.v1");
    op.outputs.push_back(img::imageOut("image", "高度/法线角/可视化"));
    op.outputs.push_back(img::imageOut("pseudo", "RGB伪彩高度"));
    op.outputs.push_back(img::imageOut("normalAngle", "F32弧度；法线与栅格正Z夹角"));
    op.outputs.push_back(img::imageOut("normalValid", "U8法线有效区域"));
    op.outputs.push_back(
        Port{"pointState", "Tensor", "父云点状态", "0invalid/1visible/2occluded/3outside", true});
    auto width = img::intParam("width", "栅格宽度", 500, 1, 32768, "完整输出栅格像素数");
    width.unit = "px";
    width.absolute = true;
    auto height = width;
    height.name = "height";
    height.label = "栅格高度";
    op.params = {width,
                 height,
                 metric("resolution", "输出分辨率", .002),
                 metric("originX", "像素0中心X", -.499),
                 metric("originY", "像素0中心Y", -.499),
                 metric("minHeight", "最低高度", -1e6),
                 metric("maxHeight", "最高高度", 1e6),
                 image_p1::text("cloudFrame", "源云三维帧", "workspace"),
                 image_p1::text("gridFrame", "栅格三维帧", "workspace"),
                 image_p1::text("imageFrame", "二维图像帧", "height_map")};
    if (advanced) {
      op.params.push_back(metric("normalResolution", "法线采样分辨率", .002));
      op.params.push_back(img::enumParam(
          "outputMode", "输出图类型", "height",
          {{"height", "高度"}, {"normal_angle", "法线角伪彩"}, {"pseudo_height", "高度伪彩"}},
          "始终额外输出原生depth及normalAngle"));
    }
    op.doc =
        "显式工作空间栅格按最高真实观测生成有符号高度，独立状态保留零/负高度与空像素。compact云及HW索引组成organized视图；父云索引和遮挡状态独立保留。";
    op.compute = [advanced](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
      return guarded([&] { return orthographic(in, p, out, ctx, advanced); });
    };
    registry.addOperator(std::move(op));
  }
  auto op =
      image_p1::descriptor("cloud.project_camera", "点云相机栅格对齐",
                           "camera projection distortion RGB depth aligned cloud zbuffer indices");
  op.inputs = {Port{"cloud", "PointCloud", "源点云", "米制", true},
               img::imageIn("image", "RGB/RGBA相机图像"),
               image_p1::recordPort("calib", calibration::kCameraType),
               Port{"cloudToCamera", "Transform", "源云到相机", "跨三维帧必填", false}};
  op.outputs = mappingOutputs("vision.ProjectionQuality.v1");
  op.outputs.push_back(img::imageOut("image", "原始图像只读透传"));
  op.outputs.push_back(
      Port{"pointState", "Tensor", "父云点状态", "0invalid/1visible/2occluded/3outside", true});
  op.params = {image_p1::text("cloudFrame", "输入云三维帧", "camera"),
               image_p1::text("imageFrame", "二维图像帧", "image"),
               img::enumParam("colorSource", "结果云颜色", "image",
                              {{"image", "相机图像"}, {"cloud", "保留源RGB"}},
                              "其余强度/法线始终保留；法线旋转到相机帧")};
  op.doc =
      "按完整K/D和显式外参投影到相机工作栅格；最近Z-buffer、compact可见云、原生像素索引与全部父点状态，保留尺度和父索引，不制造PointCloud组织属性。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    return guarded([&] { return projectCamera(in, p, out, ctx); });
  };
  registry.addOperator(std::move(op));

  op = image_p1::descriptor(
      "image.depth_filter", "物理深度感知滤波",
      "depth mean median gaussian invalid IDW morphology protrusion depression filter");
  op.inputs = {img::imageIn("depth", "F32米制深度"),
               Port{"cloud", "PointCloud", "映射源云", "与映射组一同提供", false},
               optionalImage("status", "映射状态"),
               Port{"indexMap", "Tensor", "栅格索引", "与映射组一同提供", false},
               image_p1::recordPort("mapping", pr::kType, false),
               image_p1::recordPort("calib", calibration::kCameraType, false)};
  op.outputs = mappingOutputs("vision.DepthFilterQuality.v1");
  op.outputs.push_back(img::imageOut("changed", "改变像素U8"));
  auto kernel = img::intParam("kernelSize", "核尺寸", 3, 1, 31, "奇数；按原图像素尺度换算");
  kernel.unit = "px";
  kernel.step = 2;
  auto sigma = img::floatParam("sigma", "高斯标准差", 1, "正像素标准差");
  sigma.unit = "px";
  op.params = {
      img::enumParam("mode", "滤波模式", "mean",
                     {{"mean", "均值"},
                      {"median", "中值"},
                      {"gaussian", "高斯"},
                      {"fill", "无效补偿"},
                      {"eliminate", "凸凹消除"}},
                     "真实有效性与边缘阈值"),
      kernel,
      img::intParam("iterations", "迭代次数", 1, 1, 10, "每轮只读上一轮"),
      sigma,
      img::boolParam("fillInvalid", "均值补无效", false, "仅均值模式允许同时补值"),
      img::enumParam("fillMethod", "补偿方法", "idw",
                     {{"idw", "反距离加权"}, {"min", "邻域最小"}, {"max", "邻域最大"}},
                     "仅无效像素"),
      img::floatParam("idwPower", "距离幂", 2, "0.01..16"),
      img::enumParam("eliminateType", "消除类型", "protrusion",
                     {{"protrusion", "凸起"}, {"depression", "凹陷"}, {"both", "凸起和凹陷"}},
                     "形态学top-hat/black-hat"),
      img::enumParam("direction", "方向", "xy", {{"x", "水平"}, {"y", "垂直"}, {"xy", "二维"}},
                     "结构元素方向"),
      metric("threshold", "凸凹阈值", .001),
      metric("maxDepthJump", "最大深度跳变", .01),
      img::boolParam("includeInterpolated", "使用合成邻域", false, "默认仅真实观测支持滤波/补偿"),
      image_p1::text("imageFrame", "直接深度像素帧", "image")};
  op.doc =
      "完整五模式深度滤波，支持带映射的有符号高度，或F32深度+标定直接建图。无变化保留真实源XYZ，改深度沿原射线；输出仅有效结果点，新索引重绑并保留父云源索引。maxDepthJump=0关闭平滑/补值的边缘门控。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    return guarded([&] { return depthFilter(in, p, out, ctx); });
  };
  registry.addOperator(std::move(op));

  op = image_p1::descriptor(
      "cloud.roi_extract", "二维实例关联点云提取",
      "P4 instance mask NHW cloud ROI organized unorganized original indices groups");
  op.inputs = {Port{"cloud", "PointCloud", "输入点云", "米制", true},
               optionalImage("depth", "组织深度"),
               optionalImage("status", "组织状态"),
               Port{"indexMap", "Tensor", "组织索引", "完整映射组", false},
               image_p1::recordPort("mapping", pr::kType, false),
               image_p1::recordPort("calib", calibration::kCameraType, false),
               Port{"cloudToCamera", "Transform", "云到相机", "跨帧必填", false},
               image_p1::recordPort("instances", poses::kPoses2DType, false),
               Port{"masks", "Tensor", "P4实例掩膜", "原生NHW", false},
               image_p1::recordPort("maskInfo", "vision.Masks2D.v1", false),
               optionalImage("mask", "单区域二值mask")};
  op.outputs = mappingOutputs("vision.CloudROIQuality.v1");
  op.outputs.push_back(Port{"indices", "Indices", "各组索引", "关联结果cloud", true});
  op.outputs.push_back(
      Port{"sourceIndices", "Indices", "结果云父索引", "关联sourceCloud，顺序对应cloud", true});
  op.outputs.push_back(image_p1::recordPort("groups", cloud_groups::kType));
  auto offsetX =
      img::floatParam("offsetX", "ROI像素原点偏移X", 0, "ROI全像素坐标=当前图像素+此偏移");
  offsetX.unit = "px";
  offsetX.absolute = true;
  auto offsetY = offsetX;
  offsetY.name = "offsetY";
  offsetY.label = "ROI像素原点偏移Y";
  op.params = {
      img::floatParam("shrink", "区域比例", 1, "0..1，绕旋转实例中心收缩"),
      img::boolParam("usingMask", "使用实例掩膜", true, "有掩膜时消费真实NHW，不退化为框"),
      img::boolParam("usingOffset", "使用裁剪偏移", false, "显式关联更大ROI图像与当前裁剪栅格"),
      img::enumParam("visibility", "可见性", "visible",
                     {{"visible", "Z-buffer可见"}, {"all", "全部投影点"}},
                     "all需要未组织calib路径"),
      img::intParam("selectedRoi", "组织视图实例序号", 0, 0, 63,
                    "分组仍保留全部ROI，此参数选择depth/indexMap组织视图"),
      offsetX,
      offsetY,
      image_p1::text("cloudFrame", "未组织源云三维帧", "camera"),
      image_p1::text("imageFrame", "二维ROI参考帧", "image")};
  op.doc =
      "消费P4实例及原生掩膜绑定，支持组织索引视图或K/D投影的未组织源云。多ROI用Groups/Indices保留重叠和稳定ID，同时交付选定ROI的组织视图；显式空ROI成功空结果。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    return guarded([&] { return roiExtract(in, p, out, ctx); });
  };
  registry.addOperator(std::move(op));
}
} // namespace lyflow::ops
