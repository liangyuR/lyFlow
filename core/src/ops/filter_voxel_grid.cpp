#include "ops.h"

namespace lyflow::ops {

void registerFilterVoxelGrid(Registry& r) {
  OperatorDesc op;
  op.id = "filter.voxel_grid";
  op.version = "1.0.0";
  op.label = "Voxel Grid";
  op.category = "Filter/Downsample";
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

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
