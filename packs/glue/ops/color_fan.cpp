// glue.color_fan「彩色扇形扫描」（docs/glue-measure-ops.md §1.3.3）：glue.fan_scan 原样用在得分图上。
// 无信息（NaN）的采样与块外一样算无效。
#include "glue.h"

namespace lyflow::packs::glue {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  ColorMapsView maps;
  if (!readColorMaps(inputs.get("maps"), &maps))
    return Status::Error(Phase::Execute, "bad_input",
                         "需要 glue.color_map 输出的 maps（Bundle<glue.ColorMaps>）", {}, "maps");
  return fanScanCompute(maps.score, params, outputs);
}

} // namespace

void registerColorFan(Registry& r) {
  OperatorDesc op;
  op.id = "glue.color_fan";
  op.version = "1.0.0";
  op.label = "彩色扇形扫描";
  op.category = "涂胶/彩色";
  op.keywords = {"glue", "color", "fan", "sector", "heading", "涂胶", "彩色", "扇形", "方向"};
  op.doc =
      "与 glue.fan_scan 相同（参数、输出、取峰规则一位不差），只是射线上采样的是 glue.color_map 的得分："
      "得分 = 有效采样之和 / 总采样数，高光、暗区（NaN）与块外的采样都算无效。输出 glue.FanScan，"
      "可直接接 glue.color_track 的 fan。";
  op.inputs = {Port{"maps", "Bundle<glue.ColorMaps>", "Maps",
                    "glue.color_map 的 maps（用 score；NaN 与块外的采样算无效）。", true}};
  op.outputs = {
      Port{
          "scan", "Record", "Scan",
          "glue.FanScan：anchor、sector、radial、angleStep、radialStep、samples、angles[]、scores[]、"
          "invalid[]、peaks[{index, angle, score}]。",
          true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：锚点、扇区、径向区间、峰方向。",
           true},
  };
  op.params = fanScanParams();
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &fanScanValidate;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
