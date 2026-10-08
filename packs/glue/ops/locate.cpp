// glue.locate「定位」（glue-plan §5，K2）：固定相机飞拍时，按示教时裁下的模板求这一帧相对示教图的刚体位姿。
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

#include <opencv2/imgcodecs.hpp>

#include "algo/flyshot.h"
#include "glue.h"
#include "lyflow_cv/adapter.h"

namespace lyflow::packs::glue {
namespace {

Status loadTemplate(const ParamView& params, cv::Mat* out) {
  std::ifstream in(params.path("template"), std::ios::binary);
  if (!in) return Status::Error(Phase::Execute, "io", "打不开模板图像", "template");
  const std::vector<uchar> bytes(std::istreambuf_iterator<char>(in), {});
  try {
    if (!bytes.empty()) *out = cv::imdecode(bytes, cv::IMREAD_GRAYSCALE | cv::IMREAD_ANYDEPTH);
  } catch (const cv::Exception&) {
    return Status::Error(Phase::Execute, "bad_param", "模板不是可解码的图像", "template");
  }
  if (out->empty() || out->type() != CV_8UC1) {
    return Status::Error(Phase::Execute, "bad_param", "模板需要非空 u8 图像", "template");
  }
  return Status::Ok();
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat gray = grayOf(inputs.get("image"));
  if (gray.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "需要全分辨率非空 u8 图像；其他位深先接 image.normalize，缩小预览请改用完整运行", {}, "image");
  }
  cv::Mat tmpl;
  if (Status s = loadTemplate(params, &tmpl); !s.ok) return s;

  LocateSpec spec;
  const auto anchor = params.vec2("anchor");
  spec.anchor = P2(anchor[0], anchor[1]);
  spec.searchRadius = params.number("searchRadius");
  spec.angleRange = params.number("angleRange");
  spec.angleStep = params.number("angleStep");
  spec.minScore = params.number("minScore");
  std::string why;
  const Pose2D pose = locateTemplate(gray, tmpl, spec, &why);

  Json rec;
  rec["ok"] = pose.ok;
  rec["message"] = pose.ok ? "" : why;
  rec["score"] = round3(pose.score);
  rec["angle"] = round3(pose.angleDeg);
  rec["center"] = pxJson(pose.center);
  rec["teachCenter"] = pxJson(pose.teachCenter);
  rec["offset"] = pxJson(pose.center - pose.teachCenter);
  rec["templateSize"] = Json::array({tmpl.cols, tmpl.rows});
  rec["minScore"] = spec.minScore;
  Record out;
  out.type = kPoseType;
  out.data = std::move(rec);
  outputs.set("pose", Data::record(std::move(out)));
  outputs.set("score", measurement(pose.score, "", why));

  // 叠画：模板四角按位姿映射到当前图（示教图上它是 anchor 起的一个框）
  const double w = tmpl.cols - 1.0, h = tmpl.rows - 1.0;
  std::vector<Px> corners;
  for (const P2 c : {P2(0, 0), P2(w, 0), P2(w, h), P2(0, h)}) {
    const P2 p = pose.apply(spec.anchor + c);
    corners.push_back(Px{p.x, p.y});
  }
  Overlay2D ov;
  const std::string label = "定位 " + fmt(pose.score, 2) + (pose.ok ? "" : "（低于 " + fmt(spec.minScore, 2) + "）");
  ov.polyline(pose.ok ? "station" : "missing", corners, /*closed=*/true, label);
  ov.points(pose.ok ? "part" : "outlier", {Px{pose.center.x, pose.center.y}});
  if (!pose.ok) ov.text("ng", "定位失败", Px{pose.center.x, pose.center.y});
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

/// 模板文件被覆盖（重新示教）时必须重算。
std::string externalKey(const ParamView& params) {
  std::error_code ec;
  const auto file = params.path("template");
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return {};
  const auto mtime = std::filesystem::last_write_time(file, ec);
  if (ec) return {};
  return std::to_string(size) + ":" + std::to_string(mtime.time_since_epoch().count());
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  if (params.number("angleRange") > 0 && params.number("angleStep") <= 0) {
    issues.push_back(Issue::error("bad_param", "angleStep 要大于 0", "angleStep"));
  }
  return issues;
}

}  // namespace

void registerLocate(Registry& r) {
  OperatorDesc op;
  op.id = "glue.locate";
  op.version = "1.0.0";
  op.label = "定位";
  op.category = "涂胶/飞拍";
  op.keywords = {"glue", "locate", "template", "match", "pose", "涂胶", "定位", "模板", "飞拍"};
  op.doc =
      "固定相机飞拍：按示教时从示教图上裁下的模板，求这一帧相对示教图的刚体位姿"
      "（T(p) = R(angle)·(p − teachCenter) + center，glue-plan K2）。只在模板外扩 searchRadius 的一块里做"
      "归一化互相关，角度在 ±angleRange 里按 angleStep 扫描后抛物线插值。\n"
      "分数低于 minScore 时 pose.ok = false（错误即值）：下游 glue.station_calipers 全部站判 pose_fail，"
      "宿主据此报定位失败。";
  op.inputs = {Port{"image", "Image", "Image", "这一帧。", true}};
  op.outputs = {
      Port{"pose", "Record", "Pose",
           "glue.Pose2D：ok、score、angle（度，屏幕上顺时针为正）、center、teachCenter、offset、templateSize。",
           true},
      Port{"score", "Measurement", "Score", "归一化互相关的分数（−1…1）。", true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：模板框按位姿映射到这一帧。", true},
  };
  Param tmpl;
  tmpl.name = "template";
  tmpl.type = ParamType::Path;
  tmpl.label = "Template";
  tmpl.doc = "模板图像（示教时从示教图上裁下的一块）。相对路径相对于图文件所在目录。";
  tmpl.def = Value::text("");
  tmpl.mode = "open";
  tmpl.filters = {FileFilter{"Image", {"png", "jpg", "jpeg", "bmp", "tif", "tiff", "pgm"}},
                  FileFilter{"All Files", {"*"}}};
  Param anchor = vec2Param("anchor", "Anchor", 0, 0, "px", {"X", "Y"},
                           "模板左上角像素在示教图上的位置（裁模板时的偏移）。");
  Param radius = floatParam("searchRadius", "Search Radius", 80.0, "px", "模板中心最多偏开示教位置这么远。");
  radius.min = 1.0;
  Param range = floatParam("angleRange", "Angle Range", 3.0, "°", "角度扫描范围 ±range；0 = 只找平移。", true);
  range.min = 0.0;
  range.max = 45.0;
  Param step = floatParam("angleStep", "Angle Step", 0.5, "°", "角度扫描步长（结果按抛物线插值到步长以下）。", true);
  step.min = 0.05;
  Param minScore = floatParam("minScore", "Min Score", 0.6, "", "分数低于它判定位失败。");
  minScore.min = -1.0;
  minScore.max = 1.0;
  op.params = {tmpl, anchor, radius, range, step, minScore};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.externalKey = &externalKey;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
