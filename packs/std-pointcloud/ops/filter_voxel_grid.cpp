#include <cmath>
#include <unordered_map>

#include "ops.h"

namespace lyflow::ops {
namespace {

/// 三维体素下标。用完整的三个 int64 做键，不把它们打包进一个 uint64 ——
/// 打包会在超出范围时静默绕回、把远处的点和原点的点求质心（core/README.md「踩过的坑」）。
struct VoxelIndex {
  std::int64_t i, j, k;
  bool operator==(const VoxelIndex& o) const { return i == o.i && j == o.j && k == o.k; }
};

struct VoxelIndexHash {
  std::size_t operator()(const VoxelIndex& v) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::int64_t c : {v.i, v.j, v.k}) {
      h ^= static_cast<std::uint64_t>(c) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    }
    return static_cast<std::size_t>(h);
  }
};

/// 下标能不能装进 int64。装不下时 static_cast 是 UB，而「坐标是 1e30」这种
/// 数据是真会从别人的 pcd 里读出来的。
constexpr double kMaxVoxelIndex = 4.0e18;

inline bool voxelIndexOf(float x, float y, float z, const std::array<float, 3>& leaf,
                         VoxelIndex& out) {
  const double qx = std::floor(static_cast<double>(x) / leaf[0]);
  const double qy = std::floor(static_cast<double>(y) / leaf[1]);
  const double qz = std::floor(static_cast<double>(z) / leaf[2]);
  if (std::fabs(qx) > kMaxVoxelIndex || std::fabs(qy) > kMaxVoxelIndex ||
      std::fabs(qz) > kMaxVoxelIndex) {
    return false;
  }
  out = VoxelIndex{static_cast<std::int64_t>(qx), static_cast<std::int64_t>(qy),
                   static_cast<std::int64_t>(qz)};
  return true;
}

struct Voxel {
  double sx = 0, sy = 0, sz = 0;
  double si = 0;
  double snx = 0, sny = 0, snz = 0;
  double sr = 0, sg = 0, sb = 0;
  std::int32_t count = 0;
  std::int32_t firstIndex = -1;
};

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const PointCloud& in = *inputs.get("cloud").asCloud();
  const std::array<float, 3> leaf = params.vec3("leafSize");
  const auto minPts = static_cast<std::int32_t>(params.integer("minPointsPerVoxel"));
  const bool nearest = params.choice("representative") == "nearest";

  if (leaf[0] <= 0 || leaf[1] <= 0 || leaf[2] <= 0) {
    return Status::Error(Phase::Execute, "bad_param", "Leaf Size 的三个分量都必须大于 0",
                         "leafSize");
  }

  const std::size_t n = in.pointCount();
  std::unordered_map<VoxelIndex, std::size_t, VoxelIndexHash> lookup;
  std::vector<Voxel> voxels;  // 插入顺序 = 输出顺序，保证两次运行结果字节一致
  lookup.reserve(n / 4 + 16);

  Ticker ticker(ctx, n);
  for (std::size_t p = 0; p < n; ++p) {
    if (ticker.tick(p)) return Status::Ok();
    const float x = in.xyz[p * 3], y = in.xyz[p * 3 + 1], z = in.xyz[p * 3 + 2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;

    VoxelIndex key{};
    if (!voxelIndexOf(x, y, z, leaf, key)) {
      return Status::Error(Phase::Execute, "bad_param",
                           "点的坐标相对 Leaf Size 太大，体素下标溢出（点云单位对吗？"
                           "或者把 Leaf Size 调大）",
                           "leafSize");
    }
    auto it = lookup.find(key);
    if (it == lookup.end()) {
      it = lookup.emplace(key, voxels.size()).first;
      voxels.emplace_back();
      voxels.back().firstIndex = static_cast<std::int32_t>(p);
    }
    Voxel& v = voxels[it->second];
    v.sx += x;
    v.sy += y;
    v.sz += z;
    v.count += 1;
    if (in.hasIntensity()) v.si += in.intensity[p];
    if (in.hasNormals()) {
      v.snx += in.normals[p * 3];
      v.sny += in.normals[p * 3 + 1];
      v.snz += in.normals[p * 3 + 2];
    }
    if (in.hasRgb()) {
      v.sr += in.rgb[p * 3];
      v.sg += in.rgb[p * 3 + 1];
      v.sb += in.rgb[p * 3 + 2];
    }
  }

  if (nearest) {
    // 最近点模式：先算质心，再在体素内找离质心最近的**原始点**，
    // 最后一次性 select —— 通道搬运交给 select 一处负责（data.h 的约定）。
    std::vector<std::int32_t> best(voxels.size(), -1);
    std::vector<double> bestDist(voxels.size(), 1e300);
    // 第二遍同样要轮询取消：漏掉的话 cancellable=true 成了谎话，
    // 而抢占式运行同步等 join，前端会整整卡住这一趟。
    Ticker ticker2(ctx, n);
    for (std::size_t p = 0; p < n; ++p) {
      if (ticker2.tick(p)) return Status::Ok();
      const float x = in.xyz[p * 3], y = in.xyz[p * 3 + 1], z = in.xyz[p * 3 + 2];
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
      VoxelIndex key{};
      if (!voxelIndexOf(x, y, z, leaf, key)) continue;  // 第一趟已经报过错，走不到这里
      const std::size_t vi = lookup.at(key);
      const Voxel& v = voxels[vi];
      const double cx = v.sx / v.count, cy = v.sy / v.count, cz = v.sz / v.count;
      const double d = (x - cx) * (x - cx) + (y - cy) * (y - cy) + (z - cz) * (z - cz);
      if (d < bestDist[vi]) {
        bestDist[vi] = d;
        best[vi] = static_cast<std::int32_t>(p);
      }
    }
    std::vector<std::int32_t> keep;
    keep.reserve(voxels.size());
    for (std::size_t vi = 0; vi < voxels.size(); ++vi) {
      if (voxels[vi].count >= minPts && best[vi] >= 0) keep.push_back(best[vi]);
    }
    outputs.set("cloud", Data::cloud(in.select(keep)));
    return Status::Ok();
  }

  // 质心模式：产出的是原始数据里不存在的新点，所以只能自己搬通道。
  PointCloud out;
  out.xyz.reserve(voxels.size() * 3);
  if (in.hasIntensity()) out.intensity.reserve(voxels.size());
  if (in.hasNormals()) out.normals.reserve(voxels.size() * 3);
  if (in.hasRgb()) out.rgb.reserve(voxels.size() * 3);

  for (const Voxel& v : voxels) {
    if (v.count < minPts) continue;
    const double inv = 1.0 / v.count;
    out.push(static_cast<float>(v.sx * inv), static_cast<float>(v.sy * inv),
             static_cast<float>(v.sz * inv));
    if (in.hasIntensity()) out.intensity.push_back(static_cast<float>(v.si * inv));
    if (in.hasNormals()) {
      double nx = v.snx * inv, ny = v.sny * inv, nz = v.snz * inv;
      const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (len > 1e-9) { nx /= len; ny /= len; nz /= len; }
      out.normals.push_back(static_cast<float>(nx));
      out.normals.push_back(static_cast<float>(ny));
      out.normals.push_back(static_cast<float>(nz));
    }
    if (in.hasRgb()) {
      out.rgb.push_back(static_cast<std::uint8_t>(v.sr * inv));
      out.rgb.push_back(static_cast<std::uint8_t>(v.sg * inv));
      out.rgb.push_back(static_cast<std::uint8_t>(v.sb * inv));
    }
  }

  outputs.set("cloud", Data::cloud(std::move(out)));
  return Status::Ok();
}

}  // namespace

void registerFilterVoxelGrid(Registry& r) {
  OperatorDesc op;
  op.id = "filter.voxel_grid";
  op.version = "1.0.0";
  op.label = "体素网格";
  op.category = "过滤/降采样";
  op.keywords = {"downsample", "voxel", "grid", "降采样", "体素", "抽稀"};
  op.doc = "用体素栅格降采样，每个体素保留一个代表点。";

  op.inputs  = {Port{"cloud", "PointCloud", "Cloud", "待降采样的点云。", true}};
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "降采样后的点云。", true}};

  Param leaf;
  leaf.name = "leafSize";
  leaf.type = ParamType::Vec3f;
  leaf.label = "Leaf Size";
  leaf.doc = "体素边长。越大点越少。三个分量通常保持一致。";
  leaf.def = Value::vec({0.01, 0.01, 0.01});
  leaf.min = 0.0001;
  leaf.softMax = 0.1;
  leaf.max = 10.0;
  leaf.step = 0.001;
  leaf.unit = "m";

  Param minPts;
  minPts.name = "minPointsPerVoxel";
  minPts.type = ParamType::Int;
  minPts.label = "Min Points / Voxel";
  minPts.doc = "体素内点数少于此值则整个体素丢弃。用来顺手滤掉孤立噪点。";
  minPts.def = Value::integer(0);
  minPts.min = 0.0;
  minPts.advanced = true;

  Param mode;
  mode.name = "representative";
  mode.type = ParamType::Enum;
  mode.label = "Representative";
  mode.doc = "体素内用哪个点代表。质心更平滑，最近点保留原始测量值。";
  mode.def = Value::text("centroid");
  mode.options = {
      EnumOption{"centroid", "Centroid", "体素内所有点的质心，会产生原始数据中不存在的新点。"},
      EnumOption{"nearest",  "Nearest To Centroid", "离质心最近的原始点，保留真实测量值。"},
  };
  mode.advanced = true;

  op.params = {leaf, minPts, mode};
  op.capabilities = {/*cancellable=*/true, /*previewable=*/true, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
