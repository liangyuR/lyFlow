#include <cmath>
#include <limits>
#include <numeric>

#include "lyflow/kernel/profile.h"
#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::std_profile {
namespace {
namespace kp = kernel::profile;

Status bad(const std::string& message, const char* port, const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}

Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "轮廓处理已取消"); }

std::pair<int, int> planeAxes(const ProfileDescriptor& descriptor) {
  if (descriptor.plane == "XY")
    return {0, 1};
  if (descriptor.plane == "YZ")
    return {1, 2};
  return {0, 2};
}

bool workCoordinate(float input, double scale, float& output) {
  const double value = static_cast<double>(input) * scale;
  if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max())
    return false;
  output = static_cast<float>(value);
  return true;
}

Status clean(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const auto* input = inputs.get("profile").asProfile();
  if (ctx.cancelled())
    return cancelled();
  const std::size_t n = input->pointCount();
  if (n == 0 || n > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return bad(n == 0 ? "空轮廓" : "轮廓槽数超过处理范围", "profile");
  const auto width = params.integer("smoothPoints");
  const auto minimum = params.integer("minValidPoints");
  const double minIntensity = params.number("minIntensity");
  const std::string& mode = params.choice("mode");

  const auto axes = planeAxes(input->descriptor);
  const double scale = input->descriptor.unit == "m" ? 1000.0 : 1.0;
  std::vector<float> z(n, std::numeric_limits<float>::quiet_NaN());
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 4095) == 0 && ctx.cancelled())
      return cancelled();
    if (!input->validity[i] ||
        (input->cloud->hasIntensity() && !(input->cloud->intensity[i] >= minIntensity)))
      continue;
    if (!workCoordinate(input->cloud->xyz[3 * i + axes.second], scale, z[i]))
      return bad("轮廓坐标换算到毫米工作标度超出范围", "profile");
  }

  try {
    const auto smoothed =
        kp::movingMedian(z, static_cast<int>(width), [&ctx] { return ctx.cancelled(); });
    std::vector<std::uint8_t> validity = input->validity;
    std::vector<std::int32_t> keep;
    keep.reserve(n);
    std::size_t validCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 4095) == 0 && ctx.cancelled())
        return cancelled();
      validity[i] = validity[i] && std::isfinite(smoothed[i]);
      validCount += validity[i];
      if (mode == "preserve_slots" || validity[i])
        keep.push_back(static_cast<std::int32_t>(i));
    }
    if (validCount < static_cast<std::size_t>(minimum))
      return bad("有效点太少（" + std::to_string(validCount) + "）", "profile");
    PointCloud cloud = input->cloud->select(keep);
    std::vector<std::uint8_t> selectedValidity;
    std::vector<std::uint32_t> selectedSources;
    selectedValidity.reserve(keep.size());
    selectedSources.reserve(keep.size());
    for (std::size_t i = 0; i < keep.size(); ++i) {
      if ((i & 4095) == 0 && ctx.cancelled())
        return cancelled();
      const auto slot = static_cast<std::size_t>(keep[i]);
      selectedValidity.push_back(validity[slot]);
      selectedSources.push_back(input->sourceIndices[slot]);
      if (validity[slot] && width > 1)
        cloud.xyz[3 * i + axes.second] = static_cast<float>(smoothed[slot] / scale);
    }
    if (ctx.cancelled())
      return cancelled();
    outputs.set("clean", Data::profile(Profile{std::make_shared<const PointCloud>(std::move(cloud)),
                                               input->descriptor, std::move(selectedValidity),
                                               std::move(selectedSources)}));
    return Status::Ok();
  } catch (const kp::Cancelled&) {
    return cancelled();
  }
}

nlohmann::json faceJson(const kp::Face& f) {
  return {{"x0", f.x0},       {"x1", f.x1},       {"z0", f.z0},
          {"z1", f.z1},       {"lenMm", f.lenMm}, {"angleDeg", f.angleDeg},
          {"rmsMm", f.rmsMm}, {"n", f.n},         {"px", f.line.px},
          {"pz", f.line.pz},  {"dx", f.line.dx},  {"dz", f.line.dz}};
}

Status split(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const Data& source = inputs.get("clean");
  const auto* input = source.asProfile();
  if (input->descriptor.sampleOrder != "acquisition")
    return bad("分片需要明确的acquisition采样列序，不能推断未知顺序", "clean");
  if (ctx.cancelled())
    return cancelled();
  if (input->pointCount() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return bad("轮廓槽数超过处理范围", "clean");
  kp::PlanarSamples active;
  std::vector<std::size_t> slots;
  const auto axes = planeAxes(input->descriptor);
  const double scale = input->descriptor.unit == "m" ? 1000.0 : 1.0;
  for (std::size_t i = 0; i < input->pointCount(); ++i) {
    if ((i & 4095) == 0 && ctx.cancelled())
      return cancelled();
    if (!input->validity[i])
      continue;
    float x, z;
    if (!workCoordinate(input->cloud->xyz[3 * i + axes.first], scale, x) ||
        !workCoordinate(input->cloud->xyz[3 * i + axes.second], scale, z))
      return bad("轮廓坐标换算到毫米工作标度超出范围", "clean");
    active.x.push_back(x);
    active.z.push_back(z);
    slots.push_back(i);
  }
  const auto minimum = params.integer("minPoints");
  kp::FaceParams fp;
  fp.fitLenMm = params.number("fitLenMm");
  fp.maxRmsMm = params.number("maxRmsMm");
  fp.angleBreakDeg = params.number("angleBreakDeg");
  fp.minLenMm = params.number("minLenMm");
  const double gap = params.number("gapMm"), jump = params.number("jumpMm");
  try {
    const auto stop = [&ctx] { return ctx.cancelled(); };
    const auto pieces = kp::splitPieces(active, gap, jump, static_cast<int>(minimum), stop);
    if (pieces.empty())
      return bad("切不出任何分片", "clean", "no_segments");
    const auto faces = kp::splitFaces(active, pieces, fp, stop);
    nlohmann::json result{{"schemaVersion", 2},
                          {"workingUnit", "mm"},
                          {"indexSpace", "active_samples"},
                          {"slotCount", input->pointCount()},
                          {"activeCount", active.size()},
                          {"parameters",
                           {{"gapMm", gap},
                            {"jumpMm", jump},
                            {"minPoints", minimum},
                            {"fitLenMm", fp.fitLenMm},
                            {"maxRmsMm", fp.maxRmsMm},
                            {"angleBreakDeg", fp.angleBreakDeg},
                            {"minLenMm", fp.minLenMm}}},
                          {"pieces", nlohmann::json::array()},
                          {"faces", nlohmann::json::array()}};
    for (const auto& piece : pieces) {
      if (ctx.cancelled())
        return cancelled();
      const auto view = kp::pieceView(active, piece, stop);
      const auto first = slots[static_cast<std::size_t>(piece.i0)];
      const auto last = slots[static_cast<std::size_t>(piece.i1)];
      result["pieces"].push_back({{"i0", piece.i0},
                                  {"i1", piece.i1},
                                  {"n", view.size()},
                                  {"x0", view.x(0)},
                                  {"x1", view.x(view.size() - 1)},
                                  {"slotStart", first},
                                  {"slotEnd", last},
                                  {"sourceStart", input->sourceIndices[first]},
                                  {"sourceEnd", input->sourceIndices[last]}});
    }
    for (const auto& face : faces) {
      if (ctx.cancelled())
        return cancelled();
      result["faces"].push_back(faceJson(face));
    }
    if (ctx.cancelled())
      return cancelled();
    outputs.set("faces", Data::record(Record{"profile.Faces.v2", std::move(result)}));
    return Status::Ok();
  } catch (const kp::Cancelled&) {
    return cancelled();
  }
}

Param real(const char* name, const char* label, double value, const char* unit = "mm") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Float;
  p.def = Value::number(value);
  p.min = 0;
  p.unit = unit;
  return p;
}

Param integer(const char* name, const char* label, int value, int minimum) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Int;
  p.def = Value::integer(value);
  p.min = minimum;
  p.max = std::numeric_limits<int>::max();
  return p;
}
} // namespace

void registerProfileProcessing(Registry& registry) {
  OperatorDesc op;
  op.id = "profile.clean";
  op.version = "1.0.0";
  op.label = "有序轮廓清洗";
  op.category = "轮廓/预处理";
  op.keywords = {"profile", "median", "轮廓", "中值", "保槽"};
  op.doc =
      "按原始槽序忽略无效邻居做中值；无效中心不补洞。保槽模式保留无效原载荷，压缩模式保留原源索引。";
  op.inputs = {{"profile", "Profile", "Profile", "带单位、平面、有效性与源索引的轮廓。", true}};
  op.outputs = {{"clean", "Profile", "Clean", "保留原描述、单位和来源的清洗轮廓。", true}};
  Param mode;
  mode.name = "mode";
  mode.label = "输出槽模式";
  mode.type = ParamType::Enum;
  mode.def = Value::text("preserve_slots");
  mode.options = {{"preserve_slots", "保留槽"}, {"compact", "仅保留有效槽"}};
  auto smooth = integer("smoothPoints", "中值窗宽", 5, 1);
  smooth.softMax = 31;
  smooth.doc = "沿输入槽序取窗，偶数宽度沿用两侧各width/2槽的规则。1不平滑。";
  op.params = {smooth, real("minIntensity", "最低亮度", .5, ""),
               integer("minValidPoints", "最少有效点", 16, 1), mode};
  op.capabilities = {true, true, true};
  op.compute = clean;
  registry.addOperator(std::move(op));

  op = {};
  op.id = "profile.split_faces";
  op.version = "2.0.0";
  op.migrations = {{1, nullptr, &profile_contract::keepInputs}};
  op.label = "有序轮廓分片与直线面";
  op.category = "轮廓/分段";
  op.keywords = {"profile", "TLS", "segmentation", "轮廓", "分片", "分面"};
  op.doc =
      "按采样序的相邻有效槽分片，分片内按第一平面轴建立索引视图，滑窗TLS提取直线面。毫米工作标度，分片记录原槽与物理源索引。";
  op.inputs = {{"clean", "Profile", "Clean", "明确acquisition采样序的轮廓。", true}};
  op.outputs = {
      {"faces", "Record", "Faces", "profile.Faces.v2毫米工作坐标及active样本索引。", true}};
  op.params = {real("gapMm", "第一轴间断", 1),
               real("jumpMm", "第二轴跳变", 1.5),
               integer("minPoints", "分片最少点数", 15, 3),
               real("fitLenMm", "滑窗长度", 5),
               real("maxRmsMm", "窗口RMS上限", .05),
               real("angleBreakDeg", "角度断开阈", 6, "deg"),
               real("minLenMm", "最短面长", 3)};
  op.capabilities = {true, true, true};
  op.compute = split;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_profile
