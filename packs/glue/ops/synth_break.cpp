// glue.synth_break「人造断胶」：验收与自测用的工具积木（glue-plan §4 第 16 条的数据集由它生成）。
// 接在一条检测链的 bead_width 后面，出的图再喂给另一条检测链，看断胶查不查得出来。
#include <cmath>

#include "algo/synth.h"
#include "glue.h"
#include "lyflow_cv/adapter.h"

namespace lyflow::packs::glue {
namespace {

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts) out.push_back(Px{p.x, p.y});
  return out;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat gray = grayOf(inputs.get("image"));
  if (gray.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "需要全分辨率非空 u8 图像；其他位深先接 image.normalize，缩小预览请改用完整运行", {}, "image");
  }
  BeadView bead;
  std::vector<Station> st;
  if (!readBead(inputs.get("bead"), &bead) || !readStations(bead.stations, &st)) {
    return Status::Error(Phase::Execute, "bad_input", "bead 不是完整的 glue.Bead", {}, "bead");
  }
  SynthSpec spec;
  spec.sStart = params.number("sStart");
  spec.length = params.number("length");
  spec.margin = params.number("margin");
  spec.shiftExtra = params.number("shiftExtra");
  spec.dirtyMax = params.number("dirtyMax");
  spec.contrastMin = params.number("contrastMin");
  spec.feather = params.number("feather");
  const bool bright = bead.info.value("polarity", std::string("dark")) == "bright";
  const int widthMax = bead.info.value("widthMax", 90);
  const double step = bead.info.value("stationStep", 4.0);

  SynthResult res;
  cv::Mat out;
  if (!bead.info.value("pathOk", false)) {
    res.reason = "这一帧没找到胶，没有胶可以盖";
    out = gray.clone();
  } else {
    res = synthBreak(gray, bead.line.view(), st, step, bright, widthMax, spec, out);
  }
  Image img;
  if (!cvx::fromMat(out, img)) {
    return Status::Error(Phase::Execute, "internal", "人造断胶的结果转不回 Image");
  }
  outputs.set("image", Data::image(std::move(img)));

  Json info;
  info["ok"] = res.ok;
  info["reason"] = res.reason;
  info["side"] = res.side > 0 ? "right" : (res.side < 0 ? "left" : "none");
  info["dirtyRight"] = numOrNull(res.dirtyRight, 4);
  info["dirtyLeft"] = numOrNull(res.dirtyLeft, 4);
  info["stations"] = res.stations;
  info["sStart"] = round3(res.sFrom);
  info["sEnd"] = round3(res.sTo);
  info["length"] = round3(res.sTo - res.sFrom);
  // 断口两端在图上的位置（原帧胶路上 s = sStart / sEnd 的点）：另一条检测链的胶路 s 与原帧的不是同一把尺子，
  // 评估时把这两个点投到它的胶路上再比
  P2 a, b, t;
  bead.line.view().at(res.sFrom, &a, &t);
  bead.line.view().at(res.sTo, &b, &t);
  info["start"] = pxJson(a);
  info["end"] = pxJson(b);
  Record rec;
  rec.type = kSynthType;
  rec.data = std::move(info);
  outputs.set("info", Data::record(std::move(rec)));

  Overlay2D ov;
  ov.polyline(res.ok ? "break" : "coarse", pxList(bead.line.between(res.sFrom, res.sTo)), false,
              res.ok ? "人造断口 s " + fmt(res.sFrom, 0) + "–" + fmt(res.sTo, 0)
                     : "没做：" + res.reason);
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

}  // namespace

void registerSynthBreak(Registry& r) {
  OperatorDesc op;
  op.id = "glue.synth_break";
  op.version = "1.0.0";
  op.label = "人造断胶";
  op.category = "涂胶/工具";
  op.keywords = {"glue", "synthetic", "break", "test", "涂胶", "断胶", "人造", "自测"};
  op.doc =
      "验收与自测用：在一帧满胶图上人造一段断胶。把胶路一侧干净的背景平移过来盖住 s ∈ [sStart, sStart + length] "
      "那一段胶，羽化约 3 px；两侧都试，源带里响应 > contrastMin 的采样超过 dirtyMax 就换一侧，两侧都不干净就不做"
      "（info.ok = false，出的图就是原图）。不用 inpaint —— 它会把胶色补回去。\n"
      "接在一条检测链的 bead_width 后面，出的图喂给另一条检测链；info 的 sStart / sEnd 就是断口的真值。";
  op.inputs = {
      Port{"image", "Image", "Image", "满胶的一帧。", true},
      Port{"bead", "Bundle<glue.Bead>", "Bead", "这一帧 bead_width 的结果：胶路与每站的两边。", true},
  };
  op.outputs = {
      Port{"image", "Image", "Image", "盖掉一段胶之后的图（单通道）。没做成时是原图。", true},
      Port{"info", "Record", "Info",
           "glue.SynthBreak：ok、reason、side（源带在哪一侧）、dirtyRight / dirtyLeft、sStart / sEnd / length。", true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：盖掉的那一段。", true},
  };
  Param sStart = floatParam("sStart", "S Start", 200.0, "px", "断口从胶路的哪里开始（s，从喷嘴沿胶路的弧长）。");
  Param length = floatParam("length", "Length", 30.0, "px", "断口多长。");
  length.min = 1.0;
  Param margin = floatParam("margin", "Margin", 4.0, "px", "盖住的带子比胶两边各宽出多少。", true);
  margin.min = 0.0;
  Param extra = floatParam("shiftExtra", "Shift Extra", 12.0, "px", "源带在盖住的带子外再错开多少。", true);
  extra.min = 0.0;
  Param dirty = floatParam("dirtyMax", "Dirty Max", 0.02, "",
                           "源带里响应 > contrastMin 的采样比例超过它就不用这一侧。", true);
  dirty.min = 0.0;
  dirty.max = 1.0;
  Param contrast = floatParam("contrastMin", "Contrast Min", 16.0, "", "判源带干不干净的响应阈值，与 bead_width 的相同。", true);
  Param feather = floatParam("feather", "Feather", 1.5, "px", "羽化的高斯 σ（过渡约两倍）。", true);
  feather.min = 0.1;
  op.params = {sStart, length, margin, extra, dirty, contrast, feather};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
