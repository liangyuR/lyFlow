#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  const nlohmann::json* fj = recordData(inputs.get("faces"), "DtsFaces");
  const nlohmann::json* dj = recordData(inputs.get("dome"), "DtsDome");
  const nlohmann::json* pj = recordData(inputs.get("pick"), "DtsMetalPick");
  const nlohmann::json* rj = recordData(inputs.get("root"), "DtsRoot");
  const Measurement* m = inputs.get("flush").asMeasurement();
  const Line2D* metal = inputs.get("metal").asLine2D();
  if (fj == nullptr || dj == nullptr || pj == nullptr || rj == nullptr || m == nullptr ||
      metal == nullptr) {
    return Status::Error(Phase::Execute, "type_mismatch", "汇总的输入类型不对", {}, "flush");
  }
  nlohmann::json j;
  j["ok"] = m->ok;
  j["flushMm"] = m->ok ? nlohmann::json(m->value) : nlohmann::json();
  j["root"] = *rj;
  j["dome"] = *dj;
  j["pick"] = *pj;
  j["faces"] = (*fj)["faces"];
  j["pieces"] = (*fj)["pieces"];
  j["metal"] = {{"x0", metal->start[0]}, {"z0", metal->start[1]},
                {"x1", metal->end[0]},   {"z1", metal->end[1]},
                {"angleDeg", std::atan2(metal->dir[1], metal->dir[0]) * 180.0 /
                                 3.14159265358979323846},
                {"rmsMm", (*pj).value("rmsMm", 0.0)}};
  if (inputs.has("foot")) {
    const Point2D* f = inputs.get("foot").asPoint2D();
    if (f != nullptr) j["perpendicular"] = {{"x", f->p[0]}, {"z", f->p[1]}};
  }
  outputs.set("bundle", Data::record(Record{"DtsProfileBundle", std::move(j)}));
  return Status::Ok();
}

}  // namespace

void registerProfileBundle(Registry& r) {
  OperatorDesc op;
  op.id = "dts.profile_bundle";
  op.version = "1.0.0";
  op.label = "单剖面汇总";
  op.category = "DTS/输出";
  op.keywords = {"bundle", "result", "汇总"};
  op.doc = "把这一条轮廓上所有中间结果装成一个 Record，宿主拿它画界面、落库。";
  op.inputs = {Port{"faces", "Record", "Faces", "面表。", true},
               Port{"dome", "Record", "Dome", "胶条主体。", true},
               Port{"pick", "Record", "Pick", "挑面结果。", true},
               Port{"root", "Record", "Root", "胶条底部。", true},
               Port{"metal", "Line2D", "Metal", "基准线。", true},
               Port{"flush", "Measurement", "Flush", "面差。", true},
               Port{"foot", "Point2D", "Foot", "垂足，可选。", false}};
  op.outputs = {Port{"bundle", "Record", "Bundle", "DtsProfileBundle。", true}};
  op.capabilities = {false, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::dts
