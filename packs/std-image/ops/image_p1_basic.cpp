#include <set>

#include "lyflow/pose.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {

Status concat(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* a = in.get("a").asImage();
  const Image* b = in.get("b").asImage();
  auto s = image_p1::image(a, "a");
  if (!s.ok)
    return s;
  s = image_p1::image(b, "b");
  if (!s.ok)
    return s;
  if (a->channels != b->channels || a->depth != b->depth || a->scale != b->scale)
    return img::badInput("拼接输入必须同通道、同位深、同预览比例", "b");
  const bool horizontal = p.choice("direction") == "horizontal";
  const int width = horizontal ? a->width + b->width : std::max(a->width, b->width);
  const int height = horizontal ? std::max(a->height, b->height) : a->height + b->height;
  if (std::uint64_t(width) * height > 16777216)
    return img::badInput("拼接输出超过16Mi像素预算");
  const double pad = p.number("padding");
  if (!std::isfinite(pad) || (a->depth == PixelDepth::U8 && (pad < 0 || pad > 255)) ||
      (a->depth == PixelDepth::U16 && (pad < 0 || pad > 65535)) ||
      (a->depth == PixelDepth::F32 && std::abs(pad) > std::numeric_limits<float>::max()))
    return img::badParam("填充值必须在输入位深的有限可表示范围内", "padding");
  if (ctx.cancelled())
    return Status::Ok();
  cv::Mat result(height, width, cvx::cvType(*a), cv::Scalar::all(pad));
  cvx::view(*a).copyTo(result(cv::Rect(0, 0, a->width, a->height)));
  cvx::view(*b).copyTo(
      result(cv::Rect(horizontal ? a->width : 0, horizontal ? 0 : a->height, b->width, b->height)));
  return image_p1::putImage(out, "image", std::move(result), a->scale);
}

Status color(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("image").asImage();
  auto s = image_p1::image(input);
  if (!s.ok)
    return s;
  const double brightness = p.number("brightness"), contrast = p.number("contrast");
  const double gamma = p.number("gamma"), hue = p.number("hue"),
               saturation = p.number("saturation");
  const auto gains = p.vec3("gain"), offsets = p.vec3("offset");
  if (!std::isfinite(brightness) || std::abs(brightness) > 1 || !std::isfinite(contrast) ||
      contrast < 0 || contrast > 16 || !std::isfinite(gamma) || gamma < 0.05 || gamma > 20 ||
      !std::isfinite(hue) || std::abs(hue) > 360 || !std::isfinite(saturation) || saturation < 0 ||
      saturation > 16)
    return img::badParam("颜色参数超出有限支持范围", "brightness");
  for (int c = 0; c < 3; ++c)
    if (!std::isfinite(gains[c]) || gains[c] < 0 || gains[c] > 16 || !std::isfinite(offsets[c]) ||
        std::abs(offsets[c]) > 1)
      return img::badParam("逐通道增益须在[0,16]、偏移须在[-1,1]", "gain");
  cv::Mat normalized;
  cvx::view(*input).convertTo(normalized, CV_32F,
                              1 / cvx::fullScale(cvx::cvDepthOf(pixelDepthName(input->depth))));
  std::vector<cv::Mat> channels;
  cv::split(normalized, channels);
  cv::Mat alpha;
  if (input->channels == 4)
    alpha = channels.back().clone();
  const int colorChannels = std::min(input->channels, 3);
  for (int c = 0; c < colorChannels; ++c) {
    cv::max(channels[c], 0, channels[c]);
    cv::min(channels[c], 1, channels[c]);
    cv::pow(channels[c], 1 / gamma, channels[c]);
    channels[c] = ((channels[c] - 0.5) * contrast + 0.5 + brightness) * gains[c] + offsets[c];
    cv::max(channels[c], 0, channels[c]);
    cv::min(channels[c], 1, channels[c]);
  }
  if (input->channels >= 3 && (hue != 0 || saturation != 1)) {
    cv::Mat rgb, hsv;
    cv::merge(std::vector<cv::Mat>{channels[0], channels[1], channels[2]}, rgb);
    cv::cvtColor(rgb, hsv, cv::COLOR_RGB2HSV);
    for (int y = 0; y < hsv.rows; ++y) {
      if (ctx.cancelled())
        return Status::Ok();
      auto* row = hsv.ptr<cv::Vec3f>(y);
      for (int x = 0; x < hsv.cols; ++x) {
        row[x][0] = static_cast<float>(std::fmod(row[x][0] + hue + 360, 360));
        row[x][1] = static_cast<float>(std::min(1.0, row[x][1] * saturation));
      }
    }
    cv::cvtColor(hsv, rgb, cv::COLOR_HSV2RGB);
    std::vector<cv::Mat> adjusted;
    cv::split(rgb, adjusted);
    for (int c = 0; c < 3; ++c)
      channels[c] = adjusted[c];
  }
  if (!alpha.empty())
    channels[3] = alpha;
  cv::Mat result;
  cv::merge(channels, normalized);
  normalized.convertTo(result, cvx::cvType(*input),
                       cvx::fullScale(cvx::cvDepthOf(pixelDepthName(input->depth))));
  if (ctx.cancelled())
    return Status::Ok();
  return image_p1::putImage(out, "image", std::move(result), input->scale);
}

Status shading(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("image").asImage();
  auto s = image_p1::image(input);
  if (!s.ok)
    return s;
  const bool reference = in.has("reference");
  cv::Mat background;
  if (reference) {
    const Image* ref = in.get("reference").asImage();
    s = image_p1::image(ref, "reference");
    if (!s.ok)
      return s;
    if (ref->width != input->width || ref->height != input->height || ref->depth != input->depth ||
        ref->channels != input->channels || ref->scale != input->scale)
      return img::badInput("平场参考必须与输入尺寸、通道、位深、比例一致", "reference");
    cvx::view(*ref).convertTo(background, CV_64F);
  } else {
    const int kernel = static_cast<int>(p.integer("kernel"));
    if (kernel < 3 || kernel > 511 || kernel % 2 != 1)
      return img::badParam("背景核必须是[3,511]内的奇数", "kernel");
    cv::Mat floating;
    cvx::view(*input).convertTo(floating, CV_64F);
    cv::GaussianBlur(floating, background, cv::Size(kernel, kernel), 0, 0, cv::BORDER_REFLECT_101);
  }
  const double epsilon = p.number("epsilon"), gain = p.number("gain");
  if (!std::isfinite(epsilon) || epsilon <= 0 || !std::isfinite(gain) || gain < 0 || gain > 16)
    return img::badParam("epsilon须为有限正数，gain须在[0,16]", "epsilon");
  cv::Mat source, result;
  cvx::view(*input).convertTo(source, CV_64F);
  std::vector<cv::Mat> bg;
  cv::split(background, bg);
  std::array<double, 4> means{};
  for (int c = 0; c < input->channels; ++c)
    means[c] = cv::mean(bg[c])[0];
  const bool divide = p.choice("mode") == "divide";
  for (int y = 0; y < source.rows; ++y) {
    if (ctx.cancelled())
      return Status::Ok();
    double* row = source.ptr<double>(y);
    const double* backgroundRow = background.ptr<double>(y);
    for (int x = 0; x < source.cols * input->channels; ++x) {
      const int channel = x % input->channels;
      if (channel == 3)
        continue;
      if (divide && backgroundRow[x] <= epsilon)
        return img::badInput("平场参考含非正或低于epsilon的值，拒绝不稳定除法", "reference");
      row[x] = gain * (divide ? row[x] * means[channel] / backgroundRow[x]
                              : row[x] - backgroundRow[x] + means[channel]);
      if (!std::isfinite(row[x]))
        return img::badInput("背景校正产生非有限值");
    }
  }
  source.convertTo(result, cvx::cvType(*input));
  return image_p1::putImage(out, "image", std::move(result), input->scale);
}

Status emitLabels(Outputs& out, cv::Mat labels, const nlohmann::json& ids, const std::string& frame,
                  int scale) {
  cv::Mat mask;
  cv::compare(labels, 0, mask, cv::CMP_GT);
  auto s = image_p1::record(
      out, "index", "vision.InstanceLabels.v1",
      {{"frame", frame},
       {"unit", "px"},
       {"ids", ids},
       {"imageSize", {std::int64_t(labels.cols) * scale, std::int64_t(labels.rows) * scale}}});
  if (!s.ok)
    return s;
  s = image_p1::putImage(out, "mask", std::move(mask), scale);
  if (!s.ok)
    return s;
  return image_p1::putImage(out, "labels", std::move(labels), scale);
}

Status instanceMask(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("image").asImage();
  auto s = image_p1::image(input);
  if (!s.ok)
    return s;
  Record instances;
  std::string why;
  if (!poses::read2DList(in.get("instances"), instances, why))
    return img::badInput(why, "instances");
  if (instances.data["unit"] != "px" || instances.data["frame"] != p.text("frame"))
    return img::badInput("工件必须是指定图像帧内的像素位姿", "instances");
  const auto& items = instances.data["items"];
  if (input->scale < 1 || items.size() * input->pixelCount() > 134217728)
    return img::badInput("实例光栅化超过128Mi像素工作预算", "instances");
  cv::Mat labels(input->height, input->width, CV_16UC1, cv::Scalar(0));
  nlohmann::json ids = nlohmann::json::array();
  int label = 0;
  for (const auto& item : items) {
    if (ctx.cancelled())
      return Status::Ok();
    ids.push_back(item["id"]);
    ++label;
    const double cx = item["position"][0].get<double>() / input->scale;
    const double cy = item["position"][1].get<double>() / input->scale;
    const double w = item["size"][0].get<double>() / input->scale;
    const double h = item["size"][1].get<double>() / input->scale;
    const double angle = item["angle"].get<double>();
    if (std::max({std::abs(cx), std::abs(cy), w, h}) > 1000000)
      return img::badInput("实例坐标或尺寸超过光栅化范围", "instances");
    std::vector<cv::Point> polygon;
    for (const auto& sign :
         {cv::Point(-1, -1), cv::Point(1, -1), cv::Point(1, 1), cv::Point(-1, 1)}) {
      const double x = sign.x * w / 2, y = sign.y * h / 2;
      polygon.emplace_back(cvRound((cx + x * std::cos(angle) - y * std::sin(angle)) * 256),
                           cvRound((cy + x * std::sin(angle) + y * std::cos(angle)) * 256));
    }
    cv::Mat mask(labels.size(), CV_8UC1, cv::Scalar(0));
    cv::fillConvexPoly(mask, polygon, cv::Scalar(255), cv::LINE_8, 8);
    cv::Mat occupied;
    cv::compare(labels, 0, occupied, cv::CMP_GT);
    cv::Mat overlap;
    cv::bitwise_and(mask, occupied, overlap);
    if (p.choice("overlap") == "error" && cv::countNonZero(overlap))
      return img::badInput("实例投影发生重叠", "instances");
    if (p.choice("overlap") == "first")
      mask.setTo(0, occupied);
    labels.setTo(label, mask);
  }
  return emitLabels(out, std::move(labels), ids, p.text("frame"), input->scale);
}

Status instanceMorphology(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const Image* input = in.get("labels").asImage();
  auto s = image_p1::image(input, "labels", true);
  if (!s.ok)
    return s;
  const Record* index = in.get("index").asRecord();
  std::string why;
  if (input->depth != PixelDepth::U16 || !index || index->type != "vision.InstanceLabels.v1" ||
      !values::boundedJson(index->data, why) || !index->data.is_object() ||
      index->data.size() != 4 || index->data.value("unit", nlohmann::json()) != "px" ||
      !index->data.contains("frame") || !index->data["frame"].is_string() ||
      index->data["frame"].get<std::string>().empty() ||
      index->data.value("imageSize", nlohmann::json()) !=
          nlohmann::json::array({std::int64_t(input->width) * input->scale,
                                 std::int64_t(input->height) * input->scale}) ||
      !index->data.contains("ids") || !index->data["ids"].is_array() ||
      index->data["ids"].size() > 4096)
    return img::badInput("需要u16标签图和匹配的vision.InstanceLabels.v1", "index");
  const auto& ids = index->data["ids"];
  std::set<std::string> unique;
  for (const auto& id : ids)
    if (!id.is_string() || id.get<std::string>().empty() ||
        !unique.insert(id.get<std::string>()).second)
      return img::badInput("标签身份必须非空且唯一", "index");
  double maxLabel = 0;
  cv::minMaxLoc(cvx::view(*input), nullptr, &maxLabel);
  if (maxLabel > ids.size() || ids.size() * input->pixelCount() > 134217728)
    return img::badInput("标签索引越界或形态学超过128Mi像素工作预算", "labels");
  const int kernel = static_cast<int>(p.integer("kernel"));
  if (kernel < 1 || kernel > 63 || kernel % 2 != 1)
    return img::badParam("核大小须为[1,63]内的奇数", "kernel");
  const cv::Mat element = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(kernel, kernel));
  const cv::Mat original = cvx::view(*input);
  cv::Mat result(original.size(), CV_16UC1, cv::Scalar(0));
  const auto& mode = p.choice("operation");
  const int operation = mode == "dilate"  ? cv::MORPH_DILATE
                        : mode == "erode" ? cv::MORPH_ERODE
                        : mode == "open"  ? cv::MORPH_OPEN
                                          : cv::MORPH_CLOSE;
  for (std::size_t label = 1; label <= ids.size(); ++label) {
    if (ctx.cancelled())
      return Status::Ok();
    cv::Mat mask;
    cv::compare(original, label, mask, cv::CMP_EQ);
    cv::morphologyEx(mask, mask, operation, element, cv::Point(-1, -1), 1, cv::BORDER_CONSTANT,
                     cv::Scalar(0));
    cv::Mat occupied;
    cv::compare(result, 0, occupied, cv::CMP_GT);
    cv::Mat overlap;
    cv::bitwise_and(mask, occupied, overlap);
    if (p.choice("overlap") == "error" && cv::countNonZero(overlap))
      return img::badInput("独立实例形态学结果发生重叠", "labels");
    if (p.choice("overlap") == "first")
      mask.setTo(0, occupied);
    result.setTo(static_cast<int>(label), mask);
  }
  if (p.flag("preserveOriginal") && mode == "dilate") {
    cv::Mat occupied;
    cv::compare(original, 0, occupied, cv::CMP_GT);
    original.copyTo(result, occupied);
  }
  return emitLabels(out, std::move(result), ids, index->data["frame"], input->scale);
}

std::vector<Param> overlapParams() {
  return {img::enumParam("overlap", "重叠处理", "first",
                         {{"first", "保留前项", "集合顺序优先"},
                          {"last", "保留后项", "后项覆盖"},
                          {"error", "报告冲突", "任意重叠即失败"}},
                         "稳定集合顺序决定标签优先级。")};
}

} // namespace

void registerImageP1Basic(Registry& r) {
  auto op = image_p1::descriptor("image.concat", "图像排布拼接",
                                 "image concatenate horizontal vertical padding");
  op.inputs = {img::imageIn("a"), img::imageIn("b")};
  op.outputs = {img::imageOut()};
  op.params = {img::enumParam("direction", "拼接方向", "horizontal",
                              {{"horizontal", "水平", ""}, {"vertical", "垂直", ""}}, ""),
               img::floatParam("padding", "边缘填充值", 0, "使用输入位深原值。")};
  op.doc = "拼接两幅同位深/通道/比例图像，尺寸不足处填充，不做全景配准或隐式颜色转换。";
  op.compute = &concat;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.color_adjust", "图像颜色调整",
                            "brightness contrast gamma hue saturation RGB channel gain offset");
  op.inputs = {img::imageIn()};
  op.outputs = {img::imageOut()};
  op.params = {img::floatParam("brightness", "亮度偏移", 0, "归一化[-1,1]。"),
               img::floatParam("contrast", "对比度增益", 1, "[0,16]，围绕0.5。"),
               img::floatParam("gamma", "Gamma", 1, "[0.05,20]，输出幂为1/gamma。"),
               img::floatParam("hue", "色相偏移", 0, "度，[-360,360]。"),
               img::floatParam("saturation", "饱和度增益", 1, "[0,16]。"),
               image_p1::vector("gain", "RGB增益", {1, 1, 1}),
               image_p1::vector("offset", "RGB归一化偏移", {0, 0, 0})};
  op.doc = "支持u8/u16/f32灰度、RGB、RGBA；归一化颜色结果裁到[0,1]，alpha保持原值。";
  op.compute = &color;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.shading_correct", "空间光照校正",
                            "shading flat field background correction");
  op.inputs = {img::imageIn(), Port{"reference", "Image", "Reference", "可选平场参考。", false}};
  op.outputs = {img::imageOut()};
  op.params = {img::enumParam("mode", "校正方法", "divide",
                              {{"divide", "平场除法", ""}, {"subtract", "背景差分", ""}}, ""),
               img::intParam("kernel", "背景估计核", 31, 3, 511, "无参考时使用奇数高斯核。"),
               img::floatParam("epsilon", "最小分母", 1e-6, "输入原值单位；非正或小分母报告错误。"),
               img::floatParam("gain", "结果增益", 1, "[0,16]。")};
  op.compute = &shading;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.instances_mask", "工件实例掩膜生成",
                            "pose instance rotated rectangle label mask rasterization");
  op.inputs = {img::imageIn(), image_p1::recordPort("instances", poses::kPoses2DType)};
  op.outputs = {img::imageOut("mask"), img::imageOut("labels"),
                image_p1::recordPort("index", "vision.InstanceLabels.v1")};
  op.params = overlapParams();
  op.params.push_back(image_p1::text("frame", "图像参考帧", "image"));
  op.doc = "像素Pose2D旋转矩形光栅化，输出二值mask、u16标签及稳定id映射。不是模型实例分割。";
  op.compute = &instanceMask;
  r.addOperator(std::move(op));

  op = image_p1::descriptor("image.instances_morphology", "逐实例形态学",
                            "instance morphology dilate erode open close label conflicts");
  op.inputs = {img::imageIn("labels"), image_p1::recordPort("index", "vision.InstanceLabels.v1")};
  op.outputs = {img::imageOut("mask"), img::imageOut("labels"),
                image_p1::recordPort("index", "vision.InstanceLabels.v1")};
  op.params = overlapParams();
  op.params.push_back(img::enumParam("operation", "形态操作", "dilate",
                                     {{"dilate", "膨胀", ""},
                                      {"erode", "腐蚀", ""},
                                      {"open", "开运算", ""},
                                      {"close", "闭运算", ""}},
                                     ""));
  op.params.push_back(img::intParam("kernel", "椭圆核大小", 3, 1, 63, "奇数。"));
  op.params.push_back(
      img::boolParam("preserveOriginal", "膨胀保留原标签", true, "仅膨胀时防止覆盖原标签像素。"));
  op.compute = &instanceMorphology;
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
