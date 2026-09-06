#include "ops.h"

namespace lyflow::ops {
namespace {

Data choiceRecord(const char* choice, const std::string& reason) {
  Record r;
  r.type = "FallbackChoice";
  r.data = nlohmann::json::object();
  r.data["choice"] = choice;
  r.data["reason"] = reason;
  return Data::record(std::move(r));
}

Status fallbackCompute(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  const Data& a = inputs.get("a");
  if (!a.empty() && !a.isError()) {
    outputs.set("out", a);
    outputs.set("choice", choiceRecord("a", std::string()));
    return Status::Ok();
  }

  const std::string reason =
      a.isError() ? a.asError()->code + ": " + a.asError()->message : "主路径没有产出";

  if (!inputs.has("b")) return Status::Demand("b");

  const Data& b = inputs.get("b");
  if (b.empty() || b.isError()) {
    const std::string tail =
        b.isError() ? "；备用路径也失败：" + b.asError()->message : "；备用路径没有产出";
    return Status::Error(Phase::Execute, a.isError() ? a.asError()->code : "upstream_failed",
                         reason + tail, {}, "a");
  }
  outputs.set("out", b);
  outputs.set("choice", choiceRecord("b", reason));
  return Status::Ok();
}

bool condHolds(const Data& cond, const std::string& path, bool& out, std::string& why) {
  if (const Measurement* m = cond.asMeasurement()) {
    out = m->ok;
    return true;
  }
  if (const Record* r = cond.asRecord()) {
    if (path.empty()) {
      why = "cond 是 Record 时必须用 path 参数指出要读哪个布尔字段";
      return false;
    }
    const nlohmann::json* node = &r->data;
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t dot = path.find('.', start);
      const std::string key = path.substr(start, dot == std::string::npos ? dot : dot - start);
      if (!node->is_object() || !node->contains(key)) {
        why = "Record 里没有字段 '" + path + "'";
        return false;
      }
      node = &node->at(key);
      if (dot == std::string::npos) break;
      start = dot + 1;
    }
    if (node->is_boolean()) {
      out = node->get<bool>();
      return true;
    }
    if (node->is_number()) {
      out = node->get<double>() != 0.0;
      return true;
    }
    why = "字段 '" + path + "' 不是布尔值也不是数字";
    return false;
  }
  why = std::string("cond 只支持 Measurement 或 Record，收到 ") + cond.typeName();
  return false;
}

Status selectCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                     ExecContext&) {
  bool takeA = false;
  std::string why;
  if (!condHolds(inputs.get("cond"), params.text("path"), takeA, why)) {
    return Status::Error(Phase::Execute, "bad_input", why, "path", "cond");
  }
  const char* want = takeA ? "a" : "b";
  if (!inputs.has(want)) return Status::Demand(want);
  const Data& picked = inputs.get(want);
  if (picked.empty() || picked.isError()) {
    return Status::Error(Phase::Execute,
                         picked.isError() ? picked.asError()->code : "upstream_failed",
                         std::string("选中的分支 '") + want + "' 没有产出", {}, want);
  }
  outputs.set("out", picked);
  outputs.set("choice", choiceRecord(want, std::string()));
  return Status::Ok();
}

}  // namespace

void registerFlowOps(Registry& r) {
  {
    OperatorDesc op;
    op.id = "flow.fallback";
    op.version = "1.0.0";
    op.label = "Fallback";
    op.category = "Flow";
    op.keywords = {"fallback", "retry", "error", "回退", "备用", "容错"};
    op.doc =
        "a 成功就透传 a；a 失败（或没有产出）才调度 b 的上游闭包并透传 b。"
        "b 的闭包在被 demand 之前不进计划，未被 demand 的节点报 skipped/not_demanded。";

    Port a{"a", "Any", "A（主）", "主路径。它的上游失败时本节点收到一个 Error 而不是被连坐。",
           true};
    a.acceptsError = true;
    Port b{"b", "Any", "B（备）", "备用路径。只有 a 失败时才会被调度。", true};
    b.lazy = true;
    // 备用路径也接 Error：两条都失败时本算子要能同时说出 a 和 b 的原因
    // （gap-inspector-integration-design.md §2.4）。
    b.acceptsError = true;
    op.inputs = {a, b};
    op.outputs = {Port{"out", "Any", "Out", "a 或 b，零拷贝透传。", true},
                  Port{"choice", "Record", "Choice",
                       "FallbackChoice：选了哪一路（choice）以及为什么（reason）。", true}};
    op.capabilities = {/*cancellable=*/false, /*previewable=*/true, /*deterministic=*/true};
    op.compute = &fallbackCompute;
    r.addOperator(std::move(op));
  }

  {
    OperatorDesc op;
    op.id = "flow.select";
    op.version = "1.0.0";
    op.label = "Select";
    op.category = "Flow";
    op.keywords = {"select", "switch", "if", "condition", "条件", "分支", "选择"};
    op.doc =
        "按 cond 选一路：Measurement 看它的 ok，Record 看 path 指向的布尔字段。"
        "两路都是惰性的，只有被选中的那一路才会被调度。";

    Port cond{"cond", "Any", "Cond", "Measurement（看 ok）或 Record（看 path 字段）。", true};
    cond.anyGroup = 1;
    Port a{"a", "Any", "A（真）", "cond 为真时选它。", true};
    a.lazy = true;
    Port b{"b", "Any", "B（假）", "cond 为假时选它。", true};
    b.lazy = true;
    a.acceptsError = true;
    b.acceptsError = true;
    op.inputs = {cond, a, b};
    op.outputs = {Port{"out", "Any", "Out", "被选中的那一路，零拷贝透传。", true},
                  Port{"choice", "Record", "Choice", "FallbackChoice：选了 a 还是 b。", true}};

    Param path;
    path.name = "path";
    path.type = ParamType::String;
    path.label = "字段路径";
    path.doc = "cond 是 Record 时读哪个布尔字段，点号分层。cond 是 Measurement 时忽略。";
    path.def = Value::text("");
    op.params = {path};

    op.capabilities = {/*cancellable=*/false, /*previewable=*/true, /*deterministic=*/true};
    op.compute = &selectCompute;
    r.addOperator(std::move(op));
  }
}

}  // namespace lyflow::ops
