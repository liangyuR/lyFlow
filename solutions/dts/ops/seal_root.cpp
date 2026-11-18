#include <cmath>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  try {
    const Data& source = inputs.get("clean");
    ActiveProfile active;
    const Status loaded = loadProfile(source, active, ctx);
    if (!loaded.ok)
      return loaded;
    const auto pieces = checkedPieces(inputs.get("faces"), active);
    const auto& dome = checkedRecord(inputs.get("dome"), "DtsDome.v3");
    const auto& pick = checkedRecord(inputs.get("pick"), "DtsMetalPick.v3");
    const View all = mergedView(active.samples, pieces, [&ctx] { return ctx.cancelled(); });
    const auto& signValue = pick.at("sealSign");
    if (!signValue.is_number_integer() || (signValue != 1 && signValue != -1))
      throw std::invalid_argument("sealSign必须为明确的-1或1");
    const int sealSign = signValue.get<int>();
    const double edge = number(pick, "edgeX"), peak = number(dome, "xPeak");
    const double bulge = params.choice("sealBulge") == "+z" ? 1.0 : -1.0;
    const double cx = number(dome, "centerX"), radius = number(dome, "radiusMm");
    const double reach = params.number("footReachMm");
    const double bumpLimit = sealSign > 0 ? cx - radius - reach : cx + radius + reach;
    RootPoint root;
    if (!findRoot(all, std::min(edge, peak), std::max(edge, peak), sealSign, bulge,
                  params.number("promSmallMm"), params.number("promMetalMm"), bumpLimit, root,
                  [&ctx] { return ctx.cancelled(); }))
      return Status::Error(Phase::Execute, "no_root", "基准面与胶条顶点之间点太少", {}, "clean");
    const auto activeIndex = static_cast<std::size_t>(all.index(root.index));
    const auto slot = active.slots.at(activeIndex);
    Point2D point;
    point.p[0] = static_cast<float>(root.x);
    point.p[1] = static_cast<float>(root.z);
    const Data geometry = Data::point2d(metricGeometry(point));
    nlohmann::json j = sceneRecord();
    j.update({{"x", root.x},
              {"z", root.z},
              {"method", root.bump ? "bump" : "valley"},
              {"nBumps", root.nBumps},
              {"promMetalMm", root.promMetal},
              {"promDomeMm", root.promDome},
              {"searchX0", std::min(edge, peak)},
              {"searchX1", std::max(edge, peak)},
              {"bumpLimitX", bumpLimit},
              {"legacySortedIndex", root.index},
              {"activeIndex", activeIndex},
              {"slotIndex", slot},
              {"sourceIndex", active.native->sourceIndices.at(slot)},
              {"metalLineMm", pick.at("metalLineMm")}});
    if (ctx.cancelled())
      return cancelledStatus();
    outputs.set("root", Data::record(Record{"DtsRoot.v3", std::move(j)}));
    outputs.set("point", geometry);
    outputs.set("pointContext",
                Data::record(profile_contract::geometryContext(source.asProfile()->descriptor)));
    return Status::Ok();
  } catch (const kernel::profile::Cancelled&) {
    return cancelledStatus();
  } catch (const std::exception& error) {
    return sceneError(error, "pick");
  }
}

} // namespace

void registerSealRoot(Registry& r) {
  OperatorDesc op;
  op.id = "dts.seal_root";
  op.version = "3.0.0";
  op.migrations = {{1, rejectLegacyProfileNode, nullptr},
                   {2, nullptr, &profile_contract::keepInputs}};
  op.label = "胶条根部定位";
  op.category = "DTS/胶条";
  op.keywords = {"seal root localization",
                 "seal foot detection",
                 "root point",
                 "胶条根部",
                 "底部特征",
                 "谷底检测",
                 "特征点定位",
                 "root",
                 "foot",
                 "bump",
                 "根部",
                 "底部",
                 "凸起"};
  op.doc = "在「基准面朝胶条那一端 → 胶条顶点」这段区间里找底部：\n"
           "优先取「凹-凸-凹」里的凸起顶点（有多个取离胶条最近的），没有凸起就取区间里的谷底。\n"
           "区间不再锚在胶条凸包的端点上 —— 那个端点会滑进台肩，窗口整个偏到钣金上去。";
  op.inputs = {Port{"clean", "Profile", "Clean", "清理过的轮廓。", true},
               Port{"faces", "Record", "Faces", "面表（取分片）。", true},
               Port{"dome", "Record", "Dome", "胶条主体。", true},
               Port{"pick", "Record", "Pick", "挑中的基准面。", true}};
  op.outputs = {Port{"root", "Record", "Root", "底部点与判定依据（DtsRoot.v3）。", true},
                Port{"point", "Point2D", "Point", "底部点米制坐标，给视图用。", true},
                Port{"pointContext", "Record", "Point Context",
                     "根部点所在空间（profile.Geometry.v3）。", true}};

  Param bulge;
  bulge.name = "sealBulge";
  bulge.type = ParamType::Enum;
  bulge.label = "凸起方向";
  bulge.def = Value::text("+z");
  bulge.options = {EnumOption{"+z", "+z（朝传感器）", ""}, EnumOption{"-z", "-z", ""}};

  Param ps;
  ps.name = "promSmallMm";
  ps.type = ParamType::Float;
  ps.label = "胶条侧凹陷下限";
  ps.doc = "凸起靠胶条那一侧的落差下限。实测可以浅到 0.05 mm。";
  ps.def = Value::number(0.03);
  ps.min = 0.0;
  ps.unit = "mm";

  Param pm;
  pm.name = "promMetalMm";
  pm.type = ParamType::Float;
  pm.label = "钣金侧凹陷下限";
  pm.doc = "凸起靠钣金那一侧的落差下限。实测 1.0–1.2 mm，给 0.3 留足余量。";
  pm.def = Value::number(0.30);
  pm.min = 0.0;
  pm.unit = "mm";

  Param reach;
  reach.name = "footReachMm";
  reach.type = ParamType::Float;
  reach.label = "脚下范围";
  reach.doc = "凸起必须落在胶条圆的钣金侧边缘再往外这么远以内 —— 「凹-凸-凹」是胶条脚下那一个，"
              "不是远处外板大弧的顶。用圆的边缘而不是凸包端点：圆稳，端点会滑。";
  reach.def = Value::number(3.0);
  reach.min = 0.0;
  reach.unit = "mm";

  op.params = {bulge, ps, pm, reach};
  op.capabilities = {true, true, true};
  op.compute = &compute;
  r.addOperator(std::move(op));
}

} // namespace lyflow::dts
