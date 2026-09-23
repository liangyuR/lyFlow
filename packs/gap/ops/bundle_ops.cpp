// 细粒度图里的 Bundle 组装与拆开（m8-plan L6「两种可以在同一张图里混用」）。
// 细粒度链末端用 make_* 把散线收成 RoiSet / ScanPair 交给 result_bundle 或积木算子；
// 积木链中途用 split_* 把字段拆成散线交给细粒度算子。只搬数据，不做任何计算。
#include "gap_fine.h"

namespace lyflow::packs::gap {
namespace {

Status makeScanPair(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  outputs.set("scan", scanPairOf(inputs.get("primary"), inputs.get("secondary"),
                                 inputs.get("merged")));
  return Status::Ok();
}

Status splitScanPair(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  ScanView scan;
  if (!readScanPair(inputs.get("scan"), &scan)) {
    return Status::Error(Phase::Execute, "bad_input", "输入不是完整的 ScanPair", {}, "scan");
  }
  outputs.set("primary", *scan.primary);
  outputs.set("secondary", *scan.secondary);
  outputs.set("merged", *scan.merged);
  return Status::Ok();
}

Status makeRoiSet(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext&) {
  const lyflow::Box2D& datum = *inputs.get("datum").asBox2D();
  const lyflow::Box2D& seamLeft = *inputs.get("seamLeft").asBox2D();
  const lyflow::Box2D& seamRight = *inputs.get("seamRight").asBox2D();
  const lyflow::Record* alignment =
      inputs.has("alignment") ? inputs.get("alignment").asRecord() : nullptr;
  const lyflow::Record* crop =
      inputs.has("cropStatus") ? inputs.get("cropStatus").asRecord() : nullptr;
  const lyflow::Box2D* overall = inputs.has("overall") ? inputs.get("overall").asBox2D() : nullptr;

  // auto：接了对齐结果就按模板坐标系里的框推（与 gap.locate_template 同一个判据），
  // 没接就按手上这四个框推（与 gap.locate_model 同一个判据）。
  const std::string side = params.choice("datumSide");
  bool datumRight = side == "right";
  if (side == "auto") {
    if (!alignment || !datumOnRightInRecord(alignment->data, &datumRight)) {
      datumRight = datumOnRight(datum, seamLeft, seamRight);
    }
  }
  nlohmann::json info =
      roiInfo(datumRight, params.choice("source"),
              alignment ? alignment->data : nlohmann::json(), overall,
              crop ? crop->data : nlohmann::json());
  outputs.set("rois", roiSetOf(inputs.get("datum"), inputs.get("target"), inputs.get("seamLeft"),
                               inputs.get("seamRight"), std::move(info)));
  return Status::Ok();
}

Status splitRoiSet(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  RoiView rois;
  if (!readRoiSet(inputs.get("rois"), &rois)) {
    return Status::Error(Phase::Execute, "bad_input", "输入不是完整的 RoiSet", {}, "rois");
  }
  outputs.set("datum", *rois.datum);
  outputs.set("target", *rois.target);
  outputs.set("seamLeft", *rois.seamLeft);
  outputs.set("seamRight", *rois.seamRight);
  outputs.set("seam", Data::box2d(seamTowardBox(*rois.seamLeft->asBox2D(), *rois.seamRight->asBox2D())));
  outputs.set("info", Data::record(*rois.info));
  return Status::Ok();
}

Port cloud(const char* name, const char* label, const char* doc) {
  return Port{name, "PointCloud", label, doc, true};
}

Port box(const char* name, const char* label, const char* doc) {
  return Port{name, "Box2D", label, doc, true};
}

}  // namespace

void registerBundleOps(Registry& r) {
  {
    OperatorDesc op;
    op.id = "gap.make_scan_pair";
    op.version = "1.0.0";
    op.label = "组剖面对";
    op.category = "间隙/组合";
    op.keywords = {"bundle", "scan pair", "组合", "打包"};
    op.doc =
        "把三片云收成一个 ScanPair（Bundle<gap.ScanPair>），交给 gap.result_bundle 或积木算子。"
        "只搬不算：三片云原样装进去。";
    op.inputs = {cloud("primary", "Primary", "Master 那一片。"),
                 cloud("secondary", "Secondary", "Slave 那一片。"),
                 cloud("merged", "Merged", "两片的合并云。")};
    op.outputs = {Port{"scan", "Bundle<gap.ScanPair>", "Scan", "ScanPair。", true}};
    op.capabilities = {false, true, true};
    op.compute = &makeScanPair;
    r.addOperator(std::move(op));
  }
  {
    OperatorDesc op;
    op.id = "gap.split_scan_pair";
    op.version = "1.0.0";
    op.label = "拆剖面对";
    op.category = "间隙/组合";
    op.keywords = {"bundle", "scan pair", "拆开"};
    op.doc = "把 ScanPair 拆成三根散线，接给细粒度算子。只搬不算。";
    op.inputs = {Port{"scan", "Bundle<gap.ScanPair>", "Scan", "ScanPair。", true}};
    op.outputs = {cloud("primary", "Primary", "Master 那一片。"),
                  cloud("secondary", "Secondary", "Slave 那一片。"),
                  cloud("merged", "Merged", "合并云。")};
    op.capabilities = {false, true, true};
    op.compute = &splitScanPair;
    r.addOperator(std::move(op));
  }
  {
    OperatorDesc op;
    op.id = "gap.make_roi_set";
    op.version = "1.0.0";
    op.label = "组角色框";
    op.category = "间隙/组合";
    op.keywords = {"bundle", "roi set", "组合", "角色框"};
    op.doc =
        "把四个业务框按角色收成一个 RoiSet（Bundle<gap.RoiSet>）。datum = 段差基准面框，"
        "target = 段差参考面框，seamLeft / seamRight = 缝左右两侧的框。info 里记"
        "基准件在哪一侧、框从哪来，以及接上来的对齐结果、整体框、裁剪状态。";
    op.inputs = {
        box("datum", "Datum", "段差基准面框（business_rois / roi_from_labels 的 flushBase）。"),
        box("target", "Target", "段差参考面框（flushRef）。"),
        box("seamLeft", "Seam Left", "缝左侧的框（gapLeft）。"),
        box("seamRight", "Seam Right", "缝右侧的框（gapRight）。"),
        withContract(Port{"alignment", "Record", "Alignment",
                          "可选：选中的 GapAlignment。接了就进 info，datumSide=auto 时按它里面"
                          "模板坐标系的框推基准件在哪一侧。",
                          false},
                     {{"recordType", "GapAlignment"}}),
        Port{"overall", "Box2D", "Overall", "可选：整体框或跟随裁剪窗，进 info.overallMm。", false},
        withContract(Port{"cropStatus", "Record", "Crop Status",
                          "可选：gap.roll_anchored_crop 的 status，进 info.cropStatus。", false},
                     {{"recordType", "GapRollCrop"}}),
    };
    op.outputs = {Port{"rois", "Bundle<gap.RoiSet>", "ROIs", "RoiSet。", true}};

    Param source;
    source.name = "source";
    source.type = ParamType::Enum;
    source.label = "Source";
    source.doc = "框从哪来，进 info.source。result_bundle 的 roi_source 取它。";
    source.def = Value::text("template");
    source.options = {EnumOption{"template", "Template", "模板 + ICP。"},
                      EnumOption{"model", "Model", "分割模型。"}};

    Param side;
    side.name = "datumSide";
    side.type = ParamType::Enum;
    side.label = "Datum Side";
    side.doc =
        "基准件在缝的哪一侧，进 info.datumSide。auto（默认）由框推出：接了 alignment 用模板"
        "坐标系里的框，否则用手上这四个框。";
    side.def = Value::text("auto");
    side.advanced = true;
    side.options = {EnumOption{"auto", "Auto", "由框推出。"}, EnumOption{"left", "Left", ""},
                    EnumOption{"right", "Right", ""}};
    op.params = {source, side};
    op.capabilities = {false, true, true};
    op.compute = &makeRoiSet;
    r.addOperator(std::move(op));
  }
  {
    OperatorDesc op;
    op.id = "gap.split_roi_set";
    op.version = "1.0.0";
    op.label = "拆角色框";
    op.category = "间隙/组合";
    op.keywords = {"bundle", "roi set", "拆开", "角色框"};
    op.doc =
        "把 RoiSet 拆成散线接给细粒度算子。另出一个 seam：两个缝框中心连线的中点，"
        "接 gap.fit_line 的 toward，与积木算子取「靠缝那一端」是同一个判据。";
    op.inputs = {Port{"rois", "Bundle<gap.RoiSet>", "ROIs", "RoiSet。", true}};
    op.outputs = {box("datum", "Datum", "段差基准面框。"), box("target", "Target", "段差参考面框。"),
                  box("seamLeft", "Seam Left", "缝左侧的框。"),
                  box("seamRight", "Seam Right", "缝右侧的框。"),
                  box("seam", "Seam", "两个缝框中心连线的中点（零尺寸框）。"),
                  Port{"info", "Record", "Info", "GapRoiInfo。", true}};
    op.capabilities = {false, true, true};
    op.compute = &splitRoiSet;
    r.addOperator(std::move(op));
  }
}

}  // namespace lyflow::packs::gap
