// glue.color_map「胶色得分图」（docs/glue-measure-ops.md §1.3.2）：按颜色模型给 roi 里每个像素打分，三态分开。
// 扇形扫描、彩色跟踪、彩色卡尺共用这一份，只算一次。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

/// model 端口优先；没接就读 model 参数。why 给不合格的原因。
bool modelOf(const Inputs& inputs, const ParamView& params, ColorModel* m, bool* fromPort,
             std::string* why) {
  *fromPort = inputs.has("model");
  if (*fromPort) {
    const Record* rec = inputs.get("model").asRecord();
    if (!rec || rec->type != kColorModelType) {
      *why = "不是 glue.ColorModel";
      return false;
    }
    return readColorModel(rec->data, m, why);
  }
  const std::string& text = params.text("model");
  if (text.empty()) {
    *why = "没给颜色模型（接 glue.color_model 的 model，或把它的 JSON 填进 model 参数）";
    return false;
  }
  const Json j = Json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    *why = "model 不是 JSON";
    return false;
  }
  return readColorModel(j, m, why);
}

cv::Rect roiOf(const ParamView& params) {
  const auto roi = params.vec4("roi");
  return cv::Rect(static_cast<int>(std::lround(roi[0])), static_cast<int>(std::lround(roi[1])),
                  static_cast<int>(std::lround(roi[2])), static_cast<int>(std::lround(roi[3])));
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const cv::Mat rgb = rgbOf(inputs.get("image"));
  if (rgb.empty())
    return badColorImage("image");
  ColorModel m;
  bool fromPort = false;
  std::string why;
  if (!modelOf(inputs, params, &m, &fromPort, &why)) {
    return fromPort ? Status::Error(Phase::Execute, "bad_input", "model：" + why, {}, "model")
                    : Status::Error(Phase::Execute, "bad_param", "model：" + why, "model");
  }
  ColorMaps maps = colorMaps(rgb, m, roiOf(params), ctx.threadBudget());
  if (maps.frame.empty()) {
    return Status::Error(Phase::Execute, "bad_param",
                         "roi 与图像（" + std::to_string(rgb.cols) + "×" +
                             std::to_string(rgb.rows) + "）不相交",
                         "roi");
  }
  outputs.set("maps", colorMapsBundle(std::move(maps), rgb.size()));
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  const auto roi = params.vec4("roi");
  const bool whole = roi[2] <= 0 && roi[3] <= 0;
  const bool part = roi[2] > 0 && roi[3] > 0;
  if (!whole && !part) {
    issues.push_back(
        Issue::error("bad_param", "roi 的宽高要同为 > 0（只算这一块）或同为 ≤ 0（整幅）", "roi"));
  }
  if (!connected.count("model")) {
    std::string why;
    if (params.text("model").empty()) {
      why = "没给颜色模型（接 glue.color_model 的 model，或把它的 JSON 填进 model 参数）";
    } else {
      const Json j = Json::parse(params.text("model"), nullptr, /*allow_exceptions=*/false);
      ColorModel m;
      if (j.is_discarded())
        why = "model 不是 JSON";
      else
        readColorModel(j, &m, &why);
    }
    if (!why.empty())
      issues.push_back(Issue::error("bad_param", "model：" + why, "model"));
  }
  return issues;
}

} // namespace

void registerColorMap(Registry& r) {
  OperatorDesc op;
  op.id = "glue.color_map";
  op.version = "1.0.0";
  op.label = "胶色得分图";
  op.category = "涂胶/彩色";
  op.keywords = {"glue", "color", "score", "map", "涂胶", "颜色", "彩色", "得分图"};
  op.doc =
      "按 glue.ColorModel 给每个像素打「像胶」的得分 ∈ [0, 1]：特征是平滑后的对数色度，胶与每个背景分量做"
      "线性判别（混合像素在中点处得 0.5），离胶与全部「胶—背景」连线都远的颜色另被压低。\n"
      "三态：高光（任一通道 ≥ satLevel，膨胀 satDilate）色度无信息，得分是 NaN；亮度 < darkRatio × 胶的亮度 P5 的"
      "判暗背景，得分 0；其余暗区（亮度 < noiseK / noiseMax）也无信息。state 图给每个像素属于哪一态。"
      "模型的像素参数全在模型里（与建模时同一份）。给了 roi 只有 roi ∩ 图像那一块（块里的值与整幅逐位相同）。";
  op.inputs = {
      Port{"image", "Image", "Image", "生产帧（u8 RGB / RGBA，全分辨率）。", true},
      withContract(Port{"model", "Record", "Model",
                        "可选：glue.color_model 的 model；接了就不用 model 参数。", false},
                   {{"recordType", kColorModelType}}),
  };
  op.outputs = {
      Port{
          "maps", "Bundle<glue.ColorMaps>", "Maps",
          "score（得分，NaN = 无信息）、luma（亮度）、state（0 有信息 / 1 高光 / 2 暗区 / 3 暗背景）三张 f32 图，"
          "只有 frame 那一块。",
          true},
  };
  Param model;
  model.name = "model";
  model.type = ParamType::Text;
  model.label = "Model";
  model.doc = "glue.ColorModel 的 JSON（宿主存在配方里逐帧给）；接了 model 端口时不用。";
  model.def = Value::text("");
  model.rows = 4;
  model.placeholder = "{\"version\": 1, \"space\": \"log_chroma\", …}";
  model.tuningRole = "detection";
  Param roi;
  roi.name = "roi";
  roi.tuningRole = "geometry";
  roi.type = ParamType::Vec4f;
  roi.label = "ROI";
  roi.doc =
      "[x, y, w, h]（px）：只算、只输出这一块（与图像求交，不相交是 bad_param）；w、h 同为 ≤ 0 表示整幅。";
  roi.def = Value::vec({0, 0, 0, 0});
  roi.unit = "px";
  roi.componentLabels = {"X", "Y", "W", "H"};
  op.params = {model, roi};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
