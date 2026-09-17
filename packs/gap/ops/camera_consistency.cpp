// gap.camera_consistency：两台线扫在同一段轮廓上是不是看到了同一个面。
//
// 双头线扫的两台相机本该在重叠区给出同一条轮廓。天幕玻璃这类工况下它们有时不是：
// 某一台吃到玻璃下表面的二次反射（或标定漂了），于是合并云里出现两条相距若干毫米的
// 平行轮廓。下游的圆/直线拟合只看合并云，会把圆拟到错的那一条上 —— 罗石天幕 R2 实测，
// 正常帧两台相机的中位高度差是 0.02 mm，出问题的帧是 1.9~3.7 mm，读数因此虚高 2~6 mm，
// 而且拟合残差依然很小（0.013 mm），从质量指标上完全看不出来。
//
// 做法：在给定的 ROI 横向范围里按 sampleStep 采样，每个采样点各取两台相机的中位高度，
// 相减；取全部采样点的中位差作为判据。中位数而不是均值，是为了不被缝底那几个点带偏。
//
// box 原样透传，方便把这个节点串在 ROI → 裁剪 之间，不改动图的其余部分。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "gap_ops.h"

namespace lyflow::packs::gap {
namespace {

std::string mmText(double mm) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", mm);
  return buf;
}

double median(std::vector<double>* v) {
  const std::size_t n = v->size();
  std::sort(v->begin(), v->end());
  return (n % 2 == 1) ? (*v)[n / 2] : 0.5 * ((*v)[n / 2 - 1] + (*v)[n / 2]);
}

/// 一台相机在 [x-half, x+half] 这一竖条里的中位高度。没有点返回 false。
bool medianHeightAt(const lyflow::PointCloud& cloud, float x, float half, double* out) {
  std::vector<double> ys;
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    const float px = cloud.xyz[3 * i];
    const float py = cloud.xyz[3 * i + 1];
    if (!std::isfinite(px) || !std::isfinite(py)) continue;
    if (px > x - half && px < x + half) ys.push_back(py);
  }
  if (ys.empty()) return false;
  *out = median(&ys);
  return true;
}

Status cameraConsistency(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& ctx) {
  const lyflow::PointCloud& primary = *inputs.get("primary").asCloud();
  const lyflow::PointCloud& secondary = *inputs.get("secondary").asCloud();
  const lyflow::Box2D& box = *inputs.get("box").asBox2D();
  outputs.set("box", Data::box2d(box));

  const std::string mode = params.text("mode");
  const double maxDeltaMm = params.number("maxDeltaMm");
  const float step = mmToM(params.number("sampleStep"));
  const float half = mmToM(params.number("halfWindow"));
  const auto minSamples = static_cast<std::size_t>(params.integer("minSamples"));

  lyflow::Record quality;
  quality.type = "GapCameraConsistency";
  quality.data["mode"] = mode;
  quality.data["maxDeltaMm"] = maxDeltaMm;

  if (mode == "off") {
    quality.data["evaluated"] = false;
    quality.data["exceeded"] = false;
    outputs.set("quality", Data::record(std::move(quality)));
    return Status::Ok();
  }

  std::vector<double> diffs;
  if (step > 0) {
    for (float x = box.min[0]; x < box.max[0]; x += step) {
      double a = 0;
      double b = 0;
      if (medianHeightAt(primary, x, half, &a) && medianHeightAt(secondary, x, half, &b)) {
        diffs.push_back(mToMm(a - b));
      }
    }
  }
  quality.data["sampleCount"] = diffs.size();

  // 采样点太少说明这一段本来就只有一台相机看得见 —— 这不是「两台不一致」，
  // 判不了就不判，交给下游自己的点数判据。
  if (diffs.size() < minSamples) {
    quality.data["evaluated"] = false;
    quality.data["exceeded"] = false;
    quality.data["reason"] = "insufficient_overlap";
    ctx.log(LogLevel::Info, "camera_consistency: 重叠采样只有 " + std::to_string(diffs.size()) +
                                " 个，判不了一致性");
    outputs.set("quality", Data::record(std::move(quality)));
    return Status::Ok();
  }

  auto sorted = diffs;
  const double deltaMedian = median(&sorted);
  const double lo = sorted.front();
  const double hi = sorted.back();
  const bool exceeded = std::fabs(deltaMedian) > maxDeltaMm;

  quality.data["evaluated"] = true;
  quality.data["deltaMedianMm"] = deltaMedian;
  quality.data["deltaMinMm"] = lo;
  quality.data["deltaMaxMm"] = hi;
  quality.data["exceeded"] = exceeded;

  const std::string summary = "两台相机在这段 ROI 里的高度差中位 " + mmText(deltaMedian) +
                              " mm（范围 " + mmText(lo) + " ~ " + mmText(hi) + " mm，" +
                              std::to_string(diffs.size()) + " 个采样），上限 " +
                              mmText(maxDeltaMm) + " mm";
  ctx.log(exceeded ? LogLevel::Warn : LogLevel::Info, "camera_consistency: " + summary);
  outputs.set("quality", Data::record(std::move(quality)));

  if (exceeded && mode == "enforce") {
    return Status::Error(Phase::Execute, "camera_disagree",
                         "两台相机看到的不是同一个面：" + summary + "。合并云里有两条平行轮廓，"
                         "拟合结果不可信",
                         {}, "primary");
  }
  return Status::Ok();
}

Param numParam(const char* name, const char* label, double def, const char* unit,
               const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = unit;
  return p;
}

}  // namespace

void registerCameraConsistency(Registry& r) {
  OperatorDesc op;
  op.id = "gap.camera_consistency";
  op.version = "1.0.0";
  op.label = "双相机一致性";
  op.category = "间隙/质量";
  op.keywords = {"camera", "consistency", "双相机", "一致性", "二次反射", "门限"};
  op.doc =
      "检查两台线扫在同一段 ROI 上是不是看到了同一个面。按 sampleStep 在 ROI 的横向范围里采样，"
      "每个采样点取两台相机各自的中位高度相减，再取全部采样点的中位差。"
      "云要是**测量帧**的（y 是高度），也就是 gap.to_measurement_frame 之后的。\n"
      "天幕玻璃上实测：正常帧中位差 0.02 mm，某一台吃到玻璃下表面二次反射的帧是 1.9~3.7 mm，"
      "而这时合并云的拟合残差依然只有 0.013 mm —— 从拟合质量上看不出来，只能这样比。\n"
      "**比的是高度差，所以对局部斜率敏感**：ROI 落在陡坡上时，两台之间零点几个像素的横向差"
      "也会放大成零点几毫米的高度差。天幕 12 个点实测，正常帧的中位差从 0.015 mm（R5）到 "
      "0.40 mm（L1）差一个量级，**门限必须逐点定**，别指望一个数通用。"
      "定法：拿一批正常件量出这一点自己的分布，看 p95 与最大值之间有没有空档 —— 有空档才适合挂闸，"
      "门限放在空档里；分布连续的点挂了就是乱杀。\n"
      "box 原样透传，方便串在 ROI → 裁剪 之间。mode=enforce 时超限直接报 camera_disagree。";
  op.inputs = {
      Port{"primary", "PointCloud", "Primary", "第一台相机的云（测量帧）。", true},
      Port{"secondary", "PointCloud", "Secondary", "第二台相机的云（测量帧）。", true},
      Port{"box", "Box2D", "Box", "要检查的横向范围，通常就是间隙那一侧的业务 ROI。", true},
  };
  op.outputs = {
      Port{"box", "Box2D", "Box", "原样透传的 box。", true},
      withExample(Port{"quality", "Record", "Quality",
                       "GapCameraConsistency：中位/最小/最大高度差、采样数、是否超限。", true},
                  examples::cameraConsistency()),
  };

  Param mode;
  mode.name = "mode";
  mode.type = ParamType::Enum;
  mode.label = "Mode";
  mode.doc = "off 不算；shadow 只记录不拦；enforce 超限就让这一点测不出来。";
  mode.def = Value::text("shadow");
  mode.options = {EnumOption{"off", "Off", ""}, EnumOption{"shadow", "Shadow", ""},
                  EnumOption{"enforce", "Enforce", ""}};

  Param minSamples;
  minSamples.name = "minSamples";
  minSamples.type = ParamType::Int;
  minSamples.label = "Min Samples";
  minSamples.doc = "两台都有点的采样点少于这个数就不判 —— 那是遮挡，不是不一致。";
  minSamples.def = Value::integer(5);
  minSamples.advanced = true;

  op.params = {
      mode,
      numParam("maxDeltaMm", "Max Delta", 0.5, "mm", "中位高度差的上限。"),
      numParam("sampleStep", "Sample Step", 0.3, "mm", "沿 x 的采样步长。"),
      numParam("halfWindow", "Half Window", 0.15, "mm", "每个采样点取点的半宽。"),
      minSamples,
  };
  op.capabilities = {false, true, true};
  op.compute = &cameraConsistency;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
