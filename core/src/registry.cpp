#include "lyflow/registry.h"

#include <algorithm>
#include <mutex>
#include <set>

#include "lyflow/json_writer.h"
#include "lyflow/version.h"

namespace lyflow {
namespace {

// param.type -> default 值应有的 Value::Kind，validate() 的依据。
// 默认值与声明类型不符应当在启动自检时炸掉，而不是让前端渲染出空控件。
bool defaultKindMatches(ParamType t, const Value& v) {
  using K = Value::Kind;
  switch (t) {
    case ParamType::Bool:      return v.kind() == K::Bool;
    case ParamType::Int:       return v.kind() == K::Int;
    case ParamType::Float:     return v.kind() == K::Float || v.kind() == K::Int;
    case ParamType::Vec2f:     return v.kind() == K::FloatVec && v.vecValue().size() == 2;
    case ParamType::Vec3f:     return v.kind() == K::FloatVec && v.vecValue().size() == 3;
    case ParamType::Vec4f:     return v.kind() == K::FloatVec && v.vecValue().size() == 4;
    case ParamType::Color:     return v.kind() == K::FloatVec &&
                                      (v.vecValue().size() == 3 || v.vecValue().size() == 4);
    case ParamType::Transform: return v.kind() == K::FloatVec && v.vecValue().size() == 16;
    case ParamType::Enum:      return v.kind() == K::String || v.kind() == K::Int;
    case ParamType::Flags:     return v.kind() == K::Int;
    case ParamType::String:
    case ParamType::Text:
    case ParamType::Path:      return v.kind() == K::String;
    case ParamType::Curve:     return true;  // M0 未定形，先不校验
  }
  return false;
}

/// semver 的主版本。解析不出来当作 1 —— 版本号本身的格式由别处报错。
int majorOf(const std::string& version) {
  try {
    const std::size_t dot = version.find('.');
    return std::stoi(dot == std::string::npos ? version : version.substr(0, dot));
  } catch (...) {
    return 1;
  }
}

void writeValue(JsonWriter& w, const Value& v) {
  switch (v.kind()) {
    case Value::Kind::Null:     w.valueNull(); break;
    case Value::Kind::Bool:     w.value(v.boolValue()); break;
    case Value::Kind::Int:      w.value(v.intValue()); break;
    case Value::Kind::Float:    w.value(v.floatValue()); break;
    case Value::Kind::String:   w.value(v.stringValue()); break;
    case Value::Kind::FloatVec:
      w.beginArray();
      for (double d : v.vecValue()) w.value(d);
      w.endArray();
      break;
  }
}

void writeCondition(JsonWriter& w, const std::string& key, const Condition& c) {
  if (!c.isSet()) return;
  w.key(key);
  w.beginObject();
  w.field("param", c.param);
  if (!c.eq.isNull()) { w.key("eq"); writeValue(w, c.eq); }
  if (!c.in.empty()) {
    w.key("in");
    w.beginArray();
    for (const auto& v : c.in) writeValue(w, v);
    w.endArray();
  }
  w.endObject();
}

void writePort(JsonWriter& w, const Port& p, bool isInput) {
  w.beginObject();
  w.field("name", p.name);
  w.field("type", p.type);
  w.fieldIfSet("label", p.label);
  w.fieldIfSet("doc", p.doc);
  // required / acceptsError / lazy 只对输入端口有意义，schema 也是这么说的。
  if (isInput && !p.required) w.field("required", false);
  if (isInput && p.acceptsError) w.field("acceptsError", true);
  if (isInput && p.lazy) w.field("lazy", true);
  w.endObject();
}

void writeParam(JsonWriter& w, const Param& p) {
  w.beginObject();
  w.field("name", p.name);
  w.field("type", std::string(toString(p.type)));
  w.fieldIfSet("label", p.label);
  w.fieldIfSet("doc", p.doc);
  w.key("default");
  writeValue(w, p.def);
  w.fieldIfSet("group", p.group);
  if (p.advanced) w.field("advanced", true);

  if (p.min)     w.field("min", *p.min);
  if (p.max)     w.field("max", *p.max);
  if (p.softMin) w.field("softMin", *p.softMin);
  if (p.softMax) w.field("softMax", *p.softMax);
  if (p.step)    w.field("step", *p.step);
  w.fieldIfSet("unit", p.unit);
  w.fieldIfSet("componentLabels", p.componentLabels);

  if (!p.options.empty()) {
    w.key("options");
    w.beginArray();
    for (const auto& o : p.options) {
      w.beginObject();
      w.field("value", o.value);
      w.field("label", o.label);
      w.fieldIfSet("doc", o.doc);
      w.endObject();
    }
    w.endArray();
  }

  w.fieldIfSet("placeholder", p.placeholder);
  w.fieldIfSet("pattern", p.pattern);
  if (p.rows) w.field("rows", static_cast<std::int64_t>(*p.rows));

  if (!p.filters.empty()) {
    w.key("filters");
    w.beginArray();
    for (const auto& f : p.filters) {
      w.beginObject();
      w.field("name", f.name);
      w.key("extensions");
      w.beginArray();
      for (const auto& e : f.extensions) w.value(e);
      w.endArray();
      w.endObject();
    }
    w.endArray();
  }

  w.fieldIfSet("mode", p.mode);
  if (p.alpha) w.field("alpha", *p.alpha);

  writeCondition(w, "visibleWhen", p.visibleWhen);
  writeCondition(w, "enabledWhen", p.enabledWhen);
  w.endObject();
}

}  // namespace

Registry& Registry::instance() {
  static Registry r;
  return r;
}

Registry& ensureRegistry() {
  static std::once_flag once;
  Registry& r = Registry::instance();
  std::call_once(once, [&] { registerBuiltinOps(r); });
  return r;
}

void Registry::addType(PortType type) { types_.push_back(std::move(type)); }

void Registry::setCurrentPack(std::string pack) { currentPack_ = std::move(pack); }

void Registry::addOperator(OperatorDesc op) {
  if (op.pack.empty()) op.pack = currentPack_;
  operators_.push_back(std::move(op));
  builtinCount_ = operators_.size();
}

void Registry::setLibraryOperators(std::vector<OperatorDesc> ops) {
  operators_.resize(builtinCount_);
  for (auto& op : ops) operators_.push_back(std::move(op));
}

void Registry::addImporter(ImporterDesc importer) {
  if (importer.pack.empty()) importer.pack = currentPack_;
  for (auto& existing : importers_) {
    if (existing.kind == importer.kind) {
      existing = std::move(importer);
      return;
    }
  }
  importers_.push_back(std::move(importer));
}

void Registry::clear() {
  types_.clear();
  operators_.clear();
  importers_.clear();
  builtinCount_ = 0;
}

const OperatorDesc* Registry::find(const std::string& id) const {
  for (const auto& op : operators_) {
    if (op.id == id) return &op;
    if (std::find(op.aliases.begin(), op.aliases.end(), id) != op.aliases.end()) return &op;
  }
  return nullptr;
}

const ImporterDesc* Registry::findImporter(const std::string& kind) const {
  for (const auto& i : importers_) {
    if (i.kind == kind) return &i;
  }
  return nullptr;
}

const PortType* Registry::findType(const std::string& name) const {
  for (const auto& t : types_) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

std::vector<std::string> Registry::validate() const {
  std::vector<std::string> problems;
  auto fail = [&](const std::string& msg) { problems.push_back(msg); };

  // 类型表自身
  std::set<std::string> typeNames;
  for (const auto& t : types_) {
    if (t.name.empty()) fail("port type with empty name");
    if (!typeNames.insert(t.name).second) fail("duplicate port type: " + t.name);
    if (t.color.empty()) fail("port type '" + t.name + "' has no color");
  }
  for (const auto& t : types_) {
    for (const auto& c : t.castableTo) {
      if (!typeNames.count(c)) {
        fail("port type '" + t.name + "' castableTo unknown type '" + c + "'");
      }
    }
  }

  std::set<std::string> opIds;
  for (const auto& op : operators_) {
    const std::string where = "operator '" + op.id + "'";
    if (op.id.empty())       fail("operator with empty id");
    if (op.version.empty())  fail(where + " has no version");
    if (op.label.empty())    fail(where + " has no label");
    if (op.category.empty()) fail(where + " has no category");
    if (!opIds.insert(op.id).second) fail("duplicate operator id: " + op.id);
    // 「有描述没实现」的算子只会在用户画完整张图、点了运行之后才暴露。
    // 挡在启动自检里，代价是一行。
    if (!op.compute) fail(where + " has no compute function");
    if (op.outputs.empty() && op.inputs.empty()) {
      fail(where + " has neither inputs nor outputs");
    }

    auto checkPorts = [&](const std::vector<Port>& ports, const char* kind) {
      const bool isInput = std::string(kind) == "input";
      std::set<std::string> seen;
      for (const auto& p : ports) {
        if (p.name.empty()) fail(where + " has a " + kind + " port with empty name");
        if (!seen.insert(p.name).second) {
          fail(where + " has duplicate " + kind + " port '" + p.name + "'");
        }
        if (!typeNames.count(p.type)) {
          fail(where + " " + kind + " port '" + p.name +
               "' uses unknown type '" + p.type + "'");
        }
        if (!isInput && (p.acceptsError || p.lazy)) {
          fail(where + " output port '" + p.name + "' sets acceptsError/lazy (inputs only)");
        }
      }
    };
    checkPorts(op.inputs, "input");
    checkPorts(op.outputs, "output");

    std::set<std::string> paramNames;
    for (const auto& p : op.params) {
      const std::string pwhere = where + " param '" + p.name + "'";
      if (p.name.empty()) fail(where + " has a param with empty name");
      if (!paramNames.insert(p.name).second) fail(where + " has duplicate param '" + p.name + "'");
      if (p.def.isNull()) {
        fail(pwhere + " has no default (schema requires one; GraphDoc 稀疏存储以它为基准)");
      } else if (!defaultKindMatches(p.type, p.def)) {
        fail(pwhere + " default does not match declared type '" + toString(p.type) + "'");
      }
      if ((p.type == ParamType::Enum || p.type == ParamType::Flags) && p.options.empty()) {
        fail(pwhere + " is enum/flags but declares no options");
      }
      if (p.type == ParamType::Enum && p.def.kind() == Value::Kind::String) {
        bool found = false;
        for (const auto& o : p.options) {
          if (o.value == p.def.stringValue()) { found = true; break; }
        }
        if (!found) fail(pwhere + " default '" + p.def.stringValue() + "' is not among its options");
      }
      if (p.min && p.max && *p.min > *p.max) fail(pwhere + " has min > max");
    }

    // 参数联动只允许引用同一算子内的其他参数
    for (const auto& p : op.params) {
      for (const Condition* c : {&p.visibleWhen, &p.enabledWhen}) {
        if (c->isSet() && !paramNames.count(c->param)) {
          fail(where + " param '" + p.name + "' references unknown param '" + c->param + "'");
        }
      }
    }

    // 迁移链必须覆盖 1..currentMajor-1 且无断档（ADR-0008）。半条链比没有链更糟：
    // 老图能打开一半、参数改了一半，最后表现成「算法结果莫名其妙」。
    const int major = majorOf(op.version);
    std::set<int> steps;
    for (const auto& m : op.migrations) {
      if (!m.apply) fail(where + " migration from major " + std::to_string(m.fromMajor) +
                         " has no function");
      if (m.fromMajor < 1 || m.fromMajor >= major) {
        fail(where + " migration fromMajor " + std::to_string(m.fromMajor) +
             " is outside 1.." + std::to_string(major - 1));
      }
      if (!steps.insert(m.fromMajor).second) {
        fail(where + " has two migrations from major " + std::to_string(m.fromMajor));
      }
    }
    for (int m = 1; m < major; ++m) {
      if (!steps.count(m)) {
        fail(where + " is v" + op.version + " but has no migration from major " +
             std::to_string(m));
      }
    }
  }

  std::set<std::string> importerKinds;
  for (const auto& i : importers_) {
    if (i.kind.empty()) fail("importer with empty kind");
    if (!i.fn) fail("importer '" + i.kind + "' has no function");
    if (!importerKinds.insert(i.kind).second) fail("duplicate importer kind: " + i.kind);
  }
  return problems;
}

std::string Registry::toManifestJson() const {
  JsonWriter w;
  w.beginObject();
  w.field("schemaVersion", 1);
  w.field("generatedBy", std::string("lyflow-core/") + LYFLOW_VERSION);

  w.key("types");
  w.beginArray();
  for (const auto& t : types_) {
    w.beginObject();
    w.field("name", t.name);
    w.field("color", t.color);
    w.fieldIfSet("castableTo", t.castableTo);
    w.fieldIfSet("doc", t.doc);
    w.endObject();
  }
  w.endArray();

  w.key("operators");
  w.beginArray();
  for (const auto& op : operators_) {
    w.beginObject();
    w.field("id", op.id);
    w.field("version", op.version);
    // S7：算子来自哪个包。core 自带的两个算子没有这一项，前端一律不解释它。
    w.fieldIfSet("pack", op.pack);
    w.fieldIfSet("aliases", op.aliases);
    w.field("label", op.label);
    w.field("category", op.category);
    w.fieldIfSet("keywords", op.keywords);
    w.fieldIfSet("doc", op.doc);
    w.fieldIfSet("preconditions", op.preconditions);

    w.key("inputs");
    w.beginArray();
    for (const auto& p : op.inputs) writePort(w, p, /*isInput=*/true);
    w.endArray();

    w.key("outputs");
    w.beginArray();
    for (const auto& p : op.outputs) writePort(w, p, /*isInput=*/false);
    w.endArray();

    w.key("params");
    w.beginArray();
    for (const auto& p : op.params) writeParam(w, p);
    w.endArray();

    w.key("capabilities");
    w.beginObject();
    w.field("cancellable", op.capabilities.cancellable);
    w.field("previewable", op.capabilities.previewable);
    w.field("deterministic", op.capabilities.deterministic);
    w.endObject();

    w.endObject();
  }
  w.endArray();

  // 导入器（ADR-0017）。前端据此列出「导入…」菜单，不必再硬编码格式名。
  if (!importers_.empty()) {
    w.key("importers");
    w.beginArray();
    for (const auto& i : importers_) {
      w.beginObject();
      w.field("kind", i.kind);
      w.field("label", i.label.empty() ? i.kind : i.label);
      w.fieldIfSet("doc", i.doc);
      w.fieldIfSet("pack", i.pack);
      w.endObject();
    }
    w.endArray();
  }

  w.endObject();
  return w.str();
}

}  // namespace lyflow
