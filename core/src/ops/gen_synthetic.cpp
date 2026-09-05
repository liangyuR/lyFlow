#include <random>

#include "ops.h"

namespace lyflow::ops {
namespace {

// 一块地面 + 一个立方体 + 高斯噪声 + 离群点。形状是刻意选的：有平面、有离群点、有厚度。
// 存在的理由是让测试与演示不依赖二进制样例数据（仓库里不放 .pcd）。
Status compute(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const auto total = static_cast<std::size_t>(params.integer("pointCount"));
  const double noise = params.number("noise");
  const double outlierRatio = params.number("outlierRatio");
  const auto seed = static_cast<std::uint32_t>(params.integer("seed"));
  const bool withIntensity = params.flag("withIntensity");

  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
  std::uniform_real_distribution<float> wide(-1.5f, 1.5f);
  std::uniform_real_distribution<float> zeroToOne(0.0f, 1.0f);
  std::normal_distribution<float> gauss(0.0f, static_cast<float>(noise));

  const std::size_t outliers = static_cast<std::size_t>(static_cast<double>(total) * outlierRatio);
  const std::size_t inliers = total - outliers;
  const std::size_t planePoints = inliers * 6 / 10;

  PointCloud cloud;
  if (withIntensity) cloud.intensity.reserve(total);
  cloud.xyz.reserve(total * 3);

  Ticker ticker(ctx, total);
  for (std::size_t i = 0; i < total; ++i) {
    if (ticker.tick(i)) return Status::Ok();

    float x, y, z, intensity;
    if (i < planePoints) {
      x = unit(rng);
      y = unit(rng);
      z = gauss(rng);
      intensity = 0.25f + 0.1f * zeroToOne(rng);
    } else if (i < inliers) {
      // 立方体表面：先随机挑一个面，再在面上均匀取点
      const float half = 0.15f;
      const float cz = 0.25f;
      const int face = static_cast<int>(zeroToOne(rng) * 6.0f) % 6;
      const float a = unit(rng) * half;
      const float b = unit(rng) * half;
      switch (face) {
        case 0: x = half;  y = a;     z = cz + b;    break;
        case 1: x = -half; y = a;     z = cz + b;    break;
        case 2: x = a;     y = half;  z = cz + b;    break;
        case 3: x = a;     y = -half; z = cz + b;    break;
        case 4: x = a;     y = b;     z = cz + half; break;
        default: x = a;    y = b;     z = cz - half; break;
      }
      x += gauss(rng);
      y += gauss(rng);
      z += gauss(rng);
      intensity = 0.7f + 0.1f * zeroToOne(rng);
    } else {
      x = wide(rng);
      y = wide(rng);
      z = wide(rng) * 0.5f + 0.4f;
      intensity = zeroToOne(rng);
    }

    cloud.push(x, y, z);
    if (withIntensity) cloud.intensity.push_back(intensity);
  }

  outputs.set("cloud", Data::cloud(std::move(cloud)));
  return Status::Ok();
}

}  // namespace

void registerGenSynthetic(Registry& r) {
  OperatorDesc op;
  op.id = "gen.synthetic";
  op.version = "1.0.0";
  op.label = "Synthetic Cloud";
  op.category = "Generate";
  op.keywords = {"synthetic", "test", "demo", "sample", "生成", "测试", "合成"};
  op.doc = "生成一片测试点云：地面 + 立方体 + 高斯噪声 + 离群点。不依赖任何数据文件。";

  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "生成的点云。", true}};

  Param count;
  count.name = "pointCount";
  count.type = ParamType::Int;
  count.label = "Point Count";
  count.def = Value::integer(40000);
  count.min = 1.0;
  count.max = 20000000.0;
  count.softMax = 500000.0;

  Param noise;
  noise.name = "noise";
  noise.type = ParamType::Float;
  noise.label = "Noise σ";
  noise.doc = "高斯噪声标准差。";
  noise.def = Value::number(0.003);
  noise.min = 0.0;
  noise.max = 1.0;
  noise.softMax = 0.05;
  noise.step = 0.001;
  noise.unit = "m";

  Param outlier;
  outlier.name = "outlierRatio";
  outlier.type = ParamType::Float;
  outlier.label = "Outlier Ratio";
  outlier.doc = "离群点占比。给离群点滤波算子准备的靶子。";
  outlier.def = Value::number(0.02);
  outlier.min = 0.0;
  outlier.max = 1.0;
  outlier.softMax = 0.2;
  outlier.step = 0.005;

  Param seed;
  seed.name = "seed";
  seed.type = ParamType::Int;
  seed.label = "Seed";
  seed.doc = "同一个种子永远生成同一片点云 —— 测试要能复现。";
  seed.def = Value::integer(1);
  seed.min = 0.0;

  Param withIntensity;
  withIntensity.name = "withIntensity";
  withIntensity.type = ParamType::Bool;
  withIntensity.label = "With Intensity";
  withIntensity.def = Value::boolean(true);
  withIntensity.advanced = true;

  op.params = {count, noise, outlier, seed, withIntensity};
  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
