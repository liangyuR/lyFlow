// 积木算子（m8-plan L5–L11）：每个对应一个人会说出口的步骤，复杂度消化在数据类型与算子里。
//
// 这里一行算法都不写：每一步都是 Step 原样调一个细粒度算子的 compute（L6），
// 积木算子只做三件事 —— 从 ScanPair / RoiSet 里挑出那一步要的字段、按角色推出方向（L7）、
// 把结果重新装回 Bundle。参数与细粒度算子同名同义，声明也从那边拷（paramOf）。
#include <array>
#include <string>
#include <vector>

#include "gap_fine.h"

namespace lyflow::packs::gap {
namespace {

namespace fs = std::filesystem;

constexpr int kTemplateSlots = 4;

// ---------------------------------------------------------------- 小工具

Status badBundle(const char* port) {
  return Status::Error(Phase::Execute, "bad_input",
                       std::string("端口 ") + port + " 上的 Bundle 缺字段（执行器应当已经查过）", {},
                       port);
}

Param enumParam(const char* name, const char* label, const char* def, const char* doc,
                std::vector<EnumOption> options, bool advanced = false) {
  Param p;
  p.name = name;
  p.type = ParamType::Enum;
  p.label = label;
  p.doc = doc;
  p.def = Value::text(def);
  p.options = std::move(options);
  p.advanced = advanced;
  return p;
}

Param boolParam(const char* name, const char* label, bool def, const char* doc,
                bool advanced = false) {
  Param p;
  p.name = name;
  p.type = ParamType::Bool;
  p.label = label;
  p.doc = doc;
  p.def = Value::boolean(def);
  p.advanced = advanced;
  return p;
}

Param textParam(const char* name, const char* label, const std::string& def, const char* doc,
                bool advanced = false) {
  Param p;
  p.name = name;
  p.type = ParamType::String;
  p.label = label;
  p.doc = doc;
  p.def = Value::text(def);
  p.advanced = advanced;
  return p;
}

/// 模板坐标系里的一个角色框。slot 是它画在哪个模板槽的左右模板上（m8-plan L15 的 2D 拖框）。
Param roiParam(const std::string& name, const char* label, const char* doc, const char* group,
               bool advanced, int slot) {
  Param p;
  p.name = name;
  p.type = ParamType::Vec4f;
  p.label = label;
  p.doc = doc;
  p.def = Value::vec({0.0, 0.0, 0.0, 0.0});
  p.unit = "mm";
  p.group = group;
  p.advanced = advanced;
  p.componentLabels = {"X Min", "Y Min", "X Max", "Y Max"};
  p.semantic = "roi";
  const std::string prefix = "template" + std::to_string(slot);
  p.roiBackdrop.dirParam = "templateDir";
  p.roiBackdrop.fileParams = {prefix + "Left", prefix + "Right"};
  return p;
}

Param renamed(Param p, const char* name) {
  p.name = name;
  return p;
}

Param withDefault(Param p, Value def) {
  p.def = std::move(def);
  return p;
}

/// ROI 框（米）的 [x0, y0, x1, y1]，只用来做加载期的几何检查。
struct MmBox {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double cx() const { return 0.5 * (x0 + x1); }
};

MmBox mmBoxOf(const ParamView& params, const std::string& name) {
  const auto v = params.vec4(name);
  return MmBox{v[0], v[1], v[2], v[3]};
}

// ================================================================ gap.read_scan

Status readScan(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                ExecContext& ctx) {
  Data primary;
  Data secondary;
  // L18：两个输入接上了（或被宿主注入了）就直接用它们，不读目录 —— source 只决定
  // 「没给输入时去哪读」。只给一个在加载期就被 validate 拦下了。
  if (inputs.has("primary") && inputs.has("secondary")) {
    primary = inputs.get("primary");
    secondary = inputs.get("secondary");
  } else if (params.choice("source") == "inputs") {
    return Status::Error(Phase::Execute, "missing_input",
                         "source=inputs，但 primary / secondary 既没有接线也没有被宿主注入", {},
                         "primary");
  } else {
    // 1280 个槽原样留着（NaN 槽不剔）：模型定位靠槽号与标签对齐，模板定位的裁剪与
    // 整体框的中位数本来就跳过非有限点，所以两条路都吃这一份。
    Step load("gap.load_profile_pair", &fine::loadPair);
    load.copyAll(params).set("dropNonFinite", Value::boolean(false));
    if (Status s = load.run(ctx, "读 PCD"); !s.ok) return s;
    primary = load.out("primary");
    secondary = load.out("secondary");
  }

  Data frames[2];
  Data finite[2];
  const Data* raw[2] = {&primary, &secondary};
  for (int i = 0; i < 2; ++i) {
    Step frame("gap.to_measurement_frame", &fine::toMeasurementFrame);
    frame.in("cloud", *raw[i]);
    if (Status s = frame.run(ctx, "换轴", i == 0 ? "primary" : "secondary"); !s.ok) return s;
    frames[i] = frame.out("cloud");
    Step drop("gap.drop_non_finite", &fine::dropNonFinite);
    drop.in("cloud", frames[i]);
    if (Status s = drop.run(ctx, "剔 NaN"); !s.ok) return s;
    finite[i] = drop.out("cloud");
  }

  // 合并：secondary 在前、primary 在后，与细粒度图的 util.merge 同一个接法。
  Step merge("util.merge");
  merge.in("a", finite[1]).in("b", finite[0]);
  if (Status s = merge.run(ctx, "合并"); !s.ok) return s;
  Data merged = merge.out("cloud");
  if (params.flag("removeOutliers")) {
    Step filter("filter.radius_outlier");
    filter.in("cloud", merged)
        .set("radius", Value::number(params.number("outlierRadiusMm") / 1000.0))
        .set("minNeighbors", Value::integer(params.integer("outlierNeighbors")));
    if (Status s = filter.run(ctx, "半径离群"); !s.ok) return s;
    merged = filter.out("cloud");
  }
  outputs.set("scan", scanPairOf(frames[0], frames[1], merged));
  return Status::Ok();
}

std::string readScanKey(const ParamView& params) {
  if (params.choice("source") == "inputs") return {};
  return fine::profilePairKey(params);
}

/// L18 之后只剩一条：两个输入要么都给、要么都不给。宿主注入也算「给了」（执行器把注入的
/// 输入端口算进 connected）；lyflow validate 看不见注入，所以 source=inputs 而两个都没接
/// 在这里不报，留到执行期报 missing_input。
std::vector<Issue> validateReadScan(const ParamView&, const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  const bool p = connected.count("primary") != 0;
  const bool s = connected.count("secondary") != 0;
  if (p != s) {
    issues.push_back(Issue::error("missing_input",
                                  std::string("primary 与 secondary 要么都接（或都注入），要么都不接；"
                                              "现在只有 ") +
                                      (p ? "primary" : "secondary"),
                                  "source", p ? "secondary" : "primary"));
  }
  return issues;
}

// ============================================================ gap.locate_template

struct Slot {
  int index = 0;  // 1..4
  std::string prefix;
  bool enabled = false;
  bool override_ = false;
};

std::vector<Slot> slotsOf(const ParamView& params) {
  std::vector<Slot> slots;
  for (int k = 1; k <= kTemplateSlots; ++k) {
    Slot s;
    s.index = k;
    s.prefix = "template" + std::to_string(k);
    s.enabled = params.flag(s.prefix + "Enabled");
    s.override_ = params.flag(s.prefix + "Override");
    if (s.enabled) slots.push_back(s);
  }
  return slots;
}

/// 槽里四个角色框的参数名（覆盖了就是槽自己的，否则是基础的那四个）。
struct SlotRois {
  std::string datum, target, seamLeft, seamRight;
};

SlotRois roisOf(const Slot& s) {
  if (!s.override_) return {"datumRoi", "targetRoi", "seamLeftRoi", "seamRightRoi"};
  return {s.prefix + "DatumRoi", s.prefix + "TargetRoi", s.prefix + "SeamLeftRoi",
          s.prefix + "SeamRightRoi"};
}

Status locateTemplate(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext& ctx) {
  ScanView scan;
  if (!readScanPair(inputs.get("scan"), &scan)) return badBundle("scan");

  Step overall("gap.overall_roi", &fine::overallRoi);
  overall.in("primary", *scan.primary)
      .in("secondary", *scan.secondary)
      .set("roi", params.raw().at("overallRoi"))
      .set("mode", params.raw().at("overallMode"))
      .set("usingCamera", params.raw().at("overallCamera"));
  if (Status s = overall.run(ctx, "整体框", "scan"); !s.ok) return s;
  const Data box = overall.out("box");

  Data cropped[3];
  const Data* clouds[3] = {scan.primary, scan.secondary, scan.merged};
  static const char* kCropLabels[3] = {"裁 primary", "裁 secondary", "裁合并云"};
  for (int i = 0; i < 3; ++i) {
    Step crop("filter.crop_box2d");
    crop.in("cloud", *clouds[i]).in("box", box).set("bounds", Value::text("open"));
    if (Status s = crop.run(ctx, kCropLabels[i], "scan"); !s.ok) return s;
    cropped[i] = crop.out("cloud");
  }

  // 模板槽：逐个配准，再挑一个（与 gap.select_alignment 同一套规则）。
  std::vector<Data> alignments;
  const std::vector<Slot> slots = slotsOf(params);
  int order = 0;
  for (const Slot& slot : slots) {
    const std::string id = params.text(slot.prefix + "Id");
    Step tpl("gap.load_template", &fine::loadTemplate);
    tpl.set("dir", params.raw().at("templateDir"))
        .set("left", params.raw().at(slot.prefix + "Left"))
        .set("right", params.raw().at(slot.prefix + "Right"));
    if (Status s = tpl.run(ctx, "模板 " + id); !s.ok) return s;

    const SlotRois rois = roisOf(slot);
    Step align("gap.align_template", &fine::alignTemplate);
    align.in("cloud", cropped[2])
        .in("tplLeft", tpl.out("left"))
        .in("tplRight", tpl.out("right"))
        .copyAll(params)
        .set("templateId", Value::text(id))
        .set("order", Value::integer(order++))
        .set("roiFlushBase", params.raw().at(rois.datum))
        .set("roiFlushRef", params.raw().at(rois.target))
        .set("roiGapLeft", params.raw().at(rois.seamLeft))
        .set("roiGapRight", params.raw().at(rois.seamRight));
    if (Status s = align.run(ctx, "ICP " + id, "scan"); !s.ok) return s;
    alignments.push_back(align.out("alignment"));
  }

  Step select("gap.select_alignment", &fine::selectAlignment);
  static const char* kPorts[kTemplateSlots] = {"a", "b", "c", "d"};
  for (std::size_t i = 0; i < alignments.size(); ++i) select.in(kPorts[i], alignments[i]);
  select.copy(params, {"minScore"});
  if (Status s = select.run(ctx, "选模板"); !s.ok) return s;
  const Data alignment = select.out("alignment");

  // 基准件在哪一侧由选中模板坐标系里的框推出（L7），business_rois 与 info 用同一个判据。
  Step business("gap.business_rois", &fine::businessRois);
  business.in("alignment", alignment).set("datumSide", Value::text("auto"));
  if (Status s = business.run(ctx, "业务框"); !s.ok) return s;

  const lyflow::Record& rec = *alignment.asRecord();
  bool datumRight = false;
  datumOnRightInRecord(rec.data, &datumRight);
  nlohmann::json info =
      roiInfo(datumRight, "template", rec.data, box.asBox2D(), nlohmann::json());
  outputs.set("rois", roiSetOf(business.out("flushBase"), business.out("flushRef"),
                               business.out("gapLeft"), business.out("gapRight"), std::move(info)));
  outputs.set("alignment", alignment);
  outputs.set("scan", scanPairOf(cropped[0], cropped[1], cropped[2]));
  return Status::Ok();
}

std::string locateTemplateKey(const ParamView& params) {
  const fs::path dir = params.path("templateDir");
  if (dir.empty()) return {};
  std::string key;
  for (const Slot& s : slotsOf(params)) {
    key += fileStamp(dir / params.text(s.prefix + "Left")) + "|" +
           fileStamp(dir / params.text(s.prefix + "Right")) + ";";
  }
  return key;
}

/// L11：ROI 在模板坐标系里是常数，「方向填反」在加载期就拦得住。
std::vector<Issue> validateLocateTemplate(const ParamView& params, const std::set<std::string>&) {
  std::vector<Issue> issues;
  const std::vector<Slot> slots = slotsOf(params);
  if (slots.empty()) {
    issues.push_back(Issue::error("bad_param", "四个模板槽一个都没启用", "template1Enabled"));
    return issues;
  }
  int firstSide = -1;
  std::string firstSlot;
  for (const Slot& slot : slots) {
    const SlotRois names = roisOf(slot);
    const std::string where = "模板槽 " + std::to_string(slot.index) + "：";
    const std::pair<const std::string*, const char*> all[4] = {
        {&names.datum, "datum"}, {&names.target, "target"},
        {&names.seamLeft, "seamLeft"}, {&names.seamRight, "seamRight"}};
    bool degenerate = false;
    for (const auto& [name, role] : all) {
      const MmBox b = mmBoxOf(params, *name);
      if (!(b.x1 > b.x0) || !(b.y1 > b.y0)) {
        issues.push_back(Issue::error("bad_param",
                                      where + role + " 框退化（max 必须大于 min）", *name));
        degenerate = true;
      }
    }
    if (degenerate) continue;
    const MmBox datum = mmBoxOf(params, names.datum);
    const MmBox target = mmBoxOf(params, names.target);
    const MmBox left = mmBoxOf(params, names.seamLeft);
    const MmBox right = mmBoxOf(params, names.seamRight);
    if (!(left.x1 <= right.x0)) {
      issues.push_back(Issue::error(
          "bad_param", where + "缝框左右颠倒或重叠：seamLeft 的右边要不超过 seamRight 的左边",
          names.seamLeft));
      continue;
    }
    const double seamX = 0.5 * (left.cx() + right.cx());
    const bool datumRight = datum.cx() > seamX;
    const bool targetRight = target.cx() > seamX;
    if (datumRight == targetRight) {
      issues.push_back(Issue::error(
          "bad_param",
          where + "datum 与 target 落在缝的同一侧（都在" + (datumRight ? "右" : "左") +
              "边）—— 段差要两侧各取一个面",
          names.datum));
      continue;
    }
    if (firstSide < 0) {
      firstSide = datumRight ? 1 : 0;
      firstSlot = std::to_string(slot.index);
    } else if (firstSide != (datumRight ? 1 : 0)) {
      issues.push_back(Issue::error(
          "bad_param", where + "datum 在缝的另一侧，与模板槽 " + firstSlot + " 不一致",
          names.datum));
    }
  }
  return issues;
}

// =============================================================== gap.locate_model

Status locateModel(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                   ExecContext& ctx) {
  ScanView scan;
  if (!readScanPair(inputs.get("scan"), &scan)) return badBundle("scan");

  // 模型要原始 1280 槽的传感器帧：ScanPair 的 primary/secondary 是换过轴的同一批槽，
  // 再换一次就回到传感器帧（换轴是交换 y、z，对有限点是精确的，NaN 槽原样留着）。
  Data sensor[2];
  const Data* measured[2] = {scan.primary, scan.secondary};
  for (int i = 0; i < 2; ++i) {
    Step frame("gap.to_measurement_frame", &fine::toMeasurementFrame);
    frame.in("cloud", *measured[i]);
    if (Status s = frame.run(ctx, "换回传感器帧", "scan"); !s.ok) return s;
    sensor[i] = frame.out("cloud");
  }

  Step tensor("gap.profile_tensor", &fine::profileTensor);
  tensor.in("primary", sensor[0]).in("secondary", sensor[1]);
  if (Status s = tensor.run(ctx, "剖面张量", "scan"); !s.ok) return s;

  Step infer("ml.onnx_run");
  infer.in("input", tensor.out("tensor"))
      .set("modelPath", Value::text(params.path("modelPath").u8string()));
  if (Status s = infer.run(ctx, "ONNX 推理"); !s.ok) {
    if (s.paramPath == "modelPath" || s.paramPath.empty()) s.paramPath = "modelPath";
    return s;
  }

  Step labels("gap.labels_from_logits", &fine::labelsFromLogits);
  labels.in("tensor", infer.out("output"));
  if (Status s = labels.run(ctx, "逐槽 argmax"); !s.ok) return s;

  Step boxes("gap.roi_from_labels", &fine::roiFromLabels);
  boxes.in("primary", sensor[0])
      .in("secondary", sensor[1])
      .in("labels", labels.out("labels"))
      .copyAll(params);
  if (Status s = boxes.run(ctx, "模型四框"); !s.ok) return s;

  Data finite[2];
  for (int i = 0; i < 2; ++i) {
    Step drop("gap.drop_non_finite", &fine::dropNonFinite);
    drop.in("cloud", *measured[i]);
    if (Status s = drop.run(ctx, "剔 NaN"); !s.ok) return s;
    finite[i] = drop.out("cloud");
  }

  Step roll("gap.roll_anchored_crop", &fine::rollAnchoredCrop);
  roll.in("primary", finite[0])
      .in("secondary", finite[1])
      .in("gapLeft", boxes.out("gapLeft"))
      .in("gapRight", boxes.out("gapRight"))
      .in("merged", *scan.merged)
      .set("enabled", params.raw().at("cropEnabled"))
      .set("halfWidth", params.raw().at("cropHalfWidth"))
      .set("halfHeight", params.raw().at("cropHalfHeight"))
      .set("maxRollBoxHeight", params.raw().at("cropMaxRollBoxHeight"))
      .set("minPointsKept", params.raw().at("cropMinPointsKept"))
      .set("usingCamera", params.raw().at("cropUsingCamera"));
  if (Status s = roll.run(ctx, "跟随零件的裁剪窗"); !s.ok) return s;

  const lyflow::Box2D& flushBase = *boxes.out("flushBase").asBox2D();
  const lyflow::Box2D& gapLeft = *boxes.out("gapLeft").asBox2D();
  const lyflow::Box2D& gapRight = *boxes.out("gapRight").asBox2D();
  nlohmann::json info = roiInfo(datumOnRight(flushBase, gapLeft, gapRight), "model",
                                nlohmann::json(), roll.out("window").asBox2D(),
                                roll.out("status").asRecord()->data);
  outputs.set("rois", roiSetOf(boxes.out("flushBase"), boxes.out("flushRef"),
                               boxes.out("gapLeft"), boxes.out("gapRight"), std::move(info)));
  outputs.set("scan", scanPairOf(roll.out("primary"), roll.out("secondary"), roll.out("merged")));
  return Status::Ok();
}

std::string locateModelKey(const ParamView& params) {
  return fileStamp(params.path("modelPath"));
}

// ================================================================= gap.role_line

/// 按角色裁一片云再拟直线。「靠缝那一端」取两个缝框中心的中点（L7），没有任何 toward 参数。
Status fitRoleLine(const ScanView& scan, const RoiView& rois, const std::string& role,
                   const Data& refLine, const ParamView& params, ExecContext& ctx, Step* fit) {
  const Data& box = rois.role(role);
  Step crop("filter.crop_box2d");
  crop.in("cloud", scan.pick(params.choice("cloud"))).in("box", box).set("bounds",
                                                                          Value::text("open"));
  if (Status s = crop.run(ctx, "裁 " + role, "rois"); !s.ok) return s;
  const lyflow::Box2D toward = seamTowardBox(*rois.seamLeft->asBox2D(), *rois.seamRight->asBox2D());
  fit->in("cloud", crop.out("cloud"))
      .in("box", box)
      .in("toward", Data::box2d(toward))
      .in("refLine", refLine)
      .copyAll(params);
  return fit->run(ctx, "拟合 " + role, "scan");
}

Status roleLine(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  ScanView scan;
  RoiView rois;
  if (!readScanPair(inputs.get("scan"), &scan)) return badBundle("scan");
  if (!readRoiSet(inputs.get("rois"), &rois)) return badBundle("rois");
  Step fit("gap.fit_line", &fine::fitLine);
  if (Status s = fitRoleLine(scan, rois, params.choice("role"), inputs.get("refLine"), params, ctx,
                             &fit);
      !s.ok) {
    return s;
  }
  outputs.set("line", fit.out("line"));
  outputs.set("innerEnd", fit.out("innerEnd"));
  outputs.set("quality", fit.out("quality"));
  return Status::Ok();
}

// ================================================================= gap.ref_point

Data placeholderLine(const Data& point) {
  lyflow::Line2D line;
  if (const lyflow::Point2D* p = point.asPoint2D()) {
    line.point[0] = p->p[0];
    line.point[1] = p->p[1];
  }
  return Data::line2d(line);
}

Data pointQuality(const std::string& method, std::size_t pointCount) {
  lyflow::Record q;
  q.type = "GapFitQuality";
  q.data = {{"model", method}, {"pointCount", pointCount}, {"lineFitted", false}};
  return Data::record(std::move(q));
}

Status refPoint(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  ScanView scan;
  RoiView rois;
  if (!readScanPair(inputs.get("scan"), &scan)) return badBundle("scan");
  if (!readRoiSet(inputs.get("rois"), &rois)) return badBundle("rois");
  const std::string method = params.choice("method");
  const std::string role = params.choice("role");

  if (method == "line_end") {
    Step fit("gap.fit_line", &fine::fitLine);
    if (Status s = fitRoleLine(scan, rois, role, Data(), params, ctx, &fit); !s.ok) return s;
    outputs.set("point", fit.out("innerEnd"));
    outputs.set("line", fit.out("line"));
    outputs.set("quality", fit.out("quality"));
    return Status::Ok();
  }

  const Data& box = rois.role(role);
  if (method == "selected_point") {
    // 取自整片云、不裁框（gap.selected_point 的语义）
    const Data& cloud = scan.pick(params.choice("cloud"));
    Step pick("gap.selected_point", &fine::selectedPoint);
    pick.in("cloud", cloud).in("box", box);
    if (Status s = pick.run(ctx, "选点", "scan"); !s.ok) return s;
    outputs.set("point", pick.out("point"));
    outputs.set("line", placeholderLine(pick.out("point")));
    outputs.set("quality", pointQuality(method, cloud.asCloud()->pointCount()));
    return Status::Ok();
  }

  // nearest_point：框里离基准线垂距最小的那个点
  Step crop("filter.crop_box2d");
  crop.in("cloud", scan.pick(params.choice("cloud"))).in("box", box).set("bounds",
                                                                          Value::text("open"));
  if (Status s = crop.run(ctx, "裁 " + role, "rois"); !s.ok) return s;
  Step nearest("gap.nearest_to_line", &fine::nearestToLine);
  nearest.in("cloud", crop.out("cloud")).in("line", inputs.get("baseLine"));
  if (Status s = nearest.run(ctx, "最近点", "baseLine"); !s.ok) return s;
  outputs.set("point", nearest.out("point"));
  outputs.set("line", placeholderLine(nearest.out("point")));
  outputs.set("quality", pointQuality(method, crop.out("cloud").asCloud()->pointCount()));
  return Status::Ok();
}

std::vector<Issue> validateRefPoint(const ParamView& params, const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  if (params.choice("method") == "nearest_point" && !connected.count("baseLine")) {
    issues.push_back(Issue::error("missing_input", "method=nearest_point 要接 baseLine（基准线）",
                                  "method", "baseLine"));
  }
  return issues;
}

// ============================================================== gap.seam_circles

Status seamCircles(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                   ExecContext& ctx) {
  ScanView scan;
  RoiView rois;
  if (!readScanPair(inputs.get("scan"), &scan)) return badBundle("scan");
  if (!readRoiSet(inputs.get("rois"), &rois)) return badBundle("rois");
  Step fit("gap.fit_gap_circles", &fine::fitGapCircles);
  fit.in("merged", *scan.merged)
      .in("primary", *scan.primary)
      .in("secondary", *scan.secondary)
      .in("boxLeft", *rois.seamLeft)
      .in("boxRight", *rois.seamRight)
      .in("refLine", inputs.get("refLine"))
      .in("refLineRight", inputs.get("refLineRight"))
      .copyAll(params);
  if (Status s = fit.run(ctx, "拟合缝两侧圆", "rois"); !s.ok) return s;
  outputs.set("left", fit.out("left"));
  outputs.set("right", fit.out("right"));
  outputs.set("quality", fit.out("quality"));
  return Status::Ok();
}

// =========================================================== gap.datum_direction

Status datumDirection(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                      ExecContext& ctx) {
  ScanView scan;
  RoiView rois;
  if (!readScanPair(inputs.get("scan"), &scan)) return badBundle("scan");
  if (!readRoiSet(inputs.get("rois"), &rois)) return badBundle("rois");
  // 长面在角色框背离缝的那一侧：datum 取 info.datumSide，target 取另一侧。x 锚在同侧的缝框上
  // （模型对缝的定位最稳），y 锚在角色框上（长面就在它上下几毫米内）。
  const std::string role = params.choice("role");
  const bool right = role == "target" ? !rois.datumRight() : rois.datumRight();
  const Data& anchor = right ? *rois.seamRight : *rois.seamLeft;

  Step window("gap.datum_window", &fine::datumWindow);
  window.in("anchor", anchor)
      .in("heightAnchor", rois.role(role))
      .set("side", Value::text(right ? "right" : "left"))
      .copy(params, {"startMm", "lengthMm", "heightMm"});
  if (Status s = window.run(ctx, "方向基准窗"); !s.ok) return s;

  Step crop("filter.crop_box2d");
  crop.in("cloud", *scan.merged).in("box", window.out("box")).set("bounds", Value::text("open"));
  if (Status s = crop.run(ctx, "裁方向基准窗", "scan"); !s.ok) return s;

  Step fit("gap.fit_line", &fine::fitLine);
  fit.in("cloud", crop.out("cloud"))
      .in("box", window.out("box"))
      .in("toward", anchor)
      .set("segmentPoints", Value::integer(0))
      .set("endpoints", Value::text("inlier_ends"))
      .copy(params, {"distThresh", "minInliers"});
  if (Status s = fit.run(ctx, "拟合方向基准线", "scan"); !s.ok) return s;
  outputs.set("line", fit.out("line"));
  outputs.set("quality", fit.out("quality"));
  return Status::Ok();
}

// ------------------------------------------------------------------ 端口

Port scanIn(const char* doc) { return Port{"scan", "Bundle<gap.ScanPair>", "Scan", doc, true}; }
Port roisIn(const char* doc) { return Port{"rois", "Bundle<gap.RoiSet>", "ROIs", doc, true}; }

const std::vector<EnumOption>& roleOptions() {
  static const std::vector<EnumOption> kOptions = {
      EnumOption{"datum", "Datum（基准面）", "RoiSet 的 datum 框。"},
      EnumOption{"target", "Target（参考面）", "RoiSet 的 target 框。"}};
  return kOptions;
}

Param cloudChoice() {
  return enumParam("cloud", "Cloud", "merged", "在 ScanPair 的哪一片云上裁框、拟合。",
                   {EnumOption{"merged", "Merged（合并云）", ""},
                    EnumOption{"primary", "Primary（Master）", ""},
                    EnumOption{"secondary", "Secondary（Slave）", ""}},
                   /*advanced=*/true);
}

}  // namespace

void registerBlockOps(Registry& r) {
  // ------------------------------------------------------------ gap.read_scan
  {
    OperatorDesc op;
    op.id = "gap.read_scan";
    // 1.1.0：接上 / 注入的两个输入压过 source（L18），validate 只查「成对给」
    op.version = "1.1.0";
    op.label = "读剖面";
    op.category = "间隙/积木";
    op.keywords = {"read", "scan", "pcd", "剖面", "测点", "积木"};
    op.doc =
        "读一个测点的双头线扫剖面，换到测量帧，出一个 ScanPair。primary / secondary 保留原始"
        "1280 个槽（NaN 槽不剔，模型定位要靠槽号）；merged 是两片有限点合并（secondary 在前）"
        "之后、按需做过半径离群剔除的云。\n"
        "primary / secondary 两个输入接上了（或被宿主注入了）就直接用它们、不读目录；"
        "宿主注入内存里的两片云就注入这个节点的这两个输入端口（C ABI 的 run inputs、"
        "CLI 的 --input n_scan.primary=<pcd>）。不想配目录时把 source 设成 inputs。";
    op.inputs = {
        Port{"primary", "PointCloud", "Primary",
             "传感器帧的 Master 云（原始槽，NaN 可留着）。接上或被注入时压过 source。", false},
        Port{"secondary", "PointCloud", "Secondary", "传感器帧的 Slave 云。与 primary 成对给。",
             false},
    };
    op.outputs = {Port{"scan", "Bundle<gap.ScanPair>", "Scan", "测量帧的剖面对。", true}};

    Param source = paramOf(r, "gap.load_profile_pair", "source");
    source.doc =
        "没给 primary / secondary 时去哪读：目录里按前缀配对，或直接指两个文件。"
        "inputs = 不读盘，两片云只从输入来（接线或宿主注入）。";
    source.options.push_back(EnumOption{"inputs", "只用两个输入", "宿主注入或上游已经读好的两片云。"});
    Param removeOutliers =
        boolParam("removeOutliers", "Remove Outliers", false,
                  "在合并云上做一次半径离群剔除（common_settings.filter.using_removal）。");
    auto outlierOnly = [](Param p) {
      p.visibleWhen.param = "removeOutliers";
      p.visibleWhen.eq = Value::boolean(true);
      return p;
    };
    Param radius;
    radius.name = "outlierRadiusMm";
    radius.type = ParamType::Float;
    radius.label = "Outlier Radius";
    radius.doc = "半径离群的邻域半径（filter_radius）。";
    radius.def = Value::number(0.4);
    radius.unit = "mm";
    radius.min = 1e-3;
    Param neighbors;
    neighbors.name = "outlierNeighbors";
    neighbors.type = ParamType::Int;
    neighbors.label = "Outlier Neighbors";
    neighbors.doc = "半径内至少要有几个邻居（filter_neighbors）。";
    neighbors.def = Value::integer(5);
    neighbors.min = 1.0;
    op.params = {
        source,
        paramOf(r, "gap.load_profile_pair", "dir"),
        paramOf(r, "gap.load_profile_pair", "primaryPrefix"),
        paramOf(r, "gap.load_profile_pair", "secondaryPrefix"),
        paramOf(r, "gap.load_profile_pair", "primaryFile"),
        paramOf(r, "gap.load_profile_pair", "secondaryFile"),
        paramOf(r, "gap.load_profile_pair", "layout", /*advanced=*/1),
        removeOutliers,
        outlierOnly(radius),
        outlierOnly(neighbors),
    };
    op.capabilities = {false, false, true};
    op.compute = &readScan;
    op.externalKey = &readScanKey;
    op.validate = &validateReadScan;
    r.addOperator(std::move(op));
  }

  // ------------------------------------------------------- gap.locate_template
  {
    OperatorDesc op;
    op.id = "gap.locate_template";
    op.version = "1.0.0";
    op.label = "模板定位";
    op.category = "间隙/积木";
    op.keywords = {"template", "icp", "locate", "模板", "定位", "积木"};
    op.doc =
        "整体框 → 裁剪 → 加载模板 → ICP 对齐 → 选模板 → 业务框，一步给出 RoiSet。"
        "四个角色框写在模板坐标系里（毫米）：datum 基准面、target 参考面、seamLeft / seamRight "
        "缝的两侧；基准件在哪一侧由它们推出，不另填。多模板用四个固定槽位，每槽可以覆盖四个框。\n"
        "scan 输出是整体框裁过的那一对云（merged 是去噪之后再裁），下游量测接它。"
        "加载期会查：框不退化、缝框左右有序不重叠、datum 与 target 落在缝的两侧、各槽的基准件同侧。";
    op.inputs = {scanIn("gap.read_scan 的剖面对。")};
    op.outputs = {
        Port{"rois", "Bundle<gap.RoiSet>", "ROIs", "搬到当前样本上的四个角色框。", true},
        withExample(Port{"alignment", "Record", "Alignment", "选中的 GapAlignment。", true},
                    examples::alignment()),
        Port{"scan", "Bundle<gap.ScanPair>", "Scan", "整体框裁过的剖面对，下游量测用它。", true},
    };

    Param dir;
    dir.name = "templateDir";
    dir.type = ParamType::Path;
    dir.label = "Template Dir";
    dir.doc = "模板目录，通常是 <配置名>/ 下的 StandardGap。四个槽的模板文件都在这里面。";
    dir.def = Value::text("");
    dir.mode = "dir";

    std::vector<Param> params = {
        dir,
        roiParam("datumRoi", "Datum ROI", "段差基准面的框（模板坐标系，毫米）。", "ROI", false, 1),
        roiParam("targetRoi", "Target ROI", "段差参考面的框。", "ROI", false, 1),
        roiParam("seamLeftRoi", "Seam Left ROI", "缝左侧的框（左圆）。", "ROI", false, 1),
        roiParam("seamRightRoi", "Seam Right ROI", "缝右侧的框（右圆）。", "ROI", false, 1),
        paramOf(r, "gap.align_template", "minScore", 0),
    };
    // 默认值是一个大到不裁的框：空白画布上拼出来的图不填高级参数也得能跑（细粒度算子的默认
    // [-1, -1, 1, 1] 会把整片剖面裁没）。导入器总是显式写这一项，导入的图不受影响。
    Param overallRoi = withDefault(
        renamed(paramOf(r, "gap.overall_roi", "roi", 1, "整体框"), "overallRoi"),
        Value::vec({-1000.0, -1000.0, 1000.0, 1000.0}));
    overallRoi.doc = "整体 ROI（测量帧，毫米）。默认大到不裁；配置里的 overall_roi 由导入器写进来。";
    Param overallMode = renamed(paramOf(r, "gap.overall_roi", "mode", 1, "整体框"), "overallMode");
    Param overallCamera =
        renamed(paramOf(r, "gap.overall_roi", "usingCamera", 1, "整体框"), "overallCamera");
    overallCamera.visibleWhen.param = "overallMode";
    params.push_back(overallRoi);
    params.push_back(overallMode);
    params.push_back(overallCamera);
    for (int k = 1; k <= kTemplateSlots; ++k) {
      const std::string prefix = "template" + std::to_string(k);
      const std::string group = "模板槽 " + std::to_string(k);
      auto slotted = [&](Param p) {
        p.group = group;
        p.advanced = true;
        if (p.name != prefix + "Enabled") {
          p.visibleWhen.param = prefix + "Enabled";
          p.visibleWhen.eq = Value::boolean(true);
        }
        return p;
      };
      const std::string defLeft = k == 1 ? "left_template.pcd" : "f" + std::to_string(k) + "_left.pcd";
      const std::string defRight =
          k == 1 ? "right_template.pcd" : "f" + std::to_string(k) + "_right.pcd";
      Param enabled = boolParam("", "Enabled", k == 1, "启用这个模板槽。");
      enabled.name = prefix + "Enabled";
      params.push_back(slotted(enabled));
      Param id = textParam("", "Template Id", "f" + std::to_string(k),
                           "模板的名字，进 GapAlignment.templateId 与结果汇总；打平时按它排。");
      id.name = prefix + "Id";
      params.push_back(slotted(id));
      Param left = textParam("", "Left File", defLeft, "左模板文件名（在模板目录里）。");
      left.name = prefix + "Left";
      params.push_back(slotted(left));
      Param right = textParam("", "Right File", defRight, "右模板文件名。");
      right.name = prefix + "Right";
      params.push_back(slotted(right));
      Param over = boolParam("", "Override ROIs", false,
                             "这个模板自带一套四框（模板坐标系），不用上面基础的那四个。");
      over.name = prefix + "Override";
      params.push_back(slotted(over));
      static const char* kRoles[4] = {"Datum", "Target", "SeamLeft", "SeamRight"};
      for (const char* role : kRoles) {
        Param roi = roiParam(prefix + role + "Roi", role, "覆盖的框（模板坐标系，毫米）。",
                             group.c_str(), true, k);
        roi.visibleWhen.param = prefix + "Override";
        roi.visibleWhen.eq = Value::boolean(true);
        params.push_back(roi);
      }
    }
    for (const char* name :
         {"initialPoseMode", "maxMatchingDist", "maxFitnessDist", "maxIterations", "normalKnn",
          "bidirection", "globalCoarse", "successGuide", "segRoi", "trustTranslation",
          "trustRotation", "degenerateRatio"}) {
      params.push_back(paramOf(r, "gap.align_template", name, 1, "ICP"));
    }
    op.params = std::move(params);
    op.capabilities = {false, false, true};
    op.compute = &locateTemplate;
    op.externalKey = &locateTemplateKey;
    op.validate = &validateLocateTemplate;
    r.addOperator(std::move(op));
  }

  // ---------------------------------------------------------- gap.locate_model
  {
    OperatorDesc op;
    op.id = "gap.locate_model";
    op.version = "1.0.0";
    op.label = "模型定位";
    op.category = "间隙/积木";
    op.keywords = {"model", "onnx", "locate", "模型", "定位", "积木"};
    op.doc =
        "分割模型给出四个角色框：剖面张量 → ONNX 推理 → 逐槽 argmax → 由标签推框（可选精修），"
        "再剔 NaN、按两个缝框做跟随零件的裁剪窗。scan 输出是裁过的那一对云（合并云也跟着裁），"
        "下游量测接它而不是 gap.read_scan 的原始云。\n"
        "模型失败时退回模板定位不做成参数：在图上用 flow.fallback 把本节点与 gap.locate_template "
        "并联（rois、scan 各一个 fallback）。";
    op.inputs = {scanIn("gap.read_scan 的剖面对（原始 1280 槽）。")};
    op.outputs = {
        Port{"rois", "Bundle<gap.RoiSet>", "ROIs", "模型给出的四个角色框。", true},
        Port{"scan", "Bundle<gap.ScanPair>", "Scan", "剔过 NaN、按跟随裁剪窗裁过的剖面对。", true},
    };
    // ml.onnx_run 在 std-ml 包里，注册期不去它那儿拷声明（只装了 gap 的注册表里没有它）。
    Param model;
    model.name = "modelPath";
    model.type = ParamType::Path;
    model.label = "Model";
    model.doc = "ONNX 模型文件（setting.yml 的 model_roi.model_path），通常绑到顶层参数 modelPath。";
    model.def = Value::text("");
    model.mode = "open";
    model.filters = {FileFilter{"ONNX", {"onnx"}}};
    std::vector<Param> params = {model};
    for (const char* name :
         {"refine", "splitStepMm", "splitSlotGap", "linkMm", "minComponentSlots", "anchorGapMm"}) {
      params.push_back(paramOf(r, "gap.roi_from_labels", name, 1, "细化"));
    }
    const std::pair<const char*, const char*> crop[6] = {
        {"enabled", "cropEnabled"},       {"halfWidth", "cropHalfWidth"},
        {"halfHeight", "cropHalfHeight"}, {"maxRollBoxHeight", "cropMaxRollBoxHeight"},
        {"minPointsKept", "cropMinPointsKept"}, {"usingCamera", "cropUsingCamera"}};
    for (const auto& [from, to] : crop) {
      params.push_back(renamed(paramOf(r, "gap.roll_anchored_crop", from, 1, "跟随裁剪"), to));
    }
    op.params = std::move(params);
    op.capabilities = {false, false, true};
    op.compute = &locateModel;
    op.externalKey = &locateModelKey;
    r.addOperator(std::move(op));
  }

  // ------------------------------------------------------------- gap.role_line
  {
    OperatorDesc op;
    op.id = "gap.role_line";
    op.version = "1.0.0";
    op.label = "按角色拟合直线";
    op.category = "间隙/积木";
    op.keywords = {"line", "fit", "datum", "直线", "拟合", "基准线", "积木"};
    op.doc =
        "按角色（datum 基准面 / target 参考面）从 RoiSet 取框，在 ScanPair 的合并云上裁出来拟一条"
        "直线（gap.fit_line）。「靠缝那一端」取两个缝框中心的中点 —— 截取与 innerEnd 都朝它，"
        "不填任何 side / toward。方向约束接 refLine（通常是 gap.datum_direction）。";
    op.inputs = {
        scanIn("定位之后的剖面对（gap.locate_template / gap.locate_model 的 scan）。"),
        roisIn("定位给出的角色框。"),
        Port{"refLine", "Line2D", "Ref Line", "方向约束的参考线。只有 dirMode 不是 free 时才需要。",
             false},
    };
    op.outputs = {
        Port{"line", "Line2D", "Line", "拟合出的直线（带端点）。", true},
        Port{"innerEnd", "Point2D", "Inner End", "内点里离缝最近的那个真实云点。", true},
        withExample(Port{"quality", "Record", "Quality", "GapFitQuality。", true},
                    examples::fitQuality()),
    };
    op.params = {
        enumParam("role", "Role", "datum", "拟合哪个角色的框。", roleOptions()),
        paramOf(r, "gap.fit_line", "distThresh", 0),
        paramOf(r, "gap.fit_line", "segmentPoints", 0),
        cloudChoice(),
        paramOf(r, "gap.fit_line", "endpoints", 1),
        paramOf(r, "gap.fit_line", "lineType", 1),
        paramOf(r, "gap.fit_line", "dirMode", 1),
        paramOf(r, "gap.fit_line", "dirNominalDeg", 1),
        paramOf(r, "gap.fit_line", "dirTolDeg", 1),
        paramOf(r, "gap.fit_line", "minInliers", 1),
    };
    op.capabilities = {false, false, true};
    op.compute = &roleLine;
    op.validate = &fine::validateFitLine;
    r.addOperator(std::move(op));
  }

  // ------------------------------------------------------------- gap.ref_point
  {
    OperatorDesc op;
    op.id = "gap.ref_point";
    op.version = "1.0.0";
    op.label = "取参考点";
    op.category = "间隙/积木";
    op.keywords = {"reference", "point", "line end", "参考点", "选点", "积木"};
    op.doc =
        "段差的参考点，三种取法：line_end = 在角色框里拟一条线、取靠缝那一端（与 gap.role_line "
        "同一个判据）；selected_point = 离角色框 min 角最近的云点（gap.selected_point）；"
        "nearest_point = 框里离基准线垂距最小的点（要接 baseLine）。\n"
        "line 只有 line_end 才是真拟出来的线；另两种给一条过参考点的占位线，quality.lineFitted=false"
        "—— 要参考线就用 gap.role_line(role=target)。";
    op.inputs = {
        scanIn("定位之后的剖面对。"),
        roisIn("定位给出的角色框。"),
        Port{"baseLine", "Line2D", "Base Line", "基准线。method=nearest_point 时必接。", false},
    };
    op.outputs = {
        Port{"point", "Point2D", "Point", "参考点。", true},
        Port{"line", "Line2D", "Line", "line_end 拟出的参考线；其余方法是占位。", true},
        withExample(Port{"quality", "Record", "Quality", "GapFitQuality。", true},
                    examples::fitQuality()),
    };
    auto lineOnly = [](Param p) {
      p.visibleWhen.param = "method";
      p.visibleWhen.eq = Value::text("line_end");
      return p;
    };
    op.params = {
        enumParam("method", "Method", "line_end", "参考点怎么取（flush.ref_type）。",
                  {EnumOption{"line_end", "Line End", "拟线、取靠缝那一端。"},
                   EnumOption{"selected_point", "Selected Point", "离框 min 角最近的云点。"},
                   EnumOption{"nearest_point", "Nearest Point", "框里离基准线最近的点。"}}),
        enumParam("role", "Role", "target", "在哪个角色的框里取。", roleOptions()),
        lineOnly(paramOf(r, "gap.fit_line", "distThresh", 0)),
        lineOnly(paramOf(r, "gap.fit_line", "segmentPoints", 0)),
        cloudChoice(),
        lineOnly(paramOf(r, "gap.fit_line", "endpoints", 1)),
        lineOnly(paramOf(r, "gap.fit_line", "lineType", 1)),
        lineOnly(paramOf(r, "gap.fit_line", "minInliers", 1)),
    };
    op.capabilities = {false, false, true};
    op.compute = &refPoint;
    op.validate = &validateRefPoint;
    r.addOperator(std::move(op));
  }

  // ---------------------------------------------------------- gap.seam_circles
  {
    OperatorDesc op;
    op.id = "gap.seam_circles";
    op.version = "1.0.0";
    op.label = "拟合缝两侧圆";
    op.category = "间隙/积木";
    op.keywords = {"circle", "gap", "seam", "圆", "间隙", "积木"};
    op.doc =
        "在 RoiSet 的两个缝框里各拟一个圆（gap.fit_gap_circles）：合并云优先，失败退到两台相机"
        "各拟一个。固定半径、逐侧相机、圆心高度带、弱拟合地板都在「高级」里；配了高度带就接 "
        "refLine（右侧要比另一条面时再接 refLineRight）。";
    op.inputs = {
        scanIn("定位之后的剖面对。"),
        roisIn("定位给出的角色框。"),
        Port{"refLine", "Line2D", "Ref Line", "圆心高度带的参考线。只有配了 centerTol 时才需要。",
             false},
        Port{"refLineRight", "Line2D", "Ref Line Right", "右侧高度带单独的参考线。", false},
    };
    op.outputs = {
        Port{"left", "Circle2D", "Left", "左圆。", true},
        Port{"right", "Circle2D", "Right", "右圆。", true},
        withExample(Port{"quality", "Record", "Quality", "GapFitQualityPair。", true},
                    examples::fitQualityPair()),
    };
    const OperatorDesc* circles = r.find("gap.fit_gap_circles");
    std::vector<Param> params;
    const std::set<std::string> visible = {"distThresh",    "nominal",       "leftRadiusMin",
                                           "leftRadiusMax", "rightRadiusMin", "rightRadiusMax"};
    for (const Param& p : circles ? circles->params : std::vector<Param>{}) {
      Param copy = p;
      copy.advanced = !visible.count(p.name);
      params.push_back(copy);
    }
    op.params = std::move(params);
    op.capabilities = {false, false, true};
    op.compute = &seamCircles;
    op.validate = &fine::validateFitGapCircles;
    r.addOperator(std::move(op));
  }

  // ------------------------------------------------------- gap.datum_direction
  {
    OperatorDesc op;
    op.id = "gap.datum_direction";
    op.version = "1.0.0";
    op.label = "方向基准";
    op.category = "间隙/积木";
    op.keywords = {"datum", "direction", "long plane", "方向", "基准", "长面", "积木"};
    op.doc =
        "基准面很窄时，它自己拟出的方向不可信：在角色框背离缝的那一侧推出一条长窗、在旁边那张"
        "长面上拟一条线，接到 gap.role_line 的 refLine 做方向约束（gap.datum_window + gap.fit_line）。"
        "哪一侧由 RoiSet 推出，不填 side。";
    op.inputs = {scanIn("定位之后的剖面对。"), roisIn("定位给出的角色框。")};
    op.outputs = {
        Port{"line", "Line2D", "Line", "长面上拟出的方向基准线。", true},
        withExample(Port{"quality", "Record", "Quality", "GapFitQuality。", true},
                    examples::fitQuality()),
    };
    op.params = {
        withDefault(paramOf(r, "gap.datum_window", "startMm", 0), Value::number(0.6)),
        withDefault(paramOf(r, "gap.datum_window", "lengthMm", 0), Value::number(13.4)),
        withDefault(paramOf(r, "gap.datum_window", "heightMm", 0), Value::number(2.5)),
        withDefault(paramOf(r, "gap.fit_line", "distThresh", 0), Value::number(0.35)),
        enumParam("role", "Role", "datum", "在哪个角色的框外面找长面。", roleOptions(),
                  /*advanced=*/true),
        withDefault(paramOf(r, "gap.fit_line", "minInliers", 1), Value::integer(60)),
    };
    op.capabilities = {false, false, true};
    op.compute = &datumDirection;
    op.validate = &fine::validateDatumWindow;
    r.addOperator(std::move(op));
  }
}

}  // namespace lyflow::packs::gap
