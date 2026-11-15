#include "lyflow/calibration.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_image {
void registerPlaneCalibUpgrade(Registry& r) {
  OperatorDesc op;
  op.id = "image.upgrade_plane_calib";
  op.version = "1.0.0";
  op.label = "平面标定契约升级";
  op.category = "图像/标定/数据适配";
  op.keywords = {"calibration", "homography", "units", "标定", "单位", "单应", "升级"};
  op.doc = "显式声明图像帧和标定平面帧，将旧PlaneCalib升级为统一的版本化契约；"
           "缺单位时必须明确指定m/mm，不猜测；齐次H归一化不改变映射。已有版本化记录拒绝重标帧。";
  op.inputs = {{"calib", "Record", "旧标定", "image.PlaneCalib。", true}};
  op.outputs = {{"calib", "Record", "标准标定", "schemaVersion=1的image.PlaneCalib。", true}};
  Param fallback;
  fallback.name = "missingUnit";
  fallback.label = "缺失单位的明确约定";
  fallback.type = ParamType::Enum;
  fallback.def = Value::text("unspecified");
  fallback.options = {{"unspecified", "未指定"}, {"m", "米"}, {"mm", "毫米"}};
  Param image;
  image.name = "imageFrame";
  image.label = "图像坐标帧";
  image.type = ParamType::String;
  image.def = Value::text("");
  Param plane = image;
  plane.name = "planeFrame";
  plane.label = "标定平面坐标帧";
  op.params = {fallback, image, plane};
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
    const auto* input = in.get("calib").asRecord();
    Record result;
    std::string why;
    if (!input ||
        !calibration::upgradeLegacyPlane(*input, p.choice("missingUnit"), p.text("imageFrame"),
                                         p.text("planeFrame"), result, why))
      return Status::Error(Phase::Execute, "bad_input", why.empty() ? "需要平面标定记录" : why, {},
                           "calib");
    out.set("calib", Data::record(std::move(result)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_image
