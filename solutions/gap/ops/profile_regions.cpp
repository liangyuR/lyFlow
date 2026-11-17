#include "profile_migration.h"
#include "profile_regions.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "lyflow/profile.h"
#include "lyflow_profile/selection.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
using Json = nlohmann::json;
Status bad(const std::string& why, const char* port = "", const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, why, {}, port);
}
Status cancelled() { return bad("场景范围计算已取消", "", "cancelled"); }
Box2D boxMm(const std::array<double, 4>& values) {
  Box2D box;
  box.min[0] = static_cast<float>(values[0]) / 1000.0F;
  box.min[1] = static_cast<float>(values[1]) / 1000.0F;
  box.max[0] = static_cast<float>(values[2]) / 1000.0F;
  box.max[1] = static_cast<float>(values[3]) / 1000.0F;
  return box;
}
bool inputProfile(const Data& data, const Record* provenance, Profile& metric, Json& sources,
                  std::string& why, ExecContext& context) {
  const auto* profile = data.asProfile();
  if (profile->descriptor.plane != "XY") {
    why = "场景范围只接受明确量测XY平面";
    return false;
  }
  if (!profile_contract::profileSources(data, provenance, sources, why))
    return false;
  return convertProfileUnit(*profile, "m", metric, why, [&context] { return context.cancelled(); });
}
bool inputBox(const Data& data, const Record& record, ProfileDescriptor& descriptor, Json& sources,
              std::string& why) {
  if (!data.asBox2D() || record.type != "profile.Geometry.v2" ||
      !profile_contract::geometryMatchesPhysical(data, &record, descriptor, why, &sources) ||
      descriptor.plane != "XY") {
    if (why.empty())
      why = "需要严格版本2上下文绑定的米制量测XY框";
    return false;
  }
  return true;
}
bool unionSources(Json& output, const Json& extra, std::string& why) {
  for (const auto& source : extra)
    if (std::find(output.begin(), output.end(), source) == output.end())
      output.push_back(source);
  ProfileDescriptor descriptor;
  return profile_contract::geometrySources(output, descriptor, why);
}
bool geometry(const Data& box, const Json& sources, const std::string& relation, Record& output,
              std::string& why) {
  try {
    output = {"profile.Geometry.v2",
              profile_contract::geometryContextV2FromSources(box, sources, relation)};
    return true;
  } catch (const std::invalid_argument& error) {
    why = error.what();
    return false;
  }
}
std::optional<std::array<float, 2>> center(const Profile& profile, ExecContext& context) {
  std::vector<float> xs, ys;
  xs.reserve(profile.pointCount());
  ys.reserve(profile.pointCount());
  for (std::size_t i = 0; i < profile.pointCount(); ++i) {
    if ((i & 4095U) == 0 && context.cancelled())
      return std::nullopt;
    const float x = profile.cloud->xyz[3 * i], y = profile.cloud->xyz[3 * i + 1];
    if (!profile.validity[i] || !std::isfinite(x) || !std::isfinite(y))
      continue;
    xs.push_back(x);
    ys.push_back(y);
  }
  if (xs.empty())
    return std::nullopt;
  const auto median = [](std::vector<float>& values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    const float upper = *middle;
    if (values.size() % 2 != 0)
      return upper;
    const auto lower = std::max_element(values.begin(), middle);
    return (*lower + upper) * .5F;
  };
  return std::array<float, 2>{median(xs), median(ys)};
}
std::optional<std::array<double, 4>> candidate(const Box2D& left, const Box2D& right,
                                               const RollCropOptions& options,
                                               std::string& reason) {
  if (!options.enabled) {
    reason = "disabled";
    return std::nullopt;
  }
  if (options.halfWidth <= 0 || options.halfHeight <= 0 || options.maxRollBoxHeight <= 0) {
    reason = "bad_config";
    return std::nullopt;
  }
  std::array<double, 4> boxes[2];
  for (const auto& entry : {std::make_pair(&left, &boxes[0]), std::make_pair(&right, &boxes[1])}) {
    const auto& input = *entry.first;
    *entry.second = {
        static_cast<double>(input.min[0]) * 1000., static_cast<double>(input.min[1]) * 1000.,
        static_cast<double>(input.max[0]) * 1000., static_cast<double>(input.max[1]) * 1000.};
    const auto& b = *entry.second;
    if (!std::isfinite(b[0]) || !std::isfinite(b[1]) || !std::isfinite(b[2]) ||
        !std::isfinite(b[3]) || b[2] <= b[0] || b[3] <= b[1]) {
      reason = "degenerate_roll_box";
      return std::nullopt;
    }
    if (b[3] - b[1] > options.maxRollBoxHeight) {
      reason = "roll_box_height";
      return std::nullopt;
    }
  }
  const auto& a = boxes[0];
  const auto& b = boxes[1];
  const double span = std::max(a[2], b[2]) - std::min(a[0], b[0]);
  if (span < 2.0 || span > 40.0) {
    reason = "span";
    return std::nullopt;
  }
  const double ax = (a[0] + a[2]) * .5, ay = (a[1] + a[3]) * .5;
  const double bx = (b[0] + b[2]) * .5, by = (b[1] + b[3]) * .5;
  const double x = (ax + bx) * .5, y = (ay + by) * .5;
  return std::array<double, 4>{x - options.halfWidth, y - options.halfHeight, x + options.halfWidth,
                               y + options.halfHeight};
}
bool bounds(const Profile& a, const Profile& b, Box2D& result, ExecContext& context) {
  float lo[2]{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
  float hi[2]{-lo[0], -lo[1]};
  bool found = false;
  for (const auto* profile : {&a, &b})
    for (std::size_t i = 0; i < profile->pointCount(); ++i) {
      if ((i & 4095U) == 0 && context.cancelled())
        return false;
      if (!profile->validity[i])
        continue;
      found = true;
      for (int k = 0; k < 2; ++k) {
        lo[k] = std::min(lo[k], profile->cloud->xyz[3 * i + k]);
        hi[k] = std::max(hi[k], profile->cloud->xyz[3 * i + k]);
      }
    }
  if (!found)
    return false;
  result.min[0] = lo[0];
  result.min[1] = lo[1];
  result.max[0] = hi[0];
  result.max[1] = hi[1];
  return true;
}
Param text(const char* name, const char* initial) {
  Param p;
  p.name = name;
  p.label = name;
  p.type = ParamType::String;
  p.def = Value::text(initial);
  return p;
}
Param choice(const char* name, const char* initial, std::initializer_list<const char*> values) {
  auto p = text(name, initial);
  p.type = ParamType::Enum;
  for (const auto* value : values)
    p.options.push_back({value, value});
  return p;
}
Param mm(const char* name, double initial) {
  auto p = text(name, "");
  p.type = ParamType::Float;
  p.def = Value::number(initial);
  p.unit = "mm";
  p.min = 0;
  return p;
}
OperatorDesc description(const char* id, const char* label) {
  OperatorDesc op;
  op.id = id;
  op.version = "2.0.0";
  declareGapGraphUpgrade(op);
  op.label = label;
  op.category = "间隙/预处理";
  op.capabilities.cancellable = true;
  op.capabilities.deterministic = true;
  return op;
}
} // namespace

Status overallRoi(const Data& primary, const Data& secondary, const Record* primaryProvenance,
                  const Record* secondaryProvenance, const OverallRoiOptions& options,
                  GeometryResult& result, ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  Profile p, s;
  Json sources, other;
  std::string why;
  if (!inputProfile(primary, primaryProvenance, p, sources, why, context) ||
      !inputProfile(secondary, secondaryProvenance, s, other, why, context))
    return context.cancelled() ? cancelled() : bad(why);
  if (!unionSources(sources, other, why))
    return bad(why);
  const auto& roi = options.roiMm;
  if (roi[0] > roi[2] || roi[1] > roi[3])
    return bad("范围边界倒置", "", "bad_param");
  std::array<double, 4> resolved{roi[0], roi[1], roi[2], roi[3]};
  bool moved = false;
  if (options.mode == "auto_center") {
    const auto left = center(p, context), right = center(s, context);
    if (context.cancelled())
      return cancelled();
    std::optional<std::array<float, 2>> value;
    if (options.usingCamera == "Left")
      value = left;
    else if (options.usingCamera == "Right")
      value = right;
    else if (left && right)
      value =
          std::array<float, 2>{((*left)[0] + (*right)[0]) * .5F, ((*left)[1] + (*right)[1]) * .5F};
    else
      value = left ? left : right;
    if (value) {
      const double halfWidth = (roi[2] - roi[0]) * .5;
      const double halfHeight = (roi[3] - roi[1]) * .5;
      const double x = static_cast<double>((*value)[0]) * 1000.;
      const double y = static_cast<double>((*value)[1]) * 1000.;
      resolved = {x - halfWidth, y - halfHeight, x + halfWidth, y + halfHeight};
      moved = true;
      context.log(LogLevel::Info,
                  "auto_center: 中心 (" + std::to_string(x) + ", " + std::to_string(y) + ") mm");
    }
  }
  GeometryResult output;
  output.box = Data::box2d(boxMm(resolved));
  if (!geometry(output.box, sources, moved ? "derived" : "declared", output.context, why))
    return bad(why);
  output.status = {
      "gap.OverallRoi.v2",
      {{"mode", options.mode}, {"usingCamera", options.usingCamera}, {"centerApplied", moved}}};
  if (context.cancelled())
    return cancelled();
  result = std::move(output);
  return Status::Ok();
}
Status datumWindow(const Data& anchor, const Record& anchorContext, const Data* heightAnchor,
                   const Record* heightContext, const DatumWindowOptions& options,
                   GeometryResult& result, ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  if ((heightAnchor != nullptr) != (heightContext != nullptr))
    return bad("高度锚框与上下文必须成对提供", "heightAnchor");
  if (options.startMm < 0 || options.lengthMm <= 0 || options.heightMm < 0)
    return bad("基准窗口毫米距离参数非法", "", "bad_param");
  Json sources, more;
  ProfileDescriptor space, other;
  std::string why;
  if (!inputBox(anchor, anchorContext, space, sources, why))
    return bad(why, "anchorContext");
  if (heightAnchor && (!inputBox(*heightAnchor, *heightContext, other, more, why) ||
                       !unionSources(sources, more, why)))
    return bad(why, "heightContext");
  const auto& a = *anchor.asBox2D();
  const auto& h = heightAnchor ? *heightAnchor->asBox2D() : a;
  const double start = static_cast<float>(options.startMm / 1000.);
  const double length = static_cast<float>(options.lengthMm / 1000.);
  const double height = static_cast<float>(options.heightMm / 1000.);
  Box2D box;
  if (options.side == "left") {
    box.max[0] = static_cast<float>(a.min[0] - start);
    box.min[0] = static_cast<float>(box.max[0] - length);
  } else {
    box.min[0] = static_cast<float>(a.max[0] + start);
    box.max[0] = static_cast<float>(box.min[0] + length);
  }
  box.min[1] = static_cast<float>(h.min[1] - height);
  box.max[1] = static_cast<float>(h.max[1] + height);
  GeometryResult output;
  output.box = Data::box2d(box);
  if (!geometry(output.box, sources, "derived", output.context, why))
    return bad(why);
  output.status = {"gap.DatumWindow.v2",
                   {{"side", options.side},
                    {"startMm", options.startMm},
                    {"lengthMm", options.lengthMm},
                    {"heightMm", options.heightMm}}};
  if (context.cancelled())
    return cancelled();
  result = std::move(output);
  return Status::Ok();
}
Status rollAnchoredCrop(const Data& scan, const Data& gapLeft, const Record& gapLeftContext,
                        const Data& gapRight, const Record& gapRightContext,
                        const RollCropOptions& options, RollCropResult& result,
                        ExecContext& context) {
  if (context.cancelled())
    return cancelled();
  ScanView view;
  std::string why;
  if (!readScanPair(scan, view, why))
    return bad(why, "scan");
  Json sources, more;
  ProfileDescriptor leftSpace, rightSpace;
  if (!inputBox(gapLeft, gapLeftContext, leftSpace, sources, why) ||
      !inputBox(gapRight, gapRightContext, rightSpace, more, why) ||
      !unionSources(sources, more, why))
    return bad(why, "gapLeftContext");
  if (!profile_contract::sameSpace(leftSpace, view.primary->asProfile()->descriptor))
    return bad("锚框与场景量测空间不一致");
  std::string reason;
  const auto requested = candidate(*gapLeft.asBox2D(), *gapRight.asBox2D(), options, reason);
  const auto minimum = static_cast<std::size_t>(std::max<std::int64_t>(0, options.minPointsKept));
  bool applied = false;
  std::string status;
  RollCropResult output;
  const auto& p = *view.primary->asProfile();
  const auto& s = *view.secondary->asProfile();
  output.primaryProvenance = view.primaryProvenance;
  output.secondaryProvenance = view.secondaryProvenance;
  std::size_t keptP = p.pointCount(), keptS = s.pointCount();
  if (requested) {
    const auto window = Data::box2d(boxMm(*requested));
    Record windowContext;
    if (!geometry(window, sources, "derived", windowContext, why))
      return bad(why);
    profile_contract::SelectionOptions selection;
    selection.bounds = "open";
    selection.emptyPolicy = "allow";
    profile_contract::SelectionResult cutP, cutS, cutMerged;
    if (auto status =
            profile_contract::cropProfile(*view.primary, window, windowContext,
                                          &view.primaryProvenance, selection, cutP, context);
        !status.ok)
      return status;
    if (auto status =
            profile_contract::cropProfile(*view.secondary, window, windowContext,
                                          &view.secondaryProvenance, selection, cutS, context);
        !status.ok)
      return status;
    const bool insufficient =
        options.usingCamera == "Left" ? cutP.selected.values.size() < minimum
        : options.usingCamera == "Right"
            ? cutS.selected.values.size() < minimum
            : cutP.selected.values.size() + cutS.selected.values.size() < minimum;
    if (insufficient)
      status = "reverted:min_points";
    else {
      if (auto status =
              profile_contract::cropProfile(*view.merged, window, windowContext,
                                            view.mergedProvenance, selection, cutMerged, context);
          !status.ok)
        return status;
      const auto& info = view.info->data;
      ScanMetadata metadata{info["sensorFrame"].get<std::string>(),
                            info["measurementFrame"].get<std::string>(),
                            "processed",
                            info["guard"],
                            nullptr,
                            info["origins"],
                            info["processing"]};
      keptP = cutP.profile.asProfile()->pointCount();
      keptS = cutS.profile.asProfile()->pointCount();
      output.primaryProvenance = std::move(cutP.provenance);
      output.secondaryProvenance = std::move(cutS.provenance);
      metadata.primaryProvenance = output.primaryProvenance;
      metadata.secondaryProvenance = output.secondaryProvenance;
      output.window = window;
      output.windowContext = windowContext;
      applied = true;
      status = "applied";
      output.status = {"gap.RollCrop.v2",
                       {{"status", status},
                        {"applied", true},
                        {"beforePrimary", p.pointCount()},
                        {"beforeSecondary", s.pointCount()},
                        {"afterPrimary", keptP},
                        {"afterSecondary", keptS},
                        {"minPointsKept", minimum},
                        {"boxMm", *requested},
                        {"mergedRebuilt", false}}};
      metadata.crop = output.status.data;
      metadata.processing["status"] = "complete";
      metadata.processing["details"]["operation"] = "roll_anchored_crop";
      metadata.processing["details"]["mergedRebuilt"] = false;
      if (!makeScanPair(*view.sensorPrimary, *view.sensorSecondary, cutP.profile, cutS.profile,
                        cutMerged.profile, cutMerged.provenance, metadata, output.scan, why))
        return bad(why);
    }
  } else
    status = std::string(options.enabled ? "rejected:" : "") + reason;
  if (!applied) {
    Box2D actual;
    if (!bounds(p, s, actual, context))
      return context.cancelled() ? cancelled()
                                 : bad("没有有效样本，不能构造实际范围", "scan", "no_result");
    Json actualSources, other;
    if (!profile_contract::profileSources(*view.primary, &view.primaryProvenance, actualSources,
                                          why) ||
        !profile_contract::profileSources(*view.secondary, &view.secondaryProvenance, other, why) ||
        !unionSources(actualSources, other, why))
      return bad(why);
    output.window = Data::box2d(actual);
    if (!geometry(output.window, actualSources, "derived", output.windowContext, why))
      return bad(why);
    output.scan = scan;
    output.status = {"gap.RollCrop.v2",
                     {{"status", status},
                      {"applied", false},
                      {"beforePrimary", p.pointCount()},
                      {"beforeSecondary", s.pointCount()},
                      {"afterPrimary", p.pointCount()},
                      {"afterSecondary", s.pointCount()},
                      {"minPointsKept", minimum},
                      {"mergedRebuilt", false}}};
    if (requested)
      output.status.data["boxMm"] = *requested;
  }
  context.log(LogLevel::Info, "roll crop " + status + ": " + std::to_string(p.pointCount()) + "+" +
                                  std::to_string(s.pointCount()) + " → " + std::to_string(keptP) +
                                  "+" + std::to_string(keptS));
  if (context.cancelled())
    return cancelled();
  result = std::move(output);
  return Status::Ok();
}

void registerProfileRegionOps(Registry& registry) {
  auto overall = description("gap.overall_roi", "原生整体范围构建");
  overall.inputs = {{"primary", "Profile", "首量测轮廓", "明确量测XY平面。", true},
                    {"secondary", "Profile", "次量测轮廓", "允许单侧无有效点。", true},
                    {"primaryProvenance", "Record", "首来源", "可选复合来源。", false},
                    {"secondaryProvenance", "Record", "次来源", "可选复合来源。", false}};
  overall.outputs = {{"box", "Box2D", "整体范围", "固定或中位数居中。", true},
                     {"context", "Record", "范围上下文", "严格几何来源。", true},
                     {"status", "Record", "状态", "是否实际应用居中。", true}};
  auto roi = text("roi", "");
  roi.type = ParamType::Vec4f;
  roi.def = Value::vec({-1, -1, 1, 1});
  roi.unit = "mm";
  roi.semantic = "roi";
  overall.params = {roi, choice("mode", "fixed", {"fixed", "auto_center"}),
                    choice("usingCamera", "Both", {"Both", "Left", "Right"})};
  overall.compute = [](const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
    const auto* a = in.get("primaryProvenance").asRecord();
    const auto* b = in.get("secondaryProvenance").asRecord();
    GeometryResult result;
    const auto status = overallRoi(
        in.get("primary"), in.get("secondary"), a, b,
        {params.vec4("roi"), params.choice("mode"), params.choice("usingCamera")}, result, ctx);
    if (!status.ok)
      return status;
    out.set("box", std::move(result.box));
    out.set("context", Data::record(std::move(result.context)));
    out.set("status", Data::record(std::move(result.status)));
    return Status::Ok();
  };
  registry.addOperator(std::move(overall));
  auto datum = description("gap.datum_window", "原生方向基准范围构建");
  datum.inputs = {{"anchor", "Box2D", "水平锚框", "定义窗口水平位置。", true},
                  {"anchorContext", "Record", "水平上下文", "严格v2几何绑定。", true},
                  {"heightAnchor", "Box2D", "高度锚框", "可选独立高度范围。", false},
                  {"heightContext", "Record", "高度上下文", "与高度锚框成对。", false}};
  datum.outputs = {{"box", "Box2D", "基准窗口", "沿明确方向推出的窗口。", true},
                   {"context", "Record", "窗口上下文", "保留双锚框来源。", true},
                   {"status", "Record", "参数记录", "实际窗口定义。", true}};
  datum.params = {choice("side", "left", {"left", "right"}), mm("startMm", 0), mm("lengthMm", 13),
                  mm("heightMm", 2.5)};
  datum.compute = [](const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
    const auto* a = in.get("anchorContext").asRecord();
    const auto* h = in.get("heightContext").asRecord();
    GeometryResult result;
    auto status = datumWindow(in.get("anchor"), *a,
                              in.has("heightAnchor") ? &in.get("heightAnchor") : nullptr, h,
                              {params.choice("side"), params.number("startMm"),
                               params.number("lengthMm"), params.number("heightMm")},
                              result, ctx);
    if (!status.ok)
      return status;
    out.set("box", std::move(result.box));
    out.set("context", Data::record(std::move(result.context)));
    out.set("status", Data::record(std::move(result.status)));
    return Status::Ok();
  };
  registry.addOperator(std::move(datum));
  auto roll = description("gap.roll_anchored_crop", "原生零件随动裁剪");
  roll.inputs = {
      {"scan", "Bundle<gap.ScanPair.v2>", "场景", "保留raw传感器与已有去噪merged。", true},
      {"gapLeft", "Box2D", "左锚框", "米制量测框。", true},
      {"gapLeftContext", "Record", "左上下文", "严格v2几何绑定。", true},
      {"gapRight", "Box2D", "右锚框", "米制量测框。", true},
      {"gapRightContext", "Record", "右上下文", "严格v2几何绑定。", true}};
  roll.outputs = {
      {"scan", "Bundle<gap.ScanPair.v2>", "裁剪场景", "未apply共享原场景；不重建merged。", true},
      {"window", "Box2D", "实际窗口", "候选窗或真实有效点范围。", true},
      {"windowContext", "Record", "窗口上下文", "实际量测来源。", true},
      {"status", "Record", "裁剪状态", "保留旧状态与前后点数。", true},
      {"primaryProvenance", "Record", "首裁剪来源", "细粒度拟合的引用来源。", true},
      {"secondaryProvenance", "Record", "次裁剪来源", "细粒度拟合的引用来源。", true}};
  auto enabled = text("enabled", "");
  enabled.type = ParamType::Bool;
  enabled.def = Value::boolean(true);
  auto minimum = text("minPointsKept", "");
  minimum.type = ParamType::Int;
  minimum.def = Value::integer(50);
  minimum.min = 0;
  roll.params = {enabled,
                 mm("halfWidth", 35),
                 mm("halfHeight", 20),
                 mm("maxRollBoxHeight", 8),
                 minimum,
                 choice("usingCamera", "Both", {"Both", "Left", "Right"})};
  roll.compute = [](const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
    const auto* a = in.get("gapLeftContext").asRecord();
    const auto* b = in.get("gapRightContext").asRecord();
    RollCropResult result;
    auto status = rollAnchoredCrop(in.get("scan"), in.get("gapLeft"), *a, in.get("gapRight"), *b,
                                   {params.flag("enabled"), params.number("halfWidth"),
                                    params.number("halfHeight"), params.number("maxRollBoxHeight"),
                                    params.integer("minPointsKept"), params.choice("usingCamera")},
                                   result, ctx);
    if (!status.ok)
      return status;
    out.set("scan", std::move(result.scan));
    out.set("window", std::move(result.window));
    out.set("windowContext", Data::record(std::move(result.windowContext)));
    out.set("status", Data::record(std::move(result.status)));
    out.set("primaryProvenance", Data::record(std::move(result.primaryProvenance)));
    out.set("secondaryProvenance", Data::record(std::move(result.secondaryProvenance)));
    return Status::Ok();
  };
  registry.addOperator(std::move(roll));
}
} // namespace lyflow::packs::gap::profile_scene
