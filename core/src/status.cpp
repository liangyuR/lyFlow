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
    w.field("nodeId", d.nodeId);
    w.field("severity", std::string(toString(d.severity)));
    w.field("phase", std::string(toString(d.status.phase)));
    w.field("code", d.status.code);
    w.field("message", d.status.message);
    w.fieldIfSet("paramPath", d.status.paramPath);
    w.fieldIfSet("portName", d.status.portName);
    w.endObject();
  }
  w.endArray();
  return w.str();
}

}  // namespace lyflow
