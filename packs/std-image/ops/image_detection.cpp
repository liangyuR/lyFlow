#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "lyflow/value.h"
#include "ops.h"
#include "lyflow_ml/detection_contract.h"
#include "params.h"

namespace lyflow::ops {
namespace {

using namespace detection;

Param textParam(const char* name, const char* label, const char* def) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(def);
  return p;
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
  const double ratio =
      std::min(double(kYOLOXInputSide) / crop.width, double(kYOLOXInputSide) / crop.height);
  const int rw = std::max(1, int(crop.width * ratio)), rh = std::max(1, int(crop.height * ratio));
  cv::Mat resized;
  cv::resize(bgr, resized, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);
  Tensor tensor;
  tensor.shape = {1, 3, kYOLOXInputSide, kYOLOXInputSide};
  tensor.data.assign(3 * kYOLOXInputSide * kYOLOXInputSide, 114.0f);
  for (int y = 0; y < rh; ++y) {
    if (ctx.cancelled())
      return Status::Ok();
    const auto* row = resized.ptr<cv::Vec3b>(y);
    for (int x = 0; x < rw; ++x)
      for (int c = 0; c < 3; ++c)
        tensor.data[(c * kYOLOXInputSide + y) * kYOLOXInputSide + x] = row[x][c];
  }
  Record mapping{kYOLOXMappingType,
                 {{"model", kYOLOXModel},
                  {"inputSize", {kYOLOXInputSide, kYOLOXInputSide}},
                  {"workingSize", {image->width, image->height}},
                  {"imageScale", image->scale},
                  {"crop", {crop.x, crop.y, crop.width, crop.height}},
                  {"resizedSize", {rw, rh}},
                  {"frame", frame}}};
  outputs.set("tensor", Data::tensor(std::move(tensor)));
  outputs.set("mapping", Data::record(std::move(mapping)));
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
  op.outputs = {
      Port{"tensor", "Tensor", "Tensor", "float32 [1,3,416,416]。", true},
      yoloxRecordPort("mapping", kYOLOXMappingType, "受校验的预处理变换与固定模型身份。")};
  op.params = {textParam("frame", "图像参考帧", "image")};
  op.capabilities = {true, false, true};
  op.compute = &preprocess;
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
