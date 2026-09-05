#include <algorithm>
#include <numeric>
#include <random>

#include "ops.h"

namespace lyflow::ops {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const std::size_t n = in.pointCount();
  const bool byRatio = params.choice("mode") == "ratio";
  const auto seed = static_cast<std::uint32_t>(params.integer("seed"));

  std::size_t want = n;
  if (byRatio) {
    want = static_cast<std::size_t>(static_cast<double>(n) * params.number("ratio"));
  } else {
    want = static_cast<std::size_t>(params.integer("count"));
  }
  want = std::min(want, n);

  std::vector<std::int32_t> keep(n);
  std::iota(keep.begin(), keep.end(), 0);
  if (want < n) {
    // 固定种子 + 部分 Fisher-Yates：同样的输入与种子必须给出同一批点，
    // 否则调参时画面每次都在变，根本判断不出改动有没有效果。
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < want; ++i) {
      if (ctx.cancelled()) return Status::Ok();
      std::uniform_int_distribution<std::size_t> pick(i, n - 1);
      std::swap(keep[i], keep[pick(rng)]);
    }
    keep.resize(want);
    // 保持原始顺序：抽样不该顺便把点云打乱，下游看 diff 会疯掉
    std::sort(keep.begin(), keep.end());
  }

  outputs.set("cloud", Data::cloud(in.select(keep)));
  return Status::Ok();
}

}  // namespace

void registerFilterRandomSample(Registry& r) {
  OperatorDesc op;
  op.id = "filter.random_sample";
  op.version = "1.0.0";
  op.label = "Random Sample";
  op.category = "Filter/Downsample";
  op.keywords = {"random", "sample", "subsample", "随机", "抽样", "降采样"};
  op.doc = "随机抽取一部分点。种子固定，同输入同结果。";

  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "输入点云。", true}};
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "抽样后的点云。", true}};

  Param mode;
  mode.name = "mode";
  mode.type = ParamType::Enum;
  mode.label = "Mode";
  mode.def = Value::text("count");
  mode.options = {
      EnumOption{"count", "Target Count", "抽到固定点数。"},
      EnumOption{"ratio", "Ratio", "按比例抽。"},
  };

  Param count;
  count.name = "count";
  count.type = ParamType::Int;
  count.label = "Count";
  count.def = Value::integer(10000);
  count.min = 0.0;
  count.softMax = 200000.0;
  count.visibleWhen.param = "mode";
  count.visibleWhen.eq = Value::text("count");

  Param ratio;
  ratio.name = "ratio";
  ratio.type = ParamType::Float;
  ratio.label = "Ratio";
  ratio.def = Value::number(0.5);
  ratio.min = 0.0;
  ratio.max = 1.0;
  ratio.softMin = 0.0;
  ratio.softMax = 1.0;
  ratio.step = 0.01;
  ratio.visibleWhen.param = "mode";
  ratio.visibleWhen.eq = Value::text("ratio");

  Param seed;
  seed.name = "seed";
  seed.type = ParamType::Int;
  seed.label = "Seed";
  seed.def = Value::integer(1);
  seed.min = 0.0;
  seed.advanced = true;

  op.params = {mode, count, ratio, seed};
  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
