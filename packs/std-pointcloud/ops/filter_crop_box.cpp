#include <cmath>

#include "ops.h"

namespace lyflow::ops {
namespace {

/// 仿射变换的逆（假定最后一行是 0 0 0 1）。
/// 行主序，m[r*4+c]。返回 false 表示 3x3 部分退化（比如缩放为 0）。
bool invertAffine(const float m[16], float out[16]) {
  const double a = m[0], b = m[1], c = m[2];
  const double d = m[4], e = m[5], f = m[6];
  const double g = m[8], h = m[9], i = m[10];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-12) return false;
  const double inv = 1.0 / det;

  const double r[9] = {
      (e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
      (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
      (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv,
  };
  const double tx = m[3], ty = m[7], tz = m[11];
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) out[row * 4 + col] = static_cast<float>(r[row * 3 + col]);
  }
  out[3]  = static_cast<float>(-(r[0] * tx + r[1] * ty + r[2] * tz));
  out[7]  = static_cast<float>(-(r[3] * tx + r[4] * ty + r[5] * tz));
  out[11] = static_cast<float>(-(r[6] * tx + r[7] * ty + r[8] * tz));
  out[12] = out[13] = out[14] = 0.0f;
  out[15] = 1.0f;
  return true;
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const std::array<float, 3> lo = params.vec3("min");
  const std::array<float, 3> hi = params.vec3("max");
  const bool invert = params.flag("invert");

  for (int i = 0; i < 3; ++i) {
    if (lo[i] >= hi[i]) {
      return Status::Error(Phase::Execute, "bad_param",
                           std::string("Min 的第 ") + static_cast<char>('X' + i) +
                               " 分量必须小于 Max 的对应分量",
                           "min");
    }
  }

  // 可选姿态输入：盒子定义在该变换的局部系里，判定时把点变到局部系再比 AABB。
  // 比把 8 个角变到世界系算 OBB 简单，且对带缩放的非正交变换也成立。
  float toLocal[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  bool hasPose = false;
  if (inputs.has("pose")) {
    const Transform* t = inputs.get("pose").asTransform();
    if (t && !invertAffine(t->m, toLocal)) {
      return Status::Error(Phase::Execute, "bad_input", "输入的变换不可逆（旋转/缩放部分退化）",
                           {}, "pose");
    }
    hasPose = t != nullptr;
  }

  const std::size_t n = in.pointCount();
  std::vector<std::int32_t> keep;
  keep.reserve(n);
  Ticker ticker(ctx, n);
  for (std::size_t p = 0; p < n; ++p) {
    if (ticker.tick(p)) return Status::Ok();
    float x = in.xyz[p * 3], y = in.xyz[p * 3 + 1], z = in.xyz[p * 3 + 2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    if (hasPose) {
      const float lx = toLocal[0] * x + toLocal[1] * y + toLocal[2] * z + toLocal[3];
      const float ly = toLocal[4] * x + toLocal[5] * y + toLocal[6] * z + toLocal[7];
      const float lz = toLocal[8] * x + toLocal[9] * y + toLocal[10] * z + toLocal[11];
      x = lx; y = ly; z = lz;
    }
    const bool inside = x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] &&
                        z <= hi[2];
    if (inside != invert) keep.push_back(static_cast<std::int32_t>(p));
  }

  outputs.set("cloud", Data::cloud(in.select(keep)));
  return Status::Ok();
}

}  // namespace

void registerFilterCropBox(Registry& r) {
  OperatorDesc op;
  op.id = "filter.crop_box";
  op.version = "1.0.0";
  op.label = "裁剪框";
  op.category = "过滤/裁剪";
  op.keywords = {"crop", "box", "roi", "aabb", "裁剪", "包围盒", "感兴趣区域"};
  op.doc = "用一个长方体裁剪点云。接了 Pose 输入时，盒子定义在该变换的局部坐标系里。";

  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "待裁剪的点云。", true},
      Port{"pose",  "Transform",  "Pose",  "可选：盒子的姿态。不接则用世界坐标系。", false},
  };
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "盒内的点。", true}};

  Param lo;
  lo.name = "min";
  lo.type = ParamType::Vec3f;
  lo.label = "Min";
  lo.def = Value::vec({-1.0, -1.0, -1.0});
  lo.step = 0.01;
  lo.unit = "m";

  Param hi;
  hi.name = "max";
  hi.type = ParamType::Vec3f;
  hi.label = "Max";
  hi.def = Value::vec({1.0, 1.0, 1.0});
  hi.step = 0.01;
  hi.unit = "m";

  Param invert;
  invert.name = "invert";
  invert.type = ParamType::Bool;
  invert.label = "Invert";
  invert.doc = "反向：保留盒子外的点。";
  invert.def = Value::boolean(false);

  op.params = {lo, hi, invert};
  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
