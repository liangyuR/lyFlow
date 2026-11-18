#pragma once

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::test {

inline void registerCloudTestOp(Registry& registry) {
  if (registry.find("test.make_cloud"))
    return;
  OperatorDesc op;
  op.id = "test.make_cloud";
  op.version = "1.0.0";
  op.label = "Point Cloud Fixture";
  op.category = "Test";
  op.outputs = {{"cloud", "PointCloud", "Cloud", "确定性测试点列。", true}};

  Param count;
  count.name = "pointCount";
  count.label = "Point Count";
  count.type = ParamType::Int;
  count.def = Value::integer(100);
  count.min = 1;
  count.max = 20000000;
  Param seed;
  seed.name = "seed";
  seed.label = "Seed";
  seed.type = ParamType::Int;
  seed.def = Value::integer(1);
  Param intensity;
  intensity.name = "withIntensity";
  intensity.label = "With Intensity";
  intensity.type = ParamType::Bool;
  intensity.def = Value::boolean(true);
  op.params = {count, seed, intensity};

  op.capabilities = {true, true, true};
  op.compute = [](const Inputs&, const ParamView& params, Outputs& out, ExecContext& ctx) {
    const auto size = static_cast<std::size_t>(params.integer("pointCount"));
    const auto offset = static_cast<float>(params.integer("seed"));
    const bool withIntensity = params.flag("withIntensity");
    PointCloud cloud;
    cloud.xyz.reserve(size * 3);
    if (withIntensity)
      cloud.intensity.reserve(size);
    Ticker ticker(ctx, size);
    for (std::size_t i = 0; i < size; ++i) {
      if (ticker.tick(i))
        return Status::Ok();
      cloud.push(static_cast<float>(i % 1000), static_cast<float>(i / 1000),
                 offset + static_cast<float>(i % 13));
      if (withIntensity)
        cloud.intensity.push_back(static_cast<float>(i % 16));
    }
    out.set("cloud", Data::cloud(std::move(cloud)));
    return Status::Ok();
  };
  registry.addOperator(std::move(op));
}

} // namespace lyflow::test
