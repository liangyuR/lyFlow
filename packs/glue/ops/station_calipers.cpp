// glue.station_calipers「逐点卡尺」（glue-plan §5，K1 / K3 / K4 / K5）：在宿主给的名义胶路测量点上，
// 沿法向量内边与胶条两条边。判定不在这里（宿主按测量模式取值、跨帧合并断胶）。
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

#include "algo/flyshot.h"
#include "glue.h"

namespace lyflow::packs::glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct StationList {
  std::vector<P2> points;
  std::vector<P2> normals;
  Json ids = Json::array();
};

Status loadStations(const ParamView& params, StationList* out) {
  const std::filesystem::path file = params.path("stations");
  auto bad = [&](const std::string& m) {
    return Status::Error(Phase::Execute, "bad_param", "stations 文件 " + file.u8string() + "：" + m, "stations");
  };
  std::ifstream in(file, std::ios::binary);
  if (!in) return Status::Error(Phase::Execute, "io_error", "打不开 stations 文件：" + file.u8string(), "stations");
  Json doc;
  try {
    doc = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
  } catch (const std::exception& e) {
    return bad(std::string("不是合法的 JSON（") + e.what() + "）");
  }
  if (!doc.is_object()) return bad("需要包含 points 与 normals 的对象");
  const Json& pts = doc.value("points", Json());
  const Json& nrm = doc.value("normals", Json());
  if (!pts.is_array() || !nrm.is_array() || pts.empty() || pts.size() != nrm.size()) {
    return bad("points 与 normals 要是非空等长的数组");
  }
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const P2 p = pxOf(pts[i]);
    const P2 n = pxOf(nrm[i]);
    if (!finite(p) || !finite(n) || !std::isfinite(length(n)) || length(n) < 1e-9) {
      return bad("第 " + std::to_string(i) + " 个点或法向不成立");
    }
    out->points.push_back(p);
    out->normals.push_back(unit(n));
  }
  if (doc.contains("ids")) {
    if (!doc["ids"].is_array() || doc["ids"].size() != pts.size()) return bad("ids 要与 points 等长");
    out->ids = doc["ids"];
  } else {
    for (std::size_t i = 0; i < pts.size(); ++i) out->ids.push_back(i);
  }
  return Status::Ok();
}

/// 读可选的 pose 输入。没接 → 恒等位姿、ok；接了但不是 glue.Pose2D → bad_input。
Status poseFromInput(const Inputs& inputs, Pose2D* pose, bool* given) {
  *given = inputs.has("pose");
  pose->ok = true;
  if (!*given) return Status::Ok();
  const Record* rec = inputs.get("pose").asRecord();
  if (!rec || rec->type != kPoseType) {
    return Status::Error(Phase::Execute, "bad_input", "pose 不是 glue.Pose2D", {}, "pose");
  }
  const Json& d = rec->data;
  if (!d.is_object() || !d.contains("ok") || !d["ok"].is_boolean()) {
    return Status::Error(Phase::Execute, "bad_input", "pose.ok 必须是布尔值", {}, "pose");
  }
  pose->ok = d.value("ok", false);
  pose->angleDeg = numOf(d.value("angle", Json()));
  pose->center = pxOf(d.value("center", Json()));
  pose->teachCenter = pxOf(d.value("teachCenter", Json()));
  if (pose->ok && (!std::isfinite(pose->angleDeg) || !finite(pose->center) || !finite(pose->teachCenter))) {
    return Status::Error(Phase::Execute, "bad_input", "pose 缺 angle / center / teachCenter", {}, "pose");
  }
  return Status::Ok();
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const cv::Mat gray = grayOf(inputs.get("image"));
  if (gray.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "需要全分辨率非空 u8 图像；其他位深先接 image.normalize，缩小预览请改用完整运行", {}, "image");
  }
  StationList list;
  if (Status s = loadStations(params, &list); !s.ok) return s;
  Pose2D pose;
  bool poseGiven = false;
  if (Status s = poseFromInput(inputs, &pose, &poseGiven); !s.ok) return s;
  Metric metric;
  if (Status s = metricFromInput(inputs, "calib", &metric); !s.ok) return s;

  CaliperSpec spec;
  spec.searchHalf = params.number("searchHalf");
  spec.caliperWidth = params.number("caliperWidth");
  const auto innerSearch = params.vec2("innerSearch");
  const auto beadSearch = params.vec2("beadSearch");
  spec.innerFrom = innerSearch[0];
  spec.innerTo = innerSearch[1];
  spec.beadFrom = beadSearch[0];
  spec.beadTo = beadSearch[1];
  spec.innerDarkToBright = params.choice("innerPolarity") == "dark_to_bright";
  spec.innerNearest = params.choice("innerSelect") == "nearest";
  spec.beadDark = params.choice("beadPolarity") == "dark";
  spec.contrastMin = params.number("contrastMin");
  spec.widthMin = params.number("widthMin");

  Json point = Json::array(), normal = Json::array(), status = Json::array(), inner = Json::array(),
       nearE = Json::array(), farE = Json::array(), innerNear = Json::array(), innerCenter = Json::array(),
       width = Json::array(), innerNearPx = Json::array(), innerCenterPx = Json::array(),
       widthPx = Json::array(), innerContrast = Json::array(), beadContrast = Json::array();
  int okCount = 0, noInner = 0, noBead = 0, incomplete = 0, poseFail = 0, outside = 0;
  double centerSum = 0;
  int centerCount = 0;
  Overlay2D ov;
  std::vector<std::array<Px, 2>> good, missing, innerLost;
  std::vector<Px> innerPts, nearPts, farPts;
  auto px = [](P2 p) { return Px{p.x, p.y}; };

  for (std::size_t i = 0; i < list.points.size(); ++i) {
    const P2 c = poseGiven ? pose.apply(list.points[i]) : list.points[i];
    const P2 n = poseGiven ? unit(pose.rotate(list.normals[i])) : list.normals[i];
    auto at = [&](double t) { return c + n * t; };
    point.push_back(pxJson(c));
    normal.push_back(Json::array({round3(n.x), round3(n.y)}));
    CaliperStation st;
    if (poseGiven && !pose.ok) {
      st.status = "pose_fail";
      st.inner = st.nearEdge = st.farEdge = kNaN;
    } else {
      st = measureCaliper(gray, c, n, spec);
    }
    status.push_back(st.status);
    const bool ok = st.status == "ok";
    inner.push_back(numOrNull(st.inner));
    nearE.push_back(numOrNull(ok ? st.nearEdge : kNaN));
    farE.push_back(numOrNull(ok ? st.farEdge : kNaN));
    innerContrast.push_back(numOrNull(std::isfinite(st.inner) ? st.innerContrast : kNaN, 1));
    beadContrast.push_back(numOrNull(ok ? st.beadContrast : kNaN, 1));
    const double mid = ok ? 0.5 * (st.nearEdge + st.farEdge) : kNaN;
    if (ok && (!std::isfinite(metric.distance(at(st.inner), at(st.nearEdge))) ||
               !std::isfinite(metric.distance(at(st.inner), at(mid))) ||
               !std::isfinite(metric.distance(at(st.nearEdge), at(st.farEdge))))) {
      return Status::Error(Phase::Execute, "bad_input", "标定无法映射当前测量点（投影接近无穷远）", {}, "calib");
    }
    innerNear.push_back(numOrNull(ok ? metric.distance(at(st.inner), at(st.nearEdge)) : kNaN));
    innerCenter.push_back(numOrNull(ok ? metric.distance(at(st.inner), at(mid)) : kNaN));
    width.push_back(numOrNull(ok ? metric.distance(at(st.nearEdge), at(st.farEdge)) : kNaN));
    innerNearPx.push_back(numOrNull(ok ? st.nearEdge - st.inner : kNaN));
    innerCenterPx.push_back(numOrNull(ok ? mid - st.inner : kNaN));
    widthPx.push_back(numOrNull(ok ? st.farEdge - st.nearEdge : kNaN));

    if (ok) {
      ++okCount;
      const double d = metric.distance(at(st.inner), at(mid));
      if (std::isfinite(d)) {
        centerSum += d;
        ++centerCount;
      }
      good.push_back({px(at(st.inner)), px(at(std::isfinite(st.farEdge) ? st.farEdge : st.nearEdge))});
      innerPts.push_back(px(at(st.inner)));
      nearPts.push_back(px(at(st.nearEdge)));
      if (std::isfinite(st.farEdge)) farPts.push_back(px(at(st.farEdge)));
    } else if (st.status == "no_inner") {
      ++noInner;
      innerLost.push_back({px(at(-spec.searchHalf)), px(at(spec.searchHalf))});
    } else {
      if (st.status == "no_bead") ++noBead;
      else if (st.status == "incomplete_bead") ++incomplete;
      else if (st.status == "pose_fail") ++poseFail;
      else ++outside;
      missing.push_back({px(at(-spec.searchHalf)), px(at(spec.searchHalf))});
      if (std::isfinite(st.inner)) innerPts.push_back(px(at(st.inner)));
    }
  }

  const std::size_t count = list.points.size();
  Json rec;
  rec["count"] = count;
  rec["unit"] = metric.unit();
  rec["poseGiven"] = poseGiven;
  rec["poseOk"] = pose.ok;
  rec["counts"] = Json{{"ok", okCount}, {"noInner", noInner}, {"noBead", noBead},
                        {"incompleteBead", incomplete}, {"poseFail", poseFail}, {"outOfImage", outside}};
  rec["ids"] = list.ids;
  rec["point"] = std::move(point);
  rec["normal"] = std::move(normal);
  rec["status"] = std::move(status);
  rec["inner"] = std::move(inner);
  rec["near"] = std::move(nearE);
  rec["far"] = std::move(farE);
  rec["innerNear"] = std::move(innerNear);
  rec["innerCenter"] = std::move(innerCenter);
  rec["width"] = std::move(width);
  rec["innerNearPx"] = std::move(innerNearPx);
  rec["innerCenterPx"] = std::move(innerCenterPx);
  rec["widthPx"] = std::move(widthPx);
  rec["innerContrast"] = std::move(innerContrast);
  rec["beadContrast"] = std::move(beadContrast);
  rec["calib"] = metric.calib ? std_image::planeCalibJson(*metric.calib) : Json();
  Record out;
  out.type = kStationMeasureType;
  out.data = std::move(rec);
  outputs.set("measure", Data::record(std::move(out)));
  const std::string why = poseGiven && !pose.ok ? "定位失败" : "没有一站量成";
  outputs.set("coverage", measurement(count ? static_cast<double>(okCount) / count : kNaN, "", why));
  outputs.set("distanceMean", measurement(centerCount ? centerSum / centerCount : kNaN, metric.unit(), why));

  ov.segments("station", good);
  ov.segments("missing", missing);
  ov.segments("outlier", innerLost);
  ov.points("part", innerPts, "内边");
  ov.points("edge.left", nearPts);
  ov.points("edge.right", farPts);
  if (poseGiven && !pose.ok && !list.points.empty()) {
    ov.text("ng", "定位失败", px(list.points.front()));
  }
  outputs.set("overlay", ov.data());
  return Status::Ok();
}

std::string externalKey(const ParamView& params) {
  std::error_code ec;
  const auto file = params.path("stations");
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return {};
  const auto mtime = std::filesystem::last_write_time(file, ec);
  if (ec) return {};
  return std::to_string(size) + ":" + std::to_string(mtime.time_since_epoch().count());
}

std::vector<Issue> validate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const auto inner = params.vec2("innerSearch");
  const auto bead = params.vec2("beadSearch");
  const double half = params.number("searchHalf");
  if (inner[0] >= inner[1]) issues.push_back(Issue::error("bad_param", "innerSearch 的起点要小于终点", "innerSearch"));
  if (bead[0] >= bead[1]) issues.push_back(Issue::error("bad_param", "beadSearch 的起点要小于终点", "beadSearch"));
  const double reach = std::max({std::fabs(inner[0]), std::fabs(inner[1]), std::fabs(bead[0]), std::fabs(bead[1])});
  if (reach > half - 4.0) {
    issues.push_back(Issue::error("bad_param", "searchHalf 要比 innerSearch / beadSearch 的最远端再长 4 px", "searchHalf"));
  }
  return issues;
}

}  // namespace

void registerStationCalipers(Registry& r) {
  OperatorDesc op;
  op.id = "glue.station_calipers";
  op.version = "1.0.0";
  op.label = "逐点卡尺";
  op.category = "涂胶/飞拍";
  op.keywords = {"glue", "caliper", "station", "inner edge", "flyshot", "涂胶", "卡尺", "内边", "飞拍", "测量点"};
  op.doc =
      "固定相机飞拍：在宿主给的名义胶路测量点上逐点卡尺，量内边与胶条两条边（glue-plan K1 / K3）。"
      "站点在名义胶条中线上，法向从内边（开口一侧）指向翻边，卡尺上 t < 0 是内边一侧。\n"
      "内边：在 innerSearch 里按 innerPolarity 找 ±1…3 px 两侧均值差 ≥ contrastMin 的跳变，几条时按 innerSelect 取"
      "（nearest = 离胶最近，对应「内边取上沿」）。胶：内边之后、beadSearch 之内离站点最近的暗谷（窗口里别的暗东西不算），按本站半高定两条边。\n"
      "接了 pose（glue.locate）先把测量点从示教图映射到这一帧；pose.ok = false 时全部站 pose_fail。"
      "胶边落在窗口外时 incomplete_bead（无效量测，不能当断胶或合格）。"
      "接了 calib 时 内边→近边、内边→胶中线、胶宽 出 mm。判定不在这里：宿主按测量模式取值、跨帧合并断胶。";
  op.inputs = {
      Port{"image", "Image", "Image", "这一帧。", true},
      withContract(Port{"pose", "Record", "Pose", "可选：glue.locate 的位姿。", false},
                   {{"recordType", kPoseType}}),
      withContract(Port{"calib", "Record", "Calib", "可选：image.PlaneCalib。接了距离出 mm。", false},
                   {{"recordType", std_image::kPlaneCalibType}}),
  };
  op.outputs = {
      Port{"measure", "Record", "Measure",
           "glue.StationMeasure：逐站 point、normal、status（ok / no_inner / no_bead / incomplete_bead / pose_fail / out_of_image）、"
           "inner / near / far（沿法向的偏移，px）、innerNear / innerCenter / width（按 unit）及其 px 版本、对比度。",
           true},
      Port{"coverage", "Measurement", "Coverage", "量成的站占比。", true},
      Port{"distanceMean", "Measurement", "Distance Mean", "内边→胶中线的均值。", true},
      Port{"overlay", "Record", "Overlay", "lyflow.overlay2d：每站的卡尺、内边点、胶的两条边。", true},
  };
  Param stations;
  stations.name = "stations";
  stations.type = ParamType::Path;
  stations.label = "Stations";
  stations.doc = "测量点文件（JSON：points[[x,y]]、normals[[nx,ny]]、可选 ids[]；示教图坐标，px）。宿主每个拍照点写一份。";
  stations.def = Value::text("");
  stations.mode = "open";
  stations.filters = {FileFilter{"JSON", {"json"}}, FileFilter{"All Files", {"*"}}};
  Param half = floatParam("searchHalf", "Search Half", 48.0, "px", "卡尺沿法向 ±searchHalf，要比 innerSearch / beadSearch 的最远端再长 4 px。");
  half.min = 8.0;
  Param widthP = floatParam("caliperWidth", "Caliper Width", 5.0, "px", "沿切向平均的宽度（压噪声）。", true);
  widthP.min = 1.0;
  Param innerSearch = vec2Param("innerSearch", "Inner Search", -40, -3, "px", {"From", "To"},
                                "内边只在卡尺上 t ∈ [From, To] 里找（t < 0 是内边一侧）。");
  Param beadSearch = vec2Param("beadSearch", "Bead Search", -15, 40, "px", {"From", "To"},
                               "胶只在内边之后、t ≤ To 且 t ≥ From 的范围里找。");
  Param innerPolarity = enumParam("innerPolarity", "Inner Polarity", "dark_to_bright",
                                  "沿法向（开口 → 翻边）看内边：暗 → 亮（默认：开口暗）或 亮 → 暗。",
                                  {EnumOption{"dark_to_bright", "暗 → 亮", ""},
                                   EnumOption{"bright_to_dark", "亮 → 暗", ""}});
  Param innerSelect = enumParam("innerSelect", "Inner Select", "nearest",
                                "能看到几条内边（拔模斜度的上沿、下沿）时取哪条：离胶最近的，或最强的。",
                                {EnumOption{"nearest", "离胶最近", ""}, EnumOption{"strongest", "最强", ""}});
  Param beadPolarity = enumParam("beadPolarity", "Bead Polarity", "dark", "胶比翻边暗（默认）还是亮。",
                                 {EnumOption{"dark", "暗胶", ""}, EnumOption{"bright", "亮胶", ""}}, true);
  Param contrastMin = floatParam("contrastMin", "Contrast Min", 25.0, "", "内边跳变与胶的对比度至少这么大（灰度级）。", true);
  contrastMin.min = 1.0;
  Param widthMin = floatParam("widthMin", "Width Min", 3.0, "px", "比它窄的暗段不算胶。", true);
  widthMin.min = 0.0;
  op.params = {stations, half, widthP, innerSearch, beadSearch, innerPolarity, innerSelect, beadPolarity, contrastMin, widthMin};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.externalKey = &externalKey;
  op.validate = &validate;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::glue
