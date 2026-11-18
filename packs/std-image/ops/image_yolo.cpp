#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "lyflow_ml/detection_contract.h"
#include "lyflow/pose.h"
#include "params.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

using namespace detection;
using Json = nlohmann::json;

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
  Record mapping{kYOLOMappingType,
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
                yoloMappingPort()};
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
}

} // namespace lyflow::ops
