// 整体 ROI、裁剪、业务 ROI、选点。坐标一律是米，参数一律是毫米。
#include <algorithm>
#include <cmath>
#include <optional>

#include "gap_detection/GapUtils.hpp"
#include "gap_ops.h"

namespace lyflow::packs::gap {
namespace {

/// 逐坐标中位数，非有限点先剔除。复刻 Alignment.cpp 匿名命名空间里的
/// robustCloudCenter —— 它没有导出，只能照抄这二十行。
std::optional<Eigen::Vector2f> robustCloudCenter(const lyflow::PointCloud& cloud) {
  std::vector<float> xs;
  std::vector<float> ys;
  const std::size_t n = cloud.pointCount();
  xs.reserve(n);
  ys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float x = cloud.xyz[i * 3];
    const float y = cloud.xyz[i * 3 + 1];
    if (!std::isfinite(x) || !std::isfinite(y)) continue;
    xs.push_back(x);
    ys.push_back(y);
  }
  if (xs.empty()) return std::nullopt;

  const auto median = [](std::vector<float>* values) {
    const auto middle = values->begin() + static_cast<std::ptrdiff_t>(values->size() / 2);
    std::nth_element(values->begin(), middle, values->end());
    const float upper = *middle;
    if (values->size() % 2 != 0) return upper;
    const auto lower = std::max_element(values->begin(), middle);
    return (*lower + upper) * 0.5F;
  };
  return Eigen::Vector2f{median(&xs), median(&ys)};
}

Status overallRoi(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                  ExecContext& ctx) {
  const std::array<float, 4> roi = params.vec4("roi");
  const std::string mode = params.choice("mode");
  const std::string camera = params.choice("usingCamera");

  std::array<double, 4> resolved{roi[0], roi[1], roi[2], roi[3]};
  if (mode == "auto_center") {
    const auto left = robustCloudCenter(*inputs.get("primary").asCloud());
    const auto right = robustCloudCenter(*inputs.get("secondary").asCloud());
    std::optional<Eigen::Vector2f> center;
    if (camera == "Left") {
      center = left;
    } else if (camera == "Right") {
      center = right;
    } else if (left && right) {
      center = (*left + *right) * 0.5F;
    } else {
      center = left ? left : right;
    }
    if (center) {
      const double halfWidth = (roi[2] - roi[0]) * 0.5;
      const double halfHeight = (roi[3] - roi[1]) * 0.5;
      const double cx = static_cast<double>(center->x()) * kScale;
      const double cy = static_cast<double>(center->y()) * kScale;
      resolved = {cx - halfWidth, cy - halfHeight, cx + halfWidth, cy + halfHeight};
      ctx.log(LogLevel::Info, "auto_center: 中心 (" + std::to_string(cx) + ", " +
                                  std::to_string(cy) + ") mm");
    }
  }
  outputs.set("box", Data::box2d(boxFromMm(resolved[0], resolved[1], resolved[2], resolved[3])));
  return Status::Ok();
}

/// Record 里那四个 ROI（毫米）。缺字段时返回 false。
bool readRois(const nlohmann::json& data, std::array<std::array<double, 4>, 4>& out) {
  static const char* kKeys[4] = {"flushBase", "gapLeft", "flushRef", "gapRight"};
  if (!data.contains("rois")) return false;
  const auto& rois = data["rois"];
  for (int i = 0; i < 4; ++i) {
    if (!rois.contains(kKeys[i])) return false;
    const auto& v = rois[kKeys[i]];
    if (!v.is_array() || v.size() != 4) return false;
    for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
        v[static_cast<std::size_t>(j)].get<double>();
  }
  return true;
}

Status businessRois(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext&) {
  const lyflow::Record* record = inputs.get("alignment").asRecord();
  if (!record || record->type != "GapAlignment") {
    return Status::Error(Phase::Execute, "bad_input", "输入不是 GapAlignment", {}, "alignment");
  }
  std::array<std::array<double, 4>, 4> roiMm{};
  if (!readRois(record->data, roiMm)) {
    return Status::Error(Phase::Execute, "bad_input", "对齐结果里没有四个业务 ROI", {},
                         "alignment");
  }
  // base_side 互换：索引 0 与 2 换位，之后 0/1 归左变换、2/3 归右变换（G8）。
  const bool baseRight = params.choice("baseSide") == "right";
  if (baseRight) std::swap(roiMm[0], roiMm[2]);

  const Eigen::Matrix3f tLeft = transformFromJson(record->data["left"]["transform"]);
  const Eigen::Matrix3f tRight = transformFromJson(record->data["right"]["transform"]);

  // 端口按**语义**给：base_side=right 时基准面那一格是槽 2，参考面是槽 0。
  // 原算法在互换 ROI 之后又把两片云换了回来，等价于这里换端口。
  const char* kPorts[4] = {"flushBase", "gapLeft", "flushRef", "gapRight"};
  if (baseRight) {
    kPorts[0] = "flushRef";
    kPorts[2] = "flushBase";
  }
  for (int i = 0; i < 4; ++i) {
    const auto& r = roiMm[static_cast<std::size_t>(i)];
    // rois_[i] << r[0], r[2], r[1], r[3] —— col(0) 是 min 角，col(1) 是 max 角
    Eigen::Vector2f lo(mmToMRoi(r[0]), mmToMRoi(r[1]));
    Eigen::Vector2f hi(mmToMRoi(r[2]), mmToMRoi(r[3]));
    const Eigen::Matrix3f& t = i < 2 ? tLeft : tRight;
    // 只变换对角两个角点，之后仍按轴对齐解释 —— 这是原算法的行为，不要「修正」（G8）
    const auto apply = [&t](const Eigen::Vector2f& p) {
      const Eigen::Vector3f h(p.x(), p.y(), 1.0F);
      const Eigen::Vector3f q = t * h;
      return Eigen::Vector2f(q.x(), q.y());
    };
    lo = apply(lo);
    hi = apply(hi);
    lyflow::Box2D box;
    box.min[0] = lo.x();
    box.min[1] = lo.y();
    box.max[0] = hi.x();
    box.max[1] = hi.y();
    outputs.set(kPorts[i], Data::box2d(box));
  }
  return Status::Ok();
}

Status selectedPoint(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  const lyflow::PointCloud& in = *inputs.get("cloud").asCloud();
  const lyflow::Box2D& box = *inputs.get("box").asBox2D();
  if (in.pointCount() == 0) {
    return Status::Error(Phase::Execute, "bad_input", "点云是空的", {}, "cloud");
  }
  // 离 ROI 的 min 角最近的点，取自**整片云**而不是裁剪结果（§3.6）
  const GapCloud cloud = toPcl(in);
  Eigen::Vector2f nearest;
  ::detection::utils::pointCloudDistance(cloud, Eigen::Vector2f(box.min[0], box.min[1]), &nearest);
  lyflow::Point2D p;
  p.p[0] = nearest.x();
  p.p[1] = nearest.y();
  outputs.set("point", Data::point2d(p));
  return Status::Ok();
}

Status nearestToLine(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  const lyflow::PointCloud& in = *inputs.get("cloud").asCloud();
  const lyflow::Line2D& line = *inputs.get("line").asLine2D();
  if (in.pointCount() == 0) {
    return Status::Error(Phase::Execute, "bad_input", "点云是空的", {}, "cloud");
  }
  // lineCloudDistance 的选点判据：到直线的垂距最小的那个点。法线不归一化方向无所谓，
  // 但要和原实现一样先归一化，比较的量级才一致。
  Eigen::Vector2f n(line.dir[1], -line.dir[0]);
  n.normalize();
  const float px = line.point[0];
  const float py = line.point[1];
  std::size_t best = 0;
  double bestDist = std::numeric_limits<double>::max();
  for (std::size_t i = 0; i < in.pointCount(); ++i) {
    const double d = std::fabs(static_cast<double>(n[0]) * (in.xyz[i * 3] - px) +
                               static_cast<double>(n[1]) * (in.xyz[i * 3 + 1] - py));
    if (bestDist > d) {
      bestDist = d;
      best = i;
    }
  }
  lyflow::Point2D p;
  p.p[0] = in.xyz[best * 3];
  p.p[1] = in.xyz[best * 3 + 1];
  outputs.set("point", Data::point2d(p));
  return Status::Ok();
}


/// 方向基准窗：把一个锚框沿 side 的方向推出去一条长窗，高度以锚框的 y 范围为中心撑开。
/// 之所以不是「把锚框加宽」：基准面常常是一道很窄的台肩，它和外面那张长面之间有台阶，
/// 加宽会让拟合横跨台阶；这里要的是**另一张面**，所以窗口整个挪出去。
Status datumWindow(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const lyflow::Box2D& anchor = *inputs.get("anchor").asBox2D();
  // 高度单独找一个锚：x 要贴着缝（模型对缝的定位最稳），y 要贴着基准面。
  const lyflow::Box2D& heightAnchor =
      inputs.has("heightAnchor") ? *inputs.get("heightAnchor").asBox2D() : anchor;
  const double start = mmToM(params.number("startMm"));
  const double length = mmToM(params.number("lengthMm"));
  const double height = mmToM(params.number("heightMm"));
  if (!(length > 0)) {
    return Status::Error(Phase::Execute, "bad_param", "窗口长度必须大于 0", "lengthMm");
  }
  const bool toLeft = params.choice("side") == "left";
  lyflow::Box2D box;
  if (toLeft) {
    box.max[0] = static_cast<float>(anchor.min[0] - start);
    box.min[0] = static_cast<float>(box.max[0] - length);
  } else {
    box.min[0] = static_cast<float>(anchor.max[0] + start);
    box.max[0] = static_cast<float>(box.min[0] + length);
  }
  box.min[1] = static_cast<float>(heightAnchor.min[1] - height);
  box.max[1] = static_cast<float>(heightAnchor.max[1] + height);
  outputs.set("box", Data::box2d(box));
  return Status::Ok();
}

Param numMm(const char* name, const char* label, double def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = "mm";
  p.min = 0.0;
  return p;
}

Param vec4Mm(const char* name, const char* label, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Vec4f;
  p.label = label;
  p.doc = doc;
  p.def = Value::vec({-1.0, -1.0, 1.0, 1.0});
  p.unit = "mm";
  p.componentLabels = {"X Min", "Y Min", "X Max", "Y Max"};
  return p;
}

}  // namespace

void registerDatumWindow(Registry& r) {
  OperatorDesc op;
  op.id = "gap.datum_window";
  op.version = "1.0.0";
  op.label = "方向基准窗";
  op.category = "间隙/预处理";
  op.keywords = {"roi", "datum", "方向", "基准", "长面"};
  op.doc =
      "由一个锚框推出一条长窗，用来在**旁边那张长面**上拟一条方向基准线。\n"
      "基准面是一道很窄的台肩时（天幕 L4 只有 2 mm），它自己拟出来的方向基本是噪声；"
      "而台肩外面那张面往往有十几毫米长、几百个点，方向稳得多。两张面之间有固定的相对"
      "倾角，量一次写进 dirNominalDeg 就行 —— 见 gap.fit_line 的方向约束。";
  op.preconditions = {
      "窗口整个挪到锚框外面，不含锚框本身 —— 台肩与长面之间通常有台阶，混在一起拟就是"
      "横跨台阶。startMm 是留给台阶过渡带的让开量。",
      "假定窗口范围内只有那一张面。长度给过头会吃进别的特征，先量一批帧的内点率再定。",
      "窗口恒为轴对齐，位置完全由锚框决定 —— 锚框跑偏，窗口就跟着跑到没有点的地方，"
      "而下游的拟合不一定会失败，可能只是拟出一条没意义的线。给拟合配上最少内点数。",
      "框恒为轴对齐；零件姿态转得厉害时 heightMm 要跟着放宽，否则面会跑出窗口。",
  };
  op.inputs = {
      Port{"anchor", "Box2D", "Anchor",
           "定 x 的锚框。取模型定位最稳的那个 —— 通常是缝的 ROI，而不是基准面 ROI："
           "很窄的基准面框偶尔会整个跑偏，拿它当 x 锚，窗口会跟着飞出去。", true},
      Port{"heightAnchor", "Box2D", "Height Anchor",
           "定 y 的锚框，不接就用 anchor。通常接基准面 ROI —— 长面就在它上下几毫米内。",
           false},
  };
  op.outputs = {Port{"box", "Box2D", "Box", "推出去的长窗。", true}};

  Param side;
  side.name = "side";
  side.type = ParamType::Enum;
  side.label = "Side";
  side.doc = "长面在锚框的哪一侧。取背离缝隙的那一侧。";
  side.def = Value::text("left");
  side.options = {EnumOption{"left", "Left", "锚框左边。"}, EnumOption{"right", "Right", "锚框右边。"}};

  op.params = {
      side,
      numMm("startMm", "Start", 0.0, "窗口离锚框那条边的让开量，用来躲开台阶的过渡带。"),
      numMm("lengthMm", "Length", 13.0, "窗口沿 x 的长度。"),
      numMm("heightMm", "Height", 2.5, "以锚框的 y 范围为中心，上下各撑开多少。"),
  };
  op.capabilities = {false, true, true};
  op.compute = &datumWindow;
  r.addOperator(std::move(op));
}

void registerOverallRoi(Registry& r) {
  OperatorDesc op;
  op.id = "gap.overall_roi";
  op.version = "1.0.0";
  op.label = "整体 ROI";
  op.category = "间隙/预处理";
  op.keywords = {"roi", "auto center", "整体", "裁剪框"};
  op.doc =
      "整体 ROI 框。auto_center 保留配置的宽高，中心跟着两片云各自的稳健中心"
      "（逐坐标中位数）的中点走。";
  op.preconditions = {
      "auto_center 假定两片云的稳健中心（逐坐标中位数）落在待测特征附近；视野里有大片背"
      "景或另一件零件时中心会被拖走。",
      "框恒为轴对齐，宽高来自配置，不随零件姿态旋转。",
  };
  op.inputs = {
      Port{"primary", "PointCloud", "Primary", "测量帧的 Master 云。", true},
      Port{"secondary", "PointCloud", "Secondary", "测量帧的 Slave 云。", true},
  };
  op.outputs = {Port{"box", "Box2D", "Box", "整体 ROI。", true}};

  Param mode;
  mode.name = "mode";
  mode.type = ParamType::Enum;
  mode.label = "Mode";
  mode.def = Value::text("fixed");
  mode.options = {EnumOption{"fixed", "Fixed", "直接用配置的框。"},
                  EnumOption{"auto_center", "Auto Center", "宽高不变，中心跟着云走。"}};

  Param camera;
  camera.name = "usingCamera";
  camera.type = ParamType::Enum;
  camera.label = "Using Camera";
  camera.doc = "auto_center 用哪几片云求中心。";
  camera.def = Value::text("Both");
  camera.options = {EnumOption{"Both", "Both", "两片云中心的中点。"},
                    EnumOption{"Left", "Left", "只用 primary。"},
                    EnumOption{"Right", "Right", "只用 secondary。"}};
  camera.visibleWhen.param = "mode";
  camera.visibleWhen.eq = Value::text("auto_center");

  op.params = {vec4Mm("roi", "ROI", "配置里的整体 ROI，毫米。"), mode, camera};
  op.capabilities = {false, true, true};
  op.compute = &overallRoi;
  r.addOperator(std::move(op));
}

void registerBusinessRois(Registry& r) {
  OperatorDesc op;
  op.id = "gap.business_rois";
  op.version = "1.0.0";
  op.label = "业务 ROI";
  op.category = "间隙/配准";
  op.keywords = {"roi", "business", "业务框"};
  op.doc =
      "把选中模板的四个业务 ROI 按 ICP 变换搬到当前样本上。"
      "只变换对角两个角点，之后仍按轴对齐解释 —— 这是原算法的行为（G8）。";
  op.preconditions = {
      "假定四个业务 ROI 是模板坐标系里的常数，样本与模板之间的差异能被一次刚体变换吃掉。",
      "只变换对角两个角点、之后仍按轴对齐解释，所以模板转角明显时框会被拉大或缩小 —— "
      "这是原算法行为，不是可以「修正」的。",
  };
  op.inputs = {withContract(Port{"alignment", "Record", "Alignment", "GapAlignment。", true},
                            {{"recordType", "GapAlignment"}})};
  op.outputs = {
      Port{"flushBase", "Box2D", "Flush Base", "段差基准面 ROI。", true},
      Port{"gapLeft", "Box2D", "Gap Left", "间隙左侧 ROI。", true},
      Port{"flushRef", "Box2D", "Flush Ref", "段差参考面 ROI。", true},
      Port{"gapRight", "Box2D", "Gap Right", "间隙右侧 ROI。", true},
  };

  Param baseSide;
  baseSide.name = "baseSide";
  baseSide.type = ParamType::Enum;
  baseSide.label = "Base Side";
  baseSide.doc = "基准面在左还是右。right 时 flush_base 与 flush_ref 互换。";
  baseSide.def = Value::text("left");
  baseSide.options = {EnumOption{"left", "Left", ""}, EnumOption{"right", "Right", ""}};
  op.params = {baseSide};
  op.capabilities = {false, true, true};
  op.compute = &businessRois;
  r.addOperator(std::move(op));
}

void registerSelectedPoint(Registry& r) {
  OperatorDesc op;
  op.id = "gap.selected_point";
  op.version = "1.0.0";
  op.label = "选点";
  op.category = "间隙/拟合";
  op.keywords = {"selected point", "选点"};
  op.doc = "取离 ROI 的 min 角最近的点。注意取自**整片云**，不裁 ROI（§3.6）。";
  op.preconditions = {
      "在**整片输入云**上选离 ROI min 角最近的点，不裁 ROI：框外更近的点照样会被选中。",
      "ROI 只用到 min 角，框的大小不影响结果。",
  };
  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "整片云（合并后的那一份）。", true},
      Port{"box", "Box2D", "Box", "业务 ROI，只用它的 min 角。", true},
  };
  op.outputs = {Port{"point", "Point2D", "Point", "最近的那个点。", true}};
  op.capabilities = {false, true, true};
  op.compute = &selectedPoint;
  r.addOperator(std::move(op));
}

void registerNearestToLine(Registry& r) {
  OperatorDesc op;
  op.id = "gap.nearest_to_line";
  op.version = "1.0.0";
  op.label = "最近点到直线";
  op.category = "间隙/拟合";
  op.keywords = {"nearest point", "最近点"};
  op.doc =
      "取离给定直线垂距最小的那个云点（复刻 lineCloudDistance 的选点），"
      "对应配置里的 ref_type: nearest point。";
  op.preconditions = {
      "判据是到直线的垂距最小，不要求点落在线段两端之间；输入云必须先按业务 ROI 裁过，"
      "否则远处同样贴线的点会被选中。",
      "只比垂距，垂距相同时取点序靠前的那个（点序就是文件里的槽序）。",
  };
  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "已经按业务 ROI 裁过的点云。", true},
      Port{"line", "Line2D", "Line", "基准线。", true},
  };
  op.outputs = {Port{"point", "Point2D", "Point", "垂距最小的那个云点。", true}};
  op.capabilities = {false, true, true};
  op.compute = &nearestToLine;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
