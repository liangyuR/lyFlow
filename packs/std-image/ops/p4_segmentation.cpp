#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <vector>

#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {
using Json = nlohmann::json;
constexpr const char* kMapping = "vision.SegmentationPreprocess.v1";
constexpr std::size_t kPixels = 8 * 1024 * 1024;
constexpr std::size_t kTensor = 16 * 1024 * 1024;
constexpr std::size_t kMasks = 32 * 1024 * 1024;
constexpr std::size_t kWork = 128 * 1024 * 1024;
constexpr std::uint16_t kUnknown = 65535;

struct Failure {
  Status status;
};
void require(bool value, const std::string& message, const char* port) {
  if (!value)
    throw Failure{img::badInput(message, port)};
}
void limit(bool value, const char* message) {
  if (!value)
    throw Failure{Status::Error(Phase::Execute, "resource_limit", message)};
}
void check(ExecContext& ctx) {
  if (ctx.cancelled())
    throw Failure{Status::Error(Phase::Execute, "cancelled", "语义分割已取消")};
}
int integer(const ParamView& p, const char* name, int lo, int hi) {
  const auto v = p.integer(name);
  if (v < lo || v > hi)
    throw Failure{img::badParam("整数参数超出范围", name)};
  return static_cast<int>(v);
}
double number(const ParamView& p, const char* name, double lo, double hi) {
  const double v = p.number(name);
  if (!std::isfinite(v) || v < lo || v > hi)
    throw Failure{img::badParam("数值参数必须有限且位于声明范围", name)};
  return v;
}
bool validText(const std::string& value) {
  if (value.empty() || value.size() > 256)
    return false;
  try {
    Json(value).dump();
    return true;
  } catch (...) {
    return false;
  }
}
Json classes(const ParamView& p) {
  const auto& text = p.text("classNames");
  if (text.size() > 65536)
    throw Failure{img::badParam("类别名称超过64KiB", "classNames")};
  auto result = Json::parse(text, nullptr, false);
  std::string why;
  if (!values::boundedJson(result, why) || !result.is_array() || result.size() < 2 ||
      result.size() > 256)
    throw Failure{img::badParam("类别必须是2..256项UTF-8名称数组", "classNames")};
  std::set<std::string> unique;
  for (const auto& item : result)
    if (!item.is_string() || !validText(item.get_ref<const std::string&>()) ||
        !unique.insert(item.get<std::string>()).second)
      throw Failure{img::badParam("类别名称必须非空、唯一且为有效UTF-8", "classNames")};
  return result;
}
const Image& image(const Inputs& in) {
  const auto* result = in.get("image").asImage();
  require(result && result->consistent() && result->scale == 1 && result->depth == PixelDepth::U8 &&
              (result->channels == 1 || result->channels == 3 || result->channels == 4),
          "需要全分辨率只读u8灰度/RGB/RGBA图像", "image");
  limit(result->pixelCount() <= kPixels, "图像超过8Mi像素预算");
  return *result;
}
std::string fingerprint(const Image& image, ExecContext& ctx) {
  std::uint64_t hash = 14695981039346656037ull;
  const auto source = cvx::view(image);
  for (int y = 0; y < image.height; ++y) {
    if ((y & 31) == 0)
      check(ctx);
    const auto* row = source.ptr<std::uint8_t>(y);
    for (std::size_t x = 0; x < static_cast<std::size_t>(image.width) * image.channels; ++x) {
      hash ^= row[x];
      hash *= 1099511628211ull;
    }
  }
  std::ostringstream text;
  text << std::hex << std::setfill('0') << std::setw(16) << hash;
  return text.str();
}
struct Roi {
  std::string id;
  cv::Rect crop;
};
struct Mapping {
  int width = 0, height = 0, channels = 0, inputWidth = 0, inputHeight = 0;
  std::string frame, hash, contract, head, retainedHash;
  int foreground = 1;
  Json names, skipped, retainedIds = Json::array();
  std::vector<Roi> rois;
};
struct Retained {
  Record instances, info;
  Tensor masks;
};
Retained emptyRetained(const Image& source, const std::string& frame) {
  Retained value;
  value.instances = poses::make2DList(Json::array(), frame, "px");
  value.info = {"vision.Masks2D.v1",
                {{"frame", frame},
                 {"unit", "px"},
                 {"width", source.width},
                 {"height", source.height},
                 {"layout", "NHW"},
                 {"items", Json::array()}}};
  value.masks.shape = {0, source.height, source.width};
  return value;
}
void validateMasks(const Record* instances, const Tensor* masks, const Record* info,
                   const Image& source, const std::string& frame, ExecContext& ctx) {
  std::string why;
  require(instances && instances->type == poses::kPoses2DType && poses::validate(*instances, why) &&
              instances->data["frame"] == frame && instances->data["unit"] == "px",
          "已有实例必须为同帧px Poses2D", "roiMasks");
  const auto& items = instances->data["items"];
  require(items.size() <= 64 && masks && masks->consistent() &&
              masks->shape == std::vector<std::int64_t>{static_cast<std::int64_t>(items.size()),
                                                        source.height, source.width},
          "已有掩膜必须为与实例同序的全图NHW Tensor", "roiMasks");
  limit(masks->data.size() <= kMasks, "已有掩膜超过32Mi float元素预算");
  require(info && info->type == "vision.Masks2D.v1" && values::boundedJson(info->data, why),
          "需要有界vision.Masks2D.v1描述", "roiMaskInfo");
  const auto& d = info->data;
  require(d.is_object() && d.size() == 6 && d.value("frame", Json()) == frame &&
              d.value("unit", Json()) == "px" && d.value("width", Json()).is_number_integer() &&
              d["width"] == source.width && d.value("height", Json()).is_number_integer() &&
              d["height"] == source.height && d.value("layout", Json()) == "NHW" &&
              d.contains("items") && d["items"].is_array() && d["items"].size() == items.size(),
          "掩膜描述尺寸/帧/数量不匹配", "roiMaskInfo");
  const auto pixels = source.pixelCount();
  for (std::size_t n = 0; n < items.size(); ++n) {
    check(ctx);
    const auto& item = d["items"][n];
    require(item.is_object() && item.size() == 4 && item.value("id", Json()) == items[n]["id"] &&
                item.value("index", Json()).is_number_integer() && item["index"] == n &&
                item.contains("hasMask") && item["hasMask"].is_boolean() &&
                item.contains("source") && item["source"].is_string() &&
                validText(item["source"].get<std::string>()),
            "掩膜ID/index/hasMask/source与实例不一致", "roiMaskInfo");
    const bool present = item["hasMask"];
    require((present && item["source"] != "none") || (!present && item["source"] == "none"),
            "掩膜来源与hasMask矛盾", "roiMaskInfo");
    if (items[n].contains("attributes") && items[n]["attributes"].contains("hasMask"))
      require(items[n]["attributes"]["hasMask"] == present, "实例与掩膜hasMask冲突", "roiMaskInfo");
    if (items[n].contains("attributes") && items[n]["attributes"].contains("maskIndex"))
      require(items[n]["attributes"]["maskIndex"].is_number_integer() &&
                  items[n]["attributes"]["maskIndex"] == n,
              "实例maskIndex与原生层顺序冲突", "roiMaskInfo");
    const auto position = items[n]["position"].get<std::array<double, 2>>(),
               size = items[n]["size"].get<std::array<double, 2>>();
    require(items[n]["angle"].get<double>() == 0, "已有掩膜仅支持轴对齐ROI", "roiMasks");
    const double left = position[0] - size[0] / 2, top = position[1] - size[1] / 2,
                 right = position[0] + size[0] / 2, bottom = position[1] + size[1] / 2;
    require(std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
                std::isfinite(bottom),
            "已有掩膜ROI范围溢出", "roiMasks");
    const int x0 = static_cast<int>(std::floor(std::clamp(left, 0.0, double(source.width)))),
              y0 = static_cast<int>(std::floor(std::clamp(top, 0.0, double(source.height)))),
              x1 = static_cast<int>(std::ceil(std::clamp(right, 0.0, double(source.width)))),
              y1 = static_cast<int>(std::ceil(std::clamp(bottom, 0.0, double(source.height))));
    bool nonzero = false;
    for (std::size_t i = 0; i < pixels; ++i) {
      if ((i & 8191) == 0)
        check(ctx);
      const float value = masks->data[n * pixels + i];
      require(value == 0 || value == 1, "已有掩膜只允许有限0/1", "roiMasks");
      if (value == 1) {
        nonzero = true;
        const int x = static_cast<int>(i % source.width), y = static_cast<int>(i / source.width);
        require(present && x >= x0 && x < x1 && y >= y0 && y < y1,
                "已有掩膜越出ROI有效像素范围或hasMask=false", "roiMasks");
      }
    }
    require(nonzero == present, "hasMask=true需要非零原生支持；false层必须为空", "roiMasks");
  }
}
std::string retainedFingerprint(const Retained& value, ExecContext& ctx) {
  std::uint64_t hash = 14695981039346656037ull;
  const auto text = value.instances.data.dump() + value.info.data.dump();
  for (const unsigned char byte : text) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  for (std::size_t i = 0; i < value.masks.data.size(); ++i) {
    if ((i & 8191) == 0)
      check(ctx);
    hash ^= value.masks.data[i] == 1 ? 1 : 0;
    hash *= 1099511628211ull;
  }
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << hash;
  return stream.str();
}
Retained selectRetained(const Inputs& in, const ParamView& params, const Image& source,
                        Mapping& mapping, ExecContext& ctx) {
  auto result = emptyRetained(source, mapping.frame);
  require(in.has("roiMasks") == in.has("roiMaskInfo") && (!in.has("roiMasks") || in.has("rois")),
          "roiMasks/roiMaskInfo需要与rois一起成组输入", "roiMasks");
  if (in.has("roiMasks")) {
    const auto* instances = in.get("rois").asRecord();
    const auto* masks = in.get("roiMasks").asTensor();
    const auto* info = in.get("roiMaskInfo").asRecord();
    validateMasks(instances, masks, info, source, mapping.frame, ctx);
    if (!params.flag("replaceExistingMask")) {
      const auto pixels = source.pixelCount();
      for (std::size_t n = 0; n < instances->data["items"].size(); ++n) {
        if (!info->data["items"][n]["hasMask"].get<bool>())
          continue;
        const std::string id = instances->data["items"][n]["id"];
        auto pose = instances->data["items"][n];
        pose["pickable"] = false;
        auto item = info->data["items"][n];
        item["index"] = result.instances.data["items"].size();
        pose["attributes"]["hasMask"] = true;
        pose["attributes"]["maskIndex"] = item["index"];
        result.instances.data["items"].push_back(std::move(pose));
        result.info.data["items"].push_back(std::move(item));
        result.masks.data.insert(result.masks.data.end(), masks->data.begin() + n * pixels,
                                 masks->data.begin() + (n + 1) * pixels);
        mapping.retainedIds.push_back(id);
        mapping.rois.erase(std::remove_if(mapping.rois.begin(), mapping.rois.end(),
                                          [&](const Roi& roi) { return roi.id == id; }),
                           mapping.rois.end());
      }
      result.masks.shape[0] = static_cast<std::int64_t>(mapping.retainedIds.size());
    }
  }
  mapping.retainedHash = retainedFingerprint(result, ctx);
  return result;
}
Mapping makeMapping(const Inputs& in, const ParamView& p, const Image& source, ExecContext& ctx) {
  Mapping result;
  result.width = source.width;
  result.height = source.height;
  result.channels = source.channels;
  result.inputWidth = integer(p, "inputWidth", 1, 2048);
  result.inputHeight = integer(p, "inputHeight", 1, 2048);
  result.frame = p.text("frame");
  if (!validText(result.frame))
    throw Failure{img::badParam("参考帧必须为非空UTF-8", "frame")};
  result.names = classes(p);
  result.skipped = Json::array();
  result.contract = p.choice("modelContract");
  result.head = p.choice("headKind");
  result.foreground = integer(p, "binaryForegroundClass", 0, 1);
  if ((result.contract != "pphumanseg_2023mar" && result.contract != "semantic_nchw") ||
      (result.head != "multiclass" && result.head != "binary") ||
      (result.head == "binary" && result.names.size() != 2) ||
      (result.contract == "pphumanseg_2023mar" &&
       (result.inputWidth != 192 || result.inputHeight != 192 || result.names.size() != 2 ||
        result.head != "multiclass")))
    throw Failure{img::badParam("模型契约/分割头/类别或尺寸矛盾", "modelContract")};
  const double dilation = number(p, "bboxDilation", 1, 4);
  if (!in.has("rois"))
    result.rois.push_back({"full", {0, 0, source.width, source.height}});
  else {
    const auto* record = in.get("rois").asRecord();
    std::string why;
    require(record && record->type == poses::kPoses2DType && poses::validate(*record, why) &&
                record->data["unit"] == "px" && record->data["frame"] == result.frame,
            "ROI需要同帧px单位vision.Poses2D.v1", "rois");
    limit(record->data["items"].size() <= 64, "ROI超过64项预算");
    for (const auto& value : record->data["items"]) {
      require(value["angle"].get<double>() == 0, "ROI仅支持angle=0轴对齐矩形", "rois");
      const auto position = value["position"].get<std::array<double, 2>>();
      const auto size = value["size"].get<std::array<double, 2>>();
      const double left = position[0] - size[0] * dilation / 2,
                   right = position[0] + size[0] * dilation / 2,
                   top = position[1] - size[1] * dilation / 2,
                   bottom = position[1] + size[1] * dilation / 2;
      require(std::isfinite(left) && std::isfinite(right) && std::isfinite(top) &&
                  std::isfinite(bottom),
              "ROI膨胀几何溢出", "rois");
      const int x0 = static_cast<int>(std::floor(std::clamp(left, 0.0, double(source.width)))),
                y0 = static_cast<int>(std::floor(std::clamp(top, 0.0, double(source.height)))),
                x1 = static_cast<int>(std::ceil(std::clamp(right, 0.0, double(source.width)))),
                y1 = static_cast<int>(std::ceil(std::clamp(bottom, 0.0, double(source.height))));
      const std::string id = value["id"];
      require(validText(id), "ROI ID必须为1..256字节UTF-8", "rois");
      if (x1 <= x0 || y1 <= y0)
        result.skipped.push_back(id);
      else
        result.rois.push_back({id, {x0, y0, x1 - x0, y1 - y0}});
    }
  }
  result.hash = fingerprint(source, ctx);
  return result;
}
Record mappingRecord(const Mapping& mapping) {
  Json rois = Json::array();
  for (const auto& roi : mapping.rois)
    rois.push_back(
        {{"id", roi.id}, {"crop", {roi.crop.x, roi.crop.y, roi.crop.width, roi.crop.height}}});
  return {kMapping,
          {{"frame", mapping.frame},
           {"unit", "px"},
           {"width", mapping.width},
           {"height", mapping.height},
           {"channels", mapping.channels},
           {"inputSize", {mapping.inputWidth, mapping.inputHeight}},
           {"layout", "NCHW"},
           {"modelContract", mapping.contract},
           {"headKind", mapping.head},
           {"binaryForegroundClass", mapping.foreground},
           {"classNames", mapping.names},
           {"sourceFingerprint", mapping.hash},
           {"rois", std::move(rois)},
           {"skippedRoiIds", mapping.skipped},
           {"retainedIds", mapping.retainedIds},
           {"retainedFingerprint", mapping.retainedHash}}};
}
bool intJson(const Json& value, int lo, int hi) {
  return value.is_number_integer() && value >= lo && value <= hi;
}
Mapping readMapping(const Record* record, const Image& source, ExecContext& ctx) {
  std::string why;
  require(record && record->type == kMapping && values::boundedJson(record->data, why),
          "需要有界vision.SegmentationPreprocess.v1", "mapping");
  const auto& d = record->data;
  require(d.is_object() && d.size() == 16 && d.value("unit", Json()) == "px" &&
              d.value("layout", Json()) == "NCHW" &&
              (d.value("modelContract", Json()) == "pphumanseg_2023mar" ||
               d.value("modelContract", Json()) == "semantic_nchw") &&
              intJson(d.value("width", Json()), 1, static_cast<int>(kPixels)) &&
              intJson(d.value("height", Json()), 1, static_cast<int>(kPixels)) &&
              intJson(d.value("channels", Json()), 1, 4) &&
              d.value("width", Json()) == source.width &&
              d.value("height", Json()) == source.height &&
              d.value("channels", Json()) == source.channels,
          "预处理元数据与图像/模型契约不符", "mapping");
  require(d.contains("inputSize") && d["inputSize"].is_array() && d["inputSize"].size() == 2 &&
              intJson(d["inputSize"][0], 1, 2048) && intJson(d["inputSize"][1], 1, 2048) &&
              d.contains("frame") && d["frame"].is_string() &&
              validText(d["frame"].get<std::string>()) && d.contains("sourceFingerprint") &&
              d["sourceFingerprint"].is_string() &&
              d["sourceFingerprint"] == fingerprint(source, ctx),
          "预处理尺寸/参考帧/源图像指纹无效", "mapping");
  Mapping m;
  m.width = source.width;
  m.height = source.height;
  m.channels = source.channels;
  m.inputWidth = d["inputSize"][0];
  m.inputHeight = d["inputSize"][1];
  m.frame = d["frame"];
  m.contract = d["modelContract"];
  require(d.contains("classNames") && d["classNames"].is_array() && d["classNames"].size() >= 2 &&
              d["classNames"].size() <= 256 && d.contains("rois") && d["rois"].is_array() &&
              d["rois"].size() <= 64 && d.contains("skippedRoiIds") &&
              d["skippedRoiIds"].is_array() && d.contains("retainedIds") &&
              d["retainedIds"].is_array() &&
              d["rois"].size() + d["skippedRoiIds"].size() + d["retainedIds"].size() <= 64 &&
              d.contains("retainedFingerprint") && d["retainedFingerprint"].is_string(),
          "类别或ROI映射非法", "mapping");
  require(
      (d.value("headKind", Json()) == "multiclass" || d.value("headKind", Json()) == "binary") &&
          intJson(d.value("binaryForegroundClass", Json()), 0, 1),
      "分割头映射非法", "mapping");
  m.head = d["headKind"];
  m.foreground = d["binaryForegroundClass"];
  require((m.head != "binary" || d["classNames"].size() == 2) &&
              (m.contract != "pphumanseg_2023mar" ||
               (m.head == "multiclass" && m.inputWidth == 192 && m.inputHeight == 192 &&
                d["classNames"].size() == 2)),
          "模型契约与分割头/类别/尺寸矛盾", "mapping");
  std::set<std::string> names, ids;
  m.retainedIds = d["retainedIds"];
  m.retainedHash = d["retainedFingerprint"];
  require(m.retainedHash.size() == 16 &&
              m.retainedHash.find_first_not_of("0123456789abcdef") == std::string::npos,
          "保留结果指纹格式无效", "mapping");
  for (const auto& id : m.retainedIds)
    require(id.is_string() && validText(id.get_ref<const std::string&>()) &&
                ids.insert(id.get<std::string>()).second,
            "保留实例ID非法或重复", "mapping");
  for (const auto& item : d["classNames"])
    require(item.is_string() && validText(item.get_ref<const std::string&>()) &&
                names.insert(item.get<std::string>()).second,
            "类别名称必须唯一UTF-8", "mapping");
  for (const auto& item : d["skippedRoiIds"])
    require(item.is_string() && validText(item.get_ref<const std::string&>()) &&
                ids.insert(item.get<std::string>()).second,
            "空ROI的ID非法或重复", "mapping");
  for (const auto& item : d["rois"]) {
    require(item.is_object() && item.size() == 2 && item.contains("id") && item["id"].is_string() &&
                validText(item["id"].get_ref<const std::string&>()) &&
                ids.insert(item["id"].get<std::string>()).second && item.contains("crop") &&
                item["crop"].is_array() && item["crop"].size() == 4,
            "ROI索引格式/ID非法", "mapping");
    const auto& crop = item["crop"];
    require(intJson(crop[0], 0, m.width - 1) && intJson(crop[1], 0, m.height - 1) &&
                intJson(crop[2], 1, m.width) && intJson(crop[3], 1, m.height),
            "ROI裁剪尺寸非法", "mapping");
    cv::Rect rect(crop[0].get<int>(), crop[1].get<int>(), crop[2].get<int>(), crop[3].get<int>());
    require(rect.x <= m.width - rect.width && rect.y <= m.height - rect.height, "ROI裁剪越界",
            "mapping");
    m.rois.push_back({item["id"].get<std::string>(), rect});
  }
  m.names = d["classNames"];
  m.skipped = d["skippedRoiIds"];
  return m;
}
Retained readRetained(const Inputs& in, const Image& source, const Mapping& mapping,
                      ExecContext& ctx) {
  const bool any =
      in.has("retainedInstances") || in.has("retainedMasks") || in.has("retainedMaskInfo");
  require(!any || (in.has("retainedInstances") && in.has("retainedMasks") &&
                   in.has("retainedMaskInfo")),
          "保留实例/掩膜/描述必须成组输入", "retainedMasks");
  auto result = emptyRetained(source, mapping.frame);
  if (any) {
    const auto* instances = in.get("retainedInstances").asRecord();
    const auto* masks = in.get("retainedMasks").asTensor();
    const auto* info = in.get("retainedMaskInfo").asRecord();
    validateMasks(instances, masks, info, source, mapping.frame, ctx);
    result.instances = *instances;
    result.info = *info;
    result.masks = *masks;
  }
  Json ids = Json::array();
  for (const auto& pose : result.instances.data["items"]) {
    require(pose.contains("pickable") && pose["pickable"] == false, "保留结果不得携带抓取许可",
            "retainedInstances");
    ids.push_back(pose["id"]);
  }
  require(ids == mapping.retainedIds && retainedFingerprint(result, ctx) == mapping.retainedHash,
          "保留结果与预处理映射不配对", "retainedMasks");
  return result;
}
Status preprocess(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    check(ctx);
    const auto& source = image(in);
    auto mapping = makeMapping(in, p, source, ctx);
    auto retained = selectRetained(in, p, source, mapping, ctx);
    limit(mapping.rois.size() * static_cast<std::size_t>(mapping.inputWidth) * mapping.inputHeight *
                  3 <=
              kTensor,
          "预处理输入超过16Mi float元素预算");
    Tensor tensor;
    tensor.shape = {static_cast<std::int64_t>(mapping.rois.size()), 3, mapping.inputHeight,
                    mapping.inputWidth};
    const auto plane = static_cast<std::size_t>(mapping.inputWidth) * mapping.inputHeight;
    tensor.data.resize(mapping.rois.size() * 3 * plane);
    for (std::size_t n = 0; n < mapping.rois.size(); ++n) {
      check(ctx);
      cv::Mat rgb, resized;
      const auto crop = cvx::view(source)(mapping.rois[n].crop);
      if (source.channels == 3)
        rgb = crop;
      else
        cv::cvtColor(crop, rgb, source.channels == 1 ? cv::COLOR_GRAY2RGB : cv::COLOR_RGBA2RGB);
      cv::resize(rgb, resized, {mapping.inputWidth, mapping.inputHeight}, 0, 0, cv::INTER_LINEAR);
      for (int y = 0; y < resized.rows; ++y) {
        if ((y & 31) == 0)
          check(ctx);
        for (int x = 0; x < resized.cols; ++x)
          for (int c = 0; c < 3; ++c)
            tensor.data[(n * 3 + c) * plane + static_cast<std::size_t>(y) * resized.cols + x] =
                resized.ptr<cv::Vec3b>(y)[x][c] / 127.5f - 1.f;
      }
    }
    auto record = mappingRecord(mapping);
    std::string why;
    limit(values::boundedJson(record.data, why), "预处理元数据超出Record预算");
    check(ctx);
    out.set("tensor", Data::tensor(std::move(tensor)));
    out.set("mapping", Data::record(std::move(record)));
    out.set("retainedInstances", Data::record(std::move(retained.instances)));
    out.set("retainedMasks", Data::tensor(std::move(retained.masks)));
    out.set("retainedMaskInfo", Data::record(std::move(retained.info)));
    return Status::Ok();
  } catch (const Failure& e) {
    return e.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "分割预处理内存不足");
  } catch (const std::exception& e) {
    return img::badInput(e.what());
  }
}

Status decode(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    check(ctx);
    const auto& source = image(in);
    const auto mapping = readMapping(in.get("mapping").asRecord(), source, ctx);
    auto retained = readRetained(in, source, mapping, ctx);
    const auto retainedCount = retained.instances.data["items"].size();
    const auto* output = in.get("output").asTensor();
    require(output && output->consistent(), "输出必须为形状一致的float32 Tensor", "output");
    const auto batch = mapping.rois.size(), count = mapping.names.size();
    const double threshold = number(p, "maskThreshold", 0, 1),
                 alpha = number(p, "overlayAlpha", 0, 1);
    const int background = integer(p, "backgroundClass", -1, static_cast<int>(count) - 1),
              minArea = integer(p, "minArea", 1, static_cast<int>(kPixels)),
              maxInstances = integer(p, "maxInstances", 1, 256);
    limit(retainedCount <= static_cast<std::size_t>(maxInstances), "保留实例超过maxInstances预算");
    const auto& mode = p.choice("outputKind");
    const auto& prefix = p.text("idPrefix");
    if (!validText(prefix) || prefix.size() > 192)
      throw Failure{img::badParam("实例ID前缀必须为1..192字节UTF-8", "idPrefix")};
    if (mode != "probabilities" && mode != "logits")
      throw Failure{img::badParam("输出类型仅允许probabilities/logits", "outputKind")};
    if (mapping.contract == "pphumanseg_2023mar" && mode != "probabilities")
      throw Failure{img::badParam("PPHumanSeg输出已是概率，禁止重复softmax", "outputKind")};
    if (mapping.head == "binary" && background != -1 && background == mapping.foreground)
      throw Failure{img::badParam("二值前景不得同时声明为背景", "backgroundClass")};
    const auto channels = mapping.head == "binary" ? std::size_t(1) : count;
    if (batch == 0)
      require(
          output->data.empty() &&
              (output->shape == std::vector<std::int64_t>{0} ||
               output->shape == std::vector<std::int64_t>{0, static_cast<std::int64_t>(channels),
                                                          mapping.inputHeight, mapping.inputWidth}),
          "空ROI只接受规范空输出，不得回退整图", "output");
    else
      require(output->shape == std::vector<std::int64_t>{static_cast<std::int64_t>(batch),
                                                         static_cast<std::int64_t>(channels),
                                                         mapping.inputHeight, mapping.inputWidth},
              "分割头必须为与ROI/类别匹配的NCHW单头", "output");
    limit(output->data.size() <= kTensor, "分割输出超过16Mi float元素预算");
    std::size_t work = 0, largest = 0;
    for (const auto& roi : mapping.rois) {
      const auto area = static_cast<std::size_t>(roi.crop.area());
      limit(area <= (kWork - work) / count, "ROI回贴×类别超过128Mi像素检查预算");
      work += area * count;
      largest = std::max(largest, area);
    }
    const auto pixels = source.pixelCount();
    const std::size_t fixedMemory =
        pixels * 24 + output->data.size() * 12 + largest * 20 + retained.masks.data.size() * 4;
    limit(fixedMemory <= 512ULL * 1024 * 1024, "分割临时内存超过512MiB预算");
    limit(fixedMemory + retained.masks.data.size() * 12 <= 512ULL * 1024 * 1024,
          "保留掩膜及临时内存超过512MiB预算");
    const auto plane = static_cast<std::size_t>(mapping.inputWidth) * mapping.inputHeight;
    std::vector<float> probabilities;
    const std::vector<float>* values = &output->data;
    limit(batch * count * plane <= kTensor, "展开后的类别概率超过16Mi float元素预算");
    if (mode == "logits" || mapping.head == "binary") {
      probabilities.resize(batch * count * plane);
      values = &probabilities;
    }
    for (std::size_t n = 0; n < batch; ++n)
      for (std::size_t i = 0; i < plane; ++i) {
        if ((i & 8191) == 0)
          check(ctx);
        if (mapping.head == "binary") {
          const float v = output->data[n * plane + i];
          require(std::isfinite(v) && (mode == "logits" || (v >= 0 && v <= 1)),
                  "二值分割头含非法值", "output");
          const double e = std::exp(v >= 0 ? -double(v) : double(v));
          const float probability =
              mode == "probabilities" ? v : static_cast<float>(v >= 0 ? 1 / (1 + e) : e / (1 + e));
          probabilities[(n * 2 + mapping.foreground) * plane + i] = probability;
          probabilities[(n * 2 + 1 - mapping.foreground) * plane + i] = 1 - probability;
          continue;
        }
        double maximum = -std::numeric_limits<double>::infinity(), sum = 0;
        for (std::size_t c = 0; c < count; ++c) {
          const float value = output->data[(n * count + c) * plane + i];
          require(std::isfinite(value) && (mode == "logits" || (value >= 0 && value <= 1)),
                  "分割输出含非有限或越界概率", "output");
          maximum = std::max(maximum, double(value));
          if (mode == "probabilities")
            sum += value;
        }
        if (mode == "probabilities")
          require(std::abs(sum - 1) <= 1e-3, "各类别概率之和必须为1", "output");
        else {
          for (std::size_t c = 0; c < count; ++c) {
            const auto index = (n * count + c) * plane + i;
            probabilities[index] =
                static_cast<float>(std::exp(double(output->data[index]) - maximum));
            sum += probabilities[index];
          }
          for (std::size_t c = 0; c < count; ++c)
            probabilities[(n * count + c) * plane + i] /= static_cast<float>(sum);
        }
      }
    cv::Mat labels(source.height, source.width, CV_16UC1, cv::Scalar(kUnknown));
    cv::Mat confidence(source.height, source.width, CV_32FC1, cv::Scalar(-1));
    Tensor masks = std::move(retained.masks);
    Json instances = std::move(retained.instances.data["items"]),
         bindings = std::move(retained.info.data["items"]);
    std::set<std::string> outputIds;
    for (const auto& pose : instances)
      outputIds.insert(pose["id"].get<std::string>());
    for (std::size_t n = 0; n < batch; ++n) {
      check(ctx);
      const auto& roi = mapping.rois[n];
      cv::Mat localLabels(roi.crop.size(), CV_16UC1, cv::Scalar(kUnknown)),
          localBest(roi.crop.size(), CV_32FC1, cv::Scalar(-1)), resized;
      for (std::size_t c = 0; c < count; ++c) {
        check(ctx);
        cv::Mat probability(mapping.inputHeight, mapping.inputWidth, CV_32FC1,
                            const_cast<float*>(values->data() + (n * count + c) * plane));
        cv::resize(probability, resized, roi.crop.size(), 0, 0, cv::INTER_LINEAR);
        for (int y = 0; y < roi.crop.height; ++y) {
          if ((y & 31) == 0)
            check(ctx);
          for (int x = 0; x < roi.crop.width; ++x)
            if (resized.ptr<float>(y)[x] > localBest.ptr<float>(y)[x]) {
              localBest.ptr<float>(y)[x] = resized.ptr<float>(y)[x];
              localLabels.ptr<std::uint16_t>(y)[x] = static_cast<std::uint16_t>(c);
            }
        }
      }
      for (int y = 0; y < roi.crop.height; ++y) {
        if ((y & 31) == 0)
          check(ctx);
        for (int x = 0; x < roi.crop.width; ++x) {
          const float score = localBest.ptr<float>(y)[x];
          if (score < threshold)
            localLabels.ptr<std::uint16_t>(y)[x] = kUnknown;
          else if (score > confidence.ptr<float>(y + roi.crop.y)[x + roi.crop.x]) {
            confidence.ptr<float>(y + roi.crop.y)[x + roi.crop.x] = score;
            labels.ptr<std::uint16_t>(y + roi.crop.y)[x + roi.crop.x] =
                localLabels.ptr<std::uint16_t>(y)[x];
          }
        }
      }
      cv::Mat visited(roi.crop.size(), CV_8UC1, cv::Scalar(0));
      std::vector<int> queue;
      for (int y = 0; y < roi.crop.height; ++y) {
        if ((y & 31) == 0)
          check(ctx);
        for (int x = 0; x < roi.crop.width; ++x) {
          const auto cls = localLabels.ptr<std::uint16_t>(y)[x];
          if (visited.ptr<std::uint8_t>(y)[x] || cls == kUnknown || cls == background)
            continue;
          queue.clear();
          queue.push_back(y * roi.crop.width + x);
          visited.ptr<std::uint8_t>(y)[x] = 1;
          int x0 = x, x1 = x, y0 = y, y1 = y;
          double sum = 0;
          for (std::size_t head = 0; head < queue.size(); ++head) {
            if ((head & 8191) == 0)
              check(ctx);
            const int px = queue[head] % roi.crop.width, py = queue[head] / roi.crop.width;
            x0 = std::min(x0, px);
            x1 = std::max(x1, px);
            y0 = std::min(y0, py);
            y1 = std::max(y1, py);
            sum += localBest.ptr<float>(py)[px];
            for (int dy = -1; dy <= 1; ++dy)
              for (int dx = -1; dx <= 1; ++dx) {
                const int nx = px + dx, ny = py + dy;
                if (nx >= 0 && ny >= 0 && nx < roi.crop.width && ny < roi.crop.height &&
                    !visited.ptr<std::uint8_t>(ny)[nx] &&
                    localLabels.ptr<std::uint16_t>(ny)[nx] == cls) {
                  visited.ptr<std::uint8_t>(ny)[nx] = 1;
                  queue.push_back(ny * roi.crop.width + nx);
                }
              }
          }
          if (queue.size() < static_cast<std::size_t>(minArea))
            continue;
          limit(instances.size() < static_cast<std::size_t>(maxInstances),
                "实例数量超过maxInstances预算");
          limit(pixels <= kMasks - masks.data.size(), "实例全图NHW掩膜超过32Mi float元素预算");
          limit(fixedMemory + (masks.data.size() + pixels) * 12 <= 512ULL * 1024 * 1024,
                "实例掩膜及临时内存超过512MiB预算");
          const auto index = instances.size(), offset = masks.data.size();
          masks.data.resize(offset + pixels, 0);
          for (std::size_t q = 0; q < queue.size(); ++q) {
            if ((q & 8191) == 0)
              check(ctx);
            const int value = queue[q];
            masks
                .data[offset +
                      static_cast<std::size_t>(value / roi.crop.width + roi.crop.y) * source.width +
                      value % roi.crop.width + roi.crop.x] = 1;
          }
          const std::string id = prefix + "-" + std::to_string(n) + "-" + std::to_string(cls) +
                                 "-" + std::to_string(index);
          require(outputIds.insert(id).second, "新实例ID与保留结果冲突，请更改idPrefix",
                  "retainedInstances");
          auto pose =
              poses::make2D(id, mapping.frame, "px",
                            {roi.crop.x + (x0 + x1 + 1) / 2.0, roi.crop.y + (y0 + y1 + 1) / 2.0}, 0,
                            {double(x1 - x0 + 1), double(y1 - y0 + 1)});
          pose.data["classId"] = std::to_string(cls);
          pose.data["classLabel"] = mapping.names[cls];
          pose.data["confidence"] = std::clamp(sum / queue.size(), 0.0, 1.0);
          pose.data["pickable"] = false;
          pose.data["attributes"] = {{"roiId", roi.id},
                                     {"maskIndex", index},
                                     {"pixelCount", queue.size()},
                                     {"hasMask", true},
                                     {"source", "semantic_connected_component"}};
          instances.push_back(std::move(pose.data));
          bindings.push_back({{"id", id},
                              {"index", index},
                              {"hasMask", true},
                              {"source", "semantic_connected_component"}});
        }
      }
    }
    cv::Mat retainedOwner;
    std::size_t unmappedRetained = 0;
    if (retainedCount)
      retainedOwner = cv::Mat(source.height, source.width, CV_16UC1, cv::Scalar(kUnknown));
    for (std::size_t n = 0; n < retainedCount; ++n) {
      int cls = -1;
      const auto& pose = instances[n];
      if (pose.contains("classId")) {
        const auto id = pose["classId"].get<std::string>();
        if (id.size() <= 3 && !id.empty() &&
            id.find_first_not_of("0123456789") == std::string::npos) {
          int value = 0;
          for (char c : id)
            value = value * 10 + c - '0';
          if (static_cast<std::size_t>(value) < count && std::to_string(value) == id &&
              (!pose.contains("classLabel") || pose["classLabel"] == mapping.names[value]))
            cls = value;
        }
      }
      if (cls < 0)
        ++unmappedRetained;
      for (std::size_t i = 0; i < pixels; ++i) {
        if ((i & 8191) == 0)
          check(ctx);
        if (masks.data[n * pixels + i] != 1)
          continue;
        const int x = static_cast<int>(i % source.width), y = static_cast<int>(i / source.width);
        if (retainedOwner.ptr<std::uint16_t>(y)[x] != kUnknown)
          continue;
        retainedOwner.ptr<std::uint16_t>(y)[x] = static_cast<std::uint16_t>(n);
        labels.ptr<std::uint16_t>(y)[x] = cls < 0 ? kUnknown : static_cast<std::uint16_t>(cls);
      }
    }
    masks.shape[0] = static_cast<std::int64_t>(instances.size());
    cv::Mat overlay;
    if (source.channels == 1)
      cv::cvtColor(cvx::view(source), overlay, cv::COLOR_GRAY2RGB);
    else
      overlay = cvx::view(source).clone();
    for (int y = 0; y < source.height; ++y) {
      if ((y & 31) == 0)
        check(ctx);
      for (int x = 0; x < source.width; ++x) {
        const auto cls = labels.ptr<std::uint16_t>(y)[x];
        const auto owner =
            retainedOwner.empty() ? kUnknown : retainedOwner.ptr<std::uint16_t>(y)[x];
        if (owner == kUnknown && (cls == kUnknown || cls == background))
          continue;
        const int palette = owner == kUnknown ? cls : owner + 1;
        const std::array<int, 3> color{(53 * palette + 89) % 256, (97 * palette + 157) % 256,
                                       (193 * palette + 43) % 256};
        auto* pixel = overlay.ptr<std::uint8_t>(y) + x * overlay.channels();
        for (int c = 0; c < 3; ++c)
          pixel[c] = cv::saturate_cast<std::uint8_t>((1 - alpha) * pixel[c] + alpha * color[c]);
      }
    }
    auto posesRecord = poses::make2DList(std::move(instances), mapping.frame, "px");
    Record maskRecord{"vision.Masks2D.v1",
                      {{"frame", mapping.frame},
                       {"unit", "px"},
                       {"width", source.width},
                       {"height", source.height},
                       {"layout", "NHW"},
                       {"items", std::move(bindings)}}};
    Record quality{
        "vision.SemanticSegmentation.v1",
        {{"frame", mapping.frame},
         {"unit", "px"},
         {"classNames", mapping.names},
         {"unknownValue", kUnknown},
         {"backgroundClass", background},
         {"roiCount", batch},
         {"skippedRoiIds", mapping.skipped},
         {"instanceCount", masks.shape[0]},
         {"retainedCount", retainedCount},
         {"inferredInstanceCount", static_cast<std::size_t>(masks.shape[0]) - retainedCount},
         {"retainedUnmappedClasses", unmappedRetained},
         {"maskThreshold", threshold},
         {"minArea", minArea},
         {"connectivity", 8},
         {"sourceFingerprint", fingerprint(source, ctx)}}};
    std::string why;
    require(poses::validate(posesRecord, why), why, "instances");
    limit(values::boundedJson(maskRecord.data, why) && values::boundedJson(quality.data, why),
          "结果Record超过预算");
    Image labelImage, overlayImage;
    require(cvx::fromMat(std::move(labels), labelImage) &&
                cvx::fromMat(std::move(overlay), overlayImage),
            "原生输出图像转换失败", "output");
    check(ctx);
    out.set("labels", Data::image(std::move(labelImage)));
    out.set("masks", Data::tensor(std::move(masks)));
    out.set("maskInfo", Data::record(std::move(maskRecord)));
    out.set("instances", Data::record(std::move(posesRecord)));
    out.set("overlay", Data::image(std::move(overlayImage)));
    out.set("quality", Data::record(std::move(quality)));
    return Status::Ok();
  } catch (const Failure& e) {
    return e.status;
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "分割解码内存不足");
  } catch (const std::exception& e) {
    return img::badInput(e.what(), "mapping");
  }
}
}

void registerP4Segmentation(Registry& r) {
  auto pre = image_p1::descriptor("image.segmentation_preprocess", "语义分割预处理",
                                  "semantic segmentation preprocess PPHumanSeg ROI");
  pre.doc =
      "全分辨率u8灰度/RGB/RGBA按轴对齐ROI裁剪并拉伸到NCHW RGB [-1,1]。未连ROI使用整图，已连空ROI保持N=0；最多64ROI。PPHumanSeg固定192×192，ONNX节点使用pphumanseg_2023mar与batchMode=serial。";
  pre.capabilities = {true, false, true};
  pre.inputs = {img::imageIn(), image_p1::recordPort("rois", poses::kPoses2DType, false),
                Port{"roiMasks", "Tensor", "Existing Masks", "与rois同序的全图NHW 0/1", false},
                image_p1::recordPort("roiMaskInfo", "vision.Masks2D.v1", false)};
  pre.outputs = {Port{"tensor", "Tensor", "Input", "float32 [N,3,H,W]", true},
                 image_p1::recordPort("mapping", kMapping),
                 image_p1::recordPort("retainedInstances", poses::kPoses2DType),
                 Port{"retainedMasks", "Tensor", "Retained Masks", "保留的原生NHW支持", true},
                 image_p1::recordPort("retainedMaskInfo", "vision.Masks2D.v1")};
  pre.params = {
      img::enumParam("modelContract", "模型契约", "pphumanseg_2023mar",
                     {{"pphumanseg_2023mar", "PPHumanSeg 2023mar", "固定192×192及2类概率"},
                      {"semantic_nchw", "显式通用NCHW", "仅RGB[-1,1]，归一化不自动推断"}},
                     "选择明确模型族"),
      img::enumParam(
          "headKind", "分割头", "multiclass",
          {{"multiclass", "多类", "C>=2 argmax"}, {"binary", "二值单通道", "C=1前景概率或logit"}},
          "二值仅通用契约"),
      img::intParam("binaryForegroundClass", "二值前景类索引", 1, 0, 1,
                    "二值输出通道对应两个类别名称中的哪一个"),
      img::intParam("inputWidth", "模型宽", 192, 1, 2048, "模型输入宽"),
      img::intParam("inputHeight", "模型高", 192, 1, 2048, "模型输入高"),
      image_p1::text("frame", "参考帧", "image"),
      image_p1::text("classNames", "类别JSON", "[\"background\",\"person\"]"),
      img::boolParam("replaceExistingMask", "替换已有掩膜", true,
                     "false逐ROI保留真实有效支持，只推理缺掩膜ROI"),
      img::floatParam("bboxDilation", "ROI膨胀", 1.1, "中心膨胀1..4并裁到图内")};
  pre.compute = &preprocess;
  r.addOperator(std::move(pre));
  auto post = image_p1::descriptor("image.segmentation_decode", "语义分割解码",
                                   "semantic segmentation masks instances overlay");
  post.doc =
      "单float32 NCHW logits/概率→全图u16类别图(65535未知)、重叠NHW二值实例Tensor与同序ID描述、像素Poses2D及只读叠加。概率先双线性回贴再argmax；8连通分量是语义实例支持，不是学习实例分割，也不授权抓取。";
  post.capabilities = {true, false, true};
  post.inputs = {
      img::imageIn(),
      Port{"output", "Tensor", "Output", "单float32 NCHW分割头", true},
      image_p1::recordPort("mapping", kMapping),
      image_p1::recordPort("retainedInstances", poses::kPoses2DType, false),
      Port{"retainedMasks", "Tensor", "Retained Masks", "与mapping配对的原生保留支持", false},
      image_p1::recordPort("retainedMaskInfo", "vision.Masks2D.v1", false)};
  post.outputs = {img::imageOut("labels", "u16全图类别；65535为unknown"),
                  Port{"masks", "Tensor", "Masks", "float32 [N,H,W] 0/1，可保留重叠", true},
                  image_p1::recordPort("maskInfo", "vision.Masks2D.v1"),
                  image_p1::recordPort("instances", poses::kPoses2DType),
                  img::imageOut("overlay", "彩色叠加；RGBA保留alpha"),
                  image_p1::recordPort("quality", "vision.SemanticSegmentation.v1")};
  post.params = {
      img::enumParam(
          "outputKind", "输出数值", "probabilities",
          {{"probabilities", "概率", "要求0..1且类别和1"}, {"logits", "Logits", "稳定softmax"}},
          "PPHumanSeg输出已是概率"),
      img::floatParam("maskThreshold", "分割阈值", .5, "最大类别概率低于阈值为unknown"),
      img::intParam("backgroundClass", "背景类别", 0, -1, 255, "背景不输出实例，-1无背景"),
      img::intParam("minArea", "最小像素面积", 1, 1, kPixels, "原图8连通分量面积"),
      img::intParam("maxInstances", "实例预算", 256, 1, 256, "超过预算原子失败，不静默截断"),
      image_p1::text("idPrefix", "实例ID前缀", "semantic"),
      img::floatParam("overlayAlpha", "叠加比例", .5, "0..1，仅混合RGB，alpha保持只读")};
  post.compute = &decode;
  r.addOperator(std::move(post));
}
} // namespace lyflow::ops
