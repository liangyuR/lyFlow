// glue.color_model「胶色建模」（docs/glue-measure-ops.md §1.3.1）：沿示教线取胶与背景的对数色度样本，拟合颜色模型。
// quality 只测量（分离度、样本数、高光占比……），够不够用由宿主判。
#include <cmath>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

ColorTrainSpec specOf(const ParamView& params) {
  ColorTrainSpec spec;
  spec.pixel.blurSigma = params.number("blurSigma");
  spec.pixel.satLevel = params.number("satLevel");
  spec.pixel.satDilate = static_cast<int>(params.integer("satDilate"));
  spec.pixel.noiseK = params.number("noiseK");
  spec.pixel.noiseMax = params.number("noiseMax");
  spec.pixel.darkRatio = params.number("darkRatio");
  spec.pixel.chromaFloor = params.number("chromaFloor");
  spec.pixel.glueGate = params.number("glueGate");
  spec.glueHalf = params.number("glueHalf");
  const auto band = params.vec2("bgBand");
  spec.bgInner = band[0];
  spec.bgOuter = band[1];
  spec.stationStep = params.number("stationStep");
  spec.recenterRounds = static_cast<int>(params.integer("recenterRounds"));
  spec.shiftMax = params.number("shiftMax");
  spec.trimSigma = params.number("trimSigma");
  spec.bgExcludeSigma = params.number("bgExcludeSigma");
  spec.maxComponents = static_cast<int>(params.integer("maxComponents"));
  return spec;
}

bool sameSpec(const ColorSpec& a, const ColorSpec& b) {
  return a.blurSigma == b.blurSigma && a.satLevel == b.satLevel && a.satDilate == b.satDilate &&
         a.noiseK == b.noiseK && a.noiseMax == b.noiseMax && a.darkRatio == b.darkRatio &&
         a.chromaFloor == b.chromaFloor && a.glueGate == b.glueGate;
}

std::vector<Px> pxList(const std::vector<P2>& pts) {
  std::vector<Px> out;
  out.reserve(pts.size());
  for (const P2& p : pts)
    out.push_back(Px{p.x, p.y});
  return out;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat rgb = rgbOf(inputs.get("image"));
  if (rgb.empty())
    return badColorImage("image");
  Polyline line;
  if (!readLine(inputs.get("line"), &line))
    return Status::Error(Phase::Execute, "bad_input", "line 不是 glue.Polyline", {}, "line");
  const ColorTrainSpec spec = specOf(params);
  ColorModel prior;
  const bool hasPrior = inputs.has("prior");
  if (hasPrior) {
    const Record* rec = inputs.get("prior").asRecord();
    std::string why;
    if (!rec || rec->type != kColorModelType || !readColorModel(rec->data, &prior, &why))
      return Status::Error(Phase::Execute, "bad_input", "prior 不是合格的 glue.ColorModel：" + why,
                           {}, "prior");
    if (!prior.hasStats)
      return Status::Error(Phase::Execute, "bad_input",
                           "prior 不带 stats（生产用的精简模型），不能累加", {}, "prior");
    if (!sameSpec(prior.spec, spec.pixel))
      return Status::Error(Phase::Execute, "bad_input",
                           "prior 的像素参数（blurSigma、satLevel……）与本节点不同，统计不能相加",
                           {}, "prior");
  }
  const ColorTrainResult tr =
      trainColorModel(rgb, line.view(), spec, hasPrior ? &prior.stats : nullptr);
  const ColorModel& m = tr.model;
  const ColorStats& st = m.stats;

  Json rec = colorModelJson(m);
  const double counted = st.glueN + st.bgN + st.bgGlueLike + st.saturated + st.dark;
  const auto frac = [&](double v) { return counted > 0 ? Json(v / counted) : Json(); };
  const double spread =
      std::sqrt(0.5 * (m.glue.cov[0] + m.glue.cov[2]) +
                std::sqrt(0.25 * (m.glue.cov[0] - m.glue.cov[2]) * (m.glue.cov[0] - m.glue.cov[2]) +
                          m.glue.cov[1] * m.glue.cov[1]));
  rec["train"] = Json{{"glueHalf", spec.glueHalf},
                      {"bgBand", Json::array({spec.bgInner, spec.bgOuter})},
                      {"stationStep", spec.stationStep},
                      {"recenterRounds", spec.recenterRounds},
                      {"shiftMax", spec.shiftMax}};
  rec["quality"] = Json{{"usable", m.usable()},
                        {"separation", exact(colorSeparation(m))},
                        {"glueCount", st.glueN},
                        {"bgCount", st.bgN},
                        {"components", m.background.size()},
                        {"glueSpread", st.glueN >= 2 ? exact(spread) : Json()},
                        {"saturatedFraction", frac(st.saturated)},
                        {"darkFraction", frac(st.dark)},
                        {"glueLikeFraction", frac(st.bgGlueLike)},
                        {"frames", st.frames},
                        {"stations", tr.s.size()},
                        {"centerShift", exact(tr.centerShift)},
                        {"recentered", tr.recentered}};
  Record out;
  out.type = kColorModelType;
  out.data = std::move(rec);
  outputs.set("model", Data::record(std::move(out)));

  Overlay2D ov;
  if (!line.points.empty())
    ov.polyline("path", pxList(line.points), false, "示教线");
  if (!tr.centers.empty()) {
    std::vector<P2> lo, hi, b0, b1, b2, b3;
    for (std::size_t i = 0; i < tr.centers.size(); ++i) {
      P2 c, t;
      line.view().at(tr.s[i], &c, &t);
      const P2 n = perp(t);
      const P2 m0 = tr.centers[i];
      lo.push_back(m0 - n * spec.glueHalf);
      hi.push_back(m0 + n * spec.glueHalf);
      b0.push_back(m0 - n * spec.bgOuter);
      b1.push_back(m0 - n * spec.bgInner);
      b2.push_back(m0 + n * spec.bgInner);
      b3.push_back(m0 + n * spec.bgOuter);
    }
    ov.polyline("center", pxList(tr.centers), false, "胶中心");
    ov.polyline("glue", pxList(lo), false, "胶样本");
    ov.polyline("glue", pxList(hi), false);
    for (const auto* b : {&b0, &b1, &b2, &b3})
      ov.polyline("background", pxList(*b), false, b == &b0 ? "背景样本" : "");
  }
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const auto band = params.vec2("bgBand");
  if (!(band[0] > params.number("glueHalf")) || !(band[1] > band[0])) {
    issues.push_back(Issue::error("bad_param",
                                  "bgBand 要满足 glueHalf < inner < outer（现在是 [" +
                                      fmt(band[0], 1) + ", " + fmt(band[1], 1) + "]）",
                                  "bgBand"));
  }
  return issues;
}

/// 建模与出图共用的像素级参数（存进模型；color_map 读模型里的这一份）。
std::vector<Param> colorPixelParams() {
  Param satDilate =
      intParam("satDilate", "Saturation Dilate", 2, "高光掩膜的膨胀半径（px）。", 0, 20, true);
  satDilate.unit = "px";
  return {
      rangedFloat("blurSigma", "Blur Sigma", 0.8, "px",
                  "求色度前各通道的高斯 σ，压掉去马赛克的假色；0 = 不平滑。", 0.0, 5.0, true),
      rangedFloat("satLevel", "Saturation Level", 250.0, "",
                  "任一通道（平滑前）≥ 它即高光：色度无信息。", 1.0, 255.0, true),
      satDilate,
      rangedFloat("noiseK", "Noise K", 4.0, "",
                  "色度噪声的标准差 ≈ noiseK / 亮度，加到每个高斯的协方差上。", 0.0, 100.0, true),
      rangedFloat("noiseMax", "Noise Max", 0.25, "",
                  "噪声大于它（亮度 < noiseK / noiseMax）的像素是暗区。", 0.001, 10.0, true),
      rangedFloat("darkRatio", "Dark Ratio", 0.5, "",
                  "亮度 < darkRatio × 胶亮度 P5 判暗背景（得分 0）；其余暗区无信息。", 0.0, 1.0,
                  true),
      rangedFloat("chromaFloor", "Chroma Floor", 0.03, "", "协方差的下限（对数色度的标准差）。",
                  0.001, 1.0, true),
      rangedFloat("glueGate", "Glue Gate", 4.0, "",
                  "离胶与全部「胶—背景」连线的马氏距离超过它，得分被压低（离群颜色不算胶）。", 0.5,
                  50.0, true),
  };
}

} // namespace

void registerColorModel(Registry& r) {
  OperatorDesc op;
  op.id = "glue.color_model";
  op.version = "1.0.0";
  op.label = "胶色建模";
  op.category = "涂胶/彩色";
  op.keywords = {"glue", "color", "model", "chroma", "涂胶", "颜色", "彩色", "建模", "示教"};
  op.doc =
      "沿示教线每 stationStep 一站，中心 ± glueHalf 取胶、两侧 bgBand 取背景，特征是平滑后的对数色度 "
      "(ln R/G, ln B/G)；高光（任一通道 ≥ satLevel）与暗区（亮度 < noiseK / noiseMax）的样本不用。"
      "胶拟合一个高斯（截掉 trimSigma 外的样本），背景从二维直方图做确定性 EM、≤ maxComponents 个高斯。"
      "按这一帧的模型给每站卡尺打分、取离中心最近的胶段中点重新居中，重复 recenterRounds 轮。\n"
      "接了 prior：统计相加后再拟合（每一帧的统计只由这一帧算，累加与顺序无关）。"
      "输出 glue.ColorModel（含可相加的 stats 与只测量的 quality），把它交给 glue.color_map。";
  op.inputs = {
      Port{"image", "Image", "Image", "示教帧（u8 RGB / RGBA，全分辨率）。", true},
      withContract(Port{"line", "Record", "Line", "glue.Polyline：示教线（沿胶中心画）。", true},
                   {{"recordType", kPolylineType}}),
      withContract(Port{"prior", "Record", "Prior",
                        "可选：之前的 glue.ColorModel，统计累加进来（像素参数要相同）。", false},
                   {{"recordType", kColorModelType}}),
  };
  op.outputs = {
      withContract(Port{"model", "Record", "Model",
                        "glue.ColorModel：version、space、spec、glue{n, mean, cov, luma}、"
                        "background[{weight, mean, cov}]、stats、train、quality。",
                        true},
                   {{"recordType", kColorModelType}}),
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：示教线、胶中心、胶与背景的取样带。",
           true},
  };
  const double kOpen = std::numeric_limits<double>::quiet_NaN();
  op.params = {
      rangedFloat("glueHalf", "Glue Half", 4.0, "px", "胶样本：中心 ± glueHalf（小于胶的半宽）。",
                  0.0, 100.0),
      vec2Param("bgBand", "Background Band", 20.0, 40.0, "px", {"Inner", "Outer"},
                "背景样本：离胶中心 [inner, outer]（两侧）。"),
      rangedFloat("stationStep", "Station Step", 4.0, "px", "沿线取样的站距。", 1.0, 100.0, true),
      intParam("recenterRounds", "Recenter Rounds", 2, "按得分重新居中的轮数；0 = 不居中。", 0, 5,
               true),
      rangedFloat("shiftMax", "Shift Max", 20.0, "px", "重新居中的偏移上限。", 0.0, kOpen, true),
      rangedFloat("trimSigma", "Trim Sigma", 3.0, "", "胶样本截尾的马氏距离。", 0.5, 20.0, true),
      rangedFloat("bgExcludeSigma", "Background Exclude Sigma", 3.0, "",
                  "背景带里离胶马氏距离小于它的样本当像胶、不进背景。", 0.0, 20.0, true),
      intParam("maxComponents", "Max Components", 3, "背景高斯的最多个数。", 1, 3, true),
  };
  for (Param& p : colorPixelParams())
    op.params.push_back(std::move(p));
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::glue
