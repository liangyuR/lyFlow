#include "profile_guard.h"

#include <cmath>
#include <cstdio>
#include <utility>

#include "lyflow/op_params.h"
#include "lyflow/operator.h"
#include "lyflow_profile/context.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
std::string mmText(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return buffer;
}
Status selectCamera(const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
  const auto& primary = in.get("primary");
  const auto& secondary = in.get("secondary");
  const auto& a = primary.asProfile()->descriptor;
  const auto& b = secondary.asProfile()->descriptor;
  const auto& action = params.choice("onDisagree");
  const auto maximum = params.number("maxDeltaMm");
  if (!profile_contract::sameSpace(a, b))
    return Status::Error(Phase::Execute, "space_mismatch", "两路轮廓必须位于同一坐标空间");
  Record quality{"gap.CameraGuard.v3",
                 {{"schemaVersion", 3},
                  {"onDisagree", action},
                  {"maxDeltaMm", maximum},
                  {"evaluated", false},
                  {"exceeded", false},
                  {"inputPrimarySourceId", a.sourceId},
                  {"inputSecondarySourceId", b.sourceId}}};
  const Data* left = &primary;
  const Data* right = &secondary;
  const char* taken = "both";
  if (action != "off") {
    if (!in.has("observation"))
      return op_params::badInput("相机分歧策略需要轮廓重叠观测", "observation");
    const auto& data = in.get("observation").asRecord()->data;
    if (!data.is_object() || data.value("unit", nlohmann::json{}) != "m" ||
        data.value("primarySourceId", nlohmann::json{}) != a.sourceId ||
        data.value("secondarySourceId", nlohmann::json{}) != b.sourceId)
      return op_params::badInput("重叠观测须来自当前有序双路轮廓，并使用米制", "observation");
    const auto evaluated = data.find("evaluated");
    const auto count = data.find("sampleCount");
    if (evaluated == data.end() || !evaluated->is_boolean() || count == data.end() ||
        !count->is_number_integer() || *count < 0)
      return op_params::badInput("重叠观测缺少评估状态或有效采样数", "observation");
    quality.data["sampleCount"] = *count;
    if (!evaluated->get<bool>()) {
      quality.data["reason"] = "insufficient_overlap";
      ctx.log(LogLevel::Info, "相机一致性观测的有效重叠采样不足");
    } else {
      const auto signedValue = data.find("signedMedian");
      const auto absoluteValue = data.find("absoluteMedian");
      if (signedValue == data.end() || !signedValue->is_number() || absoluteValue == data.end() ||
          !absoluteValue->is_number())
        return op_params::badInput("重叠观测缺少高度差中位数", "observation");
      const double delta = absoluteValue->get<double>() * 1000.0;
      const double signedDelta = signedValue->get<double>() * 1000.0;
      if (!std::isfinite(delta) || !std::isfinite(signedDelta) || delta < 0)
        return op_params::badInput("重叠观测的高度差中位数非法", "observation");
      const bool exceeded = delta > maximum;
      quality.data["evaluated"] = true;
      quality.data["exceeded"] = exceeded;
      quality.data["deltaMedianMm"] = delta;
      quality.data["signedMedianMm"] = signedDelta;
      const std::string summary = "重叠区高度差中位 " + mmText(delta) + " mm（带符号 " +
                                  mmText(signedDelta) + "），上限 " + mmText(maximum) + " mm";
      ctx.log(exceeded ? LogLevel::Warn : LogLevel::Info, summary);
      if (exceeded && action == "keepPrimary") {
        right = &primary;
        taken = "primary";
      } else if (exceeded && action == "keepSecondary") {
        left = &secondary;
        taken = "secondary";
      } else if (exceeded && action == "fail") {
        return Status::Error(Phase::Execute, "camera_disagree",
                             "相机看到的不是同一个面：" + summary);
      }
    }
  }
  if (ctx.cancelled())
    return Status::Error(Phase::Execute, "cancelled", "相机选择已取消");
  quality.data["taken"] = taken;
  quality.data["primarySourceId"] = left->asProfile()->descriptor.sourceId;
  quality.data["secondarySourceId"] = right->asProfile()->descriptor.sourceId;
  out.set("primary", *left);
  out.set("secondary", *right);
  out.set("quality", Data::record(std::move(quality)));
  return Status::Ok();
}
}

void registerCameraSelection(Registry& registry) {
  using namespace op_params;
  OperatorDesc op;
  op.id = "gap.select_camera";
  op.version = "1.0.0";
  op.label = "按轮廓分歧选择相机";
  op.category = "间隙/质量";
  op.doc =
      "按绝对高度差中位数记录、择相机或报错。重叠不足保留双路，择一路时输出该路的原始物理来源。";
  op.inputs = {{"primary", "Profile", "第一相机", "第一逻辑角色的真实物理源。", true},
               {"secondary", "Profile", "第二相机", "第二逻辑角色的真实物理源。", true},
               withContract({"observation", "Record", "重叠观测", "关闭策略时可省略。", false},
                            {{"recordType", "profile.OverlapComparison.v1"}})};
  op.outputs = {{"primary", "Profile", "Primary", "所选原始轮廓。", true},
                {"secondary", "Profile", "Secondary", "所选原始轮廓。", true},
                withContract({"quality", "Record", "质量", "评估、分歧、选择与真实来源。", true},
                             {{"recordType", "gap.CameraGuard.v3"}})};
  auto maximum = floatParam("maxDeltaMm", "允许的高度差", .5, "绝对中位数等于上限时保留双路。");
  maximum.unit = "mm";
  maximum.min = 0;
  op.params = {maximum, enumParam("onDisagree", "分歧处置", "record",
                                  {{"off", "关闭"},
                                   {"record", "记录"},
                                   {"keepPrimary", "保留第一路"},
                                   {"keepSecondary", "保留第二路"},
                                   {"fail", "报错"}},
                                  "只在重叠评估超限时应用。")};
  op.validate = [](const ParamView& params, const std::set<std::string>& inputs) {
    if (params.choice("onDisagree") == "off" || inputs.count("observation"))
      return std::vector<Issue>{};
    return std::vector<Issue>{
        Issue::error("unconnected_input", "相机分歧策略需要轮廓重叠观测", {}, "observation")};
  };
  op.capabilities = {true, true, true};
  op.compute = selectCamera;
  registry.addOperator(std::move(op));
}
}
