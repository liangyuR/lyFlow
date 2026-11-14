#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <opencv2/imgcodecs.hpp>

#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {
using namespace image_p1;
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr std::size_t kMaxPixels = 8U * 1024U * 1024U;
constexpr std::size_t kMaxMaskElements = 32U * 1024U * 1024U;
constexpr std::size_t kMaxRasterWork = 128U * 1024U * 1024U;

struct Failure {
  Status status;
};
void require(bool condition, const std::string& message, const char* code = "bad_input") {
  if (!condition)
    throw Failure{Status::Error(Phase::Execute, code, message, "path")};
}
void poll(ExecContext& ctx) { require(!ctx.cancelled(), "标注加载已取消", "cancelled"); }
std::string text(const Json& value, std::size_t maximum = 512) {
  require(value.is_string(), "标注字符串类型不正确");
  const auto result = value.get<std::string>();
  require(!result.empty() && result.size() <= maximum, "标注字符串为空或超过长度限制");
  for (unsigned char c : result)
    require(c >= 32 && c != 127, "标注字符串包含控制字符");
  static_cast<void>(Json(result).dump());
  return result;
}
bool validTimestamp(const std::string& value, bool legacy) {
  return !value.empty() && value.size() <= 20 && (!legacy || value.size() == 13) &&
         value.find_first_not_of("0123456789") == std::string::npos;
}
void timestamp(const std::string& value, bool legacy) {
  require(validTimestamp(value, legacy), "时间戳必须为1..20位十进制字符串；xyz格式要求13位");
}
std::string bytes(const fs::path& path, std::size_t maximum, ExecContext& ctx) {
  std::error_code ec;
  require(fs::is_regular_file(path, ec) && !ec, "数据文件不存在或不是普通文件：" + path.u8string(),
          "io_error");
  const auto size = fs::file_size(path, ec);
  require(!ec && size <= maximum, "数据文件超过读取预算", "resource_limit");
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "数据文件不能读取：" + path.u8string(), "io_error");
  std::string result;
  result.reserve(static_cast<std::size_t>(size));
  char block[65536];
  while (input) {
    poll(ctx);
    input.read(block, sizeof(block));
    const auto count = static_cast<std::size_t>(input.gcount());
    require(count <= maximum - result.size(), "读取期间数据文件超过预算", "resource_limit");
    result.append(block, count);
  }
  require(input.eof(), "读取数据文件失败", "io_error");
  return result;
}
Json jsonFile(const fs::path& path, ExecContext& ctx) {
  const auto source = bytes(path, 4U * 1024U * 1024U, ctx);
  return Json::parse(source, [&](int depth, Json::parse_event_t, Json&) {
    require(depth <= 16, "标注JSON嵌套超过16层", "resource_limit");
    return true;
  });
}
fs::path relativePath(const std::string& source) {
  auto normalized = source;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  require(!normalized.empty() && normalized.size() <= 4096 &&
              normalized.find(':') == std::string::npos,
          "数据路径必须为数据集内的相对路径");
  for (unsigned char c : normalized)
    require(c >= 32 && c != 127, "数据路径包含控制字符");
  const auto result = fs::u8path(normalized);
  require(!result.is_absolute() && !result.has_root_path(), "不允许数据集绝对路径");
  for (const auto& part : result)
    require(part != ".." && part != "." && !part.empty(), "不允许数据集路径上溯或歧义分量");
  return result;
}
fs::path inside(const fs::path& root, const std::string& relative) {
  const auto candidate = fs::weakly_canonical(root / relativePath(relative));
  auto actual = candidate.begin();
  for (auto expected = root.begin(); expected != root.end(); ++expected, ++actual)
    require(actual != candidate.end() && *expected == *actual, "数据集符号链接或路径越界");
  require(actual != candidate.end(), "数据文件不能是数据集根目录");
  return candidate;
}
std::vector<std::string> csv(const std::string& line) {
  std::vector<std::string> result;
  std::string value;
  bool quoted = false, closed = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        value += '"';
        ++i;
      } else if (c == '"') {
        quoted = false;
        closed = true;
      } else
        value += c;
    } else if (c == ',') {
      result.push_back(value);
      value.clear();
      closed = false;
    } else if (c == '"') {
      require(value.empty() && !closed, "CSV引号位置不正确");
      quoted = true;
    } else {
      require(!closed || c == ' ' || c == '\t' || c == '\r', "CSV引号结束后出现非分隔字符");
      if (!closed)
        value += c;
    }
    require(result.size() < 64 && value.size() <= 4096, "CSV列数或路径长度超限", "resource_limit");
  }
  require(!quoted, "CSV字段引号未闭合，不支持字段内换行");
  result.push_back(value);
  return result;
}
struct Sample {
  std::string image, annotation;
};
Sample sample(const fs::path& path, bool legacy, const std::string& requested, ExecContext& ctx) {
  std::map<std::string, Sample> samples;
  auto add = [&](std::string time, std::string image, std::string annotation) {
    timestamp(time, legacy);
    relativePath(image);
    relativePath(annotation);
    require(
        samples.emplace(std::move(time), Sample{std::move(image), std::move(annotation)}).second,
        "数据集时间戳重复，不允许覆盖已有样本");
    require(samples.size() <= 65536, "数据集样本超过65536项", "resource_limit");
  };
  if (legacy) {
    std::istringstream input(bytes(path, 4U * 1024U * 1024U, ctx));
    std::string line;
    while (std::getline(input, line)) {
      poll(ctx);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty() || line == "\r")
        continue;
      const auto columns = csv(line);
      require(columns.size() >= 3, "samples_v2至少需要RGB、depth、annotation三列");
      const auto name = relativePath(columns[0]).filename().u8string();
      require(name.size() >= 13, "RGB文件名缺少13位时间戳");
      add(name.substr(0, 13), columns[0], columns[2]);
    }
  } else {
    const auto index = jsonFile(path, ctx);
    require(index.is_object() && index.contains("schemaVersion") && index["schemaVersion"] == 1 &&
                index.contains("samples") && index["samples"].is_array(),
            "原生标注索引需要schemaVersion=1和samples数组");
    for (const auto& item : index["samples"]) {
      poll(ctx);
      require(item.is_object(), "数据集样本必须为对象");
      add(text(item.at("timestamp"), 20), text(item.at("image"), 4096),
          text(item.at("annotation"), 4096));
    }
  }
  const auto found = samples.find(requested);
  require(found != samples.end(), "数据集中没有指定时间戳：" + requested, "sample_not_found");
  return found->second;
}
double number(const Json& value) {
  require(values::safeNumber(value), "标注坐标或置信度必须为有限数值");
  return value.get<double>();
}
bool flag(const Json& object, const char* key, bool legacy, bool fallback = false) {
  if (!object.contains(key))
    return fallback;
  const auto& value = object[key];
  if (legacy) {
    require(value.is_number_integer() && (value == 0 || value == 1), "旧标注布尔字段必须为0/1");
    return value == 1;
  }
  require(value.is_boolean(), "原生标注布尔字段必须为bool");
  return value.get<bool>();
}
struct Part {
  std::vector<cv::Point> polygon;
  std::vector<std::vector<cv::Point>> holes;
  cv::Rect bounds;
};
struct Instance {
  std::string id, label;
  Json classId;
  double confidence = 1;
  bool pickable = false, occluded = false;
  cv::Rect2d box;
  std::vector<Part> parts;
};
struct Budget {
  std::size_t vertices = 0, raster = 0;
  void work(std::size_t count) {
    require(count <= kMaxRasterWork - raster, "标注光栅工作超过128Mi像素预算", "resource_limit");
    raster += count;
  }
};
std::vector<cv::Point> polygon(const Json& value, int width, int height, Budget& budget) {
  require(value.is_array() && value.size() >= 3, "polygon至少需要三个二维顶点");
  require(value.size() <= 100000 - budget.vertices, "polygon顶点总数超过100000", "resource_limit");
  budget.vertices += value.size();
  std::vector<cv::Point> result;
  for (const auto& item : value) {
    require(item.is_array() && item.size() == 2, "polygon顶点必须为二维数组");
    const double x = number(item[0]), y = number(item[1]);
    require(x >= 0 && y >= 0 && x <= width && y <= height, "polygon超出原图范围");
    result.emplace_back(static_cast<int>(std::lround(x * 256)),
                        static_cast<int>(std::lround(y * 256)));
  }
  require(std::abs(cv::contourArea(result)) > 0, "polygon不能退化为直线或重合点");
  return result;
}
Part part(const Json& value, int width, int height, bool legacy, Budget& budget) {
  require(value.is_object() && value.contains("polygon"), "part缺少polygon");
  Part result;
  result.polygon = polygon(value["polygon"], width, height, budget);
  if (!legacy && value.contains("holes")) {
    require(value["holes"].is_array(), "holes必须为polygon数组");
    for (const auto& hole : value["holes"])
      result.holes.push_back(polygon(hole, width, height, budget));
  }
  auto rectangle = cv::boundingRect(result.polygon);
  for (const auto& hole : result.holes)
    rectangle |= cv::boundingRect(hole);
  const int x = std::max(0, rectangle.x / 256), y = std::max(0, rectangle.y / 256);
  const int right = (rectangle.x + rectangle.width - 1 + 255) / 256 + 1;
  const int bottom = (rectangle.y + rectangle.height - 1 + 255) / 256 + 1;
  result.bounds = cv::Rect(x, y, right - x, bottom - y);
  require(!result.bounds.empty(), "polygon没有可用图像范围");
  budget.work(static_cast<std::size_t>(result.bounds.area()) * (2 + result.holes.size() * 3));
  return result;
}
cv::Rect raster(const Part& value, cv::Mat& destination, ExecContext& ctx) {
  poll(ctx);
  cv::Mat local = cv::Mat::zeros(value.bounds.size(), CV_8U);
  const cv::Point offset(-value.bounds.x * 256, -value.bounds.y * 256);
  cv::fillPoly(local, std::vector<std::vector<cv::Point>>{value.polygon}, cv::Scalar(255),
               cv::LINE_8, 8, offset);
  const cv::Mat original = value.holes.empty() ? cv::Mat{} : local.clone();
  for (const auto& hole : value.holes) {
    poll(ctx);
    cv::Mat holeMask = cv::Mat::zeros(local.size(), CV_8U), outside;
    cv::fillPoly(holeMask, std::vector<std::vector<cv::Point>>{hole}, cv::Scalar(255), cv::LINE_8,
                 8, offset);
    cv::bitwise_not(original, outside);
    cv::bitwise_and(holeMask, outside, outside);
    require(cv::countNonZero(outside) == 0, "hole超出所属part的polygon像素");
    local.setTo(0, holeMask);
  }
  const auto visible = value.bounds & cv::Rect(0, 0, destination.cols, destination.rows);
  require(!visible.empty(), "polygon没有可用图像范围");
  const auto visibleLocal = local(cv::Rect(visible.x - value.bounds.x, visible.y - value.bounds.y,
                                           visible.width, visible.height));
  auto occupied = cv::boundingRect(visibleLocal);
  if (!occupied.empty()) {
    occupied.x += visible.x;
    occupied.y += visible.y;
  }
  cv::bitwise_or(destination(visible), visibleLocal, destination(visible));
  return occupied;
}
Image loadImage(const fs::path& path, int width, int height, ExecContext& ctx) {
  const auto source = bytes(path, 32U * 1024U * 1024U, ctx);
  std::vector<unsigned char> encoded(source.begin(), source.end());
  cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
  require(!decoded.empty() && decoded.cols == width && decoded.rows == height,
          "对应图片解码失败或真实尺寸与标注不一致");
  if (decoded.channels() == 3)
    cv::cvtColor(decoded, decoded, cv::COLOR_BGR2RGB);
  else if (decoded.channels() == 4)
    cv::cvtColor(decoded, decoded, cv::COLOR_BGRA2RGBA);
  Image result;
  require(cvx::fromMat(std::move(decoded), result), "对应图片位深或通道不受支持");
  require(image(&result, "path", false, kMaxPixels).ok, "对应图片含非有限值或超过图像预算");
  return result;
}
void bounded(const Record& value) {
  std::string why;
  require(values::boundedJson(value.data, why), "标注输出元信息无效：" + why, "resource_limit");
}
Status loadAnnotations(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  try {
    const auto format = params.choice("format");
    if (format != "lyflow-annotations-v1" && format != "xyz-samples-v2")
      return Status::Error(Phase::Execute, "bad_param", "不支持的标注格式", "format");
    const bool legacy = format == "xyz-samples-v2";
    const auto requested = params.text("timestamp");
    if (!validTimestamp(requested, legacy))
      return Status::Error(Phase::Execute, "bad_param",
                           "时间戳必须为1..20位十进制字符串；xyz格式要求13位", "timestamp");
    const auto frame = text(Json(params.text("frame")), 128);
    const auto path = fs::weakly_canonical(params.path("path"));
    const auto root = path.parent_path();
    const auto selected = sample(path, legacy, requested, ctx);
    const auto annotation = jsonFile(inside(root, selected.annotation), ctx);
    require(annotation.is_object() && (legacy || (annotation.contains("schemaVersion") &&
                                                  annotation["schemaVersion"] == 1)),
            "标注需要对象及正确schemaVersion");
    require(annotation.contains("width") && annotation.contains("height") &&
                annotation["width"].is_number_integer() && annotation["height"].is_number_integer(),
            "标注缺少整数width/height");
    const auto width64 = annotation["width"].get<std::int64_t>(),
               height64 = annotation["height"].get<std::int64_t>();
    require(width64 > 0 && height64 > 0 && width64 <= 16384 && height64 <= 16384 &&
                static_cast<std::uint64_t>(width64) * static_cast<std::uint64_t>(height64) <=
                    kMaxPixels,
            "标注图像尺寸超过8Mi像素或16384边长预算", "resource_limit");
    const int width = static_cast<int>(width64), height = static_cast<int>(height64);
    if (!legacy && annotation.contains("frame"))
      require(text(annotation["frame"], 128) == frame, "标注frame与参数不一致");
    require(annotation.contains("instances") && annotation["instances"].is_array() &&
                annotation["instances"].size() <= 512,
            "instances必须为最多512项的数组");
    Budget budget;
    std::vector<Instance> instances;
    std::set<std::string> ids;
    std::size_t sourceIndex = 0, filtered = 0;
    for (const auto& value : annotation["instances"]) {
      poll(ctx);
      require(value.is_object(), "instance必须为对象");
      Instance instance;
      instance.id = value.contains("id") ? text(value["id"], 128)
                                         : "annotation-" + std::to_string(sourceIndex);
      ++sourceIndex;
      require(ids.insert(instance.id).second, "instance id重复");
      instance.label = text(value.at(legacy ? "instance_category" : "label"));
      instance.pickable = flag(value, "pickable", legacy);
      instance.occluded = flag(value, legacy ? "isoccluded" : "occluded", legacy);
      if (!legacy && value.contains("confidence"))
        instance.confidence = number(value["confidence"]);
      require(instance.confidence >= 0 && instance.confidence <= 1, "confidence必须在0..1");
      if (!legacy && value.contains("classId")) {
        require(value["classId"].is_number_integer() && number(value["classId"]) >= 0 &&
                    number(value["classId"]) <= 65534,
                "classId必须为0..65534整数");
        instance.classId = value["classId"];
      }
      const auto& box = value.at("bbox");
      require(box.is_array() && box.size() == 4, "bbox需要xywh四分量");
      instance.box = {number(box[0]), number(box[1]), number(box[2]), number(box[3])};
      require(instance.box.x >= 0 && instance.box.y >= 0 && instance.box.width > 0 &&
                  instance.box.height > 0 && instance.box.x + instance.box.width <= width &&
                  instance.box.y + instance.box.height <= height,
              "bbox尺寸非法或超出图像");
      if (value.contains("parts")) {
        require(value["parts"].is_array(), "parts必须为数组");
        for (const auto& item : value["parts"]) {
          auto region = part(item, width, height, legacy, budget);
          instance.parts.push_back(std::move(region));
        }
      }
      if ((params.flag("pickableOnly") && !instance.pickable) ||
          (!params.flag("includeOccluded") && instance.occluded)) {
        ++filtered;
        continue;
      }
      instances.push_back(std::move(instance));
    }
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    require(instances.size() <= kMaxMaskElements / pixels, "实例掩膜超过32Mi float32预算",
            "resource_limit");
    budget.work(instances.size() * pixels * 2);
    std::vector<std::pair<std::string, Part>> rois;
    std::set<std::string> roiIds;
    if (legacy && annotation.contains("roi")) {
      rois.push_back({"annotation-roi",
                      part(Json{{"polygon", annotation["roi"]}}, width, height, true, budget)});
    } else if (!legacy && annotation.contains("rois")) {
      require(annotation["rois"].is_array() && annotation["rois"].size() <= 512,
              "rois必须为最多512项的数组");
      for (const auto& value : annotation["rois"]) {
        const auto id = text(value.at("id"), 128);
        require(roiIds.insert(id).second, "ROI id重复");
        rois.push_back({id, part(value, width, height, false, budget)});
      }
    }
    Image source = loadImage(inside(root, selected.image), width, height, ctx);
    cv::Mat overlay = cvx::view(source).clone();
    cv::Mat alpha;
    if (source.channels == 4)
      cv::extractChannel(cvx::view(source), alpha, 3);
    const double maximum = source.depth == PixelDepth::U8    ? 255
                           : source.depth == PixelDepth::U16 ? 65535
                                                             : 1;
    Tensor masks;
    masks.shape = {static_cast<std::int64_t>(instances.size()), height, width};
    masks.data.assign(instances.size() * pixels, 0);
    Json poseItems = Json::array(), maskItems = Json::array(), roiItems = Json::array();
    for (std::size_t index = 0; index < instances.size(); ++index) {
      poll(ctx);
      const auto& value = instances[index];
      cv::Mat mask = cv::Mat::zeros(height, width, CV_8U);
      for (const auto& item : value.parts) {
        raster(item, mask, ctx);
      }
      for (int row = 0; row < height; ++row) {
        if (row % 64 == 0)
          poll(ctx);
        const auto* raw = mask.ptr<unsigned char>(row);
        auto* target = masks.data.data() + index * pixels + static_cast<std::size_t>(row) * width;
        for (int column = 0; column < width; ++column)
          target[column] = raw[column] ? 1.f : 0.f;
      }
      auto box = value.box;
      const auto occupied = cv::boundingRect(mask);
      const bool hasMask = !occupied.empty();
      if (!occupied.empty())
        box |= cv::Rect2d(occupied);
      auto pose =
          poses::make2D(value.id, frame, "px", {box.x + box.width / 2, box.y + box.height / 2}, 0,
                        {box.width, box.height});
      pose.data["classLabel"] = value.label;
      pose.data["confidence"] = value.confidence;
      pose.data["pickable"] = false;
      if (!value.classId.is_null())
        pose.data["classId"] = std::to_string(value.classId.get<std::int64_t>());
      pose.data["attributes"] = {{"annotationPickable", value.pickable},
                                 {"occluded", value.occluded},
                                 {"hasMask", hasMask}};
      poseItems.push_back(std::move(pose.data));
      maskItems.push_back({{"id", value.id},
                           {"index", index},
                           {"hasMask", hasMask},
                           {"source", hasMask ? "annotation_polygon" : "none"}});
      const cv::Scalar color(maximum * .25, maximum * .85, maximum * .5, maximum);
      cv::rectangle(overlay, box, color, 1);
    }
    if (!alpha.empty())
      cv::insertChannel(alpha, overlay, 3);
    cv::Mat roiMask = cv::Mat::zeros(height, width, CV_8U);
    for (const auto& value : rois) {
      poll(ctx);
      const auto box = raster(value.second, roiMask, ctx);
      if (box.empty())
        continue;
      auto pose = poses::make2D(value.first, frame, "px",
                                {box.x + box.width / 2.0, box.y + box.height / 2.0}, 0,
                                {static_cast<double>(box.width), static_cast<double>(box.height)});
      pose.data["pickable"] = false;
      roiItems.push_back(std::move(pose.data));
    }
    const auto retainedRoiCount = roiItems.size();
    Record poseRecord = poses::make2DList(std::move(poseItems), frame, "px");
    Record roiRecord = poses::make2DList(std::move(roiItems), frame, "px");
    Record maskInfo{"vision.Masks2D.v1",
                    {{"frame", frame},
                     {"unit", "px"},
                     {"width", width},
                     {"height", height},
                     {"layout", "NHW"},
                     {"items", std::move(maskItems)}}};
    Record quality{"image.Annotations.v1",
                   {{"timestamp", requested},
                    {"format", format},
                    {"image", selected.image},
                    {"annotation", selected.annotation},
                    {"width", width},
                    {"height", height},
                    {"sourceCount", sourceIndex},
                    {"retainedCount", instances.size()},
                    {"filteredCount", filtered},
                    {"roiProvided", annotation.contains(legacy ? "roi" : "rois")},
                    {"roiCount", retainedRoiCount}}};
    std::string why;
    require(poses::validate(poseRecord, why) && poses::validate(roiRecord, why),
            "标注实例结构无效：" + why);
    bounded(poseRecord);
    bounded(roiRecord);
    bounded(maskInfo);
    bounded(quality);
    Image overlayImage, roiImage;
    require(cvx::fromMat(std::move(overlay), overlayImage) &&
                cvx::fromMat(std::move(roiMask), roiImage),
            "标注输出图像无效");
    poll(ctx);
    outputs.set("image", Data::image(std::move(source)));
    outputs.set("overlay", Data::image(std::move(overlayImage)));
    outputs.set("instances", Data::record(std::move(poseRecord)));
    outputs.set("masks", Data::tensor(std::move(masks)));
    outputs.set("maskInfo", Data::record(std::move(maskInfo)));
    outputs.set("rois", Data::record(std::move(roiRecord)));
    outputs.set("roiMask", Data::image(std::move(roiImage)));
    outputs.set("quality", Data::record(std::move(quality)));
    return Status::Ok();
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "标注加载内存不足", "path");
  } catch (const std::exception& error) {
    return Status::Error(Phase::Execute, "bad_input", std::string("标注加载失败：") + error.what(),
                         "path");
  }
}
}

void registerP4Annotations(Registry& registry) {
  auto op = descriptor("image.load_annotations", "数据集标注加载",
                       "load timestamp annotations dataset polygons masks");
  op.capabilities = {true, false, false};
  op.doc =
      "按显式时间戳读取数据集内的对应图片、实例和ROI。多polygon/洞保留原生NHW掩膜，缺mask不伪造bbox分割。源pickable仅供筛选，交付实例仍不可抓取。关闭结果缓存，每次重读文件。";
  auto path = image_p1::text("path", "数据集索引", "");
  path.type = ParamType::Path;
  path.mode = "open";
  path.filters = {FileFilter{"数据集索引", {"json", "txt"}}};
  op.params = {
      path,
      img::enumParam("format", "格式", "lyflow-annotations-v1",
                     {{"lyflow-annotations-v1", "LyFlow标注v1", "带samples的JSON索引"},
                      {"xyz-samples-v2", "XYZ samples_v2", "显式旧CSV索引与标注字段"}},
                     "不自动猜测协议。"),
      image_p1::text("timestamp", "时间戳", ""),
      image_p1::text("frame", "参考帧", "image"),
      img::boolParam("pickableOnly", "按源可抓取标注筛选", false, "只筛选数据，不授予抓取许可。"),
      img::boolParam("includeOccluded", "包含遮挡标注", true, "保留源遮挡状态。")};
  op.outputs = {img::imageOut("image"),
                img::imageOut("overlay"),
                recordPort("instances", poses::kPoses2DType),
                Port{"masks", "Tensor", "Masks", "原生float32 [N,H,W]二值掩膜。", true},
                recordPort("maskInfo", "vision.Masks2D.v1"),
                recordPort("rois", poses::kPoses2DType),
                img::imageOut("roiMask"),
                recordPort("quality", "image.Annotations.v1")};
  op.compute = &loadAnnotations;
  registry.addOperator(std::move(op));
}
}
