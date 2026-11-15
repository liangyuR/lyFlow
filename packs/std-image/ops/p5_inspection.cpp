#include <map>
#include <numeric>

#include "p5_inspection_common.h"

namespace lyflow::ops {
namespace {
using namespace inspection5;
struct Ellipse {
  cv::RotatedRect raw;
  std::vector<cv::Point2f> points;
  double score = 0;
};
cv::Matx33d conic(const cv::RotatedRect& r) {
  const double a = r.size.width / 2, b = r.size.height / 2;
  const double angle = r.angle * poses::kPi / 180, c = std::cos(angle), s = std::sin(angle);
  const double u = c * c / (a * a) + s * s / (b * b), v = c * s * (1 / (a * a) - 1 / (b * b)),
               w = s * s / (a * a) + c * c / (b * b);
  const double x = r.center.x, y = r.center.y;
  return {u,
          v,
          -u * x - v * y,
          v,
          w,
          -v * x - w * y,
          -u * x - v * y,
          -v * x - w * y,
          u * x * x + 2 * v * x * y + w * y * y - 1};
}
double ellipseScore(const cv::RotatedRect& ellipse, const std::vector<cv::Point2f>& points) {
  if (!(ellipse.size.width >= 2 && ellipse.size.height >= 2) || !std::isfinite(ellipse.center.x) ||
      !std::isfinite(ellipse.center.y) || !std::isfinite(ellipse.size.width) ||
      !std::isfinite(ellipse.size.height))
    return 0;
  std::array<bool, 72> occupied{};
  const double angle = ellipse.angle * poses::kPi / 180, c = std::cos(angle), s = std::sin(angle);
  std::size_t support = 0;
  for (std::size_t index = 0; index < points.size(); ++index) {
    const auto& point = points[index];
    const double x = point.x - ellipse.center.x, y = point.y - ellipse.center.y,
                 u = (c * x + s * y) / (ellipse.size.width / 2),
                 v = (-s * x + c * y) / (ellipse.size.height / 2);
    if (std::abs(std::hypot(u, v) - 1) > .08)
      continue;
    if (index >= 3 && index + 3 < points.size()) {
      const auto before = point - points[index - 3], after = points[index + 3] - point;
      if (cv::norm(before) <= 8 && cv::norm(after) <= 8) {
        const auto tangent = points[index + 3] - points[index - 3];
        const double tu = c * tangent.x + s * tangent.y, tv = -s * tangent.x + c * tangent.y,
                     gu = u / (ellipse.size.width / 2), gv = v / (ellipse.size.height / 2),
                     denominator = std::hypot(tu, tv) * std::hypot(gu, gv);
        if (denominator > 1e-12 &&
            std::abs(tu * gu + tv * gv) > std::sin(20 * poses::kPi / 180) * denominator)
          continue;
      }
    }
    const int bin = std::clamp(
        static_cast<int>((std::atan2(v, u) + poses::kPi) * 72 / (2 * poses::kPi)), 0, 71);
    occupied[bin] = true;
    ++support;
  }
  return std::count(occupied.begin(), occupied.end(), true) / 72. * support /
         std::max<std::size_t>(1, points.size());
}
Status circles(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    const auto* image = in.get("image").asImage();
    auto valid = image_p1::image(image, "image", false, 2097152);
    if (!valid.ok)
      return valid;
    need(image->scale == 1, "椭圆空间恢复需要全分辨率图像");
    need(!p.text("imageFrame").empty() && p.text("imageFrame").size() <= 256, "图像参考帧非法");
    calibration::Camera camera;
    valid = image_p1::camera(in, *image, camera);
    if (!valid.ok)
      return valid;
    const double theta = scalar(p, "thetaFsa", 0, 90), ratio = scalar(p, "lengthFsa", 1.01, 100),
                 minimum = scalar(p, "minScore", 0, 1),
                 eccentricity = scalar(p, "circleThreshold", 0, 1),
                 defaultRadius = scalar(p, "radius", 1e-9, 1000),
                 thickness = scalar(p, "thickness", 1e-9, 1000),
                 edgeTolerance = scalar(p, "edgeTolerance", .1, 16);
    const auto viz = p.choice("visualization");
    need(viz == "circles" || viz == "canny", "可视化模式非法");
    cv::Mat gray;
    const double scale = image->depth == PixelDepth::U8    ? 1
                         : image->depth == PixelDepth::U16 ? 255. / 65535
                                                           : 255.;
    if (image->depth == PixelDepth::F32) {
      double lo, hi;
      cv::minMaxLoc(cvx::view(*image).reshape(1), &lo, &hi);
      need(lo >= 0 && hi <= 1, "f32图像需要0..1");
    }
    image_p1::gray(*image).convertTo(gray, CV_8U, scale);
    int low = integer(p, "lowThreshold", 0, 255), high = integer(p, "highThreshold", 0, 255);
    if (p.flag("autoCanny")) {
      std::array<std::size_t, 256> hist{};
      for (int y = 0; y < gray.rows; ++y) {
        poll(ctx);
        for (int x = 0; x < gray.cols; ++x)
          ++hist[gray.at<unsigned char>(y, x)];
      }
      std::size_t sum = 0;
      int median = 0;
      for (; median < 255 && sum + hist[median] < image->pixelCount() / 2; ++median)
        sum += hist[median];
      low = std::max(1, static_cast<int>(.66 * median));
      high = std::max(low + 1, std::min(255, static_cast<int>(1.33 * median)));
    }
    need(low < high, "Canny低阈值必须小于高阈值");
    poll(ctx);
    cv::Mat edge;
    cv::Canny(gray, edge, low, high);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(edge.clone(), contours, cv::RETR_LIST, cv::CHAIN_APPROX_NONE);
    if (contours.size() > 4096)
      return Status::Error(Phase::Execute, "resource_limit", "椭圆轮廓超过4096候选");
    std::uint64_t work = image->pixelCount() * 16;
    std::vector<Ellipse> candidates;
    for (const auto& contour : contours) {
      charge(work, contour.size() * 16, ctx);
      if (contour.size() < 16)
        continue;
      if (contour.size() > 262144)
        return Status::Error(Phase::Execute, "resource_limit", "单轮廓超过262144点");
      std::vector<cv::Point2f> arc;
      auto fit = [&]() {
        if (arc.size() < 12)
          return;
        const auto ellipse = cv::fitEllipse(arc);
        if (ellipse.size.width > image->width * 4. || ellipse.size.height > image->height * 4.)
          return;
        const double score = ellipseScore(ellipse, arc);
        if (score >= .08)
          candidates.push_back({ellipse, arc, score});
      };
      for (std::size_t j = 0; j < contour.size(); ++j) {
        if (j >= 3 && j + 3 < contour.size()) {
          cv::Point2d a = contour[j] - contour[j - 3], b = contour[j + 3] - contour[j];
          const double an = cv::norm(a), bn = cv::norm(b);
          if (std::max(an, bn) > ratio * std::min(an, bn) ||
              a.dot(b) < std::cos(theta * poses::kPi / 180) * an * bn - 1e-9) {
            fit();
            arc.clear();
          }
        }
        arc.emplace_back(contour[j]);
      }
      fit();
      if (candidates.size() > 4096)
        return Status::Error(Phase::Execute, "resource_limit", "椭圆拟合候选超过4096");
    }
    const std::size_t seedCount = candidates.size();
    std::vector<Ellipse> pairSeeds;
    for (std::size_t a = 0; a < seedCount; ++a)
      for (std::size_t b = a + 1; b < seedCount; ++b) {
        if (candidates[a].score >= minimum || candidates[b].score >= minimum)
          continue;
        charge(work, (candidates[a].points.size() + candidates[b].points.size()) * 32, ctx);
        auto support = candidates[a].points;
        support.insert(support.end(), candidates[b].points.begin(), candidates[b].points.end());
        const auto fit = cv::fitEllipse(support);
        const double score = ellipseScore(fit, support);
        if (score >= .08 && fit.size.width <= image->width * 4. &&
            fit.size.height <= image->height * 4.)
          pairSeeds.push_back({fit, std::move(support), score});
        if (pairSeeds.size() + seedCount > 4096)
          return Status::Error(Phase::Execute, "resource_limit", "弧对假设超过4096预算");
      }
    std::vector<Ellipse> seeds = candidates;
    seeds.insert(seeds.end(), std::make_move_iterator(pairSeeds.begin()),
                 std::make_move_iterator(pairSeeds.end()));
    std::vector<Ellipse> grouped;
    for (const auto& seed : seeds) {
      std::vector<cv::Point2f> support;
      const auto q = conic(seed.raw);
      for (const auto& arc : candidates) {
        charge(work, arc.points.size() * 16, ctx);
        std::size_t compatible = 0;
        for (const auto& point : arc.points) {
          const cv::Vec3d h(point.x, point.y, 1);
          const double level = 1 + h.dot(q * h);
          if (level >= 0 && std::abs(std::sqrt(level) - 1) *
                                    std::min(seed.raw.size.width, seed.raw.size.height) / 2 <=
                                edgeTolerance)
            ++compatible;
        }
        if (compatible * 5 >= arc.points.size() * 4)
          support.insert(support.end(), arc.points.begin(), arc.points.end());
      }
      if (support.size() < 12)
        continue;
      charge(work, support.size() * 16, ctx);
      const auto fitted = cv::fitEllipse(support);
      const double score = ellipseScore(fitted, support);
      if (score > 0 && score >= minimum && fitted.size.width <= image->width * 4. &&
          fitted.size.height <= image->height * 4.)
        grouped.push_back({fitted, std::move(support), score});
    }
    candidates = std::move(grouped);
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
      if (a.score != b.score)
        return a.score > b.score;
      return std::tie(a.raw.center.y, a.raw.center.x) < std::tie(b.raw.center.y, b.raw.center.x);
    });
    std::vector<Ellipse> found;
    for (auto& candidate : candidates) {
      charge(work, found.size() + 1, ctx);
      bool duplicate = false;
      for (const auto& previous : found) {
        const double large = std::max(candidate.raw.size.width, candidate.raw.size.height),
                     small = std::min(candidate.raw.size.width, candidate.raw.size.height),
                     previousLarge = std::max(previous.raw.size.width, previous.raw.size.height),
                     previousSmall = std::min(previous.raw.size.width, previous.raw.size.height);
        if (cv::norm(candidate.raw.center - previous.raw.center) <= edgeTolerance &&
            std::abs(large - previousLarge) <= edgeTolerance &&
            std::abs(small - previousSmall) <= edgeTolerance)
          duplicate = true;
      }
      if (!duplicate)
        found.push_back(std::move(candidate));
    }
    if (found.size() > static_cast<std::size_t>(integer(p, "maxCircles", 1, 128)))
      return Status::Error(Phase::Execute, "resource_limit", "空间圆数量超过预算，不截断");
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
      return std::tie(a.raw.center.y, a.raw.center.x) < std::tie(b.raw.center.y, b.raw.center.x);
    });
    const auto* radii = in.has("radii") ? in.get("radii").asTensor() : nullptr;
    if (in.has("radii")) {
      need(radii && radii->consistent() && radii->shape.size() == 1 &&
               (radii->data.size() == 1 || radii->data.size() == found.size()),
           "半径Tensor需要单值广播或按稳定椭圆顺序一一对应", "radii");
      for (float radius : radii->data)
        need(std::isfinite(radius) && radius > 0 && radius <= 1000, "半径需要有限正米制", "radii");
    }
    Json ellipses = Json::array(), two = Json::array(), three = Json::array(),
         instances = Json::array();
    cv::Mat overlay = cvx::view(*image).clone();
    projection5::RayBudget rays;
    rays.context = &ctx;
    for (std::size_t index = 0; index < found.size(); ++index) {
      auto& detected = found[index];
      charge(work, detected.points.size() * 64, ctx);
      const auto& fit = detected.raw;
      const std::string id = "ellipse-" + std::to_string(index);
      const double major = std::max(fit.size.width, fit.size.height) / 2,
                   minor = std::min(fit.size.width, fit.size.height) / 2,
                   ecc = std::sqrt(std::max(0., 1 - minor * minor / (major * major))),
                   radius =
                       radii ? radii->data[radii->data.size() == 1 ? 0 : index] : defaultRadius;
      ellipses.push_back({{"id", id},
                          {"center", {fit.center.x, fit.center.y}},
                          {"diameters", {fit.size.width, fit.size.height}},
                          {"angle", fit.angle * poses::kPi / 180},
                          {"score", detected.score},
                          {"eccentricity", ecc}});
      if (ecc < eccentricity)
        two.push_back({{"id", id},
                       {"center", {fit.center.x, fit.center.y}},
                       {"radius", (major + minor) / 2},
                       {"eccentricity", ecc}});
      std::vector<cv::Point2f> normalized;
      const auto evaluations = rays.evaluations;
      for (const auto& point : detected.points) {
        cv::Vec3d ray;
        rayResult(projection5::normalizedRay(camera, point.x, point.y, ray, &rays), rays,
                  "空间圆无法可靠反畸变到原图观测", "calib");
        normalized.emplace_back(static_cast<float>(ray[0]), static_cast<float>(ray[1]));
      }
      charge(work, rays.evaluations - evaluations, ctx);
      const auto normalizedFit = cv::fitEllipse(normalized);
      const auto q = conic(normalizedFit);
      cv::Mat eigenvalues, vectors;
      need(cv::eigen(cv::Mat(q), eigenvalues, vectors), "圆锥分解失败");
      const double l1 = eigenvalues.at<double>(0), l2 = eigenvalues.at<double>(1),
                   l3 = eigenvalues.at<double>(2);
      need(std::isfinite(l1) && std::isfinite(l2) && std::isfinite(l3) && l1 >= l2 && l2 > 0 &&
               l3 < 0,
           "椭圆未定义有限可见圆锥");
      const cv::Vec3d e1(vectors.at<double>(0, 0), vectors.at<double>(0, 1),
                         vectors.at<double>(0, 2)),
          e3(vectors.at<double>(2, 0), vectors.at<double>(2, 1), vectors.at<double>(2, 2));
      const double a = std::sqrt(std::max(0., (l1 - l2) / (l1 - l3))),
                   b = std::sqrt((l2 - l3) / (l1 - l3));
      const bool ambiguous = a > 1e-5;
      for (int sign : {-1, 1}) {
        if (sign == 1 && !ambiguous)
          continue;
        cv::Vec3d normal = sign * a * e1 + b * e3;
        if (normal[2] < 0)
          normal = -normal;
        const cv::Vec3d direction = q.inv() * normal;
        const double coefficient = normal.dot(direction),
                     square = -l2 * radius * radius / coefficient;
        need(std::isfinite(square) && square > 0, "空间圆尺度退化");
        cv::Vec3d center = std::sqrt(square) * direction;
        if (center[2] < 0)
          center = -center;
        double sum = 0, maximum = 0;
        for (const auto& point : normalized) {
          const cv::Vec3d ray(point.x, point.y, 1);
          const double denominator = ray.dot(normal);
          need(std::abs(denominator) > 1e-12, "圆平面与视线相切");
          const auto onCircle = ray * (normal.dot(center) / denominator);
          const double residual = std::abs(cv::norm(onCircle - center) - radius);
          sum += residual * residual;
          maximum = std::max(maximum, residual);
        }
        const double rms = std::sqrt(sum / normalized.size());
        need(std::isfinite(rms) && std::isfinite(center[2]) && center[2] > 0 &&
                 maximum <= radius * .15,
             "空间圆径向误差超限");
        const std::string solutionId = id + (sign < 0 ? ":0" : ":1");
        auto pose = poses::make3D(solutionId, camera.frame, vec(center), rotation(normal),
                                  {2 * radius, 2 * radius, thickness});
        pose.data["pickable"] = false;
        pose.data["confidence"] = detected.score;
        pose.data["attributes"] = {{"ellipseId", id},        {"ambiguous", ambiguous},
                                   {"shape", "circle"},      {"radius", radius},
                                   {"circleRadius", radius}, {"normal", vec(normal)},
                                   {"radialRms", rms}};
        instances.push_back(std::move(pose.data));
        three.push_back({{"id", solutionId},
                         {"ellipseId", id},
                         {"center", vec(center)},
                         {"normal", vec(normal)},
                         {"radius", radius},
                         {"ambiguous", ambiguous},
                         {"radialRms", rms},
                         {"maxRadialError", maximum}});
      }
      const double maximum = cvx::fullScale(overlay.depth());
      cv::ellipse(overlay, fit,
                  overlay.channels() == 1 ? cv::Scalar(maximum * .5) : cv::Scalar(0, maximum, 0, 0),
                  1, cv::LINE_AA);
    }
    if (image->channels == 4) {
      cv::Mat alpha;
      cv::extractChannel(cvx::view(*image), alpha, 3);
      cv::insertChannel(alpha, overlay, 3);
    }
    Record ellipseRecord{"image.Ellipses.v1",
                         {{"frame", p.text("imageFrame")}, {"unit", "px"}, {"items", ellipses}}},
        twoRecord{"image.Circles2D.v1",
                  {{"frame", p.text("imageFrame")}, {"unit", "px"}, {"items", two}}},
        threeRecord{"geometry.Circles3D.v1",
                    {{"frame", camera.frame}, {"unit", "m"}, {"items", three}}},
        poseRecord = poses::make3DList(instances, camera.frame);
    for (const auto* record : {&ellipseRecord, &twoRecord, &threeRecord, &poseRecord})
      bounded(*record);
    std::string why;
    need(poses::validate(poseRecord, why), "空间圆位姿非法：" + why);
    auto display = native(viz == "canny" ? edge.clone() : overlay);
    auto edges = native(edge);
    poll(ctx);
    out.set("instances", Data::record(std::move(poseRecord)));
    out.set("circles", Data::record(std::move(threeRecord)));
    out.set("ellipses", Data::record(std::move(ellipseRecord)));
    out.set("circles2d", Data::record(std::move(twoRecord)));
    out.set("image", Data::image(std::move(display)));
    out.set("edges", Data::image(std::move(edges)));
    return Status::Ok();
  } catch (const projection5::Cancelled&) {
    return Status::Error(Phase::Execute, "cancelled", "任务已取消");
  } catch (const Failure& e) {
    return e.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "空间圆检测内存不足");
  } catch (const std::exception& e) {
    return caught(e);
  }
}
Status fillHoles(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    Depth view(in, ctx);
    const int closing = integer(p, "closeKernel", 1, 63), window = integer(p, "fillKernel", 1, 63),
              minimum = integer(p, "minNeighbors", 1, 3969);
    need(closing % 2 == 1 && window % 2 == 1, "形态学和补值窗口必须为奇数");
    const double spread = scalar(p, "maxDepthSpread", 0, 1e6);
    std::uint64_t work = 0;
    charge(work, view.depth->pixelCount() * (8ULL + 4 * closing), ctx);
    cv::Mat observed(view.grid.height, view.grid.width, CV_8U, cv::Scalar(0));
    for (int y = 0; y < observed.rows; ++y) {
      poll(ctx);
      for (int x = 0; x < observed.cols; ++x)
        if (view.status(x, y) & projection5::kObserved)
          observed.at<unsigned char>(y, x) = 255;
    }
    cv::Mat closed;
    cv::morphologyEx(observed, closed, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, {closing, closing}));
    cv::Mat candidates = closed & (cvx::view(*view.state) == 0);
    if (!p.flag("fillBoundary")) {
      cv::Mat exterior = (cvx::view(*view.state) == 0);
      for (int x = 0; x < exterior.cols; ++x) {
        if (exterior.at<unsigned char>(0, x) == 255)
          cv::floodFill(exterior, {x, 0}, cv::Scalar(128));
        if (exterior.at<unsigned char>(exterior.rows - 1, x) == 255)
          cv::floodFill(exterior, {x, exterior.rows - 1}, cv::Scalar(128));
      }
      for (int y = 0; y < exterior.rows; ++y) {
        if (exterior.at<unsigned char>(y, 0) == 255)
          cv::floodFill(exterior, {0, y}, cv::Scalar(128));
        if (exterior.at<unsigned char>(y, exterior.cols - 1) == 255)
          cv::floodFill(exterior, {exterior.cols - 1, y}, cv::Scalar(128));
      }
      candidates.setTo(0, exterior == 128);
    }
    PointCloud cloud;
    cloud.xyz = view.cloud->xyz;
    cloud.normals = view.cloud->normals;
    cloud.intensity = view.cloud->intensity;
    cloud.rgb = view.cloud->rgb;
    Tensor indices = *view.indices;
    cv::Mat depth = cvx::view(*view.depth).clone(), status = cvx::view(*view.state).clone(),
            filled = cv::Mat::zeros(depth.size(), CV_8U);
    Indices filledIndices;
    filledIndices.sourceCloudId = cloud.id;
    const int candidateCount = cv::countNonZero(candidates);
    projection5::RayBudget rays;
    rays.context = &ctx;
    for (int y = 0; y < depth.rows; ++y) {
      poll(ctx);
      for (int x = 0; x < depth.cols; ++x) {
        if (!candidates.at<unsigned char>(y, x))
          continue;
        charge(work, static_cast<std::uint64_t>(window) * window, ctx);
        std::vector<std::pair<float, int>> neighbors;
        for (int yy = std::max(0, y - window / 2); yy <= std::min(depth.rows - 1, y + window / 2);
             ++yy)
          for (int xx = std::max(0, x - window / 2); xx <= std::min(depth.cols - 1, x + window / 2);
               ++xx)
            if (view.status(xx, yy) & projection5::kObserved)
              neighbors.emplace_back(view.z(xx, yy), view.index(xx, yy));
        if (neighbors.size() < static_cast<std::size_t>(minimum))
          continue;
        std::sort(neighbors.begin(), neighbors.end());
        if (static_cast<double>(neighbors.back().first) - neighbors.front().first > spread)
          continue;
        const auto middle = neighbors[neighbors.size() / 2];
        cv::Vec3d point;
        const auto evaluations = rays.evaluations;
        rayResult(projection5::backProject(view.grid, x, y, middle.first, point, &rays), rays,
                  "补值反投影无法满足正投影误差");
        charge(work, rays.evaluations - evaluations, ctx);
        if (cloud.pointCount() >= 2097152)
          return Status::Error(Phase::Execute, "resource_limit", "填补后点云超过2Mi点");
        const auto index = static_cast<std::int32_t>(cloud.pointCount());
        cloud.push(static_cast<float>(point[0]), static_cast<float>(point[1]),
                   static_cast<float>(point[2]));
        if (view.cloud->hasIntensity())
          cloud.intensity.push_back(view.cloud->intensity[middle.second]);
        if (view.cloud->hasNormals())
          for (int c = 0; c < 3; ++c)
            cloud.normals.push_back(view.cloud->normals[3 * middle.second + c]);
        if (view.cloud->hasRgb())
          for (int c = 0; c < 3; ++c)
            cloud.rgb.push_back(view.cloud->rgb[3 * middle.second + c]);
        indices.data[static_cast<std::size_t>(y) * depth.cols + x] = static_cast<float>(index);
        filledIndices.values.push_back(index);
        depth.at<float>(y, x) = middle.first;
        status.at<unsigned char>(y, x) = projection5::kInterpolated;
        filled.at<unsigned char>(y, x) = 255;
      }
    }
    auto depthOut = native(depth), stateOut = native(status), filledOut = native(filled);
    auto mapping = projection5::rebind(*view.mapping, cloud, depthOut, stateOut, indices, ctx);
    Record quality{"cloud.HoleFill.v1",
                   {{"sourceCloudId", std::to_string(view.cloud->id)},
                    {"resultCloudId", std::to_string(cloud.id)},
                    {"frame", view.grid.cloudFrame},
                    {"unit", "m"},
                    {"candidateCount", candidateCount},
                    {"filledCount", filledIndices.values.size()},
                    {"unfilledCount", candidateCount - filledIndices.values.size()},
                    {"measured", false},
                    {"channelPolicy", "copy median-depth observed source channel tuple"}}};
    bounded(mapping);
    bounded(quality);
    poll(ctx);
    out.set("cloud", Data::cloud(std::move(cloud)));
    out.set("depth", Data::image(std::move(depthOut)));
    out.set("status", Data::image(std::move(stateOut)));
    out.set("indexMap", Data::tensor(std::move(indices)));
    out.set("mapping", Data::record(std::move(mapping)));
    out.set("filledMask", Data::image(std::move(filledOut)));
    out.set("filledIndices", Data::indices(std::move(filledIndices)));
    out.set("quality", Data::record(std::move(quality)));
    return Status::Ok();
  } catch (const projection5::Cancelled&) {
    return Status::Error(Phase::Execute, "cancelled", "任务已取消");
  } catch (const projection5::PrecisionLoss& error) {
    return Status::Error(Phase::Execute, "precision_loss", error.what(), {}, "cloud");
  } catch (const Failure& e) {
    return e.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "孔洞填补内存不足");
  } catch (const std::exception& e) {
    return caught(e);
  }
}

Status defects(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    Depth view(in, ctx);
    Base base(in, p, view);
    auto roi = selection(in, view);
    const auto mode = p.choice("mode");
    need(mode == "up" || mode == "down" || mode == "both", "缺陷方向非法");
    const double threshold = scalar(p, "distanceThreshold", 0, 1e6),
                 minCoverage = scalar(p, "minCoverage", 0, 1);
    const int kernel = integer(p, "denoiseKernel", 1, 63),
              maximum = integer(p, "maxDefects", 1, 256);
    need(kernel % 2 == 1, "去噪窗口必须为奇数");
    const std::array<const char*, 4> enabled{"checkLength", "checkWidth", "checkArea",
                                             "checkVolume"},
        limits{"lengthThreshold", "widthThreshold", "areaThreshold", "volumeThreshold"};
    std::array<double, 4> thresholds{};
    for (int j = 0; j < 4; ++j)
      thresholds[j] = scalar(p, limits[j], 0, 1e12);
    const bool include = p.flag("includeInterpolated");
    std::uint64_t work = 0;
    charge(work, view.depth->pixelCount() * 24, ctx);
    cv::Mat height = cv::Mat::zeros(roi.size(), CV_32F),
            distance = cv::Mat::zeros(roi.size(), CV_64F),
            valid = cv::Mat::zeros(roi.size(), CV_8U),
            overlay(roi.size(), CV_8UC3, cv::Scalar(24, 24, 24)),
            all = cv::Mat::zeros(roi.size(), CV_8U);
    std::size_t observed = 0, synthetic = 0;
    for (int y = 0; y < roi.rows; ++y) {
      poll(ctx);
      for (int x = 0; x < roi.cols; ++x) {
        if (!roi.at<unsigned char>(y, x) || !view.selected(x, y, include))
          continue;
        const double h = base.height(view.point(x, y));
        need(std::isfinite(h) && std::abs(h) <= std::numeric_limits<float>::max(),
             "有符号高度超出范围", "plane");
        height.at<float>(y, x) = static_cast<float>(h);
        distance.at<double>(y, x) = h;
        valid.at<unsigned char>(y, x) = 255;
        if (view.status(x, y) & projection5::kObserved)
          ++observed;
        else
          ++synthetic;
        overlay.at<cv::Vec3b>(y, x) = {64, 64, 64};
      }
    }
    const auto rot = rotation(base.normal);
    const auto matrix = poses::rotationMatrix(rot);
    const cv::Vec3d bx(matrix[0], matrix[3], matrix[6]), by(matrix[1], matrix[4], matrix[7]),
        planeOrigin = -base.offset * base.normal;
    Json reports = Json::array(), posesOut = Json::array();
    projection5::RayBudget rays;
    rays.context = &ctx;
    bool failed = false;
    for (int sign : {1, -1}) {
      if ((mode == "up" && sign < 0) || (mode == "down" && sign > 0))
        continue;
      cv::Mat raw = (distance * sign > threshold) & valid, binary = raw.clone();
      if (p.flag("denoise")) {
        charge(work, view.depth->pixelCount() * kernel * 4, ctx);
        cv::morphologyEx(raw, binary, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_RECT, {kernel, kernel}));
        cv::bitwise_and(binary, valid, binary);
      }
      cv::Mat labels;
      const int components = cv::connectedComponents(binary, labels, 8, CV_32S);
      if (components - 1 > maximum)
        return Status::Error(Phase::Execute, "resource_limit", "缺陷连通分量超过预算");
      std::vector<std::vector<cv::Point>> supports(components);
      for (int y = 0; y < labels.rows; ++y) {
        poll(ctx);
        for (int x = 0; x < labels.cols; ++x)
          if (raw.at<unsigned char>(y, x))
            supports[labels.at<int>(y, x)].push_back({x, y});
      }
      for (int component = 1; component < components; ++component) {
        const auto& points = supports[component];
        if (points.empty())
          continue;
        charge(work, points.size() * 256, ctx);
        double area = 0, volume = 0, sum = 0, low = std::numeric_limits<double>::infinity(),
               high = -low;
        const auto localOrigin = base.project(view.point(points.front().x, points.front().y));
        std::vector<cv::Point2f> footprint;
        footprint.reserve(points.size() * 4);
        for (const auto& point : points) {
          const double h = distance.at<double>(point.y, point.x);
          const std::array<cv::Point2d, 4> corners{{{point.x - .5, point.y - .5},
                                                    {point.x + .5, point.y - .5},
                                                    {point.x + .5, point.y + .5},
                                                    {point.x - .5, point.y + .5}}};
          std::array<cv::Vec3d, 4> physical;
          const auto evaluations = rays.evaluations;
          for (int j = 0; j < 4; ++j) {
            cv::Vec3d observedCorner;
            rayResult(projection5::backProjectBoundary(view.grid, corners[j].x, corners[j].y,
                                                       view.z(point.x, point.y), observedCorner,
                                                       &rays),
                      rays, "观测深度像素角点反投影失败");
            physical[j] = base.project(observedCorner);
            const auto relative = physical[j] - planeOrigin;
            const double xx = relative.dot(bx), yy = relative.dot(by);
            need(std::isfinite(xx) && std::isfinite(yy) && std::abs(xx) < 1e10 &&
                     std::abs(yy) < 1e10,
                 "缺陷平面足迹退化", "plane");
            const auto local = physical[j] - localOrigin;
            footprint.emplace_back(static_cast<float>(local.dot(bx)),
                                   static_cast<float>(local.dot(by)));
          }
          charge(work, rays.evaluations - evaluations, ctx);
          const double cell = std::abs(base.normal.dot(
                                  (physical[2] - physical[0]).cross(physical[3] - physical[1]))) /
                              2;
          need(std::isfinite(cell) && cell > 0, "像素物理面积退化", "mapping");
          area += cell;
          volume += cell * std::abs(h);
          sum += h;
          low = std::min(low, h);
          high = std::max(high, h);
          all.at<unsigned char>(point.y, point.x) = sign > 0 ? 255 : 128;
          overlay.at<cv::Vec3b>(point.y, point.x) =
              sign > 0 ? cv::Vec3b(0, 255, 0) : cv::Vec3b(255, 0, 0);
        }
        const auto rectangle = cv::minAreaRect(footprint);
        const std::array<double, 4> metrics{std::max(rectangle.size.width, rectangle.size.height),
                                            std::min(rectangle.size.width, rectangle.size.height),
                                            area, volume};
        Json exceeded = Json::array();
        for (int j = 0; j < 4; ++j) {
          need(std::isfinite(metrics[j]), "缺陷量测非有限");
          if (p.flag(enabled[j]) && metrics[j] > thresholds[j])
            exceeded.push_back(limits[j]);
        }
        const bool defect = !exceeded.empty();
        failed |= defect;
        const std::string id = "defect-" + std::to_string(reports.size());
        const auto center = localOrigin + bx * rectangle.center.x + by * rectangle.center.y +
                            base.normal * ((low + high) / 2);
        const double a = rectangle.angle * poses::kPi / 180;
        const cv::Vec3d xAxis = bx * std::cos(a) + by * std::sin(a),
                        yAxis = -bx * std::sin(a) + by * std::cos(a);
        const auto q = poses::quaternion({xAxis[0], yAxis[0], base.normal[0], xAxis[1], yAxis[1],
                                          base.normal[1], xAxis[2], yAxis[2], base.normal[2]});
        auto pose = poses::make3D(id, view.grid.cloudFrame, vec(center), q,
                                  {std::max(1e-9, double(rectangle.size.width)),
                                   std::max(1e-9, double(rectangle.size.height)),
                                   std::max(1e-9, high - low)});
        pose.data["pickable"] = false;
        pose.data["attributes"] = {{"defect", defect},
                                   {"direction", sign > 0 ? "up" : "down"},
                                   {"support", points.size()}};
        posesOut.push_back(std::move(pose.data));
        reports.push_back({{"id", id},
                           {"direction", sign > 0 ? "up" : "down"},
                           {"support", points.size()},
                           {"length", metrics[0]},
                           {"width", metrics[1]},
                           {"area", area},
                           {"volume", volume},
                           {"meanHeight", sum / points.size()},
                           {"minHeight", low},
                           {"maxHeight", high},
                           {"defect", defect},
                           {"exceeded", exceeded}});
        if (reports.size() > static_cast<std::size_t>(maximum))
          return Status::Error(Phase::Execute, "resource_limit", "累计缺陷超过预算");
      }
    }
    const auto selected = static_cast<std::size_t>(cv::countNonZero(roi));
    const double coverage = static_cast<double>(observed + synthetic) / selected;
    const bool complete = observed + synthetic > 0 && coverage >= minCoverage;
    Record result{"inspection.Defects.v1",
                  {{"frame", view.grid.cloudFrame},
                   {"unit", "m"},
                   {"sourceCloudId", std::to_string(view.cloud->id)},
                   {"state", failed     ? "defect"
                             : complete ? "clear"
                                        : "unknown"},
                   {"valid", complete},
                   {"good", failed     ? Json(false)
                            : complete ? Json(true)
                                       : Json(nullptr)},
                   {"observedCount", observed},
                   {"interpolatedCount", synthetic},
                   {"coverage", coverage},
                   {"minCoverage", minCoverage},
                   {"areaUnit", "m2"},
                   {"volumeUnit", "m3"},
                   {"areaModel", "observed_depth_orthogonal_footprint"},
                   {"volumeModel", "center_signed_height_times_projected_cell_area"},
                   {"items", reports}}};
    auto instances = poses::make3DList(posesOut, view.grid.cloudFrame);
    std::string why;
    need(poses::validate(instances, why), "缺陷姿态非法：" + why);
    bounded(instances);
    bounded(result);
    auto maskOut = native(all), heightOut = native(height), overlayOut = native(overlay);
    poll(ctx);
    out.set("instances", Data::record(std::move(instances)));
    out.set("defects", Data::record(std::move(result)));
    out.set("mask", Data::image(std::move(maskOut)));
    out.set("height", Data::image(std::move(heightOut)));
    out.set("overlay", Data::image(std::move(overlayOut)));
    return Status::Ok();
  } catch (const projection5::Cancelled&) {
    return Status::Error(Phase::Execute, "cancelled", "任务已取消");
  } catch (const Failure& e) {
    return e.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "缺陷检测内存不足");
  } catch (const std::exception& e) {
    return caught(e);
  }
}
struct ProfilePoint {
  cv::Vec3d point;
  cv::Vec3i sources;
  double x = 0, h = 0;
  int parent = 0, chain = 0;
};
struct Feature {
  bool valid = false;
  double x = 0, h = 0, slope = 0;
  int chain = 0;
};
Feature feature(const std::vector<ProfilePoint>& points, const ParamView& p, const char* prefix) {
  const std::string key = prefix;
  const double center = scalar(p, (key + "Center").c_str(), -1e6, 1e6),
               width = scalar(p, (key + "Width").c_str(), 1e-9, 1e6),
               threshold = scalar(p, (key + "Gradient").c_str(), 0, 1);
  const auto mode = p.choice((key + "Type").c_str());
  need(mode == "left-right-inflection" || mode == "right-left-inflection" || mode == "top" ||
           mode == "bottom" || mode == "left-most" || mode == "right-most",
       "剖面特征模式非法");
  std::vector<const ProfilePoint*> selected;
  for (const auto& point : points)
    if (std::abs(point.x - center) <= width / 2)
      selected.push_back(&point);
  if (selected.empty())
    return {};
  std::sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) {
    return std::tie(a->chain, a->x, a->h) < std::tie(b->chain, b->x, b->h);
  });
  if (mode == "left-right-inflection" || mode == "right-left-inflection") {
    struct Gradient {
      const ProfilePoint* left;
      const ProfilePoint* right;
      double slope;
    };
    std::vector<Gradient> gradients;
    double maximum = 0;
    for (std::size_t i = 1; i < selected.size(); ++i) {
      const auto* a = selected[i - 1];
      const auto* b = selected[i];
      if (a->chain != b->chain || b->x - a->x <= 1e-12)
        continue;
      const double slope = std::abs((b->h - a->h) / (b->x - a->x));
      if (!std::isfinite(slope))
        continue;
      gradients.push_back({a, b, slope});
      maximum = std::max(maximum, slope);
    }
    if (maximum <= 1e-12)
      return {};
    const bool right = mode == "right-left-inflection";
    std::sort(gradients.begin(), gradients.end(), [&](const auto& a, const auto& b) {
      return right ? a.right->x > b.right->x : a.left->x < b.left->x;
    });
    for (const auto& g : gradients)
      if (g.slope >= threshold * maximum) {
        const auto* v = right ? g.right : g.left;
        return {true, v->x, v->h, g.slope, v->chain};
      }
    return {};
  }
  const auto* chosen = selected.front();
  for (const auto* point : selected) {
    if ((mode == "top" && point->h > chosen->h) || (mode == "bottom" && point->h < chosen->h) ||
        (mode == "left-most" && point->x < chosen->x) ||
        (mode == "right-most" && point->x > chosen->x))
      chosen = point;
  }
  return {true, chosen->x, chosen->h, 0, chosen->chain};
}
Json featureJson(const Feature& f) {
  return {{"valid", f.valid},
          {"x", f.valid ? Json(f.x) : Json(nullptr)},
          {"height", f.valid ? Json(f.h) : Json(nullptr)},
          {"gradient", f.valid ? Json(f.slope) : Json(nullptr)},
          {"chain", f.valid ? Json(f.chain) : Json(nullptr)}};
}
Status profiles(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    Depth view(in, ctx);
    Base base(in, p, view);
    const int count = integer(p, "sliceCount", 1, 1024),
              gap = integer(p, "meshPixelDistance", 1, 32),
              visual = integer(p, "visualizeSlice", 0, 1023);
    need(visual < count, "可视化截面序号越界");
    const double edgeLength = scalar(p, "maxEdgeLength", 1e-9, 1e6),
                 lower = scalar(p, "min", 0, 1e6), upper = scalar(p, "max", 0, 1e6);
    need(lower <= upper, "截面属性上下限颠倒");
    const auto direction = p.choice("direction"), mode = p.choice("attributeMode");
    need((direction == "x" || direction == "y") && (mode == "x-x" || mode == "y-y"),
         "截面方向或属性模式非法");
    cv::Point2d center((view.grid.width - 1) / 2., (view.grid.height - 1) / 2.);
    double width = view.grid.width - 1, height = view.grid.height - 1, angle = 0;
    need(width > 0 && height > 0, "截面需要至少2x2栅格");
    if (in.has("region")) {
      const auto* region = in.get("region").asRecord();
      std::string why;
      need(region && region->type == poses::kPose2DType && poses::validate(*region, why) &&
               region->data["frame"] == view.grid.imageFrame && region->data["unit"] == "px",
           "截面区域需要同帧像素Pose2D", "region");
      center = {region->data["position"][0].get<double>(),
                region->data["position"][1].get<double>()};
      width = region->data["size"][0];
      height = region->data["size"][1];
      angle = region->data["angle"];
    }
    const cv::Point2d ux(std::cos(angle), std::sin(angle)), uy(-std::sin(angle), std::cos(angle));
    std::array<cv::Point2d, 4> pixels{{center - ux * (width / 2) - uy * (height / 2),
                                       center + ux * (width / 2) - uy * (height / 2),
                                       center + ux * (width / 2) + uy * (height / 2),
                                       center - ux * (width / 2) + uy * (height / 2)}};
    for (const auto& point : pixels)
      need(point.x >= 0 && point.y >= 0 && point.x <= view.grid.width - 1 &&
               point.y <= view.grid.height - 1,
           "旋转区域必须完整落在栅格范围内", "region");
    std::array<cv::Vec3d, 4> corners;
    projection5::RayBudget rays;
    rays.context = &ctx;
    for (int j = 0; j < 4; ++j)
      corners[j] = base.onRay(view.grid, pixels[j].x, pixels[j].y, &rays);
    const bool synthetic = p.flag("includeInterpolated");
    const auto inside = [&](int x, int y) {
      const cv::Point2d relative = cv::Point2d(x, y) - center;
      return std::abs(relative.dot(ux)) <= width / 2 + 1e-9 &&
             std::abs(relative.dot(uy)) <= height / 2 + 1e-9;
    };
    std::vector<cv::Vec3i> triangles;
    std::uint64_t work = rays.evaluations;
    charge(work, view.depth->pixelCount() * 8, ctx);
    for (int y = 0; y < view.grid.height - 1; ++y) {
      poll(ctx);
      for (int x = 0; x < view.grid.width - 1; ++x) {
        if (!view.selected(x, y, synthetic) || !inside(x, y))
          continue;
        charge(work, 2ULL * gap, ctx);
        int xx = x + 1, yy = y + 1;
        while (xx < view.grid.width && xx - x <= gap && !view.selected(xx, y, synthetic))
          ++xx;
        while (yy < view.grid.height && yy - y <= gap && !view.selected(x, yy, synthetic))
          ++yy;
        if (xx >= view.grid.width || yy >= view.grid.height || xx - x > gap || yy - y > gap ||
            !inside(xx, y) || !inside(x, yy))
          continue;
        auto add = [&](int a, int b, int c) {
          const auto point = [&](int n) {
            return view.point(n % view.grid.width, n / view.grid.width);
          };
          const auto A = point(a), B = point(b), C = point(c);
          if (cv::norm(B - A) > edgeLength || cv::norm(C - A) > edgeLength ||
              cv::norm(C - B) > edgeLength || cv::norm((B - A).cross(C - A)) < 1e-18)
            return;
          triangles.emplace_back(a, b, c);
        };
        add(y * view.grid.width + x, y * view.grid.width + xx, yy * view.grid.width + x);
        if (view.selected(xx, yy, synthetic) && inside(xx, yy))
          add(yy * view.grid.width + xx, yy * view.grid.width + x, y * view.grid.width + xx);
      }
    }
    charge(work, static_cast<std::uint64_t>(triangles.size()) * count * 16, ctx);
    Tensor output, attributes, attributeValid, supports;
    attributes.shape = {count};
    attributeValid.shape = {count};
    PointCloud cloud;
    Indices indices;
    indices.sourceCloudId = cloud.id;
    std::vector<cloud_groups::Group> groups;
    Json reports = Json::array();
    cv::Mat overlay(view.grid.height, view.grid.width, CV_8UC3, cv::Scalar(24, 24, 24)),
        plot(256, 512, CV_8UC3, cv::Scalar(16, 16, 16));
    bool bad = false;
    int validCount = 0;
    std::size_t unresolvedNormals = 0, nearestNormals = 0;
    for (int slice = 0; slice < count; ++slice) {
      poll(ctx);
      const double t = (slice + 1.) / (count + 1.);
      const auto a = corners[0] * (1 - t) + corners[direction == "x" ? 3 : 1] * t,
                 b = corners[direction == "x" ? 1 : 3] * (1 - t) + corners[2] * t;
      const auto origin = (a + b) * .5;
      auto along = b - a;
      const double length = cv::norm(along);
      need(length > 1e-12, "截面基线退化");
      along /= length;
      auto normal = along.cross(base.normal);
      need(cv::norm(normal) > 1e-12, "截面法向退化");
      normal /= cv::norm(normal);
      std::vector<ProfilePoint> points;
      std::map<std::array<std::int64_t, 3>, int> lookup;
      auto root = [&](int i) {
        while (points[i].parent != i) {
          points[i].parent = points[points[i].parent].parent;
          i = points[i].parent;
        }
        return i;
      };
      auto emit = [&](const cv::Vec3d& point, const cv::Vec3i& sources) {
        const double alongX = (point - origin).dot(along), h = base.height(point);
        if (std::abs(alongX) > length / 2 + 1e-9)
          return -1;
        need(std::isfinite(alongX) && std::isfinite(h) && std::abs(alongX) < 1e6 &&
                 std::abs(h) < 1e6,
             "截面坐标超出有限范围");
        std::array<std::int64_t, 3> key{};
        for (int j = 0; j < 3; ++j) {
          need(std::isfinite(point[j]) && std::abs(point[j]) < 1e8, "截面世界坐标超出预算");
          key[j] = static_cast<std::int64_t>(std::llround(point[j] * 1e8));
        }
        auto entry = lookup.find(key);
        if (entry != lookup.end())
          return entry->second;
        if (points.size() + cloud.pointCount() >= 1000000)
          throw Failure{Status::Error(Phase::Execute, "resource_limit", "截面交点超过1M")};
        const int id = static_cast<int>(points.size());
        lookup.emplace(key, id);
        points.push_back({point, sources, alongX, h, id, 0});
        return id;
      };
      for (const auto& triangle : triangles) {
        if ((points.size() & 1023) == 0)
          poll(ctx);
        std::array<cv::Vec3d, 3> vertex;
        std::array<double, 3> distance{};
        cv::Vec3i source;
        for (int j = 0; j < 3; ++j) {
          vertex[j] = view.point(triangle[j] % view.grid.width, triangle[j] / view.grid.width);
          distance[j] = normal.dot(vertex[j] - origin);
          source[j] = view.index(triangle[j] % view.grid.width, triangle[j] / view.grid.width);
        }
        if ((*std::min_element(distance.begin(), distance.end()) > 1e-10) ||
            (*std::max_element(distance.begin(), distance.end()) < -1e-10))
          continue;
        std::vector<int> intersection;
        for (int j = 0; j < 3; ++j) {
          const int next = (j + 1) % 3;
          if (std::abs(distance[j]) <= 1e-10) {
            const int id = emit(vertex[j], source);
            if (id >= 0)
              intersection.push_back(id);
          }
          if (distance[j] * distance[next] < 0) {
            const double alpha = distance[j] / (distance[j] - distance[next]);
            const int id = emit(vertex[j] + alpha * (vertex[next] - vertex[j]), source);
            if (id >= 0)
              intersection.push_back(id);
          }
        }
        for (std::size_t j = 1; j < intersection.size(); ++j) {
          int aa = root(intersection[0]), bb = root(intersection[j]);
          if (aa != bb)
            points[bb].parent = aa;
        }
      }
      std::map<int, int> chains;
      charge(work, points.size() * 32, ctx);
      for (std::size_t j = 0; j < points.size(); ++j) {
        const int parent = root(static_cast<int>(j));
        if (!chains.count(parent))
          chains[parent] = static_cast<int>(chains.size());
        points[j].chain = chains[parent];
      }
      std::sort(points.begin(), points.end(), [](const auto& l, const auto& r) {
        return std::tie(l.chain, l.x, l.h) < std::tie(r.chain, r.x, r.h);
      });
      const auto A = feature(points, p, "a"), B = feature(points, p, "b");
      const bool valid = A.valid && B.valid;
      const double value = valid ? std::abs(mode == "x-x" ? A.x - B.x : A.h - B.h) : 0;
      const bool good = valid && value >= lower && value <= upper;
      bad |= valid && !good;
      validCount += valid;
      attributes.data.push_back(static_cast<float>(value));
      attributeValid.data.push_back(valid ? 1.f : 0.f);
      const auto begin = cloud.pointCount();
      for (const auto& point : points) {
        indices.values.push_back(static_cast<std::int32_t>(cloud.pointCount()));
        cloud.push(static_cast<float>(point.point[0]), static_cast<float>(point.point[1]),
                   static_cast<float>(point.point[2]));
        std::array<cv::Vec3d, 3> sourcePoints;
        for (int j = 0; j < 3; ++j) {
          const auto n = static_cast<std::size_t>(point.sources[j]);
          sourcePoints[j] = {view.cloud->xyz[3 * n], view.cloud->xyz[3 * n + 1],
                             view.cloud->xyz[3 * n + 2]};
        }
        const auto v0 = sourcePoints[1] - sourcePoints[0], v1 = sourcePoints[2] - sourcePoints[0],
                   v2 = point.point - sourcePoints[0];
        const double d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0),
                     d21 = v2.dot(v1), denominator = d00 * d11 - d01 * d01;
        need(std::isfinite(denominator) && denominator > 0, "截面支持三角形退化");
        std::array<double, 3> weights{0, (d11 * d20 - d01 * d21) / denominator,
                                      (d00 * d21 - d01 * d20) / denominator};
        weights[0] = 1 - weights[1] - weights[2];
        for (double& weight : weights) {
          need(std::isfinite(weight) && weight >= -1e-5 && weight <= 1 + 1e-5,
               "截面点不在溯源三角形内");
          weight = std::clamp(weight, 0., 1.);
        }
        const double weightSum = weights[0] + weights[1] + weights[2];
        for (double& weight : weights)
          weight /= weightSum;
        if (view.cloud->hasIntensity()) {
          double value = 0;
          for (int j = 0; j < 3; ++j)
            value += weights[j] * view.cloud->intensity[point.sources[j]];
          cloud.intensity.push_back(static_cast<float>(value));
        }
        if (view.cloud->hasRgb())
          for (int c = 0; c < 3; ++c) {
            double value = 0;
            for (int j = 0; j < 3; ++j)
              value += weights[j] * view.cloud->rgb[3 * point.sources[j] + c];
            cloud.rgb.push_back(
                static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L)));
          }
        if (view.cloud->hasNormals()) {
          cv::Vec3d n(0, 0, 0);
          for (int j = 0; j < 3; ++j)
            for (int c = 0; c < 3; ++c)
              n[c] += weights[j] * view.cloud->normals[3 * point.sources[j] + c];
          if (cv::norm(n) <= 1e-12) {
            ++nearestNormals;
            double nearest = std::numeric_limits<double>::infinity();
            for (int j = 0; j < 3; ++j) {
              cv::Vec3d candidate;
              for (int c = 0; c < 3; ++c)
                candidate[c] = view.cloud->normals[3 * point.sources[j] + c];
              const double distance = cv::norm(sourcePoints[j] - point.point);
              if (cv::norm(candidate) > 1e-12 && distance < nearest) {
                nearest = distance;
                n = candidate;
              }
            }
          }
          if (cv::norm(n) > 1e-12)
            n /= cv::norm(n);
          else {
            n = {0, 0, 0};
            ++unresolvedNormals;
          }
          for (int c = 0; c < 3; ++c)
            cloud.normals.push_back(static_cast<float>(n[c]));
        }
        output.data.insert(output.data.end(),
                           {static_cast<float>(slice), static_cast<float>(point.x),
                            static_cast<float>(point.h), static_cast<float>(point.chain)});
        for (int j = 0; j < 3; ++j)
          supports.data.push_back(static_cast<float>(point.sources[j]));
      }
      const auto id = "slice-" + std::to_string(slice);
      groups.push_back({id, begin, cloud.pointCount() - begin});
      reports.push_back({{"id", id},
                         {"valid", valid},
                         {"good", valid ? Json(good) : Json(nullptr)},
                         {"value", valid ? Json(value) : Json(nullptr)},
                         {"pointCount", points.size()},
                         {"chainCount", chains.size()},
                         {"a", featureJson(A)},
                         {"b", featureJson(B)},
                         {"origin", vec(origin)},
                         {"direction", vec(along)}});
      const auto pa = pixels[0] * (1 - t) + pixels[direction == "x" ? 3 : 1] * t,
                 pb = pixels[direction == "x" ? 1 : 3] * (1 - t) + pixels[2] * t;
      cv::line(overlay, pa, pb, slice == visual ? cv::Scalar(255, 220, 0) : cv::Scalar(0, 160, 255),
               1);
      if (slice == visual && !points.empty()) {
        double h0 = points[0].h, h1 = h0;
        for (const auto& v : points) {
          h0 = std::min(h0, v.h);
          h1 = std::max(h1, v.h);
        }
        if (h1 - h0 < 1e-9) {
          h0 -= .001;
          h1 += .001;
        }
        auto pixel = [&](double x, double h) {
          return cv::Point(static_cast<int>(20 + 472 * (x / length + .5)),
                           static_cast<int>(236 - 216 * (h - h0) / (h1 - h0)));
        };
        for (std::size_t j = 1; j < points.size(); ++j)
          if (points[j].chain == points[j - 1].chain)
            cv::line(plot, pixel(points[j - 1].x, points[j - 1].h), pixel(points[j].x, points[j].h),
                     {96, 192, 255}, 1);
        if (A.valid)
          cv::circle(plot, pixel(A.x, A.h), 3, {0, 255, 0}, 1);
        if (B.valid)
          cv::circle(plot, pixel(B.x, B.h), 3, {255, 80, 0}, 1);
      }
    }
    output.shape = {static_cast<std::int64_t>(cloud.pointCount()), 4};
    supports.shape = {static_cast<std::int64_t>(cloud.pointCount()), 3};
    auto grouping = cloud_groups::make(cloud, groups, view.grid.cloudFrame);
    Record result{
        "inspection.Profiles.v1",
        {{"frame", view.grid.cloudFrame},
         {"unit", "m"},
         {"sourceCloudId", std::to_string(view.cloud->id)},
         {"sliceCloudId", std::to_string(cloud.id)},
         {"state", bad                   ? "ng"
                   : validCount == count ? "good"
                                         : "unknown"},
         {"good", bad                   ? Json(false)
                  : validCount == count ? Json(true)
                                        : Json(nullptr)},
         {"validCount", validCount},
         {"sliceCount", count},
         {"attributeMode", mode},
         {"derivation", "triangle_plane_intersection"},
         {"measured", false},
         {"meshPixelDistance", gap},
         {"includeInterpolated", synthetic},
         {"channelPolicy",
          "barycentric intensity/RGB; normalized barycentric normals; nearest nonzero normal fallback; zero+unresolved if none"},
         {"nearestNormalCount", nearestNormals},
         {"unresolvedNormalCount", unresolvedNormals},
         {"items", reports}}};
    bounded(result);
    bounded(grouping);
    auto overlayOut = native(overlay), plotOut = native(plot);
    poll(ctx);
    out.set("profiles", Data::tensor(std::move(output)));
    out.set("attributes", Data::tensor(std::move(attributes)));
    out.set("attributeValid", Data::tensor(std::move(attributeValid)));
    out.set("supports", Data::tensor(std::move(supports)));
    out.set("sliceCloud", Data::cloud(std::move(cloud)));
    out.set("indices", Data::indices(std::move(indices)));
    out.set("groups", Data::record(std::move(grouping)));
    out.set("features", Data::record(std::move(result)));
    out.set("overlay", Data::image(std::move(overlayOut)));
    out.set("plot", Data::image(std::move(plotOut)));
    return Status::Ok();
  } catch (const projection5::Cancelled&) {
    return Status::Error(Phase::Execute, "cancelled", "任务已取消");
  } catch (const Failure& e) {
    return e.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "组织截面内存不足");
  } catch (const std::exception& e) {
    return caught(e);
  }
}
}
void registerP5Inspection(Registry& r) {
  auto op = image_p1::descriptor("image.find_circles_3d", "空间圆孔检测",
                                 "ellipse circle 3d conic calibrated ambiguity canny");
  op.inputs = {img::imageIn(), image_p1::recordPort("calib", calibration::kCameraType),
               Port{"radii", "Tensor", "半径", "可选[N]米制，单值广播", false}};
  op.outputs = {image_p1::recordPort("instances", poses::kPoses3DType),
                image_p1::recordPort("circles", "geometry.Circles3D.v1"),
                image_p1::recordPort("ellipses", "image.Ellipses.v1"),
                image_p1::recordPort("circles2d", "image.Circles2D.v1"),
                img::imageOut(),
                img::imageOut("edges")};
  op.params = {number("thetaFsa", "弧方向偏差", 60, 0, 90, "deg"),
               number("lengthFsa", "相邻弧长度比", 3.4, 1.01, 100),
               img::boolParam("autoCanny", "自动边缘阈值", true, ""),
               img::intParam("lowThreshold", "低阈值", 100, 0, 255, ""),
               img::intParam("highThreshold", "高阈值", 150, 0, 255, ""),
               number("minScore", "最小角覆盖评分", .6, 0, 1),
               number("circleThreshold", "二维圆偏心率上限", .2, 0, 1),
               number("radius", "已知圆半径", .01, 1e-9, 1000, "m"),
               number("edgeTolerance", "弧合并及重复边容差", 2, .1, 16, "px"),
               number("thickness", "姿态薄片厚度", .0001, 1e-9, 1000, "m"),
               img::intParam("maxCircles", "空间圆候选预算", 128, 1, 128, ""),
               image_p1::text("imageFrame", "图像参考帧", "image"),
               img::enumParam("visualization", "显示内容", "circles",
                              {{"circles", "椭圆", ""}, {"canny", "边缘", ""}}, "")};
  op.capabilities.previewable = false;
  op.doc =
      "独立Canny弧段/最小二乘椭圆与角覆盖评分；已知米制半径、K/D校正后按圆锥二次型真实恢复两歧义空间圆，不选择一个冒充确定姿态。稳定原图顺序匹配半径；二维圆按偏心率独立筛选。";
  op.compute = circles;
  r.addOperator(std::move(op));
  auto fill = image_p1::descriptor(
      "cloud.fill_holes", "组织点云孔洞填补",
      "organized cloud depth morphological closing median hole fill observed interpolated");
  const std::vector<Port> depthInputs{{"cloud", "PointCloud", "源云", "索引绑定原云", true},
                                      img::grayIn("depth"),
                                      img::grayIn("status"),
                                      {"indexMap", "Tensor", "索引图", "[H,W]原生索引", true},
                                      image_p1::recordPort("mapping", projection5::kType)};
  fill.inputs = depthInputs;
  fill.outputs = {{"cloud", "PointCloud", "补值云", "新身份保留原点并追加补点", true},
                  img::imageOut("depth"),
                  img::imageOut("status"),
                  {"indexMap", "Tensor", "新索引图", "绑定新云", true},
                  image_p1::recordPort("mapping", projection5::kType),
                  img::imageOut("filledMask"),
                  {"filledIndices", "Indices", "补点索引", "只指向新云追加点", true},
                  image_p1::recordPort("quality", "cloud.HoleFill.v1")};
  fill.params = {
      img::intParam("closeKernel", "闭运算窗口", 3, 1, 63, "奇数"),
      img::intParam("fillKernel", "原观测中值窗口", 3, 1, 63, "奇数"),
      img::intParam("minNeighbors", "最少原观测", 3, 1, 3969, ""),
      number("maxDepthSpread", "邻域深度跨度上限", .02, 0, 1e6, "m"),
      img::boolParam("fillBoundary", "允许外边界补值", false, "默认不填与图边相连的未知区域")};
  fill.capabilities.previewable = false;
  fill.compute = fillHoles;
  r.addOperator(std::move(fill));
  auto defect = image_p1::descriptor(
      "depth.defects", "深度平面缺陷检测",
      "depth defect convex concave signed plane physical area volume length width");
  defect.inputs = depthInputs;
  defect.inputs.push_back({"plane", "Plane", "基准平面", "源云同帧", true});
  defect.inputs.push_back({"roi", "Box2D", "像素ROI", "半开区域", false});
  defect.inputs.push_back({"mask", "Image", "区域mask", "同映射尺寸u8", false});
  defect.outputs = {image_p1::recordPort("instances", poses::kPoses3DType),
                    image_p1::recordPort("defects", "inspection.Defects.v1"), img::imageOut("mask"),
                    img::imageOut("height"), img::imageOut("overlay")};
  defect.params = {image_p1::text("planeFrame", "平面参考帧", "input"),
                   img::enumParam("mode", "缺陷方向", "both",
                                  {{"up", "凸起", ""}, {"down", "凹陷", ""}, {"both", "双向", ""}},
                                  ""),
                   number("distanceThreshold", "有符号距离阈值", .000005, 0, 1e6, "m"),
                   img::boolParam("denoise", "闭运算连接", false, "量测仍仅使用真实超阈值支持"),
                   img::intParam("denoiseKernel", "闭运算窗口", 5, 1, 63, "奇数"),
                   number("minCoverage", "判清晰最低观测覆盖", 1, 0, 1),
                   img::boolParam("includeInterpolated", "显式纳入补值", false, "补值单独计数"),
                   img::intParam("maxDefects", "缺陷结果预算", 256, 1, 256, "")};
  for (const auto& metric : std::array<std::array<const char*, 4>, 4>{
           {{{"checkLength", "lengthThreshold", "长度", "m"}},
            {{"checkWidth", "widthThreshold", "宽度", "m"}},
            {{"checkArea", "areaThreshold", "面积", "m2"}},
            {{"checkVolume", "volumeThreshold", "体积", "m3"}}}}) {
    defect.params.push_back(
        img::boolParam(metric[0], metric[2], true, "启用后任一项严格超过阈值即缺陷"));
    defect.params.push_back(number(metric[1], metric[2], 0, 0, 1e12, metric[3]));
  }
  defect.capabilities.previewable = false;
  defect.compute = defects;
  r.addOperator(std::move(defect));
  auto profile = image_p1::descriptor(
      "depth.profile_slices", "组织深度轮廓截面",
      "organized depth profile mesh plane intersection inflection gradient width height");
  profile.inputs = depthInputs;
  profile.inputs.push_back({"plane", "Plane", "基准平面", "源云同帧", true});
  profile.inputs.push_back(image_p1::recordPort("region", poses::kPose2DType, false));
  profile.outputs = {{"profiles", "Tensor", "截面点", "[M,4] slice,x,height,chain", true},
                     {"attributes", "Tensor", "属性", "[S]属性，无效由attributeValid标记", true},
                     {"attributeValid", "Tensor", "属性有效性", "[S]0/1", true},
                     {"supports", "Tensor", "源三角顶点索引", "[M,3]对应父cloud", true},
                     {"sliceCloud", "PointCloud", "截面交点", "插值几何交点，不是直接观测", true},
                     {"indices", "Indices", "交点索引", "指向sliceCloud", true},
                     image_p1::recordPort("groups", cloud_groups::kType),
                     image_p1::recordPort("features", "inspection.Profiles.v1"),
                     img::imageOut("overlay"),
                     img::imageOut("plot")};
  profile.params = {
      image_p1::text("planeFrame", "平面参考帧", "input"),
      img::intParam("sliceCount", "截面数", 100, 1, 1024, ""),
      img::enumParam("direction", "截面方向", "x", {{"x", "X", ""}, {"y", "Y", ""}}, "区域局部轴"),
      img::enumParam("attributeMode", "属性模式", "x-x",
                     {{"x-x", "横向差", ""}, {"y-y", "高度差", ""}}, ""),
      img::intParam("meshPixelDistance", "组织连接像素跨度", 1, 1, 32, "大于1显式允许跨缺测连接"),
      number("maxEdgeLength", "三角边长上限", .05, 1e-9, 1e6, "m"),
      img::intParam("visualizeSlice", "显示截面序号", 0, 0, 1023, "从0开始"),
      number("min", "属性下限", 0, 0, 1e6, "m"),
      number("max", "属性上限", .05, 0, 1e6, "m"),
      img::boolParam("includeInterpolated", "纳入补值顶点", false, "默认只使用实测栅格")};
  for (const auto* name : {"a", "b"}) {
    const std::string key = name;
    profile.params.push_back(number((key + "Center").c_str(), "特征区域中心",
                                    name[0] == 'a' ? -.001 : .001, -1e6, 1e6, "m"));
    profile.params.push_back(number((key + "Width").c_str(), "特征区域宽度", .002, 1e-9, 1e6, "m"));
    profile.params.push_back(number((key + "Gradient").c_str(), "相对最大梯度阈值", .5, 0, 1));
    profile.params.push_back(
        img::enumParam((key + "Type").c_str(), "特征类型",
                       name[0] == 'a' ? "left-right-inflection" : "right-left-inflection",
                       {{"left-right-inflection", "左到右首显著梯度", ""},
                        {"right-left-inflection", "右到左首显著梯度", ""},
                        {"top", "最高点", ""},
                        {"bottom", "最低点", ""},
                        {"left-most", "最左点", ""},
                        {"right-most", "最右点", ""}},
                       ""));
  }
  profile.capabilities.previewable = false;
  profile.compute = profiles;
  r.addOperator(std::move(profile));
}
}
