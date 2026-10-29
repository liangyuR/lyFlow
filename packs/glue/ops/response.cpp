// glue.response「响应图」（docs/glue-measure-ops.md §1）：整幅（或 roi）的黑 / 白顶帽，附带陡度用的平滑灰度与局部亮度。
// 扇形扫描、粗跟踪、横向卡尺共用这一份，只算一次（1.1 的 bead_path 与 bead_width 各算一遍）。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat gray = grayOf(inputs.get("image"));
  if (gray.empty())
    return badImage("image", false);
  ResponseSpec spec;
  spec.bright = params.choice("polarity") == "bright";
  spec.diameter = static_cast<int>(params.integer("diameter"));
  const auto roi = params.vec4("roi");
  spec.roi = cv::Rect(static_cast<int>(std::lround(roi[0])), static_cast<int>(std::lround(roi[1])),
                      static_cast<int>(std::lround(roi[2])), static_cast<int>(std::lround(roi[3])));
  spec.responseSigma = params.number("responseSigma");
  spec.graySigma = params.number("graySigma");
  spec.levelSigma = params.number("levelSigma");
  ResponseMaps maps = responseMaps(gray, spec);
  outputs.set("response", f32Image(std::move(maps.response)));
  outputs.set("gray", f32Image(std::move(maps.gray)));
  outputs.set("level", f32Image(std::move(maps.level)));
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const auto roi = params.vec4("roi");
  const bool whole = roi[2] <= 0 && roi[3] <= 0;
  const bool part = roi[2] > 0 && roi[3] > 0;
  if (!whole && !part) {
    issues.push_back(
        Issue::error("bad_param", "roi 的宽高要同为 > 0（只算这一块）或同为 ≤ 0（整幅）", "roi"));
  }
  return issues;
}

} // namespace

void registerResponse(Registry& r) {
  OperatorDesc op;
  op.id = "glue.response";
  op.version = "1.0.0";
  op.label = "响应图";
  op.category = "涂胶/测量";
  op.keywords = {"glue", "tophat", "response", "涂胶", "顶帽", "响应图", "测量"};
  op.doc =
      "胶的响应图：暗胶取黑顶帽（闭运算 − 原图），亮胶取白顶帽（原图 − 开运算），再做 5×5 高斯。"
      "比结构元窄的暗带才有完整响应，所以 diameter 要大于期望最大胶宽。顺带输出陡度用的平滑灰度 gray"
      "（亮胶时取 255 − g，「胶比背景暗」恒成立）与局部亮度 level（暗胶取闭运算、亮胶取原图）。\n"
      "三张图都是单通道 f32、与输入同尺寸；给了 roi 只算那一块（块里的值与整幅计算逐位相同），块外是 NaN。"
      "扇形扫描、粗跟踪、横向卡尺都接它，极性只在这里设一次。";
  op.inputs = {
      Port{"image", "Image", "Image", "胶枪相机的一帧（u8 灰度 / RGB / RGBA，全分辨率）。", true}};
  op.outputs = {
      Port{"response", "Image", "Response", "顶帽响应（f32，≥ 0；roi 外 NaN）。", true},
      Port{"gray", "Image", "Gray", "平滑灰度（f32，亮胶已取反；roi 外 NaN），边缘陡度用。", true},
      Port{"level", "Image", "Level", "局部亮度（f32；roi 外 NaN），对比度归一化用。", true},
  };
  Param polarity = enumParam("polarity", "Polarity", "dark",
                             "胶比背景暗（dark，黑顶帽）还是亮（bright，白顶帽）。",
                             {EnumOption{"dark", "暗胶", ""}, EnumOption{"bright", "亮胶", ""}});
  Param diameter =
      intParam("diameter", "Diameter", 91,
               "结构元直径（px），应大于期望最大胶宽（1.1 用的是 widthMax + 1）。", 3, 401);
  diameter.unit = "px";
  Param roi;
  roi.name = "roi";
  roi.tuningRole = "geometry";
  roi.type = ParamType::Vec4f;
  roi.label = "ROI";
  roi.doc = "[x, y, w, h]（px）：只算这一块（与图像求交）；w、h 同为 ≤ 0 表示整幅。";
  roi.def = Value::vec({0, 0, 0, 0});
  roi.unit = "px";
  roi.componentLabels = {"X", "Y", "W", "H"};
  Param responseSigma = rangedFloat("responseSigma", "Response Sigma", 1.2, "px",
                                    "响应图的 5×5 高斯 σ；0 = 不平滑。", 0.0, 3.0, true);
  Param graySigma = rangedFloat("graySigma", "Gray Sigma", 1.0, "px",
                                "gray 的 5×5 高斯 σ；0 = 不平滑。", 0.0, 3.0, true);
  Param levelSigma = rangedFloat("levelSigma", "Level Sigma", 1.2, "px",
                                 "level 的 5×5 高斯 σ；0 = 不平滑。", 0.0, 3.0, true);
  op.params = {polarity, diameter, roi, responseSigma, graySigma, levelSigma};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
