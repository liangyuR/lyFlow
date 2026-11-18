#include <string>
#include <utility>

#include "lyflow/calibration.h"
#include "lyflow/kernel/metrology.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_metrology {
void registerProfileMetrology(Registry&);
void registerProfileGapFlush(Registry&);
void registerProfileDimensions(Registry&);
void registerCompareTolerance(Registry&);
namespace {

Status calibratedDistance(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  Measurement measurement;
  std::string why, port;
  if (!kernel::metrology::calibratedDistance(in.get("calib").asRecord(), in.get("a").asPoint2D(),
                                             in.get("b").asPoint2D(), p.text("imageFrame"),
                                             measurement, why, port))
    return Status::Error(Phase::Execute, "bad_input", std::move(why), {}, std::move(port));
  out.set("distance", Data::measurement(std::move(measurement)));
  return Status::Ok();
}

} // namespace

void registerPackOps(Registry& registry) {
  OperatorDesc op;
  op.id = "image.calibrated_distance";
  op.version = "1.1.0";
  op.label = "平面标定距离量测";
  op.category = "图像/视觉";
  op.keywords = {"calibrated physical planar point distance homography", op.label};
  op.capabilities = {true, false, true};
  op.inputs = {Port{"a", "Point2D", "A", "原图像素点。", true},
               Port{"b", "Point2D", "B", "原图像素点。", true},
               withContract(Port{"calib", "Record", "calib", calibration::kPlaneType, true},
                            {{"recordType", calibration::kPlaneType}})};
  op.outputs = {Port{"distance", "Measurement", "Distance", "米制平面距离。", true}};
  Param imageFrame;
  imageFrame.name = "imageFrame";
  imageFrame.label = "量测点图像坐标帧";
  imageFrame.type = ParamType::String;
  imageFrame.def = Value::text("");
  op.params = {std::move(imageFrame)};
  op.compute = &calibratedDistance;
  registry.addOperator(std::move(op));
  registerProfileGapFlush(registry);
  registerProfileMetrology(registry);
  registerProfileDimensions(registry);
  registerCompareTolerance(registry);
}

} // namespace lyflow::packs::std_metrology
