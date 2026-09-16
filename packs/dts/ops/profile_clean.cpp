#include <cmath>
#include <vector>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const PointCloud& in = *inputs.get("profile").asCloud();
  const int width = static_cast<int>(params.integer("smoothPoints"));
  const double minIntensity = params.number("minIntensity");
  const std::size_t n = in.pointCount();
  if (n == 0) return Status::Error(Phase::Execute, "bad_input", "空轮廓", {}, "profile");

  std::vector<float> z(n);
  const bool hasIntensity = in.hasIntensity();
  for (std::size_t i = 0; i < n; ++i) {
    const float zi = in.xyz[3 * i + 2];
    const bool valid = std::isfinite(zi) && (!hasIntensity || in.intensity[i] >= minIntensity);
    z[i] = valid ? zi : std::numeric_limits<float>::quiet_NaN();
  }
  const std::vector<float> zs = movingMedian(z, width);

  PointCloud out;
  out.reserve(n);
  std::vector<float> intensity;
  intensity.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (!std::isfinite(zs[i])) continue;
    out.push(in.xyz[3 * i], 0.0f, zs[i]);
    intensity.push_back(hasIntensity ? in.intensity[i] : 1.0f);
  }
  if (out.pointCount() < 16) {
    return Status::Error(Phase::Execute, "bad_input",
                         "有效点太少（" + std::to_string(out.pointCount()) + "）", {}, "profile");
  }
  out.intensity = std::move(intensity);
  outputs.set("clean", Data::cloud(std::move(out)));
  return Status::Ok();
}

}  // namespace

void registerProfileClean(Registry& r) {
  OperatorDesc op;
  op.id = "dts.profile_clean";
  op.version = "1.0.0";
  op.label = "轮廓清理";
  op.category = "DTS/预处理";
  op.keywords = {"clean", "median", "smooth", "清理", "中值", "平滑"};
  op.doc =
      "丢掉无效点（亮度低于阈值或 z 非有限），其余点做一次忽略无效邻居的滑动中值。\n"
      "**保持列序**：切片靠相邻列判断，按 x 排序会把重叠面交错起来。";
  op.inputs = {Port{"profile", "PointCloud", "Profile", "原始轮廓。", true}};
  op.outputs = {Port{"clean", "PointCloud", "Clean", "去掉无效点、已平滑的轮廓，列序不变。", true}};

  Param smooth;
  smooth.name = "smoothPoints";
  smooth.type = ParamType::Int;
  smooth.label = "中值窗宽";
  smooth.doc = "滑动中值的点数，取奇数。1 = 不平滑。";
  smooth.def = Value::integer(5);
  smooth.min = 1.0;
  smooth.softMax = 31.0;

  Param minI;
  minI.name = "minIntensity";
  minI.type = ParamType::Float;
  minI.label = "最低亮度";
  minI.doc = "低于它的点当无效丢掉。DP2240 的无效点亮度就是 0。输入没有亮度通道时不生效。";
  minI.def = Value::number(0.5);
  minI.min = 0.0;

  op.params = {smooth, minI};
  op.capabilities = {false, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::dts
