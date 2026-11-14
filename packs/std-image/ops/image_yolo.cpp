#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "detection_common.h"
#include "lyflow/pose.h"
#include "params.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

constexpr const char* kMappingType = "vision.YOLOPreprocess.v1";
using Json = nlohmann::json;

bool familyValid(const std::string& family) {
  return family == "yolov8" || family == "yolo11" || family == "yolo26";
}

int roundedSize(double value) {
  const double base = std::floor(value), part = value - base;
  return static_cast<int>(base + (part > 0.5 || (part == 0.5 && std::fmod(base, 2) != 0)));
}

bool integer(const Json& value, int& result, int minimum, int maximum) {
  if (!value.is_number_integer() || value < minimum || value > maximum)
    return false;
  result = value.get<int>();
  return true;
}

bool classesValid(const Json& value) {
  if (!value.is_array() || value.empty() || value.size() > 512)
    return false;
  for (const auto& item : value)
    if (!item.is_string() || item.get_ref<const std::string&>().empty() ||
        item.get_ref<const std::string&>().size() > 256)
      return false;
  return true;
}

struct Mapping {
  std::string family, format, frame;
  int width = 0, height = 0, scale = 1, x = 0, y = 0, cropWidth = 0, cropHeight = 0;
  int inputWidth = 0, inputHeight = 0, resizedWidth = 0, resizedHeight = 0, left = 0, top = 0;
  Json classes;
};

bool parseMapping(const Record* record, Mapping& m, std::string& why) {
  if (!record || record->type != kMappingType || !values::boundedJson(record->data, why)) {
    why = "需要有界 vision.YOLOPreprocess.v1 元数据";
    return false;
  }
  const auto& d = record->data;
  if (!d.is_object() || d.size() != 10)
    return false;
  for (const auto& pair : std::vector<std::pair<const char*, int>>{
           {"inputSize", 2}, {"workingSize", 2}, {"crop", 4}, {"resizedSize", 2}, {"padding", 2}})
    if (!d.contains(pair.first) || !d[pair.first].is_array() ||
        d[pair.first].size() != static_cast<std::size_t>(pair.second))
      return false;
  for (const char* key : {"modelFamily", "outputFormat", "frame"})
    if (!d.contains(key) || !d[key].is_string())
      return false;
  m.family = d["modelFamily"];
  m.format = d["outputFormat"];
  m.frame = d["frame"];
  if (!familyValid(m.family) || (m.format != "raw" && m.format != "end2end") || m.frame.empty() ||
      m.frame.size() > 256 || !d.contains("classNames") || !classesValid(d["classNames"]) ||
      !integer(d["inputSize"][0], m.inputWidth, 32, 2048) ||
      !integer(d["inputSize"][1], m.inputHeight, 32, 2048) || m.inputWidth % 32 ||
      m.inputHeight % 32 || !integer(d["workingSize"][0], m.width, 1, 1 << 20) ||
      !integer(d["workingSize"][1], m.height, 1, 1 << 20) ||
      !integer(d.value("imageScale", Json()), m.scale, 1, 1 << 20) ||
      !integer(d["crop"][0], m.x, 0, 1 << 20) || !integer(d["crop"][1], m.y, 0, 1 << 20) ||
      !integer(d["crop"][2], m.cropWidth, 1, 1 << 20) ||
      !integer(d["crop"][3], m.cropHeight, 1, 1 << 20) ||
      !integer(d["resizedSize"][0], m.resizedWidth, 1, 2048) ||
      !integer(d["resizedSize"][1], m.resizedHeight, 1, 2048) ||
      !integer(d["padding"][0], m.left, 0, 2048) || !integer(d["padding"][1], m.top, 0, 2048) ||
      m.x > m.width - m.cropWidth || m.y > m.height - m.cropHeight)
    return false;
  const double ratio =
      std::min(double(m.inputWidth) / m.cropWidth, double(m.inputHeight) / m.cropHeight);
  if (m.resizedWidth != std::max(1, roundedSize(m.cropWidth * ratio)) ||
      m.resizedHeight != std::max(1, roundedSize(m.cropHeight * ratio)) ||
      m.left != (m.inputWidth - m.resizedWidth) / 2 ||
      m.top != (m.inputHeight - m.resizedHeight) / 2)
    return false;
  m.classes = d["classNames"];
  return true;
}

Status preprocessYolo(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* image = in.get("image").asImage();
  if (!image || !image->consistent() || image->depth != PixelDepth::U8 || image->scale < 1 ||
      image->scale > (1 << 20) || image->width > (1 << 20) || image->height > (1 << 20))
    return img::badInput("YOLO 预处理需要有效 u8 灰度/RGB/RGBA 图像");
  const auto& family = p.choice("modelFamily");
  const int width = static_cast<int>(p.integer("inputWidth")),
            height = static_cast<int>(p.integer("inputHeight"));
  const auto& frame = p.text("frame");
  if (!familyValid(family) || width < 32 || height < 32 || width > 2048 || height > 2048 ||
      width % 32 || height % 32 || frame.empty() || frame.size() > 256)
    return img::badParam("模型系列、参考帧或32倍数的输入尺寸无效", "modelFamily");
  const auto& labelText = p.text("classNames");
  if (labelText.size() > 65536)
    return img::badParam("类别名称 JSON 超过64KiB", "classNames");
  Json classes = Json::parse(labelText, nullptr, false);
  std::string why;
  if (!values::boundedJson(classes, why) || !classesValid(classes))
    return img::badParam("类别名称必须是1..512项非空UTF-8文本数组", "classNames");
  std::string format = p.choice("outputFormat");
  if (format == "auto")
    format = family == "yolo26" ? "end2end" : "raw";
  if (format != "raw" && format != "end2end")
    return img::badParam("输出格式必须为raw或end2end", "outputFormat");
  cv::Rect crop(0, 0, image->width, image->height);
  if (in.has("roi")) {
    const Box2D* roi = in.get("roi").asBox2D();
    if (!roi || roi->unit != Unit2D::Pixel || !std::isfinite(roi->min[0]) ||
        !std::isfinite(roi->min[1]) || !std::isfinite(roi->max[0]) || !std::isfinite(roi->max[1]) ||
        roi->min[0] < 0 || roi->min[1] < 0 || roi->max[0] > image->width ||
        roi->max[1] > image->height || roi->max[0] <= roi->min[0] || roi->max[1] <= roi->min[1])
      return img::badInput("ROI 必须为图内非空像素 Box2D", "roi");
    const int x = static_cast<int>(std::floor(roi->min[0])),
              y = static_cast<int>(std::floor(roi->min[1]));
    crop = cv::Rect(x, y, static_cast<int>(std::ceil(roi->max[0])) - x,
                    static_cast<int>(std::ceil(roi->max[1])) - y);
  }
  if (ctx.cancelled())
    return Status::Ok();
  const double ratio = std::min(double(width) / crop.width, double(height) / crop.height);
  const int rw = std::max(1, roundedSize(crop.width * ratio)),
            rh = std::max(1, roundedSize(crop.height * ratio));
  const int left = (width - rw) / 2, top = (height - rh) / 2;
  cv::Mat rgb;
  const cv::Mat source = cvx::view(*image)(crop);
  if (image->channels == 3)
    rgb = source;
  else
    cv::cvtColor(source, rgb, image->channels == 1 ? cv::COLOR_GRAY2RGB : cv::COLOR_RGBA2RGB);
  cv::Mat resized;
  cv::resize(rgb, resized, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);
  Tensor tensor;
  tensor.shape = {1, 3, height, width};
  tensor.data.assign(static_cast<std::size_t>(3) * width * height, 114.0f / 255);
  for (int y = 0; y < rh; ++y) {
    if (ctx.cancelled())
      return Status::Ok();
    const auto* row = resized.ptr<cv::Vec3b>(y);
    for (int x = 0; x < rw; ++x)
      for (int c = 0; c < 3; ++c)
        tensor.data[(static_cast<std::size_t>(c) * height + y + top) * width + x + left] =
            row[x][c] / 255.0f;
  }
  Record mapping{kMappingType,
                 {{"modelFamily", family},
                  {"outputFormat", format},
                  {"inputSize", {width, height}},
                  {"workingSize", {image->width, image->height}},
                  {"imageScale", image->scale},
                  {"crop", {crop.x, crop.y, crop.width, crop.height}},
                  {"resizedSize", {rw, rh}},
                  {"padding", {left, top}},
                  {"frame", frame},
                  {"classNames", std::move(classes)}}};
  out.set("tensor", Data::tensor(std::move(tensor)));
  out.set("mapping", Data::record(std::move(mapping)));
  return Status::Ok();
}

struct Box {
  double x0, y0, x1, y1, score;
  int cls, index;
};

double overlap(const Box& a, const Box& b) {
  const double common = std::max(0.0, std::min(a.x1, b.x1) - std::max(a.x0, b.x0)) *
                        std::max(0.0, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
  const double total = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - common;
  return total > 0 ? common / total : 0;
}

Status decodeYolo(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Mapping m;
  std::string why;
  if (!parseMapping(in.get("mapping").asRecord(), m, why))
    return img::badInput("YOLO预处理元数据无效或不匹配：" + why, "mapping");
  const Tensor* prediction = in.get("prediction").asTensor();
  if (!prediction || !prediction->consistent() || prediction->shape.size() != 3 ||
      prediction->shape[0] != 1 || prediction->data.size() > 16u * 1024u * 1024u)
    return img::badInput("需要 batch=1、最多16Mi元素的float32检测输出", "prediction");
  const bool end2end = m.format == "end2end";
  int count = 0;
  if (end2end) {
    if (prediction->shape[2] != 6 || prediction->shape[1] < 1 || prediction->shape[1] > 4096)
      return img::badInput("端到端输出必须为[1,N,6]，N在1..4096", "prediction");
    count = static_cast<int>(prediction->shape[1]);
  } else {
    for (int stride : {8, 16, 32})
      count += (m.inputWidth / stride) * (m.inputHeight / stride);
    if (prediction->shape[1] != static_cast<std::int64_t>(4 + m.classes.size()) ||
        prediction->shape[2] != count)
      return img::badInput("常规输出必须为[1,4+类别数,N]，N对应8/16/32步长检测头", "prediction");
  }
  const double threshold = p.number("scoreThreshold"), nms = p.number("iouThreshold");
  const auto maxCandidates = p.integer("maxCandidates"), maxDetections = p.integer("maxDetections");
  if (!std::isfinite(threshold) || threshold < 0 || threshold > 1 || !std::isfinite(nms) ||
      nms < 0 || nms > 1 || maxCandidates < 1 || maxCandidates > 4096 || maxDetections < 1 ||
      maxDetections > 4096)
    return img::badParam("阈值需在[0,1]，候选/结果上限需在1..4096", "scoreThreshold");
  std::vector<Box> candidates;
  const double sx = double(m.resizedWidth) / m.cropWidth,
               sy = double(m.resizedHeight) / m.cropHeight;
  for (int i = 0; i < count; ++i) {
    if (ctx.cancelled())
      return Status::Ok();
    double coordinate[4];
    double score = 0;
    int cls = 0;
    if (end2end) {
      const float* row = prediction->data.data() + i * 6;
      for (int c = 0; c < 6; ++c)
        if (!std::isfinite(row[c]))
          return img::badInput("端到端检测含非有限值", "prediction");
      if (row[4] < 0 || row[4] > 1 || row[5] != std::floor(row[5]) || row[5] < 0 ||
          row[5] >= m.classes.size())
        return img::badInput("端到端置信度或类别索引非法", "prediction");
      std::copy(row, row + 4, coordinate);
      score = row[4];
      cls = static_cast<int>(row[5]);
    } else {
      for (int c = 0; c < 4; ++c) {
        coordinate[c] = prediction->data[c * count + i];
        if (!std::isfinite(coordinate[c]))
          return img::badInput("常规检测框含非有限值", "prediction");
      }
      for (std::size_t c = 0; c < m.classes.size(); ++c) {
        const double value = prediction->data[(c + 4) * count + i];
        if (!std::isfinite(value) || value < 0 || value > 1)
          return img::badInput("常规检测类别概率需在[0,1]", "prediction");
        if (value > score) {
          score = value;
          cls = static_cast<int>(c);
        }
      }
      const double cx = coordinate[0], cy = coordinate[1], width = coordinate[2],
                   height = coordinate[3];
      if (width < 0 || height < 0)
        return img::badInput("常规检测框宽高不能为负", "prediction");
      coordinate[0] = cx - width / 2;
      coordinate[1] = cy - height / 2;
      coordinate[2] = cx + width / 2;
      coordinate[3] = cy + height / 2;
    }
    if (coordinate[2] < coordinate[0] || coordinate[3] < coordinate[1])
      return img::badInput("检测框边界顺序错误", "prediction");
    if (score == 0 || score < threshold)
      continue;
    if (coordinate[2] <= coordinate[0] || coordinate[3] <= coordinate[1])
      continue;
    candidates.push_back(
        {coordinate[0], coordinate[1], coordinate[2], coordinate[3], score, cls, i});
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Box& a, const Box& b) { return a.score > b.score; });
  if (candidates.size() > static_cast<std::size_t>(maxCandidates))
    candidates.resize(static_cast<std::size_t>(maxCandidates));
  std::vector<Box> kept;
  for (const auto& candidate : candidates) {
    if (ctx.cancelled())
      return Status::Ok();
    bool suppressed = false;
    if (!end2end)
      for (const auto& other : kept)
        if ((p.flag("classAgnostic") || candidate.cls == other.cls) &&
            overlap(candidate, other) > nms) {
          suppressed = true;
          break;
        }
    if (!suppressed)
      kept.push_back(candidate);
    if (kept.size() >= static_cast<std::size_t>(maxDetections))
      break;
  }
  Json items = Json::array();
  for (auto box : kept) {
    box.x0 = std::clamp((box.x0 - m.left) / sx, 0.0, double(m.cropWidth));
    box.y0 = std::clamp((box.y0 - m.top) / sy, 0.0, double(m.cropHeight));
    box.x1 = std::clamp((box.x1 - m.left) / sx, 0.0, double(m.cropWidth));
    box.y1 = std::clamp((box.y1 - m.top) / sy, 0.0, double(m.cropHeight));
    if (box.x1 <= box.x0 || box.y1 <= box.y0)
      continue;
    box.x0 = (box.x0 + m.x) * m.scale;
    box.y0 = (box.y0 + m.y) * m.scale;
    box.x1 = (box.x1 + m.x) * m.scale;
    box.y1 = (box.y1 + m.y) * m.scale;
    items.push_back(
        {{"id", "det-" + std::to_string(box.index) + "-" + std::to_string(box.cls)},
         {"frame", m.frame},
         {"unit", "px"},
         {"position", {(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2}},
         {"angle", 0.0},
         {"size", {box.x1 - box.x0, box.y1 - box.y0}},
         {"classId", std::to_string(box.cls)},
         {"classLabel", m.classes[box.cls]},
         {"confidence", box.score},
         {"pickable", false},
         {"attributes",
          {{"modelFamily", m.family}, {"sourceAnchor", box.index}, {"outputFormat", m.format}}}});
  }
  Record detections = poses::make2DList(std::move(items), m.frame, "px");
  if (!poses::validate(detections, why))
    return img::badInput("检测结果超过实例契约：" + why, "prediction");
  out.set("detections", Data::record(std::move(detections)));
  return Status::Ok();
}

Port mappingPort() {
  return withContract(Port{"mapping", "Record", "变换", "同张图预处理输出。", true},
                      {{"recordType", kMappingType}});
}

Param stringParam(const char* name, const char* label, const std::string& value) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Text;
  p.def = Value::text(value);
  return p;
}
}

void registerImageYolo(Registry& r) {
  OperatorDesc op;
  op.id = "image.yolo_preprocess";
  op.version = "1.0.0";
  op.label = "YOLO 图像预处理";
  op.category = "机器学习/目标检测";
  op.keywords = {"YOLOv8 YOLO11 YOLO26 RGB letterbox preprocessing", "目标检测", "归一化",
                 "预处理"};
  op.inputs = {img::imageIn(),
               Port{"roi", "Box2D", "ROI", "可选图内像素矩形，向外整数取整。", false}};
  op.outputs = {Port{"tensor", "Tensor", "Tensor", "RGB float32 [1,3,H,W]，0..1。", true},
                mappingPort()};
  const Json classes = detection::kCocoClasses;
  op.params = {img::enumParam("modelFamily", "模型系列", "yolov8",
                              {{"yolov8", "YOLOv8", "常规检测头。"},
                               {"yolo11", "YOLO11", "常规检测头。"},
                               {"yolo26", "YOLO26", "默认端到端检测头。"}},
                              "显式选择模型系列。"),
               img::enumParam("outputFormat", "ONNX输出格式", "auto",
                              {{"auto", "系列默认", "8/11常规，26端到端。"},
                               {"raw", "常规检测头", "[1,4+C,N] cx/cy/w/h+类别概率。"},
                               {"end2end", "端到端或内置NMS", "[1,N,6] xyxy/分数/类别，不再NMS。"}},
                              "应与模型导出参数一致。"),
               img::intParam("inputWidth", "模型宽度", 640, 32, 2048, "32的倍数。"),
               img::intParam("inputHeight", "模型高度", 640, 32, 2048, "32的倍数。"),
               stringParam("classNames", "类别名称JSON", classes.dump()),
               stringParam("frame", "图像参考帧", "image")};
  op.doc =
      "支持YOLOv8/11/26标准float32检测ONNX：RGB/255，居中letterbox、填充114，批量固定1；类别列表与输入宽高显式，不猜输出布局。";
  op.capabilities = {true, false, true};
  op.compute = &preprocessYolo;
  r.addOperator(std::move(op));
  op = OperatorDesc{};
  op.id = "image.yolo_decode";
  op.version = "1.0.0";
  op.label = "YOLO 检测结果解码";
  op.category = "机器学习/目标检测";
  op.keywords = {"YOLOv8 YOLO11 YOLO26 detection NMS end2end", "目标检测", "解码", "实例",
                 "非极大值抑制"};
  op.inputs = {Port{"prediction", "Tensor", "Prediction", "显式格式的检测输出。", true},
               mappingPort()};
  op.outputs = {
      withContract(Port{"detections", "Record", "实例", "原图像素vision.Poses2D.v1。", true},
                   {{"recordType", poses::kPoses2DType}})};
  op.params = {
      img::floatParam("scoreThreshold", "置信度阈值", 0.3,
                      "类别概率，不乘不存在的objectness；含等号。"),
      img::floatParam("iouThreshold", "IoU阈值", 0.45, "仅常规头使用，严格大于阈值抑制。"),
      img::intParam("maxCandidates", "候选上限", 3000, 1, 4096, "稳定排序后截断。"),
      img::intParam("maxDetections", "输出上限", 300, 1, 4096, "最终实例数。"),
      img::boolParam("classAgnostic", "常规头忽略类别NMS", false, "端到端输出不再执行NMS。")};
  op.doc =
      "8/11及26常规头：[1,4+C,N]最高类别概率，类别感知稳定IoU NMS；26默认或内置NMS导出：[1,N,6]，只筛选/回映，不重复NMS。支持自定义类别和原图ROI/预览坐标还原。";
  op.capabilities = {true, false, true};
  op.compute = &decodeYolo;
  r.addOperator(std::move(op));
}

}
