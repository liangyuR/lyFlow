#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "lyflow/op_params.h"
#include "lyflow/pose.h"
#include "lyflow_ml/detection_contract.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

using namespace detection;

bool integer(const Json& value, int& result, int minimum, int maximum) {
  if (!value.is_number_integer() || value < minimum || value > maximum)
    return false;
  result = value.get<int>();
  return true;
}

struct Mapping {
  std::string family, format, frame;
  int width = 0, height = 0, scale = 1, x = 0, y = 0, cropWidth = 0, cropHeight = 0;
  int inputWidth = 0, inputHeight = 0, resizedWidth = 0, resizedHeight = 0, left = 0, top = 0;
  Json classes;
};

bool parseMapping(const Record* record, Mapping& m, std::string& why) {
  if (!record || record->type != kYOLOMappingType || !values::boundedJson(record->data, why)) {
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
    return op_params::badInput("YOLO预处理元数据无效或不匹配：" + why, "mapping");
  const Tensor* prediction = in.get("prediction").asTensor();
  if (!prediction || !prediction->consistent() || prediction->shape.size() != 3 ||
      prediction->shape[0] != 1 || prediction->data.size() > 16u * 1024u * 1024u)
    return op_params::badInput("需要 batch=1、最多16Mi元素的float32检测输出", "prediction");
  const bool end2end = m.format == "end2end";
  int count = 0;
  if (end2end) {
    if (prediction->shape[2] != 6 || prediction->shape[1] < 1 || prediction->shape[1] > 4096)
      return op_params::badInput("端到端输出必须为[1,N,6]，N在1..4096", "prediction");
    count = static_cast<int>(prediction->shape[1]);
  } else {
    for (int stride : {8, 16, 32})
      count += (m.inputWidth / stride) * (m.inputHeight / stride);
    if (prediction->shape[1] != static_cast<std::int64_t>(4 + m.classes.size()) ||
        prediction->shape[2] != count)
      return op_params::badInput("常规输出必须为[1,4+类别数,N]，N对应8/16/32步长检测头",
                                 "prediction");
  }
  const double threshold = p.number("scoreThreshold"), nms = p.number("iouThreshold");
  const auto maxCandidates = p.integer("maxCandidates"), maxDetections = p.integer("maxDetections");
  if (!std::isfinite(threshold) || threshold < 0 || threshold > 1 || !std::isfinite(nms) ||
      nms < 0 || nms > 1 || maxCandidates < 1 || maxCandidates > 4096 || maxDetections < 1 ||
      maxDetections > 4096)
    return op_params::badParam("阈值需在[0,1]，候选/结果上限需在1..4096", "scoreThreshold");
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
          return op_params::badInput("端到端检测含非有限值", "prediction");
      if (row[4] < 0 || row[4] > 1 || row[5] != std::floor(row[5]) || row[5] < 0 ||
          row[5] >= m.classes.size())
        return op_params::badInput("端到端置信度或类别索引非法", "prediction");
      std::copy(row, row + 4, coordinate);
      score = row[4];
      cls = static_cast<int>(row[5]);
    } else {
      for (int c = 0; c < 4; ++c) {
        coordinate[c] = prediction->data[c * count + i];
        if (!std::isfinite(coordinate[c]))
          return op_params::badInput("常规检测框含非有限值", "prediction");
      }
      for (std::size_t c = 0; c < m.classes.size(); ++c) {
        const double value = prediction->data[(c + 4) * count + i];
        if (!std::isfinite(value) || value < 0 || value > 1)
          return op_params::badInput("常规检测类别概率需在[0,1]", "prediction");
        if (value > score) {
          score = value;
          cls = static_cast<int>(c);
        }
      }
      const double cx = coordinate[0], cy = coordinate[1], width = coordinate[2],
                   height = coordinate[3];
      if (width < 0 || height < 0)
        return op_params::badInput("常规检测框宽高不能为负", "prediction");
      coordinate[0] = cx - width / 2;
      coordinate[1] = cy - height / 2;
      coordinate[2] = cx + width / 2;
      coordinate[3] = cy + height / 2;
    }
    if (coordinate[2] < coordinate[0] || coordinate[3] < coordinate[1])
      return op_params::badInput("检测框边界顺序错误", "prediction");
    if (score == 0 || score < threshold)
      continue;
    if (coordinate[2] <= coordinate[0] || coordinate[3] <= coordinate[1])
      continue;
    if (coordinate[2] <= m.left || coordinate[3] <= m.top ||
        coordinate[0] >= m.left + m.resizedWidth || coordinate[1] >= m.top + m.resizedHeight)
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
    return op_params::badInput("检测结果超过实例契约：" + why, "prediction");
  out.set("detections", Data::record(std::move(detections)));
  return Status::Ok();
}

} // namespace

void registerYoloDecode(Registry& r) {
  OperatorDesc op;
  op.id = "image.yolo_decode";
  op.version = "1.0.0";
  op.label = "YOLO 检测结果解码";
  op.category = "机器学习/目标检测";
  op.keywords = {"YOLOv8 YOLO11 YOLO26 detection NMS end2end", "目标检测", "解码", "实例",
                 "非极大值抑制"};
  op.inputs = {Port{"prediction", "Tensor", "Prediction", "显式格式的检测输出。", true},
               yoloMappingPort()};
  op.outputs = {
      withContract(Port{"detections", "Record", "实例", "原图像素vision.Poses2D.v1。", true},
                   {{"recordType", poses::kPoses2DType}})};
  op.params = {
      op_params::floatParam("scoreThreshold", "置信度阈值", 0.3,
                            "类别概率，不乘不存在的objectness；含等号。"),
      op_params::floatParam("iouThreshold", "IoU阈值", 0.45, "仅常规头使用，严格大于阈值抑制。"),
      op_params::intParam("maxCandidates", "候选上限", 3000, 1, 4096, "稳定排序后截断。"),
      op_params::intParam("maxDetections", "输出上限", 300, 1, 4096, "最终实例数。"),
      op_params::boolParam("classAgnostic", "常规头忽略类别NMS", false, "端到端输出不再执行NMS。")};
  op.doc =
      "8/11及26常规头：[1,4+C,N]最高类别概率，类别感知稳定IoU NMS；26默认或内置NMS导出：[1,N,6]，只筛选/回映，不重复NMS。支持自定义类别和原图ROI/预览坐标还原。";
  op.capabilities = {true, false, true};
  op.compute = &decodeYolo;
  r.addOperator(std::move(op));
}

}
