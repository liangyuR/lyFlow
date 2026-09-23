// 两种 Bundle 的声明与读写、方向推导、Step（m8-plan L4 / L6 / L7）。
#include <cmath>
#include <stdexcept>

#include "gap_fine.h"

namespace lyflow::packs::gap {

// ------------------------------------------------------------------ Bundle 声明

void registerBundles(Registry& r) {
  r.addBundle(BundleDesc{
      kScanPairKind,
      "剖面对",
      "一次采集的两片剖面与它们的合并云，测量帧、米。gap.read_scan 出的是原始的一对"
      "（primary / secondary 保留 1280 个槽，NaN 槽还在；merged 只含有限点、已按需去噪）；"
      "gap.locate_template / gap.locate_model 出的是定位之后裁过的那一对，下游量测用它。",
      "",
      {BundleField{"primary", "PointCloud", "Master（L0）那一片。"},
       BundleField{"secondary", "PointCloud", "Slave（R1）那一片。"},
       BundleField{"merged", "PointCloud", "secondary 在前、primary 在后的合并云。"}}});
  r.addBundle(BundleDesc{
      kRoiSetKind,
      "角色框",
      "定位给出的四个业务框，按角色命名：datum 基准面、target 参考面、seamLeft / seamRight "
      "缝的左右两侧。「靠缝那一端」「基准件在哪一侧」都由它们推出，下游不填任何左右。",
      "",
      {BundleField{"datum", "Box2D", "段差基准面的框。"},
       BundleField{"target", "Box2D", "段差参考面（被测面）的框。"},
       BundleField{"seamLeft", "Box2D", "缝左侧的框（左圆）。"},
       BundleField{"seamRight", "Box2D", "缝右侧的框（右圆）。"},
       BundleField{"info", "Record",
                   "GapRoiInfo：datumSide（left | right，由框推出）、source（template | model）、"
                   "alignment（选中的 GapAlignment，模型路径为 null）、overallMm（整体框或"
                   "跟随裁剪窗，毫米）、cropStatus（GapRollCrop，模板路径为 null）。"}}});
}

// ------------------------------------------------------------------ ScanPair

Data scanPairOf(Data primary, Data secondary, Data merged) {
  Bundle b(kScanPairKind);
  b.set("primary", std::move(primary));
  b.set("secondary", std::move(secondary));
  b.set("merged", std::move(merged));
  return Data::bundle(std::move(b));
}

const Data& ScanView::pick(const std::string& which) const {
  if (which == "primary") return *primary;
  if (which == "secondary") return *secondary;
  return *merged;
}

bool readScanPair(const Data& d, ScanView* out) {
  const Bundle* b = d.asBundle();
  if (!b || b->kind != kScanPairKind) return false;
  out->primary = b->field("primary");
  out->secondary = b->field("secondary");
  out->merged = b->field("merged");
  return out->primary && out->secondary && out->merged && out->primary->asCloud() &&
         out->secondary->asCloud() && out->merged->asCloud();
}

// ------------------------------------------------------------------ RoiSet

const Data& RoiView::role(const std::string& which) const {
  return which == "target" ? *target : *datum;
}

bool RoiView::datumRight() const {
  return info && info->data.value("datumSide", std::string("left")) == "right";
}

bool readRoiSet(const Data& d, RoiView* out) {
  const Bundle* b = d.asBundle();
  if (!b || b->kind != kRoiSetKind) return false;
  out->datum = b->field("datum");
  out->target = b->field("target");
  out->seamLeft = b->field("seamLeft");
  out->seamRight = b->field("seamRight");
  const Data* info = b->field("info");
  out->info = info ? info->asRecord() : nullptr;
  for (const Data* box : {out->datum, out->target, out->seamLeft, out->seamRight}) {
    if (!box || !box->asBox2D()) return false;
  }
  return out->info != nullptr;
}

std::array<double, 4> boxToMm(const lyflow::Box2D& box) {
  return {mToMm(box.min[0]), mToMm(box.min[1]), mToMm(box.max[0]), mToMm(box.max[1])};
}

nlohmann::json roiInfo(bool datumRight, const std::string& source, const nlohmann::json& alignment,
                       const lyflow::Box2D* overall, const nlohmann::json& cropStatus) {
  nlohmann::json info;
  info["datumSide"] = datumRight ? "right" : "left";
  info["source"] = source;
  info["alignment"] = alignment;
  info["overallMm"] = overall ? nlohmann::json(boxToMm(*overall)) : nlohmann::json();
  info["cropStatus"] = cropStatus;
  return info;
}

Data roiSetOf(Data datum, Data target, Data seamLeft, Data seamRight, nlohmann::json info) {
  lyflow::Record rec;
  rec.type = kRoiInfoType;
  rec.data = std::move(info);
  Bundle b(kRoiSetKind);
  b.set("datum", std::move(datum));
  b.set("target", std::move(target));
  b.set("seamLeft", std::move(seamLeft));
  b.set("seamRight", std::move(seamRight));
  b.set("info", Data::record(std::move(rec)));
  return Data::bundle(std::move(b));
}

// ------------------------------------------------------------------ 方向推导

bool datumOnRight(const std::array<double, 4>& datum, const std::array<double, 4>& seamLeft,
                  const std::array<double, 4>& seamRight) {
  const double datumX = 0.5 * (datum[0] + datum[2]);
  const double seamX = 0.25 * (seamLeft[0] + seamLeft[2] + seamRight[0] + seamRight[2]);
  return datumX > seamX;
}

bool datumOnRight(const lyflow::Box2D& datum, const lyflow::Box2D& seamLeft,
                  const lyflow::Box2D& seamRight) {
  return datumOnRight(boxToMm(datum), boxToMm(seamLeft), boxToMm(seamRight));
}

bool datumOnRightInRecord(const nlohmann::json& alignmentData, bool* right) {
  const auto rois = alignmentData.find("rois");
  if (rois == alignmentData.end() || !rois->is_object()) return false;
  std::array<std::array<double, 4>, 3> boxes{};
  const char* keys[3] = {"flushBase", "gapLeft", "gapRight"};
  for (int i = 0; i < 3; ++i) {
    const auto it = rois->find(keys[i]);
    if (it == rois->end() || !it->is_array() || it->size() != 4) return false;
    for (std::size_t j = 0; j < 4; ++j) {
      if (!(*it)[j].is_number()) return false;
      boxes[static_cast<std::size_t>(i)][j] = (*it)[j].get<double>();
    }
  }
  *right = datumOnRight(boxes[0], boxes[1], boxes[2]);
  return true;
}

lyflow::Box2D seamTowardBox(const lyflow::Box2D& seamLeft, const lyflow::Box2D& seamRight) {
  const double lx = 0.5 * (static_cast<double>(seamLeft.min[0]) + seamLeft.max[0]);
  const double ly = 0.5 * (static_cast<double>(seamLeft.min[1]) + seamLeft.max[1]);
  const double rx = 0.5 * (static_cast<double>(seamRight.min[0]) + seamRight.max[0]);
  const double ry = 0.5 * (static_cast<double>(seamRight.min[1]) + seamRight.max[1]);
  lyflow::Box2D box;
  box.min[0] = box.max[0] = static_cast<float>(0.5 * (lx + rx));
  box.min[1] = box.max[1] = static_cast<float>(0.5 * (ly + ry));
  return box;
}

// ------------------------------------------------------------------ Step

Step::Step(const char* opId, ComputeFn fn) : opId_(opId), fn_(fn) {
  desc_ = ensureRegistry().find(opId_);
  if (desc_) {
    for (const Param& p : desc_->params) params_[p.name] = p.def;
    if (!fn_) fn_ = desc_->compute;
  }
}

Step& Step::in(const char* port, const Data& value) {
  if (!value.empty()) inputs_[port] = value;
  return *this;
}

Step& Step::set(const char* param, Value value) {
  params_[param] = std::move(value);
  return *this;
}

Step& Step::copy(const ParamView& from, std::initializer_list<const char*> names) {
  for (const char* n : names) {
    const auto it = from.raw().find(n);
    if (it != from.raw().end()) params_[n] = it->second;
  }
  return *this;
}

Step& Step::copyAll(const ParamView& from) {
  if (!desc_) return *this;
  for (const Param& p : desc_->params) {
    const auto it = from.raw().find(p.name);
    if (it != from.raw().end()) params_[p.name] = it->second;
  }
  return *this;
}

Status Step::run(ExecContext& ctx, const std::string& label, const char* port) {
  outputs_.clear();
  if (!desc_ || !fn_) {
    return Status::Error(Phase::Execute, "internal",
                         "[" + label + "] 注册表里没有算子 " + opId_ + "（它所在的包没编进来？）",
                         {}, port);
  }
  Inputs inputs(inputs_);
  Outputs outputs(outputs_);
  ParamView view(params_, ctx.baseDir());
  Status s = Status::Ok();
  try {
    s = fn_(inputs, view, outputs, ctx);
  } catch (const std::exception& e) {
    s = Status::Error(Phase::Execute, "internal", std::string("算子内部异常: ") + e.what());
  }
  if (!s.ok) {
    s.message = "[" + label + "] " + s.message;
    // paramPath 原样留着：积木算子的参数与细粒度算子同名（copy / copyAll 的约定），
    // 名字对不上的那些 Inspector 找不到框，也不会标错地方。
    s.portName = port;
  }
  return s;
}

const Data& Step::out(const char* port) const {
  static const Data kEmpty;
  const auto it = outputs_.find(port);
  return it == outputs_.end() ? kEmpty : it->second;
}

Param paramOf(const Registry& r, const char* opId, const char* name, int advanced,
              const char* group) {
  const OperatorDesc* op = r.find(opId);
  const Param* p = op ? findParam(*op, name) : nullptr;
  if (!p) throw std::logic_error(std::string("paramOf: ") + opId + " 没有参数 " + name);
  Param copy = *p;
  if (advanced >= 0) copy.advanced = advanced != 0;
  if (group) copy.group = group;
  return copy;
}

}  // namespace lyflow::packs::gap
