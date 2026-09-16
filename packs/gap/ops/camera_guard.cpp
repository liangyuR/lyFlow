// gap.camera_guard：两台线扫看的是不是同一个面；不是的话，把不可信的那台挡在模型之前。
//
// 双头线扫的两台相机本该在重叠区给出同一条轮廓。天幕玻璃这类工况下它们有时不是：
// 某一台吃到玻璃下表面的二次反射（或标定漂了），两条轮廓相距若干毫米。
// 这时候**模型和测量一起被带偏** —— 模型的输入张量是 [2, 6, 1280]、一台一行，
// 污染的那一行会把 ROI 推偏；合并云里两套点交错，圆又拟到错的那条上。
// 罗石天幕 R2、09-15 的 332 帧实测：出问题的 12 帧读数虚高 2~6 mm（最高 13.3，标称 7.5），
// **而拟合残差只有 0.013 mm、内点也够**，从任何现有质量指标上都看不出来。
//
// 所以这道闸站在 n_load 之后、模型之前：两台一致就原样透传，下游完全无感；
// 不一致就按 onDisagree 处理 —— 把可信那台的云送到两个输出端口上，或者直接判这一帧测不了。
//
// **为什么「只用一台」是把它填进两个端口**：模型的张量形状 [2, ...] 是训练时定死的，
// 单行喂不进去。填两遍等于告诉模型「两个视角一致，就是这个视角」，实测 ROI 是好的。
// 代价是合并云里这一台的点算了两遍（只发生在判为分歧的帧上），比例类判据不受影响，
// 但 quality 里的 point_count 会翻倍。
//
// 判据是**两台重叠段上逐点高度差的绝对值中位数**，不需要 ROI —— 正是因为不需要，
// 它才能站在模型之前。R2 实测：正常帧中位 0.083 mm（最大 0.143），出问题的 12 帧
// 1.03~5.6 mm，中间空了 7 倍。比「在某个 ROI 里比」跨测点稳得多：后者落在陡坡上时，
// 两台之间零点几个像素的横向差就放大成零点几毫米，逐点门限能差一个量级。
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

/// 高度在 y 还是 z：传感器帧（n_load 之后）在 z，测量帧（to_measurement_frame 之后）在 y。
/// 自己认，省得多一个只会被填错的参数。
bool heightInZ(const lyflow::PointCloud& cloud) {
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    const float y = cloud.xyz[3 * i + 1];
    if (std::isfinite(y) && y != 0.0F) return false;
  }
  return true;
}

struct Profile {
  std::vector<float> x;
  std::vector<float> h;

  bool empty() const { return x.empty(); }
  float lo() const { return *std::min_element(x.begin(), x.end()); }
  float hi() const { return *std::max_element(x.begin(), x.end()); }
};

Profile profileOf(const lyflow::PointCloud& cloud) {
  const bool inZ = heightInZ(cloud);
  Profile out;
  out.x.reserve(cloud.pointCount());
  out.h.reserve(cloud.pointCount());
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    const float px = cloud.xyz[3 * i];
    const float ph = cloud.xyz[3 * i + (inZ ? 2 : 1)];
    if (!std::isfinite(px) || !std::isfinite(ph)) continue;
    out.x.push_back(px);
    out.h.push_back(ph);
  }
  return out;
}

/// 一片剖面在 [x-half, x+half] 这一竖条里的中位高度。没有点返回 false。
bool medianHeightAt(const Profile& profile, float x, float half, double* out) {
  std::vector<double> hs;
  for (std::size_t i = 0; i < profile.x.size(); ++i) {
    if (profile.x[i] > x - half && profile.x[i] < x + half) hs.push_back(profile.h[i]);
  }
  if (hs.empty()) return false;
  *out = median(&hs);
  return true;
}

Status cameraGuard(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                   ExecContext& ctx) {
  const lyflow::PointCloud& primary = *inputs.get("primary").asCloud();
  const lyflow::PointCloud& secondary = *inputs.get("secondary").asCloud();

  const std::string onDisagree = params.text("onDisagree");
  const double maxDeltaMm = params.number("maxDeltaMm");
  const float step = mmToM(params.number("sampleStep"));
  const float half = mmToM(params.number("halfWindow"));
  const auto minSamples = static_cast<std::size_t>(params.integer("minSamples"));

  lyflow::Record quality;
  quality.type = "GapCameraGuard";
  quality.data["onDisagree"] = onDisagree;
  quality.data["maxDeltaMm"] = maxDeltaMm;

  const auto emit = [&](const lyflow::PointCloud& a, const lyflow::PointCloud& b,
                        const char* taken) {
    outputs.set("primary", Data::cloud(a));
    outputs.set("secondary", Data::cloud(b));
    quality.data["taken"] = taken;
    outputs.set("quality", Data::record(std::move(quality)));
  };

  if (onDisagree == "off") {
    quality.data["evaluated"] = false;
    quality.data["exceeded"] = false;
    emit(primary, secondary, "both");
    return Status::Ok();
  }

  const Profile a = profileOf(primary);
  const Profile b = profileOf(secondary);
  std::vector<double> diffs;
  if (!a.empty() && !b.empty() && step > 0) {
    // 两台的公共 x 区间，整段采样 —— 不裁 ROI，这道闸才站得到模型前面。
    const float lo = std::max(a.lo(), b.lo());
    const float hi = std::min(a.hi(), b.hi());
    const lyflow::Box2D* box = inputs.has("box") ? inputs.get("box").asBox2D() : nullptr;
    const float from = box ? std::max(lo, box->min[0]) : lo;
    const float to = box ? std::min(hi, box->max[0]) : hi;
    for (float x = from; x < to; x += step) {
      double ha = 0;
      double hb = 0;
      if (medianHeightAt(a, x, half, &ha) && medianHeightAt(b, x, half, &hb)) {
        diffs.push_back(mToMm(ha - hb));
      }
    }
  }
  quality.data["sampleCount"] = diffs.size();

  // 重叠采样太少说明这一段本来就只有一台看得见 —— 那是遮挡，不是不一致，判不了就不判。
  if (diffs.size() < minSamples) {
    quality.data["evaluated"] = false;
    quality.data["exceeded"] = false;
    quality.data["reason"] = "insufficient_overlap";
    ctx.log(LogLevel::Info,
            "camera_guard: 重叠采样只有 " + std::to_string(diffs.size()) + " 个，判不了");
    emit(primary, secondary, "both");
    return Status::Ok();
  }

  std::vector<double> magnitude;
  magnitude.reserve(diffs.size());
  for (const double d : diffs) magnitude.push_back(std::fabs(d));
  const double deltaMedian = median(&magnitude);
  auto signedCopy = diffs;
  const double signedMedian = median(&signedCopy);  // + = primary 更深 / 更远离传感器
  const bool exceeded = deltaMedian > maxDeltaMm;

  quality.data["evaluated"] = true;
  quality.data["deltaMedianMm"] = deltaMedian;
  quality.data["signedMedianMm"] = signedMedian;
  quality.data["exceeded"] = exceeded;

  const std::string summary = "两台在重叠段上的高度差中位 " + mmText(deltaMedian) + " mm（带符号 " +
                              mmText(signedMedian) + "，" + std::to_string(diffs.size()) +
                              " 个采样），上限 " + mmText(maxDeltaMm) + " mm";
  if (!exceeded) {
    ctx.log(LogLevel::Info, "camera_guard: " + summary);
    emit(primary, secondary, "both");
    return Status::Ok();
  }

  ctx.log(LogLevel::Warn, "camera_guard: " + summary);
  if (onDisagree == "keepPrimary") {
    emit(primary, primary, "primary");
    return Status::Ok();
  }
  if (onDisagree == "keepSecondary") {
    emit(secondary, secondary, "secondary");
    return Status::Ok();
  }
  if (onDisagree == "record") {
    emit(primary, secondary, "both");
    return Status::Ok();
  }
  return Status::Error(Phase::Execute, "camera_disagree",
                       "两台相机看到的不是同一个面：" + summary +
                           "。模型的两行输入和合并云都会被带偏，结果不可信",
                       {}, "primary");
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

void registerCameraGuard(Registry& r) {
  OperatorDesc op;
  op.id = "gap.camera_guard";
  op.version = "1.0.0";
  op.label = "双相机闸";
  op.category = "间隙/质量";
  op.keywords = {"camera", "guard", "consistency", "双相机", "一致性", "二次反射"};
  op.doc =
      "比两台线扫在重叠段上的高度，判断它们看到的是不是同一个面；不是的话把不可信的那台"
      "挡在模型之前。**串在 gap.load_profile_pair 与下游之间**，两台一致时原样透传，"
      "下游完全无感。\n"
      "判据是重叠段上逐点高度差的**绝对值中位数**，不需要 ROI —— 正因为不需要，它才能站在"
      "模型前面。高度在 y 还是 z 自己认（传感器帧在 z，测量帧在 y），不用填。\n"
      "`onDisagree` 决定超限时怎么办：`fail` 判这一帧测不了；`keepPrimary` / `keepSecondary` "
      "把可信那台的云送到**两个**输出端口上；`record` 只记录、照常透传；`off` 连算都不算。\n"
      "**为什么「只用一台」要填两个端口**：模型输入张量是 [2, 6, 1280]、一台一行，形状是训练时"
      "定死的，单行喂不进去；填两遍等于告诉模型「两个视角一致，就是这个视角」。"
      "代价是合并云里这一台的点算了两遍，比例类判据不受影响，但 point_count 会翻倍。\n"
      "罗石天幕 R2 实测：正常帧中位差 0.083 mm（最大 0.143），二次反射的 12 帧 1.03~5.6 mm，"
      "中间空了 7 倍；而那 12 帧的拟合残差只有 0.013 mm，从别的质量指标上根本看不出来。";
  op.inputs = {
      Port{"primary", "PointCloud", "Primary", "第一台相机的云。", true},
      Port{"secondary", "PointCloud", "Secondary", "第二台相机的云。", true},
      Port{"box", "Box2D", "Box", "只比这个横向范围；不接就比整个重叠段。", false},
  };
  op.outputs = {
      Port{"primary", "PointCloud", "Primary", "下游该用的第一路。", true},
      Port{"secondary", "PointCloud", "Secondary", "下游该用的第二路。", true},
      Port{"quality", "Record", "Quality",
           "GapCameraGuard：高度差中位（绝对值与带符号）、采样数、是否超限、留了哪一台。", true},
  };

  Param onDisagree;
  onDisagree.name = "onDisagree";
  onDisagree.type = ParamType::Enum;
  onDisagree.label = "On Disagree";
  onDisagree.doc = "超限时怎么办。off 连算都不算，record 只记录。";
  onDisagree.def = Value::text("record");
  onDisagree.options = {EnumOption{"off", "Off", ""}, EnumOption{"record", "Record only", ""},
                        EnumOption{"keepPrimary", "Keep primary", ""},
                        EnumOption{"keepSecondary", "Keep secondary", ""},
                        EnumOption{"fail", "Fail", ""}};

  Param minSamples;
  minSamples.name = "minSamples";
  minSamples.type = ParamType::Int;
  minSamples.label = "Min Samples";
  minSamples.doc = "两台都有点的采样点少于这个数就不判 —— 那是遮挡，不是不一致。";
  minSamples.def = Value::integer(20);
  minSamples.advanced = true;

  op.params = {
      onDisagree,
      numParam("maxDeltaMm", "Max Delta", 0.5, "mm", "高度差中位的上限。"),
      numParam("sampleStep", "Sample Step", 0.5, "mm", "沿 x 的采样步长。"),
      numParam("halfWindow", "Half Window", 0.25, "mm", "每个采样点取点的半宽。"),
      minSamples,
  };
  op.capabilities = {false, true, true};
  op.compute = &cameraGuard;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
