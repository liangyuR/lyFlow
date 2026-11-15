#include "p1_common.h"
#include "p5_projection_common.h"
#include "../../std-geometry/ops/p5_pose_geometry_common.h"
#include <opencv2/imgproc.hpp>
#include <map>

namespace lyflow::ops {
namespace {
namespace g = lyflow::packs::std_geometry::p5;
using Json = nlohmann::json;
constexpr std::size_t kPixels = 4U * 1024U * 1024U, kMasks = 32U * 1024U * 1024U,
                      kWork = 64U * 1024U * 1024U;
struct Failure {
  Status status;
};
void require(bool v, const std::string& why, const char* port = "poses") {
  if (!v)
    throw Failure{g::bad(why, port)};
}
void limit(bool v) {
  if (!v)
    throw Failure{g::resource()};
}
void poll(ExecContext& ctx) {
  if (ctx.cancelled())
    throw Failure{g::cancelled()};
}
calibration::Camera camera(const Inputs& in) {
  calibration::Camera c;
  std::string why;
  const auto* r = in.get("calib").asRecord();
  require(r && calibration::parseCamera(*r, c, why), "标定非法：" + why, "calib");
  limit(static_cast<std::size_t>(c.width) * c.height <= kPixels);
  return c;
}
Record read3(const Inputs& in, const char* port) {
  Record r;
  auto s = g::list(in, port, r);
  if (!s.ok)
    throw Failure{s};
  return r;
}
Record read2(const Inputs& in, const char* port, const std::string& frame) {
  Record r;
  std::string why;
  require(poses::read2DList(in.get(port), r, why) && r.data["frame"] == frame &&
              r.data["unit"] == "px",
          "二维实例须为声明图像帧原像素：" + why, port);
  return r;
}
g::M4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
g::M4 toCamera(const Inputs& in, const char* port, const std::string& frame,
               const calibration::Camera& c) {
  auto m = identity();
  poses::Quaternion q;
  std::string why;
  if (in.has(port))
    require(g::rigid(in.get(port).asTransform(), m, q, why), why, port);
  else
    require(frame == c.frame, "三维源帧不同于camera.frame，需source→camera Transform", port);
  return m;
}
std::vector<cv::Point2d> normalized(const std::vector<cv::Point2d>& points,
                                    const calibration::Camera& c, projection5::RayBudget& budget) {
  std::vector<cv::Point2d> result;
  result.reserve(points.size());
  for (const auto point : points) {
    cv::Vec3d ray;
    const bool ok = projection5::normalizedRay(c, point.x, point.y, ray, &budget);
    limit(!budget.exhausted);
    require(ok && g::validNumber(ray[0]) && g::validNumber(ray[1]),
            "畸变模型在请求像素不可稳定求逆", "calib");
    result.emplace_back(ray[0], ray[1]);
  }
  return result;
}
struct Masks {
  const Tensor* tensor = nullptr;
  const Record* info = nullptr;
  std::size_t pixels = 0;
};
Masks masks(const Inputs& in, const Record& poses, const calibration::Camera& c, ExecContext& ctx) {
  require(in.has("masks") == in.has("maskInfo"), "Masks和maskInfo须成组", "masks");
  Masks m;
  if (!in.has("masks"))
    return m;
  m.tensor = in.get("masks").asTensor();
  m.info = in.get("maskInfo").asRecord();
  m.pixels = static_cast<std::size_t>(c.width) * c.height;
  require(in.get("masks").tensorScale() == 1 && m.tensor && m.tensor->consistent() && m.info &&
              m.info->type == "vision.Masks2D.v1" && g::bounded(*m.info),
          "需要scale1原生NHW Masks及有界绑定", "masks");
  const auto& d = m.info->data;
  const auto n = poses.data["items"].size();
  require(d.is_object() && d.size() == 6 && d.value("frame", Json{}) == poses.data["frame"] &&
              d.value("unit", Json{}) == "px" && d.value("layout", Json{}) == "NHW" &&
              d.contains("width") && d["width"].is_number_integer() && d.contains("height") &&
              d["height"].is_number_integer() && d.value("width", Json{}) == c.width &&
              d.value("height", Json{}) == c.height && d.contains("items") &&
              d["items"].is_array() && d["items"].size() == n &&
              m.tensor->shape ==
                  std::vector<std::int64_t>{static_cast<std::int64_t>(n), c.height, c.width},
          "掩膜维度/帧/原图extent/顺序不一致", "maskInfo");
  limit(m.tensor->data.size() <= kMasks);
  for (std::size_t j = 0; j < n; ++j) {
    const auto& item = d["items"][j];
    require(item.is_object() && item.size() == 4 &&
                item.value("id", Json{}) == poses.data["items"][j]["id"] &&
                item.contains("index") && item["index"].is_number_integer() && item["index"] == j &&
                item.contains("hasMask") && item["hasMask"].is_boolean() &&
                item.contains("source") && item["source"].is_string(),
            "掩膜ID/index/hasMask/source非法", "maskInfo");
    if (poses.data["items"][j].contains("attributes")) {
      const auto& at = poses.data["items"][j]["attributes"];
      if (at.contains("hasMask"))
        require(at["hasMask"] == item["hasMask"], "Pose hasMask与maskInfo矛盾", "maskInfo");
      if (at.contains("maskIndex"))
        require(at["maskIndex"] == j, "Pose maskIndex与原生顺序矛盾", "maskInfo");
    }
    const auto position = poses.data["items"][j]["position"].get<std::array<double, 2>>(),
               size = poses.data["items"][j]["size"].get<std::array<double, 2>>();
    const double angle = poses.data["items"][j]["angle"].get<double>(), cs = std::cos(angle),
                 sn = std::sin(angle);
    bool any = false;
    for (std::size_t i = 0; i < m.pixels; ++i) {
      if ((i & 8191) == 0)
        poll(ctx);
      const float v = m.tensor->data[j * m.pixels + i];
      require(v == 0 || v == 1, "掩膜只允许有限0/1", "masks");
      if (v == 1) {
        const double dx = double(i % static_cast<std::size_t>(c.width)) - position[0],
                     dy = double(i / static_cast<std::size_t>(c.width)) - position[1];
        require(std::abs(cs * dx + sn * dy) <= size[0] / 2 &&
                    std::abs(-sn * dx + cs * dy) <= size[1] / 2,
                "mask支持越出其Pose有效范围", "masks");
      }
      any = any || v == 1;
    }
    require(any == item["hasMask"].get<bool>() &&
                (any ? item["source"] != "none" : item["source"] == "none"),
            "hasMask和实际非零支持不一致", "maskInfo");
  }
  return m;
}
Json infoItem(const Json& id, std::size_t index, bool has, const char* source) {
  return {{"id", id}, {"index", index}, {"hasMask", has}, {"source", has ? source : "none"}};
}
Record maskInfo(const std::string& frame, int w, int h, Json items) {
  return {"vision.Masks2D.v1",
          {{"frame", frame},
           {"unit", "px"},
           {"width", w},
           {"height", h},
           {"layout", "NHW"},
           {"items", std::move(items)}}};
}
struct Box {
  g::V3 origin, half;
  poses::Quaternion inverse;
  std::string shape;
};
double hitBox(const Box& b, g::V3 ray, double near, bool top) {
  const auto o = g::rotated(b.inverse, g::mul(b.origin, -1)), d = g::rotated(b.inverse, ray);
  if (b.shape == "circle") {
    if (d[2] == 0)
      return -1;
    const double t = -o[2] / d[2];
    auto p = g::add(o, g::mul(d, t));
    return t >= near && std::hypot(p[0], p[1]) <= b.half[0] ? t : -1;
  }
  if (b.shape == "sphere") {
    const double length = std::hypot(d[0], d[1], d[2]);
    const auto direction = g::mul(d, 1 / length);
    const double along = -g::dot(o, direction);
    g::V3 closest{};
    for (int k = 0; k < 3; ++k)
      closest[k] = std::fma(direction[k], along, o[k]);
    const double distance = std::hypot(closest[0], closest[1], closest[2]);
    if (!std::isfinite(distance) || distance > b.half[0])
      return -1;
    const double delta = std::sqrt((b.half[0] - distance) * (b.half[0] + distance));
    const double low = (along - delta) / length, high = (along + delta) / length;
    return high >= near ? std::max(low, near) : -1;
  }
  if (b.shape == "cylinder") {
    if (top) {
      if (d[2] == 0)
        return -1;
      double t = (b.half[2] - o[2]) / d[2];
      auto p = g::add(o, g::mul(d, t));
      return t >= near && std::hypot(p[0], p[1]) <= b.half[0] ? t : -1;
    }
    const double radialLength = std::hypot(d[0], d[1]);
    double lo = near, hi = std::numeric_limits<double>::infinity();
    if (radialLength == 0) {
      if (std::hypot(o[0], o[1]) > b.half[0])
        return -1;
    } else {
      const double dx = d[0] / radialLength, dy = d[1] / radialLength,
                   along = -(o[0] * dx + o[1] * dy);
      const double distance = std::hypot(std::fma(dx, along, o[0]), std::fma(dy, along, o[1]));
      if (!std::isfinite(distance) || distance > b.half[0])
        return -1;
      const double delta = std::sqrt((b.half[0] - distance) * (b.half[0] + distance));
      lo = std::max(lo, (along - delta) / radialLength);
      hi = std::min(hi, (along + delta) / radialLength);
    }
    if (d[2] == 0) {
      if (std::abs(o[2]) > b.half[2])
        return -1;
    } else {
      double a = (-b.half[2] - o[2]) / d[2], btop = (b.half[2] - o[2]) / d[2];
      if (a > btop)
        std::swap(a, btop);
      lo = std::max(lo, a);
      hi = std::min(hi, btop);
    }
    return hi >= lo && std::isfinite(lo) ? lo : -1;
  }
  if (top) {
    if (d[2] == 0)
      return -1;
    double t = (b.half[2] - o[2]) / d[2];
    if (t < near || !std::isfinite(t))
      return -1;
    auto p = g::add(o, g::mul(d, t));
    return std::abs(p[0]) <= b.half[0] && std::abs(p[1]) <= b.half[1] ? t : -1;
  }
  double lo = near, hi = std::numeric_limits<double>::infinity();
  for (int j = 0; j < 3; ++j) {
    if (d[j] == 0) {
      if (std::abs(o[j]) > b.half[j])
        return -1;
    } else {
      double a = (-b.half[j] - o[j]) / d[j], z = (b.half[j] - o[j]) / d[j];
      if (a > z)
        std::swap(a, z);
      lo = std::max(lo, a);
      hi = std::min(hi, z);
    }
  }
  return hi >= lo && std::isfinite(lo) ? lo : -1;
}
Status project(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    poll(ctx);
    const auto c = camera(in);
    projection5::RayBudget rayBudget;
    rayBudget.context = &ctx;
    auto r = read3(in, "poses");
    const auto original = r;
    auto s = g::transformList(in, "posesToCamera", r, c.frame);
    if (!s.ok)
      return s;
    const auto frame = p.text("imageFrame");
    require(!frame.empty(), "图像帧不能为空");
    const auto pixels = static_cast<std::size_t>(c.width) * c.height, n = r.data["items"].size();
    limit(n == 0 || pixels <= kMasks / n);
    limit(n == 0 || pixels <= kWork / n);
    const auto near = p.number("nearDepth");
    require(std::isfinite(near) && near > 0, "nearDepth须正有限米");
    const auto dilation = p.integer("dilate");
    require(dilation >= 0 && dilation <= 256, "膨胀半径须0..256");
    const bool merge = p.choice("mode") == "union", top = p.choice("surface") == "top";
    require(merge || p.choice("mode") == "individual", "未知投影模式");
    require(top || p.choice("surface") == "full", "未知投影面");
    require(p.choice("occlusion") == "independent" || p.choice("occlusion") == "nearest",
            "未知遮挡模式");
    require(p.choice("occlusion") != "nearest" || dilation == 0,
            "遮挡深度模式不允许无深度依据的膨胀");
    std::vector<Box> boxes;
    for (const auto& item : r.data["items"]) {
      auto center = g::jvector(item["position"]), size = g::jvector(item["size"]);
      auto q = item["rotation"].get<poses::Quaternion>();
      for (double x : size)
        require(g::validNumber(x) && x > 0, "有限体尺寸超限");
      std::string shape = "box";
      if (item.contains("attributes") && item["attributes"].contains("shape")) {
        require(item["attributes"]["shape"].is_string(), "shape必须文本");
        shape = item["attributes"]["shape"].get<std::string>();
      }
      require(shape == "box" || shape == "circle" || shape == "cylinder" || shape == "sphere",
              "不支持的显式工件几何");
      if (shape != "box")
        require(std::abs(size[0] - size[1]) <= 1e-10 * std::max(size[0], size[1]),
                "圆形几何XY直径必须一致");
      if (shape == "sphere")
        require(std::abs(size[0] - size[2]) <= 1e-10 * std::max(size[0], size[2]),
                "球体XYZ直径必须一致");
      if (item.contains("attributes")) {
        const auto& at = item["attributes"];
        for (const char* key : {"circleRadius", "radius"})
          if (shape == "circle" && at.contains(key))
            require(values::safeNumber(at[key]) &&
                        std::abs(at[key].get<double>() - size[0] / 2) <= 1e-10 * size[0],
                    "circle半径与size矛盾");
        if (at.contains("normal") && shape == "circle") {
          require(poses::numericArray(at["normal"], 3), "circle法向非法");
          auto normal = g::jvector(at["normal"]);
          require(g::normalize(normal) &&
                      g::dot(normal, g::rotated(original.data["items"][boxes.size()]["rotation"]
                                                    .get<poses::Quaternion>(),
                                                {0, 0, 1})) > 1 - 1e-8,
                  "circle法向与Pose.localZ矛盾");
        }
      }
      if (p.choice("anchor") == "top")
        center = g::sub(center, g::rotated(q, {0, 0, size[2] / 2}));
      else
        require(p.choice("anchor") == "center", "未知位姿锚点");
      for (double v : center)
        require(g::validNumber(v), "几何中心非有限或超出计算范围");
      boxes.push_back({center, g::mul(size, .5), g::inverse(q), shape});
    }
    Record rois;
    const bool roiProvided = in.has("rois");
    if (roiProvided)
      rois = read2(in, "rois", frame);
    const auto allowed = [&](int x, int y) {
      if (!roiProvided)
        return true;
      for (const auto& roi : rois.data["items"]) {
        const auto pos = roi["position"].get<std::array<double, 2>>(),
                   size = roi["size"].get<std::array<double, 2>>();
        double a = roi["angle"].get<double>(), dx = x - pos[0], dy = y - pos[1],
               u = std::cos(a) * dx + std::sin(a) * dy, v = -std::sin(a) * dx + std::cos(a) * dy;
        if (std::abs(u) <= size[0] / 2 && std::abs(v) <= size[1] / 2)
          return true;
      }
      return false;
    };
    if (roiProvided)
      limit(rois.data["items"].empty() || pixels <= kWork / rois.data["items"].size());
    std::vector<unsigned char> roiPixels(pixels, 1);
    if (roiProvided)
      for (int y = 0; y < c.height; ++y) {
        poll(ctx);
        for (int x = 0; x < c.width; ++x)
          roiPixels[static_cast<std::size_t>(y) * c.width + x] = allowed(x, y) ? 1 : 0;
      }
    Tensor all;
    all.shape = {static_cast<std::int64_t>(n), c.height, c.width};
    all.data.assign(n * pixels, 0);
    std::vector<double> depths;
    if (p.choice("occlusion") == "nearest")
      depths.assign(pixels, std::numeric_limits<double>::infinity());
    std::vector<int> owner(pixels, -1);
    for (int y = 0; n && y < c.height; ++y) {
      poll(ctx);
      std::vector<cv::Point2d> points;
      std::vector<int> columns;
      points.reserve(c.width);
      columns.reserve(c.width);
      for (int x = 0; x < c.width; ++x)
        if (roiPixels[static_cast<std::size_t>(y) * c.width + x]) {
          points.emplace_back(x, y);
          columns.push_back(x);
        }
      auto rays = normalized(points, c, rayBudget);
      for (std::size_t column = 0; column < columns.size(); ++column) {
        const int x = columns[column];
        const auto k = static_cast<std::size_t>(y) * c.width + x;
        for (std::size_t j = 0; j < n; ++j) {
          double d = hitBox(boxes[j], {rays[column].x, rays[column].y, 1}, near, top);
          if (d < 0)
            continue;
          if (depths.empty())
            all.data[j * pixels + k] = 1;
          else if (d < depths[k]) {
            depths[k] = d;
            owner[k] = static_cast<int>(j);
          }
        }
      }
    }
    if (!depths.empty())
      for (std::size_t i = 0; i < pixels; ++i)
        if (owner[i] >= 0)
          all.data[static_cast<std::size_t>(owner[i]) * pixels + i] = 1;
    if (dilation) {
      const auto kernelWork = static_cast<std::size_t>((2 * dilation + 1) * (2 * dilation + 1));
      limit(n == 0 || (pixels <= kWork / n && pixels * n <= kWork / kernelWork));
      const auto kernel =
          cv::getStructuringElement(cv::MORPH_ELLIPSE, {static_cast<int>(2 * dilation + 1),
                                                        static_cast<int>(2 * dilation + 1)});
      for (std::size_t j = 0; j < n; ++j) {
        poll(ctx);
        cv::Mat plane(c.height, c.width, CV_32F, all.data.data() + j * pixels);
        cv::dilate(plane, plane, kernel);
        for (std::size_t k = 0; k < pixels; ++k)
          if (!roiPixels[k])
            all.data[j * pixels + k] = 0;
      }
    }
    Json items = Json::array(), mi = Json::array(), quality = Json::array();
    Tensor result;
    result.shape = {0, c.height, c.width};
    std::vector<float> united;
    if (merge)
      united.assign(pixels, 0);
    Json members = Json::array();
    for (std::size_t j = 0; j < n; ++j) {
      poll(ctx);
      cv::Mat plane(c.height, c.width, CV_32F, all.data.data() + j * pixels);
      cv::Mat u8;
      plane.convertTo(u8, CV_8U);
      const auto box = cv::boundingRect(u8);
      const auto support = cv::countNonZero(u8);
      quality.push_back({{"id", r.data["items"][j]["id"]},
                         {"pixels", support},
                         {"status", support ? "visible"
                                            : (roiProvided && rois.data["items"].empty()
                                                   ? "empty_roi"
                                                   : "no_visible_support")}});
      if (!support)
        continue;
      if (merge) {
        for (std::size_t k = 0; k < pixels; ++k)
          united[k] = std::max(united[k], all.data[j * pixels + k]);
        members.push_back(original.data["items"][j]);
        continue;
      }
      const auto& source = original.data["items"][j];
      auto pose = poses::make2D(source["id"], frame, "px",
                                {box.x + (box.width - 1) / 2., box.y + (box.height - 1) / 2.}, 0,
                                {double(box.width), double(box.height)});
      for (const char* key : {"name", "classId", "classLabel", "confidence", "attributes"})
        if (source.contains(key))
          pose.data[key] = source[key];
      g::unsafe(pose.data);
      pose.data["attributes"]["sourcePose3D"] = source;
      pose.data["attributes"]["hasMask"] = true;
      pose.data["attributes"]["maskIndex"] = items.size();
      mi.push_back(infoItem(source["id"], items.size(), true, "primitive_projection"));
      items.push_back(std::move(pose.data));
      result.data.insert(result.data.end(), all.data.begin() + j * pixels,
                         all.data.begin() + (j + 1) * pixels);
    }
    if (merge && !members.empty()) {
      cv::Mat plane(c.height, c.width, CV_32F, united.data());
      cv::Mat u8;
      plane.convertTo(u8, CV_8U);
      auto box = cv::boundingRect(u8);
      auto pose = poses::make2D(p.text("unionId"), frame, "px",
                                {box.x + (box.width - 1) / 2., box.y + (box.height - 1) / 2.}, 0,
                                {double(box.width), double(box.height)});
      pose.data["attributes"] = {{"sourcePoses3D", members}, {"hasMask", true}, {"maskIndex", 0}};
      g::unsafe(pose.data);
      items.push_back(pose.data);
      mi.push_back(infoItem(pose.data["id"], 0, true, "primitive_projection_union"));
      result.data = std::move(united);
    }
    result.shape[0] = static_cast<std::int64_t>(items.size());
    auto output = poses::make2DList(items, frame, "px");
    auto info = maskInfo(frame, c.width, c.height, mi);
    Record q{"geometry.PrimitiveProjection.v1",
             {{"cameraFrame", c.frame},
              {"imageFrame", frame},
              {"width", c.width},
              {"height", c.height},
              {"scale", 1},
              {"surface", p.choice("surface")},
              {"items", quality}}};
    std::string why;
    require(poses::validate(output, why), why);
    limit(g::bounded(info) && g::bounded(q));
    poll(ctx);
    out.set("poses", Data::record(std::move(output)));
    out.set("masks", Data::tensor(std::move(result)));
    out.set("maskInfo", Data::record(std::move(info)));
    out.set("quality", Data::record(std::move(q)));
    return Status::Ok();
  } catch (const projection5::Cancelled&) {
    return g::cancelled();
  } catch (const Failure& e) {
    return e.status;
  } catch (const cv::Exception& e) {
    return g::bad(e.what(), "calib");
  }
}

struct Plane3 {
  g::V3 normal;
  double d;
};
g::V3 intersection(cv::Point2d ray, const Plane3& plane, double near) {
  g::V3 v{ray.x, ray.y, 1};
  double denominator = g::dot(plane.normal, v);
  require(std::isfinite(denominator) && std::abs(denominator) > 1e-12, "射线平行平面，深度未知");
  double z = -plane.d / denominator;
  require(std::isfinite(z) && z >= near, "平面交点不在有效正深度");
  auto result = g::mul(v, z);
  require(g::finite(result), "平面交点溢出");
  return result;
}
std::vector<Plane3> planes(const Inputs& in, const ParamView& p, const Record& poses,
                           const calibration::Camera& c) {
  std::vector<Plane3> result;
  std::string frame;
  const auto mode = p.choice("planeSource");
  if (mode == "workspace_top" || mode == "workspace_bottom") {
    require(!in.has("plane") && !in.has("planes"), "Workspace模式不消费输入plane/planes");
    auto workspace = read3(in, "workspace");
    require(workspace.data["items"].size() == 1, "需要单个中心锚定Workspace region", "workspace");
    frame = workspace.data["frame"];
    const auto& item = workspace.data["items"][0];
    auto normal = g::rotated(item["rotation"].get<poses::Quaternion>(), {0, 0, 1});
    auto point = g::add(
        g::jvector(item["position"]),
        g::mul(normal, item["size"][2].get<double>() * (mode == "workspace_top" ? .5 : -.5)));
    result.push_back({normal, -g::dot(normal, point)});
  } else if (mode == "input") {
    require(!in.has("workspace"), "输入平面模式不消费workspace");
    frame = p.text("planeFrame");
    const auto* native = in.has("plane") ? in.get("plane").asPlane() : nullptr;
    if (native) {
      require(!in.has("planes"), "不能同时连接plane与planes");
      g::V3 normal{native->normal[0], native->normal[1], native->normal[2]};
      double length = std::hypot(normal[0], normal[1], normal[2]);
      require(std::isfinite(length) && length > 0 && std::isfinite(native->d), "平面法向和d非法",
              "plane");
      result.push_back({g::mul(normal, 1 / length), native->d / length});
    } else {
      const auto* r = in.has("planes") ? in.get("planes").asRecord() : nullptr;
      require(r && r->type == "geometry.Planes3D.v1" && g::bounded(*r), "需要plane或Planes3D.v1",
              "planes");
      const auto& d = r->data;
      require(d.is_object() && d.size() == 3 && d.value("unit", Json{}) == "m" &&
                  d.value("frame", Json{}) == frame && d.contains("items") && d["items"].is_array(),
              "Plane列表帧/单位/结构非法", "planes");
      std::map<std::string, Plane3> byId;
      for (const auto& item : d["items"]) {
        require(item.is_object() && item.size() == 3 && item.contains("id") &&
                    item["id"].is_string() && item.contains("normal") &&
                    poses::numericArray(item["normal"], 3) && item.contains("d") &&
                    values::safeNumber(item["d"]),
                "Plane项非法", "planes");
        auto normal = g::jvector(item["normal"]);
        const double length = std::hypot(normal[0], normal[1], normal[2]);
        require(length > 0 && std::isfinite(length), "Plane法向非法", "planes");
        require(byId.emplace(item["id"].get<std::string>(),
                             Plane3{g::mul(normal, 1 / length), item["d"].get<double>() / length})
                    .second,
                "PlaneID重复", "planes");
      }
      if (byId.size() == 1 && byId.count("*"))
        result.push_back(byId.at("*"));
      else {
        require(byId.size() == poses.data["items"].size(), "Plane数量必须和Pose匹配或一个*广播",
                "planes");
        for (const auto& item : poses.data["items"]) {
          auto it = byId.find(item["id"].get<std::string>());
          require(it != byId.end(), "缺少PoseID对应平面", "planes");
          result.push_back(it->second);
        }
      }
    }
  } else
    throw Failure{g::badParam("未知平面来源", "planeSource")};
  auto m = toCamera(in, "planeToCamera", frame, c);
  poses::Quaternion rotation;
  std::string why;
  Transform native;
  for (int i = 0; i < 16; ++i)
    native.m[i] = static_cast<float>(m[i]);
  require(g::rigid(&native, m, rotation, why), why, "planeToCamera");
  for (auto& plane : result) {
    plane.normal = g::rotated(rotation, plane.normal);
    plane.d -= g::dot(plane.normal, {m[3], m[7], m[11]});
    require(std::isfinite(plane.d), "变换平面溢出");
  }
  if (result.size() == 1)
    result.resize(poses.data["items"].size(), result.front());
  return result;
}
std::vector<cv::Point2d> clippedBox(const std::array<double, 2>& position,
                                    const std::array<double, 2>& size, double angle,
                                    const calibration::Camera& c) {
  std::vector<cv::Point2d> poly;
  const double cs = std::cos(angle), sn = std::sin(angle);
  for (auto v :
       {cv::Point2d(-.5, -.5), cv::Point2d(.5, -.5), cv::Point2d(.5, .5), cv::Point2d(-.5, .5)})
    poly.emplace_back(position[0] + cs * v.x * size[0] - sn * v.y * size[1],
                      position[1] + sn * v.x * size[0] + cs * v.y * size[1]);
  for (int boundary = 0; boundary < 4; ++boundary) {
    if (poly.empty())
      break;
    const int axis = boundary / 2;
    const double limit = boundary % 2 ? (axis ? c.height - .5 : c.width - .5) : -.5;
    const auto inside = [&](cv::Point2d p) {
      double v = axis ? p.y : p.x;
      return boundary % 2 ? v <= limit : v >= limit;
    };
    std::vector<cv::Point2d> next;
    auto prev = poly.back();
    bool was = inside(prev);
    for (auto point : poly) {
      bool now = inside(point);
      if (now != was) {
        const double a = axis ? prev.y : prev.x, b = axis ? point.y : point.x,
                     t = (limit - a) / (b - a);
        next.push_back(prev + (point - prev) * t);
      }
      if (now)
        next.push_back(point);
      prev = point;
      was = now;
    }
    poly = std::move(next);
  }
  return poly;
}
Status lift(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    poll(ctx);
    const auto c = camera(in);
    projection5::RayBudget rayBudget;
    rayBudget.context = &ctx;
    const auto input = read2(in, "poses", p.text("imageFrame"));
    const auto ms = masks(in, input, c, ctx);
    auto ps = planes(in, p, input, c);
    const double near = p.number("nearDepth"), thickness = p.number("thickness");
    require(std::isfinite(near) && near > 0 && std::isfinite(thickness) && thickness > 0,
            "深度下限与厚度须正有限米");
    const bool dimensions = p.choice("mode") == "dimensions";
    require(dimensions || p.choice("mode") == "center", "未知反投影模式");
    if (dimensions && !ms.tensor)
      for (const auto& item : input.data["items"])
        if (item.contains("attributes") && item["attributes"].contains("hasMask"))
          require(item["attributes"]["hasMask"] == false,
                  "尺寸拟合实例声明真实掩膜但缺少原生masks及maskInfo", "masks");
    limit(input.data["items"].empty() ||
          static_cast<std::size_t>(c.width) * c.height <= kWork / input.data["items"].size());
    const auto supplied = g::vector(p, "size3D");
    for (double d : supplied)
      require(std::isfinite(d) && d > 0, "center模式已知物理尺寸须正有限");
    Json output = Json::array(), quality = Json::array();
    std::size_t work = 0;
    for (std::size_t i = 0; i < input.data["items"].size(); ++i) {
      poll(ctx);
      const auto& item = input.data["items"][i];
      const auto pos = item["position"].get<std::array<double, 2>>(),
                 sz = item["size"].get<std::array<double, 2>>();
      const double a = item["angle"].get<double>();
      for (double v : pos)
        require(g::validNumber(v), "二维位置超出计算范围");
      for (double v : sz)
        require(g::validNumber(v), "二维尺寸超出计算范围");
      require(dimensions || (pos[0] >= -.5 && pos[0] <= c.width - .5 && pos[1] >= -.5 &&
                             pos[1] <= c.height - .5),
              "center反投影中心位于传感器范围外");
      auto pp = normalized({{pos[0], pos[1]}, {pos[0] + std::cos(a), pos[1] + std::sin(a)}}, c,
                           rayBudget);
      auto center = intersection(pp[0], ps[i], near),
           x = g::sub(intersection(pp[1], ps[i], near), center);
      require(g::normalize(x), "平面轴方向退化");
      auto z = ps[i].normal;
      if (p.flag("flipZ"))
        z = g::mul(z, -1);
      auto y = g::cross(z, x);
      require(g::normalize(y), "无法建立平面正交轴");
      x = g::cross(y, z);
      g::V3 size = supplied;
      std::size_t support = 0;
      double maxPlaneError = 0;
      if (dimensions) {
        std::vector<cv::Point2d> pixels;
        if (ms.tensor) {
          const auto& mi = ms.info->data["items"][i];
          require(mi["hasMask"] == true, "尺寸拟合需要真实非零mask支持", "masks");
          const auto* data = ms.tensor->data.data() + i * ms.pixels;
          for (int row = 0; row < c.height; ++row) {
            poll(ctx);
            for (int col = 0; col < c.width; ++col) {
              const auto k = static_cast<std::size_t>(row) * c.width + col;
              if (data[k] != 1)
                continue;
              ++support;
              if (row > 0 && row + 1 < c.height && col > 0 && col + 1 < c.width && data[k - 1] &&
                  data[k + 1] && data[k - c.width] && data[k + c.width])
                continue;
              limit(pixels.size() + 4 <= 2U * 1024U * 1024U);
              for (auto offset : {cv::Point2d(-.5, -.5), cv::Point2d(.5, -.5), cv::Point2d(.5, .5),
                                  cv::Point2d(-.5, .5)})
                pixels.emplace_back(col + offset.x, row + offset.y);
            }
          }
        } else {
          const auto polygon = clippedBox(pos, sz, a, c);
          for (std::size_t edge = 0; edge < polygon.size(); ++edge) {
            auto from = polygon[edge], to = polygon[(edge + 1) % polygon.size()];
            const int steps = std::max(1, static_cast<int>(std::ceil(cv::norm(to - from))));
            for (int k = 0; k <= steps; ++k)
              pixels.push_back(from + (to - from) * (double(k) / steps));
          }
          support = pixels.size();
        }
        require(!pixels.empty(), "没有可用于尺寸拟合的传感器内支持");
        limit(pixels.size() <= kWork - work);
        work += pixels.size();
        auto rays = normalized(pixels, c, rayBudget);
        std::vector<g::V3> planePoints;
        planePoints.reserve(rays.size());
        double extent = 0;
        for (auto ray : rays) {
          auto point = intersection(ray, ps[i], near);
          auto relative = g::sub(point, center);
          extent = std::max({extent, std::abs(g::dot(relative, x)), std::abs(g::dot(relative, y))});
          planePoints.push_back(point);
          maxPlaneError = std::max(maxPlaneError, std::abs(g::dot(ps[i].normal, point) + ps[i].d));
        }
        require(std::isfinite(extent) && extent > 0, "平面支持范围退化");
        std::vector<cv::Point2f> local;
        local.reserve(planePoints.size());
        for (auto point : planePoints) {
          auto relative = g::sub(point, center);
          local.emplace_back(static_cast<float>(g::dot(relative, x) / extent),
                             static_cast<float>(g::dot(relative, y) / extent));
        }
        const auto box = cv::minAreaRect(local);
        const double theta = box.angle * poses::kPi / 180;
        auto nx = g::add(g::mul(x, std::cos(theta)), g::mul(y, std::sin(theta))),
             ny = g::cross(z, nx);
        center = g::add(center,
                        g::add(g::mul(x, box.center.x * extent), g::mul(y, box.center.y * extent)));
        x = nx;
        y = ny;
        size = {box.size.width * extent, box.size.height * extent, thickness};
        for (auto point : planePoints) {
          auto d = g::sub(point, center);
          for (int axis = 0; axis < 2; ++axis) {
            const auto v = axis == 0 ? x : y;
            size[axis] = std::max(size[axis], 2 * std::abs(g::dot(d, v)));
          }
        }
      }
      poses::Matrix3 rotation{x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]};
      auto pose = poses::make3D(item["id"], c.frame, center, poses::quaternion(rotation), size);
      for (const char* key : {"name", "classId", "classLabel", "confidence", "attributes"})
        if (item.contains(key))
          pose.data[key] = item[key];
      g::unsafe(pose.data);
      pose.data["attributes"]["sourcePose2D"] = item;
      pose.data["attributes"]["shape"] = "box";
      pose.data["attributes"]["normal"] = z;
      for (const char* key : {"circleRadius", "radius", "maskIndex", "hasMask"})
        pose.data["attributes"].erase(key);
      output.push_back(pose.data);
      quality.push_back({{"id", item["id"]},
                         {"supportCount", support},
                         {"planeError", maxPlaneError},
                         {"status", "measured"}});
    }
    auto result = poses::make3DList(output, c.frame);
    std::string why;
    require(poses::validate(result, why), why);
    Record q{"geometry.PlaneLift3D.v1",
             {{"imageFrame", p.text("imageFrame")},
              {"cameraFrame", c.frame},
              {"unit", "m"},
              {"items", quality}}};
    limit(g::bounded(q));
    poll(ctx);
    out.set("poses", Data::record(std::move(result)));
    out.set("quality", Data::record(std::move(q)));
    return Status::Ok();
  } catch (const projection5::Cancelled&) {
    return g::cancelled();
  } catch (const Failure& e) {
    return e.status;
  } catch (const cv::Exception& e) {
    return g::bad(e.what(), "calib");
  }
}

Status removeMask(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    poll(ctx);
    const auto c = camera(in);
    const auto items = read2(in, "detections", p.text("imageFrame"));
    const auto ms = masks(in, items, c, ctx);
    require(ms.tensor, "需要真实Masks2D绑定", "masks");
    const auto* cloud = in.get("cloud").asCloud();
    require(cloud && cloud->channelsConsistent(), "点云及可选通道不合法", "cloud");
    limit(cloud->pointCount() <= 4U * 1024U * 1024U);
    auto prims = read3(in, "poses");
    auto m = toCamera(in, "cloudToCamera", p.text("cloudFrame"), c);
    std::map<std::string, std::size_t> masksById;
    for (std::size_t i = 0; i < items.data["items"].size(); ++i)
      masksById.emplace(items.data["items"][i]["id"].get<std::string>(), i);
    std::set<std::string> selected;
    for (const auto& item : prims.data["items"]) {
      require(item.contains("attributes") && item["attributes"].contains("primitive2DId") &&
                  item["attributes"]["primitive2DId"].is_string(),
              "每个3D实例须显式primitive2DId关联");
      auto id = item["attributes"]["primitive2DId"].get<std::string>();
      require(masksById.count(id) && selected.insert(id).second, "关联mask不存在或重复");
    }
    const auto expand = p.integer("topExpand");
    require(expand >= 0 && expand <= c.height, "顶部扩展须0..imageHeight");
    limit(selected.empty() || ms.pixels <= kWork / selected.size());
    std::vector<unsigned char> unionMask(ms.pixels, 0);
    for (const auto& id : selected) {
      poll(ctx);
      auto j = masksById[id];
      require(ms.info->data["items"][j]["hasMask"] == true, "关联实例没有真实mask");
      const float* data = ms.tensor->data.data() + j * ms.pixels;
      for (int x = 0; x < c.width; ++x) {
        int first = c.height;
        for (int y = 0; y < c.height; ++y) {
          const auto k = static_cast<std::size_t>(y) * c.width + x;
          if (data[k]) {
            unionMask[k] = 1;
            first = std::min(first, y);
          }
        }
        for (int y = std::max(0, first - static_cast<int>(expand)); y < first && first < c.height;
             ++y)
          unionMask[static_cast<std::size_t>(y) * c.width + x] = 1;
      }
    }
    std::vector<std::int32_t> keep, removed, unknown;
    keep.reserve(cloud->pointCount());
    std::size_t outOfBounds = 0;
    for (std::size_t start = 0; start < cloud->pointCount(); start += 4096) {
      poll(ctx);
      const auto end = std::min(cloud->pointCount(), start + 4096);
      std::vector<cv::Point3d> points;
      std::vector<std::size_t> indices;
      for (std::size_t i = start; i < end; ++i) {
        g::V3 source{cloud->xyz[3 * i], cloud->xyz[3 * i + 1], cloud->xyz[3 * i + 2]};
        if (!g::finite(source)) {
          keep.push_back(static_cast<std::int32_t>(i));
          unknown.push_back(static_cast<std::int32_t>(i));
          continue;
        }
        auto point = lyflow::packs::std_geometry::pose_detail::apply(m, source);
        if (!g::finite(point) || point[2] <= 0) {
          keep.push_back(static_cast<std::int32_t>(i));
          unknown.push_back(static_cast<std::int32_t>(i));
          continue;
        }
        points.emplace_back(point[0], point[1], point[2]);
        indices.push_back(i);
      }
      std::vector<cv::Point2d> uv;
      if (!points.empty())
        cv::projectPoints(points, cv::Vec3d::all(0), cv::Vec3d::all(0), image_p1::cameraMatrix(c),
                          image_p1::distortion(c), uv);
      for (std::size_t j = 0; j < uv.size(); ++j) {
        auto point = uv[j];
        const auto i = static_cast<std::int32_t>(indices[j]);
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < -.5 || point.y < -.5 ||
            point.x >= c.width - .5 || point.y >= c.height - .5) {
          keep.push_back(i);
          ++outOfBounds;
          continue;
        }
        const auto x = static_cast<int>(std::floor(point.x + .5)),
                   y = static_cast<int>(std::floor(point.y + .5));
        (unionMask[static_cast<std::size_t>(y) * c.width + x] ? removed : keep).push_back(i);
      }
    }
    std::sort(keep.begin(), keep.end());
    std::sort(removed.begin(), removed.end());
    std::sort(unknown.begin(), unknown.end());
    auto result = cloud->select(keep);
    Record q{"cloud.MaskPrimitiveRemoval.v1",
             {{"cloudFrame", p.text("cloudFrame")},
              {"imageFrame", p.text("imageFrame")},
              {"cameraFrame", c.frame},
              {"sourceCloudId", std::to_string(cloud->id)},
              {"removed", removed.size()},
              {"unknown", unknown.size()},
              {"outOfBoundsKept", outOfBounds},
              {"topExpand", expand}}};
    poll(ctx);
    out.set("cloud", Data::cloud(std::move(result)));
    out.set("keptIndices", Data::indices({cloud->id, std::move(keep)}));
    out.set("removedIndices", Data::indices({cloud->id, std::move(removed)}));
    out.set("unknownIndices", Data::indices({cloud->id, std::move(unknown)}));
    out.set("quality", Data::record(std::move(q)));
    return Status::Ok();
  } catch (const projection5::Cancelled&) {
    return g::cancelled();
  } catch (const Failure& e) {
    return e.status;
  } catch (const cv::Exception& e) {
    return g::bad(e.what(), "calib");
  }
}
}

void registerP5PoseProjection(Registry& r) {
  auto op = g::descriptor("pose.project_3d", "三维有限工件原图掩膜投影",
                          "3D cuboid ray silhouette mask projection");
  op.inputs = {g::record("poses"), g::record("calib"), g::port("posesToCamera", "Transform", false),
               g::record("rois", false)};
  op.outputs = {g::record("poses"), g::port("masks", "Tensor"), g::record("maskInfo"),
                g::record("quality")};
  op.params = {g::text("imageFrame", "二维像素参考帧", "image"),
               g::choice("mode", "结果组合", "individual", {"individual", "union"}),
               g::choice("surface", "投影表面", "full", {"full", "top"}),
               g::choice("anchor", "输入Pose锚点", "center", {"center", "top"}),
               g::choice("occlusion", "遮挡", "independent", {"independent", "nearest"}),
               g::number("nearDepth", "正深度下限", 1e-6, "m"),
               g::integer("dilate", "全图像素膨胀半径", 0),
               g::text("unionId", "合并ID", "union")};
  op.compute = g::atomic(&project);
  r.addOperator(std::move(op));
  op = g::descriptor("pose.project_to_plane", "二维实例任意平面反投影及实体尺寸",
                     "ray plane dimensions workspace top bottom");
  op.inputs = {g::record("poses"),
               g::record("calib"),
               g::port("plane", "Plane", false),
               g::record("planes", false),
               g::record("workspace", false),
               g::port("planeToCamera", "Transform", false),
               g::port("masks", "Tensor", false),
               g::record("maskInfo", false)};
  op.outputs = {g::record("poses"), g::record("quality")};
  op.params = {
      g::text("imageFrame", "二维像素参考帧", "image"),
      g::text("planeFrame", "输入平面三维帧", "camera"),
      g::choice("planeSource", "平面来源", "input", {"input", "workspace_top", "workspace_bottom"}),
      g::choice("mode", "物理尺寸来源", "center", {"center", "dimensions"}),
      g::vec("size3D", "center模式已知尺寸米", {.1, .1, .01}),
      g::number("thickness", "尺寸拟合已知厚度", .01, "m"),
      g::number("nearDepth", "正深度下限", 1e-6, "m"),
      g::flag("flipZ", "反向平面法向", false)};
  op.compute = g::atomic(&lift);
  r.addOperator(std::move(op));
  op = g::descriptor("cloud.remove_mask_primitives", "按三维工件关联像素掩膜移除云",
                     "mask primitive cloud remove top expand");
  op.inputs = {g::port("cloud", "PointCloud"),
               g::record("poses"),
               g::record("detections"),
               g::record("calib"),
               g::port("masks", "Tensor"),
               g::record("maskInfo"),
               g::port("cloudToCamera", "Transform", false)};
  op.outputs = {g::port("cloud", "PointCloud"), g::port("keptIndices", "Indices"),
                g::port("removedIndices", "Indices"), g::port("unknownIndices", "Indices"),
                g::record("quality")};
  op.params = {g::text("imageFrame", "二维像素参考帧", "image"),
               g::text("cloudFrame", "云三维米制帧", "camera"),
               g::integer("topExpand", "掩膜向上扩展全图像素", 0)};
  op.compute = g::atomic(&removeMask);
  r.addOperator(std::move(op));
}
}
