#include "lyflow/status.h"

#include "lyflow/json_writer.h"

namespace lyflow {

const char* toString(Phase p) {
  switch (p) {
    case Phase::Validate: return "validate";
    case Phase::Compile:  return "compile";
    case Phase::Execute:  return "execute";
  }
  return "execute";
}

const char* toString(Severity s) {
  switch (s) {
    case Severity::Error:   return "error";
    case Severity::Warning: return "warning";
  }
  return "error";
}

std::vector<Diagnostic> Diagnostics::forNode(const std::string& nodeId) const {
  std::vector<Diagnostic> out;
  for (const auto& d : items_) {
    if (d.nodeId == nodeId) out.push_back(d);
  }
  return out;
}

std::string Diagnostics::toJson() const {
  JsonWriter w;
  w.beginArray();
  for (const auto& d : items_) {
    w.beginObject();
    // kind 让前端一眼分得出普通诊断和迁移动作，不用靠字段有无去猜（ADR-0008）。
    w.field("kind", std::string(d.migration ? "migration" : "diagnostic"));
    w.field("nodeId", d.nodeId);
    w.field("severity", std::string(toString(d.severity)));
    w.field("phase", std::string(toString(d.status.phase)));
    w.field("code", d.status.code);
    w.field("message", d.status.message);
    w.fieldIfSet("paramPath", d.status.paramPath);
    w.fieldIfSet("portName", d.status.portName);
    if (d.migration) {
      w.field("op", d.migration->op);
      w.field("opVersion", d.migration->opVersion);
      // params 已经是 JSON 文本，原样嵌进去 —— 再解析一遍只是为了重新写出来。
      w.key("params");
      w.raw(d.migration->paramsJson);
      w.fieldIfSet("notes", d.migration->notes);
      if (!d.migration->editsJson.empty()) {
        w.key("edits");
        w.raw(d.migration->editsJson);
      }
    }
    w.endObject();
  }
  w.endArray();
  return w.str();
}

}  // namespace lyflow
