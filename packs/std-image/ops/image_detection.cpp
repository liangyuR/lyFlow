#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "lyflow/pose.h"
#include "lyflow/value.h"
#include "ops.h"
#include "detection_common.h"
#include "params.h"

namespace lyflow::ops {
namespace {

constexpr int kSide = 416;
constexpr int kAnchors = 3549;
constexpr int kFields = 85;
constexpr const char* kModel = "yolox-nano-0.1.1rc0";
constexpr const char* kPreprocess = "vision.YOLOXPreprocess.v1";
using detection::kCocoClasses;

Port recordPort(const char* name, const char* type, const char* doc) {
  return withContract(Port{name, "Record", name, doc, true}, {{"recordType", type}});
}

Param textParam(const char* name, const char* label, const char* def) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(def);
  return p;
}

struct Mapping {
  int width = 0, height = 0, scale = 1, x = 0, y = 0, cropWidth = 0, cropHeight = 0;
  int resizedWidth = 0, resizedHeight = 0;
  std::string frame;
};

bool positiveInt(const nlohmann::json& value, int& out, bool zero = false) {
  if (!value.is_number_integer())
    return false;
  if (value < (zero ? 0 : 1) || value > (1 << 20))
    return false;
  out = value.get<int>();
  return true;
}

bool parseMapping(const Record* record, Mapping& m, std::string& why) {
  if (!record || record->type != kPreprocess || !values::boundedJson(record->data, why)) {
    why = "需要有效的 vision.YOLOXPreprocess.v1";
    return false;
  }
  const auto& d = record->data;
  if (!d.is_object() || d.size() != 7 || d.value("model", nlohmann::json()) != kModel ||
      !d.contains("workingSize") || !d["workingSize"].is_array() || d["workingSize"].size() != 2 ||
      !d.contains("crop") || !d["crop"].is_array() || d["crop"].size() != 4 ||
      !d.contains("resizedSize") || !d["resizedSize"].is_array() || d["resizedSize"].size() != 2 ||
      d.value("inputSize", nlohmann::json()) != nlohmann::json::array({kSide, kSide}) ||
      !d.contains("frame") || !d["frame"].is_string() ||
      !positiveInt(d["workingSize"][0], m.width) || !positiveInt(d["workingSize"][1], m.height) ||
      !positiveInt(d.value("imageScale", nlohmann::json()), m.scale) ||
      !positiveInt(d["crop"][0], m.x, true) || !positiveInt(d["crop"][1], m.y, true) ||
      !positiveInt(d["crop"][2], m.cropWidth) || !positiveInt(d["crop"][3], m.cropHeight) ||
      !positiveInt(d["resizedSize"][0], m.resizedWidth) ||
      !positiveInt(d["resizedSize"][1], m.resizedHeight)) {
    why = "预处理元数据字段、尺寸或模型不匹配";
    return false;
  }
  m.frame = d["frame"].get<std::string>();
  if (m.frame.empty() || m.frame.size() > 256 || m.x > m.width - m.cropWidth ||
      m.y > m.height - m.cropHeight) {
    why = "预处理 ROI 或参考帧无效";
    return false;
  }
  const double ratio = std::min(double(kSide) / m.cropWidth, double(kSide) / m.cropHeight);
  if (m.resizedWidth != std::max(1, int(m.cropWidth * ratio)) ||
      m.resizedHeight != std::max(1, int(m.cropHeight * ratio))) {
    why = "预处理尺寸与声明的 letterbox 规则不符";
    return false;
  }
  return true;
}

Status preprocess(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                  ExecContext& ctx) {
  const Image* image = inputs.get("image").asImage();
  if (!image || !image->consistent() || image->depth != PixelDepth::U8 || image->scale < 1 ||
      image->width > (1 << 20) || image->height > (1 << 20) || image->scale > (1 << 20))
    return img::badInput("YOLOX-Nano 预处理需要有效 u8 灰度/RGB/RGBA 图像");
  const auto& frame = params.text("frame");
  if (frame.empty() || frame.size() > 256)
    return img::badParam("参考帧不能为空且最多 256 字节", "frame");
  cv::Rect crop(0, 0, image->width, image->height);
  if (inputs.has("roi")) {
    const Box2D* roi = inputs.get("roi").asBox2D();
    if (!roi || roi->unit != Unit2D::Pixel || !std::isfinite(roi->min[0]) ||
        !std::isfinite(roi->min[1]) || !std::isfinite(roi->max[0]) || !std::isfinite(roi->max[1]) ||
        roi->min[0] < 0 || roi->min[1] < 0 || roi->max[0] > image->width ||
        roi->max[1] > image->height || roi->max[0] <= roi->min[0] || roi->max[1] <= roi->min[1])
      return img::badInput("ROI 必须是图内非空像素 Box2D", "roi");
    const int x = static_cast<int>(std::floor(roi->min[0]));
    const int y = static_cast<int>(std::floor(roi->min[1]));
    crop = cv::Rect(x, y, static_cast<int>(std::ceil(roi->max[0])) - x,
                    static_cast<int>(std::ceil(roi->max[1])) - y);
  }
  if (ctx.cancelled())
    return Status::Ok();
  cv::Mat bgr;
  const cv::Mat source = cvx::view(*image)(crop);
  cv::cvtColor(source, bgr,
               image->channels == 1   ? cv::COLOR_GRAY2BGR
               : image->channels == 3 ? cv::COLOR_RGB2BGR
                                      : cv::COLOR_RGBA2BGR);
  const double ratio = std::min(double(kSide) / crop.width, double(kSide) / crop.height);
  const int rw = std::max(1, int(crop.width * ratio)), rh = std::max(1, int(crop.height * ratio));
  cv::Mat resized;
  cv::resize(bgr, resized, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);
  Tensor tensor;
  tensor.shape = {1, 3, kSide, kSide};
  tensor.data.assign(3 * kSide * kSide, 114.0f);
  for (int y = 0; y < rh; ++y) {
    if (ctx.cancelled())
      return Status::Ok();
    const auto* row = resized.ptr<cv::Vec3b>(y);
    for (int x = 0; x < rw; ++x)
      for (int c = 0; c < 3; ++c)
        tensor.data[(c * kSide + y) * kSide + x] = row[x][c];
  }
  Record mapping{kPreprocess,
                 {{"model", kModel},
                  {"inputSize", {kSide, kSide}},
                  {"workingSize", {image->width, image->height}},
                  {"imageScale", image->scale},
                  {"crop", {crop.x, crop.y, crop.width, crop.height}},
                  {"resizedSize", {rw, rh}},
                  {"frame", frame}}};
  outputs.set("tensor", Data::tensor(std::move(tensor)));
  outputs.set("mapping", Data::record(std::move(mapping)));
  return Status::Ok();
}

struct Detection {
  double x0, y0, x1, y1, score;
  int classId, anchor;
};

double iou(const Detection& a, const Detection& b) {
  const double overlap = std::max(0.0, std::min(a.x1, b.x1) - std::max(a.x0, b.x0)) *
                         std::max(0.0, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
  const double total = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - overlap;
  return total > 0 ? overlap / total : 0;
}

Status decode(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  Mapping mapping;
  std::string why;
  if (!parseMapping(inputs.get("mapping").asRecord(), mapping, why))
    return img::badInput(why, "mapping");
  const Tensor* prediction = inputs.get("prediction").asTensor();
  if (!prediction || prediction->shape != std::vector<std::int64_t>({1, kAnchors, kFields}) ||
      !prediction->consistent())
    return img::badInput("固定模型输出必须是 float32 [1,3549,85]，未包含网格解码", "prediction");
  const double confidence = params.number("scoreThreshold"), nms = params.number("iouThreshold");
  if (!std::isfinite(confidence) || confidence < 0 || confidence > 1 || !std::isfinite(nms) ||
      nms < 0 || nms > 1)
    return img::badParam("置信度与 IoU 阈值必须位于 [0,1]", "scoreThreshold");
  const int maxCandidates = static_cast<int>(params.integer("maxCandidates"));
  const int maxDetections = static_cast<int>(params.integer("maxDetections"));
  if (maxCandidates < 1 || maxCandidates > 4096 || maxDetections < 1 || maxDetections > 4096)
    return img::badParam("候选和输出上限必须位于 [1,4096]", "maxCandidates");
  std::vector<Detection> candidates;
  const double sx = double(mapping.resizedWidth) / mapping.cropWidth;
  const double sy = double(mapping.resizedHeight) / mapping.cropHeight;
  int anchor = 0;
  for (int stride : {8, 16, 32}) {
    const int side = kSide / stride;
    for (int gy = 0; gy < side; ++gy) {
      if (ctx.cancelled())
        return Status::Ok();
      for (int gx = 0; gx < side; ++gx, ++anchor) {
        const float* row = prediction->data.data() + anchor * kFields;
        for (int c = 0; c < kFields; ++c)
          if (!std::isfinite(row[c]) || (c >= 4 && (row[c] < 0 || row[c] > 1)))
            return img::badInput("模型输出含非有限坐标或 [0,1] 外的概率", "prediction");
        if (std::abs(row[2]) > 80 || std::abs(row[3]) > 80)
          return img::badInput("模型输出宽高的对数超过支持范围 [-80,80]", "prediction");
        const int cls = static_cast<int>(std::max_element(row + 5, row + kFields) - (row + 5));
        const double score = double(row[4]) * row[5 + cls];
        if (score == 0 || score < confidence)
          continue;
        const double cx = (double(row[0]) + gx) * stride, cy = (double(row[1]) + gy) * stride;
        const double width = std::exp(double(row[2])) * stride;
        const double height = std::exp(double(row[3])) * stride;
        const double x0 = cx - width / 2, y0 = cy - height / 2;
        const double x1 = cx + width / 2, y1 = cy + height / 2;
        if (x1 <= x0 || y1 <= y0)
          continue;
        candidates.push_back({x0, y0, x1, y1, score, cls, anchor});
      }
    }
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Detection& a, const Detection& b) { return a.score > b.score; });
  if (candidates.size() > static_cast<std::size_t>(maxCandidates))
    candidates.resize(static_cast<std::size_t>(maxCandidates));
  std::vector<Detection> kept;
  for (const Detection& candidate : candidates) {
    if (ctx.cancelled())
      return Status::Ok();
    bool suppressed = false;
    for (const Detection& other : kept)
      if ((params.flag("classAgnostic") || candidate.classId == other.classId) &&
          iou(candidate, other) > nms) {
        suppressed = true;
        break;
      }
    if (!suppressed)
      kept.push_back(candidate);
    if (kept.size() >= static_cast<std::size_t>(maxDetections))
      break;
  }
  nlohmann::json items = nlohmann::json::array();
  for (auto d : kept) {
    d.x0 = std::clamp(d.x0 / sx, 0.0, double(mapping.cropWidth));
    d.y0 = std::clamp(d.y0 / sy, 0.0, double(mapping.cropHeight));
    d.x1 = std::clamp(d.x1 / sx, 0.0, double(mapping.cropWidth));
    d.y1 = std::clamp(d.y1 / sy, 0.0, double(mapping.cropHeight));
    if (d.x1 <= d.x0 || d.y1 <= d.y0)
      continue;
    d.x0 = (d.x0 + mapping.x) * mapping.scale;
    d.y0 = (d.y0 + mapping.y) * mapping.scale;
    d.x1 = (d.x1 + mapping.x) * mapping.scale;
    d.y1 = (d.y1 + mapping.y) * mapping.scale;
    items.push_back({{"id", "det-" + std::to_string(d.anchor) + "-" + std::to_string(d.classId)},
                     {"frame", mapping.frame},
                     {"unit", "px"},
                     {"position", {(d.x0 + d.x1) / 2, (d.y0 + d.y1) / 2}},
                     {"angle", 0.0},
                     {"size", {d.x1 - d.x0, d.y1 - d.y0}},
                     {"classId", std::to_string(d.classId)},
                     {"classLabel", kCocoClasses[d.classId]},
                     {"confidence", d.score},
                     {"pickable", false},
                     {"attributes", {{"model", kModel}, {"sourceAnchor", d.anchor}}}});
  }
  Record result{"vision.Poses2D.v1",
                {{"frame", mapping.frame}, {"unit", "px"}, {"items", std::move(items)}}};
  if (!poses::validate(result, why))
    return img::badInput("检测结果超过有界实例契约：" + why, "prediction");
  outputs.set("detections", Data::record(std::move(result)));
  return Status::Ok();
}

} // namespace

void registerImageDetection(Registry& r) {
  OperatorDesc op;
  op.id = "image.yolox_preprocess";
  op.version = "1.0.0";
  op.label = "YOLOX 图像预处理";
  op.category = "机器学习/目标检测";
  op.keywords = {"YOLOX nano preprocessing letterbox BGR NCHW", "目标检测", "图像预处理"};
  op.doc =
      "固定 YOLOX-Nano 0.1.1rc0：416×416、BGR、float32 NCHW、0..255，右/下填充114；保留实际缩放和ROI供坐标回映。";
  op.inputs = {img::imageIn(),
               Port{"roi", "Box2D", "ROI", "可选图内像素矩形，向外整像素取整。", false}};
  op.outputs = {Port{"tensor", "Tensor", "Tensor", "float32 [1,3,416,416]。", true},
                recordPort("mapping", kPreprocess, "受校验的预处理变换与固定模型身份。")};
  op.params = {textParam("frame", "图像参考帧", "image")};
  op.capabilities = {true, false, true};
  op.compute = &preprocess;
  r.addOperator(std::move(op));

  op = OperatorDesc{};
  op.id = "image.yolox_decode";
  op.version = "1.0.0";
  op.label = "YOLOX 检测结果解码";
  op.category = "机器学习/目标检测";
  op.keywords = {"YOLOX nano detection decode NMS COCO", "目标检测", "非极大值抑制", "实例"};
  op.doc =
      "固定未解码[1,3549,85]输出，每锚点最高分类；网格/步长解码、分数筛选、稳定候选截断、类别感知NMS、原图像素坐标回映。空目标是有效空列表，无实例分割或旋转框。";
  op.inputs = {Port{"prediction", "Tensor", "Prediction", "固定YOLOX-Nano原始输出。", true},
               recordPort("mapping", kPreprocess, "同一张图预处理输出。")};
  op.outputs = {recordPort("detections", "vision.Poses2D.v1",
                           "原图像素实例，固定COCO80类别；pickable=false。")};
  op.params = {img::floatParam("scoreThreshold", "置信度阈值", 0.3,
                               "置信度含等号，objectness×最高类别概率。"),
               img::floatParam("iouThreshold", "IoU阈值", 0.45, "交并比严格大于阈值才抑制。"),
               img::intParam("maxCandidates", "候选上限", 1000, 1, 4096, "排序后NMS前截断。"),
               img::intParam("maxDetections", "输出上限", 300, 1, 4096, "最终保留实例数。"),
               img::boolParam("classAgnostic", "忽略类别进行NMS", false, "默认各类别分别抑制。")};
  op.capabilities = {true, false, true};
  op.compute = &decode;
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
