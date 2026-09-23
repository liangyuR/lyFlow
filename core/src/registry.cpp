#include "lyflow/registry.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#include "lyflow/contract.h"
#include "lyflow/json_writer.h"
#include "lyflow/version.h"

namespace lyflow {
namespace {

/// 「什么类型都可能」的端口上不查契约与类型的搭配：具体类型要等编译期沿边推导。
constexpr const char* kAnyTypeName = "Any";

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
  // 契约（ADR-0024）与样例（m6-plan H8）两端口都可以有：契约在输入端被执行器检查，
  // 写在输出端上是给读图的人看「这一维保证是什么」。空的一律不出现。
  if (p.contract.is_object() && !p.contract.empty()) {
    w.key("contract");
    w.raw(p.contract.dump());
  }
  if (!p.example.is_null()) {
    w.key("example");
    w.raw(p.example.dump());
  }
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
  w.fieldIfSet("semantic", p.semantic);
  if (p.roiBackdrop.isSet()) {
    w.key("roiBackdrop");
    w.beginObject();
    w.field("dir", p.roiBackdrop.dirParam);
    w.fieldIfSet("files", p.roiBackdrop.fileParams);
    w.fieldIfSet("label", p.roiBackdrop.label);
    w.fieldIfSet("labelParam", p.roiBackdrop.labelParam);
    w.endObject();
  }
  w.endObject();
}

bool typesCompatibleIn(const Registry& r, const std::string& from, const std::string& to) {
  if (from == to || from == kAnyTypeName || to == kAnyTypeName) return true;
  const PortType* t = r.findType(from);
  return t && std::find(t->castableTo.begin(), t->castableTo.end(), to) != t->castableTo.end();
}

const Port* portNamed(const std::vector<Port>& ports, const std::string& name) {
  for (const auto& p : ports) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

/// 片段文件的自检（m8-plan L14）：算子都注册了、参数名都认得、边两端的端口存在且类型接得上、
/// 对外端口提示指到真实的端口上。坏片段在启动时就报，而不是等人插进画布才发现一半节点缺失。
std::vector<std::string> checkSnippet(const Registry& r, const nlohmann::json& b) {
  using nlohmann::json;
  std::vector<std::string> out;
  auto str = [](const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  if (b.value("schemaVersion", 0) != 1) out.push_back("schemaVersion 必须是 1");
  if (str(b, "id").empty()) out.push_back("没有 id");
  if (str(b, "label").empty()) out.push_back("没有 label");
  const json nodes = b.value("nodes", json::array());
  if (!nodes.is_array() || nodes.empty()) {
    out.push_back("nodes 为空");
    return out;
  }
  std::map<std::string, const OperatorDesc*> ops;
  for (const json& n : nodes) {
    const std::string id = str(n, "id");
    const std::string opId = str(n, "op");
    if (id.empty()) {
      out.push_back("有一个节点没有 id");
      continue;
    }
    if (ops.count(id)) out.push_back("节点 id 重复: " + id);
    const OperatorDesc* op = r.find(opId);
    if (!op) out.push_back("节点 " + id + " 的算子 '" + opId + "' 没有注册");
    ops[id] = op;
    if (op && n.contains("params")) {
      if (!n["params"].is_object()) {
        out.push_back("节点 " + id + " 的 params 不是对象");
      } else {
        for (auto it = n["params"].begin(); it != n["params"].end(); ++it) {
          if (!findParam(*op, it.key())) {
            out.push_back("节点 " + id + " 的算子 " + opId + " 没有参数 '" + it.key() + "'");
          }
        }
      }
    }
  }
  auto endpoint = [&](const json& ref, bool input, const std::string& what) -> const Port* {
    const std::string node = str(ref, "node");
    const std::string port = str(ref, "port");
    auto it = ops.find(node);
    if (it == ops.end()) {
      out.push_back(what + " 指向片段里没有的节点 '" + node + "'");
      return nullptr;
    }
    if (!it->second) return nullptr;  // 算子缺失已经报过
    const Port* p = portNamed(input ? it->second->inputs : it->second->outputs, port);
    if (!p) {
      out.push_back(what + " 指向 " + node + " 没有的" + (input ? "输入" : "输出") + "端口 '" +
                    port + "'");
    }
    return p;
  };
  std::set<std::string> fed;
  for (const json& e : b.value("edges", json::array())) {
    const Port* from = endpoint(e.value("from", json::object()), false, "边");
    const Port* to = endpoint(e.value("to", json::object()), true, "边");
    const std::string target = str(e.value("to", json::object()), "node") + "." +
                               str(e.value("to", json::object()), "port");
    if (!fed.insert(target).second) out.push_back("输入端口 " + target + " 接了不止一条边");
    if (from && to && !typesCompatibleIn(r, from->type, to->type)) {
      out.push_back("边 → " + target + " 类型接不上：" + from->type + " → " + to->type);
    }
  }
  const json ports = b.value("ports", json::object());
  for (const json& p : ports.value("inputs", json::array())) {
    if (endpoint(p, true, "对外输入") &&
        fed.count(str(p, "node") + "." + str(p, "port"))) {
      out.push_back("对外输入 " + str(p, "node") + "." + str(p, "port") + " 在片段里已经接了边");
    }
  }
  for (const json& p : ports.value("outputs", json::array())) endpoint(p, false, "对外输出");
  return out;
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

void Registry::addBundle(BundleDesc bundle) {
  if (bundle.pack.empty()) bundle.pack = currentPack_;
  for (auto& existing : bundles_) {
    if (existing.kind == bundle.kind) {
      existing = std::move(bundle);
      return;
    }
  }
  bundles_.push_back(std::move(bundle));
}

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

void Registry::addSnippet(SnippetDesc snippet) {
  if (snippet.pack.empty()) snippet.pack = currentPack_;
  for (auto& existing : snippets_) {
    if (!snippet.id.empty() && existing.id == snippet.id) {
      existing = std::move(snippet);
      return;
    }
  }
  snippets_.push_back(std::move(snippet));
}

void Registry::clear() {
  types_.clear();
  bundles_.clear();
  operators_.clear();
  importers_.clear();
  snippets_.clear();
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

const BundleDesc* Registry::findBundle(const std::string& kind) const {
  for (const auto& b : bundles_) {
    if (b.kind == kind) return &b;
  }
  return nullptr;
}

bool Registry::knowsType(const std::string& typeName) const {
  std::string kind;
  if (parseBundleType(typeName, &kind)) return findBundle(kind) != nullptr;
  return findType(typeName) != nullptr;
}

std::string Registry::checkBundle(const std::string& declaredType, const Data& value,
                                  nlohmann::json* expected, nlohmann::json* actual) const {
  std::string kind;
  if (!parseBundleType(declaredType, &kind)) return "端口类型 '" + declaredType + "' 不是 Bundle<kind>";
  const BundleDesc* desc = findBundle(kind);
  const Bundle* got = value.asBundle();
  if (expected) {
    nlohmann::json want = nlohmann::json::object();
    if (desc) {
      for (const BundleField& f : desc->fields) want[f.name] = f.type;
    }
    *expected = {{"bundle", kind}, {"fields", want}};
  }
  if (actual) {
    nlohmann::json have = nlohmann::json::object();
    if (got) {
      for (const auto& f : got->fields) have[f.first] = f.second.typeName();
    }
    *actual = got ? nlohmann::json{{"bundle", got->kind}, {"fields", have}}
                  : nlohmann::json{{"type", value.typeName()}};
  }
  if (!desc) return "Bundle kind '" + kind + "' 没有在 manifest 里声明";
  if (!got) return std::string("应当是 ") + declaredType + "，实际是 " + value.typeName();
  if (got->kind != kind) return "应当是 " + declaredType + "，实际是 " + got->typeName;
  for (const BundleField& f : desc->fields) {
    const Data* d = got->field(f.name);
    if (!d || d->empty()) return declaredType + " 缺字段 '" + f.name + "'";
    const Data::Kind want = kindFromTypeName(f.type);
    if (want != Data::Kind::None && d->kind() != want) {
      return declaredType + " 的字段 '" + f.name + "' 应当是 " + f.type + "，实际是 " +
             d->typeName();
    }
  }
  for (const auto& f : got->fields) {
    bool declared = false;
    for (const BundleField& df : desc->fields) {
      if (df.name == f.first) { declared = true; break; }
    }
    if (!declared) return declaredType + " 多了一个没声明的字段 '" + f.first + "'";
  }
  return {};
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

  // Bundle 表（m8-plan L2）。字段只能是类型表里的具体类型：Any 没法校验，
  // Error 只该出现在 acceptsError 端口上，Bundle 套 Bundle 被 L1 明确排除。
  std::set<std::string> bundleKinds;
  for (const auto& b : bundles_) {
    const std::string where = "bundle '" + b.kind + "'";
    if (b.kind.empty() || b.kind.find_first_of("<> ") != std::string::npos) {
      fail("bundle with bad kind '" + b.kind + "'（不能为空，不能含 < > 空格）");
    }
    if (!bundleKinds.insert(b.kind).second) fail("duplicate bundle kind: " + b.kind);
    if (b.fields.empty()) fail(where + " declares no fields");
    std::set<std::string> names;
    for (const auto& f : b.fields) {
      if (f.name.empty() || f.name.find('.') != std::string::npos) {
        fail(where + " field '" + f.name + "' 名字不能为空也不能含 '.'（`<port>.<field>` 靠它分隔）");
      }
      if (!names.insert(f.name).second) fail(where + " has duplicate field '" + f.name + "'");
      if (f.type == kAnyTypeName || f.type == "Error" || parseBundleType(f.type, nullptr)) {
        fail(where + " field '" + f.name + "' 的类型不能是 " + f.type);
      } else if (!typeNames.count(f.type)) {
        fail(where + " field '" + f.name + "' uses unknown type '" + f.type + "'");
      }
    }
  }
  auto portTypeKnown = [&](const std::string& type) {
    std::string kind;
    if (parseBundleType(type, &kind)) return bundleKinds.count(kind) != 0;
    return typeNames.count(type) != 0;
  };

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
        if (p.type == "Bundle") {
          fail(where + " " + kind + " port '" + p.name + "' 写的是裸的 Bundle，应当写成 Bundle<kind>");
        } else if (!portTypeKnown(p.type)) {
          fail(where + " " + kind + " port '" + p.name +
               "' uses unknown type '" + p.type + "'");
        }
        if (!isInput && (p.acceptsError || p.lazy)) {
          fail(where + " output port '" + p.name + "' sets acceptsError/lazy (inputs only)");
        }
        // 端口契约（ADR-0024）。四种键之外的任何东西都在这里被拒 ——
        // `lyflow manifest --check` 与 startup 自检走的是同一个 validate()。
        const std::string portWhere = where + " " + kind + " port '" + p.name + "'";
        for (const std::string& problem : validatePortContract(p.contract, portWhere)) {
          fail(problem);
        }
        if (p.contract.is_object()) {
          // 契约与端口类型对不对得上。声明了 shape 却接在 PointCloud 上，
          // 只会在第一次真跑的时候变成一个莫名其妙的 contract_violation。
          if (p.contract.contains("shape") && p.type != "Tensor" && p.type != kAnyTypeName) {
            fail(portWhere + " contract: shape 只对 Tensor 端口有意义，这个端口是 " + p.type);
          }
          if (p.contract.contains("recordType") && p.type != "Record" && p.type != kAnyTypeName) {
            fail(portWhere + " contract: recordType 只对 Record 端口有意义，这个端口是 " + p.type);
          }
          if (p.contract.contains("finite") && p.type != "PointCloud" && p.type != "Tensor" &&
              p.type != "Measurement" && p.type != kAnyTypeName) {
            fail(portWhere +
                 " contract: finite 只对 PointCloud / Tensor / Measurement 端口有意义，"
                 "这个端口是 " + p.type);
          }
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

    // 语义标记（m8-plan L15）：只认 kParamSemantics 里的名字，roi 只能标在 vec4f 上，
    // roiBackdrop 指的必须是本算子自己的参数。
    for (const auto& p : op.params) {
      const std::string pwhere = where + " param '" + p.name + "'";
      if (p.semantic.empty()) {
        if (p.roiBackdrop.isSet()) fail(pwhere + " 给了 roiBackdrop 却没有 semantic=roi");
        continue;
      }
      bool known = false;
      for (const char* s : kParamSemantics) known = known || p.semantic == s;
      if (!known) {
        fail(pwhere + " semantic '" + p.semantic + "' 不认识");
        continue;
      }
      if (p.semantic == "roi" && p.type != ParamType::Vec4f) {
        fail(pwhere + " semantic=roi 只能标在 vec4f 参数上");
      }
      if (!p.roiBackdrop.isSet()) continue;
      const Param* dir = findParam(op, p.roiBackdrop.dirParam);
      if (!dir || dir->type != ParamType::Path) {
        fail(pwhere + " roiBackdrop.dir '" + p.roiBackdrop.dirParam + "' 要指向本算子的一个 path 参数");
      }
      for (const std::string& f : p.roiBackdrop.fileParams) {
        const Param* fp = findParam(op, f);
        if (!fp || (fp->type != ParamType::String && fp->type != ParamType::Path)) {
          fail(pwhere + " roiBackdrop.files 里的 '" + f + "' 要指向本算子的一个 string / path 参数");
        }
      }
      if (!p.roiBackdrop.labelParam.empty()) {
        const Param* lp = findParam(op, p.roiBackdrop.labelParam);
        if (!lp || lp->type != ParamType::String) {
          fail(pwhere + " roiBackdrop.labelParam '" + p.roiBackdrop.labelParam +
               "' 要指向本算子的一个 string 参数");
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

  for (const auto& s : snippets_) {
    const std::string where = "snippet '" + (s.id.empty() ? s.source : s.id) + "'" +
                              (s.source.empty() ? std::string() : "（" + s.source + "）");
    if (!s.parseError.empty()) {
      fail(where + ": " + s.parseError);
      continue;
    }
    try {
      for (const std::string& problem : checkSnippet(*this, s.body)) fail(where + ": " + problem);
    } catch (const std::exception& e) {
      // 字段类型写错（比如 ports 写成数组）时 nlohmann 的 value() 会抛
      fail(where + ": 结构不对（" + e.what() + "）");
    }
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

  // Bundle 表（m8-plan L2）。没有包声明时整段不出现，不带 gap 的构建 manifest 字节不变。
  if (!bundles_.empty()) {
    w.key("bundles");
    w.beginArray();
    for (const auto& b : bundles_) {
      w.beginObject();
      w.field("kind", b.kind);
      w.fieldIfSet("label", b.label);
      w.fieldIfSet("doc", b.doc);
      w.fieldIfSet("pack", b.pack);
      w.key("fields");
      w.beginArray();
      for (const auto& f : b.fields) {
        w.beginObject();
        w.field("name", f.name);
        w.field("type", f.type);
        w.fieldIfSet("doc", f.doc);
        w.endObject();
      }
      w.endArray();
      w.endObject();
    }
    w.endArray();
  }

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

  // 片段（m8-plan L14）。坏的那几份（解析失败）不进 manifest，自检另报。没有就整段不出现。
  bool anySnippet = false;
  for (const auto& s : snippets_) anySnippet = anySnippet || s.parseError.empty();
  if (anySnippet) {
    w.key("snippets");
    w.beginArray();
    for (const auto& s : snippets_) {
      if (!s.parseError.empty()) continue;
      w.beginObject();
      w.field("id", s.id);
      w.field("label", s.label.empty() ? s.id : s.label);
      w.fieldIfSet("category", s.category);
      w.fieldIfSet("doc", s.doc);
      w.fieldIfSet("pack", s.pack);
      for (const char* key : {"nodes", "edges", "ports"}) {
        auto it = s.body.find(key);
        if (it == s.body.end()) continue;
        w.key(key);
        w.raw(it->dump());
      }
      w.endObject();
    }
    w.endArray();
  }

  w.endObject();
  return w.str();
}

}  // namespace lyflow
