#include "gap_detection/GapUtils.hpp"
#include "gap_fine.h"
#include "profile_template.h"

namespace lyflow::packs::gap {
namespace fine {

Status alignTemplate(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext& ctx) {
  Record result;
  const auto status = profile_template::alignClouds(
      *inputs.get("cloud").asCloud(), *inputs.get("tplLeft").asCloud(),
      *inputs.get("tplRight").asCloud(), profile_template::alignmentOptions(params), result, ctx);
  if (status.ok)
    outputs.set("alignment", Data::record(std::move(result)));
  return status;
}

Status selectAlignment(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                       ExecContext& ctx) {
  std::vector<const lyflow::Record*> candidates;
  for (const char* port : {"a", "b", "c", "d"}) {
    if (!inputs.has(port))
      continue;
    const lyflow::Record* rec = inputs.get(port).asRecord();
    if (!rec || rec->type != "GapAlignment") {
      return Status::Error(Phase::Execute, "bad_input", "输入不是 GapAlignment", {}, port);
    }
    candidates.push_back(rec);
  }
  if (candidates.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "一个候选都没有", {}, "a");
  }

  std::vector<::detection::utils::TemplateIcpScore> scores;
  scores.reserve(candidates.size());
  for (const auto* rec : candidates) {
    ::detection::utils::TemplateIcpScore s;
    s.id = rec->data.value("templateId", std::string());
    s.left = rec->data["left"].value("score", 0.0);
    s.right = rec->data["right"].value("score", 0.0);
    s.configuration_order = static_cast<std::size_t>(rec->data.value("order", 0));
    scores.push_back(std::move(s));
  }
  const double minScore = static_cast<double>(params.integer("minScore"));
  const auto picked = ::detection::utils::selectBestTemplateByIcp(scores, minScore);
  if (!picked) {
    double worst = scores.front().left;
    if (std::isfinite(scores.front().right))
      worst = std::min(worst, scores.front().right);
    return Status::Error(Phase::Execute, "icp_score_low",
                         "所有候选模板的 ICP 分都低于 " + std::to_string(minScore) +
                             "（最好的一份是 " + std::to_string(worst) + "）");
  }
  ctx.log(LogLevel::Info, "选中模板 " + scores[*picked].id);
  outputs.set("alignment", Data::record(*candidates[*picked]));
  return Status::Ok();
}

} // namespace fine

void registerAlignTemplate(Registry& r) {
  OperatorDesc op;
  op.id = "gap.align_template";
  op.version = "1.1.0";
  op.label = "模板配准";
  op.category = "间隙/配准";
  op.keywords = {"template registration",
                 "2D ICP",
                 "iterative closest point",
                 "rigid registration",
                 "模板对齐",
                 "二维刚体配准",
                 "粗配准",
                 "icp",
                 "template",
                 "align",
                 "配准",
                 "模板"};
  op.doc = "把一对模板配到当前样本上（evaluate_pair）：全局粗配 → 左右两侧 ICP、"
           "信赖域钳制、退化方向锁定；模板两侧内容相同时右侧复用左侧的结果。\n"
           "刚体 2D 配准，假定样本与模板之间只差一个小的平移加旋转：超出信赖域的部分被钳掉而不是"
           "报错（quality 里记 trustClamped）；沿长直边滑动这类退化方向被锁定。分数低不判失败，"
           "照样输出 GapAlignment —— 丢弃低分候选是 gap.select_alignment 的事。";
  op.inputs = {
      Port{"cloud", "PointCloud", "Cloud", "合并并滤波之后的测量帧点云。", true},
      Port{"tplLeft", "PointCloud", "Template Left", "左模板。", true},
      Port{"tplRight", "PointCloud", "Template Right", "右模板。", true},
  };
  op.outputs = {withExample(Port{"alignment", "Record", "Alignment", "GapAlignment。", true},
                            examples::alignment())};

  op.params = profile_template::alignmentParameters();
  op.capabilities = {false, false, true};
  op.compute = &fine::alignTemplate;
  r.addOperator(std::move(op));
}

void registerSelectAlignment(Registry& r) {
  OperatorDesc op;
  op.id = "gap.select_alignment";
  op.version = "1.0.0";
  op.label = "配准结果选择";
  op.category = "间隙/配准";
  op.keywords = {"alignment selection",
                 "registration candidate selection",
                 "best ICP",
                 "score ranking",
                 "配准候选",
                 "模板优选",
                 "配准评分",
                 "template",
                 "select",
                 "模板选择"};
  op.doc = "在候选模板里挑一个：先过滤左右都 ≥ minScore 的，"
           "再按 min(l,r) 降、mean 降、配置顺序升、id 升排序取第一个；全打平时由 templateId 定，"
           "与接在哪个端口无关。只挑不配；全部低于 minScore 时报 icp_score_low，不退而求其次。";
  // 四个候选端口都只吃 GapAlignment（compute 里本来就查，写成契约之后第一帧就报，
  // 而且带得出「实际是什么类型」——Status 的那句话带不出来）。
  const nlohmann::json isAlignment = {{"recordType", "GapAlignment"}};
  op.inputs = {
      withContract(Port{"a", "Record", "A", "第一个候选。", true}, isAlignment),
      withContract(Port{"b", "Record", "B", "第二个候选。", false}, isAlignment),
      withContract(Port{"c", "Record", "C", "第三个候选。", false}, isAlignment),
      withContract(Port{"d", "Record", "D", "第四个候选。", false}, isAlignment),
  };
  op.outputs = {withExample(Port{"alignment", "Record", "Alignment", "选中的那一份。", true},
                            examples::alignment())};
  op.params = profile_template::selectionParameters();
  op.capabilities = {false, true, true};
  op.compute = &fine::selectAlignment;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::gap
