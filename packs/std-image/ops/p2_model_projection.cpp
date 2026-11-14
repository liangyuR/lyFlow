#include "lyflow_cv/compat.h"

#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {
using Json = nlohmann::json;
using Matrix = cv::Matx44d;
constexpr std::size_t kMaxPoints = 2 * 1024 * 1024;
constexpr std::size_t kMaxPixels = 8 * 1024 * 1024;
constexpr std::size_t kMaxProjected = 8 * 1024 * 1024;

Status fail(const std::string& message, const char* port, const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}

Status cloudInput(const PointCloud* cloud, const char* port) {
  if (!cloud || !cloud->channelsConsistent())
    return fail("点云或可选通道长度无效", port);
  if (cloud->pointCount() > kMaxPoints)
    return fail("投影点数超过2Mi预算", port, "resource_limit");
  for (const auto* channel : {&cloud->xyz, &cloud->normals, &cloud->intensity})
    for (float value : *channel)
      if (!std::isfinite(value))
        return fail("点云坐标及可选数值通道必须有限", port);
  return Status::Ok();
}

Status cameraInput(const Inputs& in, const Image& image, calibration::Camera& camera) {
  const auto* record = in.get("calib").asRecord();
  std::string why;
  if (!record || !calibration::parseCamera(*record, camera, why))
    return fail("需要相机标定：" + why, "calib");
  if ((std::int64_t(camera.width) + image.scale - 1) / image.scale != image.width ||
      (std::int64_t(camera.height) + image.scale - 1) / image.scale != image.height)
    return fail("图像尺寸和比例必须对应标定的完整图像", "calib");
  return Status::Ok();
}

bool rigid(const Transform& value, Matrix& matrix) {
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column) {
      const double v = value.m[row * 4 + column];
      if (!std::isfinite(v))
        return false;
      matrix(row, column) = v;
    }
  for (int column = 0; column < 4; ++column)
    if (std::abs(matrix(3, column) - (column == 3 ? 1 : 0)) > 1e-6)
      return false;
  cv::Matx33d rotation;
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
      rotation(row, column) = matrix(row, column);
  const cv::Matx33d residual = rotation.t() * rotation - cv::Matx33d::eye();
  return cv::norm(residual) <= 1e-5 && std::abs(cv::determinant(rotation) - 1) <= 1e-5;
}

Status frameTransform(const Inputs& in, const char* port, const std::string& frame,
                      const calibration::Camera& camera, Matrix& matrix) {
  if (frame.empty() || frame.size() > 1024)
    return fail("源参考帧不能为空或超过1024字节", port);
  matrix = Matrix::eye();
  if (in.has(port)) {
    const auto* value = in.get(port).asTransform();
    if (!value || !rigid(*value, matrix))
      return fail("需要源帧到相机帧的有限右手刚体变换", port);
  } else if (frame != camera.frame) {
    return fail("源参考帧与相机不同，必须提供显式变换", port);
  }
  return Status::Ok();
}

struct Pixel {
  int x = -1, y = -1;
  double depth = 0;
};

Status project(const PointCloud& cloud, const Matrix& matrix, const calibration::Camera& camera,
               const Image& image, std::vector<Pixel>& pixels, ExecContext& ctx) {
  pixels.assign(cloud.pointCount(), Pixel{});
  std::vector<cv::Point3d> points;
  std::vector<std::size_t> ids;
  std::vector<cv::Point2d> projected;
  points.reserve(4096);
  ids.reserve(4096);
  for (std::size_t offset = 0; offset < cloud.pointCount(); offset += 4096) {
    if (ctx.cancelled())
      return fail("点云投影已取消", "cloud", "cancelled");
    points.clear();
    ids.clear();
    const auto end = std::min(offset + 4096, cloud.pointCount());
    for (std::size_t i = offset; i < end; ++i) {
      const cv::Vec4d p =
          matrix * cv::Vec4d(cloud.xyz[3 * i], cloud.xyz[3 * i + 1], cloud.xyz[3 * i + 2], 1);
      if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && p[2] > 0) {
        points.emplace_back(p[0], p[1], p[2]);
        ids.push_back(i);
      }
    }
    if (points.empty())
      continue;
    cv::projectPoints(points, cv::Vec3d::all(0), cv::Vec3d::all(0), image_p1::cameraMatrix(camera),
                      image_p1::distortion(camera), projected);
    for (std::size_t j = 0; j < ids.size(); ++j) {
      const double u = projected[j].x, v = projected[j].y;
      if (!std::isfinite(u) || !std::isfinite(v) || !(u > -0.5 && v > -0.5) ||
          !(u < camera.width - 0.5 && v < camera.height - 0.5))
        continue;
      const auto x = static_cast<int>(std::floor((u + 0.5) / image.scale));
      const auto y = static_cast<int>(std::floor((v + 0.5) / image.scale));
      if (x >= 0 && x < image.width && y >= 0 && y < image.height)
        pixels[ids[j]] = {x, y, points[j].z};
    }
  }
  return Status::Ok();
}

Status imageRoi(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto* cloud = in.get("cloud").asCloud();
  auto status = cloudInput(cloud, "cloud");
  if (!status.ok)
    return status;
  const auto* mask = in.get("mask").asImage();
  status = image_p1::image(mask, "mask", true, kMaxPixels);
  if (!status.ok)
    return status;
  if (mask->depth != PixelDepth::U8)
    return fail("ROI掩膜必须是u8单通道，0为外部、非0为内部", "mask");
  calibration::Camera camera;
  status = cameraInput(in, *mask, camera);
  if (!status.ok)
    return status;
  Matrix matrix;
  status = frameTransform(in, "cloudToCamera", p.text("cloudFrame"), camera, matrix);
  if (!status.ok)
    return status;
  std::vector<Pixel> pixels;
  status = project(*cloud, matrix, camera, *mask, pixels, ctx);
  if (!status.ok)
    return status;
  const bool visibleOnly = p.flag("visibleOnly");
  const double tolerance = p.number("occlusionTolerance");
  if (!std::isfinite(tolerance) || tolerance < 0 || tolerance > 1000000)
    return img::badParam("遮挡深度容差必须为[0,1000000]有限米制距离", "occlusionTolerance");
  std::vector<double> depth;
  if (visibleOnly) {
    depth.assign(mask->pixelCount(), std::numeric_limits<double>::infinity());
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      if ((i & 8191) == 0 && ctx.cancelled())
        return fail("ROI可见性计算已取消", "cloud", "cancelled");
      const auto& pixel = pixels[i];
      if (pixel.x >= 0) {
        auto& z = depth[static_cast<std::size_t>(pixel.y) * mask->width + pixel.x];
        z = std::min(z, pixel.depth);
      }
    }
  }
  Indices indices;
  indices.sourceCloudId = cloud->id;
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    if ((i & 8191) == 0 && ctx.cancelled())
      return fail("ROI索引选择已取消", "cloud", "cancelled");
    const auto& pixel = pixels[i];
    if (pixel.x < 0)
      continue;
    const auto index = static_cast<std::size_t>(pixel.y) * mask->width + pixel.x;
    if (mask->pixels.get()[index] != 0 && (!visibleOnly || pixel.depth <= depth[index] + tolerance))
      indices.values.push_back(static_cast<std::int32_t>(i));
  }
  auto selected = cloud->select(indices.values);
  Record quality{"cloud.ImageROI.v1",
                 {{"sourceCloudId", std::to_string(cloud->id)},
                  {"cloudFrame", p.text("cloudFrame")},
                  {"cameraFrame", camera.frame},
                  {"unit", "m"},
                  {"selected", indices.values.size()},
                  {"inputCount", cloud->pointCount()},
                  {"imageSize", {camera.width, camera.height}},
                  {"imageScale", mask->scale},
                  {"visibleOnly", visibleOnly},
                  {"occlusionTolerance", tolerance}}};
  if (ctx.cancelled())
    return fail("ROI选择已取消", "cloud", "cancelled");
  out.set("indices", Data::indices(std::move(indices)));
  out.set("cloud", Data::cloud(std::move(selected)));
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}

Matrix poseMatrix(const Json& pose) {
  poses::Quaternion q;
  for (int i = 0; i < 4; ++i)
    q[static_cast<std::size_t>(i)] = pose.at("rotation")[i];
  const auto rotation = poses::rotationMatrix(q);
  Matrix matrix = Matrix::eye();
  for (int row = 0; row < 3; ++row) {
    matrix(row, 3) = pose.at("position")[row];
    for (int column = 0; column < 3; ++column)
      matrix(row, column) = rotation[static_cast<std::size_t>(row * 3 + column)];
  }
  return matrix;
}

Status render(const Inputs& in, const ParamView&, Outputs& out, ExecContext& ctx) {
  const auto* image = in.get("image").asImage();
  auto status = image_p1::image(image, "image", false, kMaxPixels);
  if (!status.ok)
    return status;
  const auto* model = in.get("model").asCloud();
  status = cloudInput(model, "model");
  if (!status.ok)
    return status;
  calibration::Camera camera;
  status = cameraInput(in, *image, camera);
  if (!status.ok)
    return status;
  const auto* instances = in.get("instances").asRecord();
  std::string why;
  if (!instances || instances->type != poses::kPoses3DType || !poses::validate(*instances, why))
    return fail("需要有效Pose3D集合：" + why, "instances");
  const auto& items = instances->data.at("items");
  if (items.size() > 256 || (items.size() && model->pointCount() > kMaxProjected / items.size()))
    return fail("模型实例投影超过8Mi点或256实例预算", "instances", "resource_limit");
  Matrix sceneToCamera;
  status = frameTransform(in, "sceneToCamera", instances->data.at("frame"), camera, sceneToCamera);
  if (!status.ok)
    return status;
  std::vector<double> depths(image->pixelCount(), std::numeric_limits<double>::infinity());
  std::vector<std::int32_t> owners(image->pixelCount(), -1);
  std::vector<Pixel> pixels;
  for (std::size_t instance = 0; instance < items.size(); ++instance) {
    status =
        project(*model, sceneToCamera * poseMatrix(items[instance]), camera, *image, pixels, ctx);
    if (!status.ok)
      return status;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      if ((i & 8191) == 0 && ctx.cancelled())
        return fail("模型叠图已取消", "model", "cancelled");
      const auto& pixel = pixels[i];
      if (pixel.x < 0)
        continue;
      const auto index = static_cast<std::size_t>(pixel.y) * image->width + pixel.x;
      if (pixel.depth < depths[index]) {
        depths[index] = pixel.depth;
        owners[index] = static_cast<std::int32_t>(instance);
      }
    }
  }
  cv::Mat overlay = cvx::view(*image).clone();
  const double full = image->depth == PixelDepth::U8    ? 255
                      : image->depth == PixelDepth::U16 ? 65535
                                                        : 1;
  std::size_t rendered = 0;
  for (int y = 0; y < image->height; ++y) {
    if (ctx.cancelled())
      return fail("模型叠图已取消", "image", "cancelled");
    for (int x = 0; x < image->width; ++x) {
      const auto index = static_cast<std::size_t>(y) * image->width + x;
      if (owners[index] < 0)
        continue;
      ++rendered;
      const auto tint = owners[index] % 3;
      cv::Scalar color = image->channels == 1 ? cv::Scalar(full) : cv::Scalar(0, full, 0, full);
      if (image->channels > 1) {
        color[tint] = full;
        color[(tint + 1) % 3] = full / 3;
      }
      cv::rectangle(overlay, cv::Rect(x, y, 1, 1), color, cv::FILLED);
    }
  }
  if (image->channels == 4) {
    const int channels[] = {3, 3};
    const cv::Mat original = cvx::view(*image);
    cv::mixChannels(&original, 1, &overlay, 1, channels, 1);
  }
  Image result;
  if (!cvx::fromMat(std::move(overlay), result))
    return fail("输出图像格式不合法", "image");
  result.scale = image->scale;
  Record quality{"image.ModelProjection.v1",
                 {{"frame", camera.frame},
                  {"unit", "px"},
                  {"imageSize", {camera.width, camera.height}},
                  {"imageScale", image->scale},
                  {"instanceCount", items.size()},
                  {"renderedPixels", rendered},
                  {"method", "calibrated-model-point-zbuffer"}}};
  if (ctx.cancelled())
    return fail("模型叠图已取消", "image", "cancelled");
  out.set("image", Data::image(std::move(result)));
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}

Status guarded(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx, bool draw) {
  try {
    return draw ? render(in, p, out, ctx) : imageRoi(in, p, out, ctx);
  } catch (const cv::Exception& error) {
    return img::fromCvError(error);
  }
}
}

void registerP2ModelProjection(Registry& registry) {
  auto op =
      image_p1::descriptor("cloud.image_roi", "图像区域选择点云",
                           "image mask calibrated cloud roi selection original indices visibility");
  op.doc =
      "按标定和显式云到相机变换投影，输出关联原云的ROI索引与保通道子云。零mask为空ROI；可选z-buffer排除遮挡点，深度容差为米。";
  op.inputs = {Port{"cloud", "PointCloud", "原始点云", "米制", true},
               img::imageIn("mask", "u8非零区域；像素网格必须对应标定"),
               image_p1::recordPort("calib", calibration::kCameraType),
               Port{"cloudToCamera", "Transform", "云到相机", "跨帧时必填", false}};
  op.outputs = {Port{"indices", "Indices", "原云索引", "关联输入cloud.id", true},
                Port{"cloud", "PointCloud", "ROI子云", "保留可选通道", true},
                image_p1::recordPort("quality", "cloud.ImageROI.v1")};
  auto tolerance = img::floatParam("occlusionTolerance", "遮挡深度容差", 0.001, "非负米制距离");
  tolerance.unit = "m";
  tolerance.min = 0;
  tolerance.max = 1000000;
  op.params = {image_p1::text("cloudFrame", "点云参考帧", "camera"),
               img::boolParam("visibleOnly", "仅可见点", true, "同一工作像素只选最近深度附近的点"),
               tolerance};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    return guarded(in, p, out, ctx, false);
  };
  registry.addOperator(std::move(op));

  op = image_p1::descriptor("image.render_model_3d", "三维模板投影叠图",
                            "render centered 3d model poses calibrated image overlay zbuffer");
  op.doc =
      "将居中模型按Pose3D变换到场景，经显式场景到相机变换及相机畸变投影。点云模板采用像素z-buffer叠图，保留位深/通道/alpha，不合成不可见的Mesh表面。";
  op.inputs = {img::imageIn(), Port{"model", "PointCloud", "居中模型", "模型局部米制坐标", true},
               image_p1::recordPort("instances", poses::kPoses3DType),
               image_p1::recordPort("calib", calibration::kCameraType),
               Port{"sceneToCamera", "Transform", "场景到相机", "跨帧时必填", false}};
  op.outputs = {img::imageOut(), image_p1::recordPort("quality", "image.ModelProjection.v1")};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    return guarded(in, p, out, ctx, true);
  };
  registry.addOperator(std::move(op));
}
} // namespace lyflow::ops
