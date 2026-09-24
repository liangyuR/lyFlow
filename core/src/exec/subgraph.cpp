#include "exec/subgraph.h"

#include <algorithm>
#include <map>
#include <set>

#include "exec/library.h"
#include "exec/plan.h"

namespace lyflow::exec {
namespace {

constexpr const char* kSubPrefix = "sub:";

/// 一个已经展开到叶子的端点。
struct Endpoint {
  std::string node, port;
};

/// 一个子图节点对外的接线面：外面的边靠它落到内部的真实节点上。
struct Boundary {
  std::map<std::string, std::vector<Endpoint>> inputs;
  std::map<std::string, Endpoint> outputs;
};

Value valueFromJson(ParamType type, const nlohmann::json& j) {
  auto numbers = [&](std::size_t fallback) {
    std::vector<double> v;
    if (j.is_array()) {
      for (const auto& e : j) v.push_back(e.is_number() ? e.get<double>() : 0.0);
    }
    if (v.empty()) v.assign(fallback, 0.0);
    return Value::vec(std::move(v));
  };
  switch (type) {
    case ParamType::Bool:      return Value::boolean(j.is_boolean() && j.get<bool>());
    case ParamType::Int:
    case ParamType::Flags:     return Value::integer(j.is_number() ? j.get<std::int64_t>() : 0);
    case ParamType::Float:     return Value::number(j.is_number() ? j.get<double>() : 0.0);
    case ParamType::Vec2f:     return numbers(2);
    case ParamType::Vec3f:     return numbers(3);
    case ParamType::Vec4f:     return numbers(4);
    case ParamType::Color:     return numbers(3);
    case ParamType::Transform: return numbers(16);
    case ParamType::Enum:
    case ParamType::String:
    case ParamType::Text:
    case ParamType::Path:      return Value::text(j.is_string() ? j.get<std::string>() : std::string());
    case ParamType::Curve:     return Value::text(j.is_null() ? std::string("{}") : j.dump());
  }
  return Value::number(0.0);
}

void readOptional(const nlohmann::json& j, const char* key, std::optional<double>& out) {
  auto it = j.find(key);
  if (it != j.end() && it->is_number()) out = it->get<double>();
}

Status subgraphStub(const Inputs&, const ParamView&, Outputs&, ExecContext&) {
  return Status::Error(Phase::Execute, "internal",
                       "子图算子应当在 compile 之前被展开掉，执行器不该看见它");
}

}  // namespace

Param paramFromDecl(const std::string& name, const nlohmann::json& j) {
  Param p;
  p.name = name;
  // json::value 碰到类型不符的字段会抛：手写的声明里 label 写成数字不该让整张图展开失败
  auto text = [&](const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  if (!parseParamType(text("type"), p.type)) p.type = ParamType::Float;
  p.label = text("label");
  p.doc = text("doc");
  p.group = text("group");
  auto advanced = j.find("advanced");
  p.advanced = advanced != j.end() && advanced->is_boolean() && advanced->get<bool>();
  p.unit = text("unit");
  p.placeholder = text("placeholder");
  p.mode = text("mode");
  // 语义标记跟着提升走（框拖动照样可用）；roiBackdrop 指的是内部算子的参数名，
  // 到了子图这一层对不上，不带过来。
  if (text("semantic") == "roi" && p.type == ParamType::Vec4f) {
    p.semantic = "roi";
  }
  readOptional(j, "min", p.min);
  readOptional(j, "max", p.max);
  readOptional(j, "softMin", p.softMin);
  readOptional(j, "softMax", p.softMax);
  readOptional(j, "step", p.step);

  auto labels = j.find("componentLabels");
  if (labels != j.end() && labels->is_array()) {
    for (const auto& e : *labels) {
      if (e.is_string()) p.componentLabels.push_back(e.get<std::string>());
    }
  }
  auto options = j.find("options");
  if (options != j.end() && options->is_array()) {
    for (const auto& o : *options) {
      if (!o.is_object()) continue;
      EnumOption e;
      // schema 允许整数的选项值（flags 的位）：json::value 碰到类型不符会抛，这里自己取
      auto v = o.find("value");
      if (v != o.end()) e.value = v->is_string() ? v->get<std::string>() : v->dump();
      auto label = o.find("label");
      e.label = label != o.end() && label->is_string() ? label->get<std::string>() : e.value;
      auto doc = o.find("doc");
      if (doc != o.end() && doc->is_string()) e.doc = doc->get<std::string>();
      p.options.push_back(std::move(e));
    }
  }
  auto def = j.find("default");
  p.def = valueFromJson(p.type, def == j.end() ? nlohmann::json() : *def);
  // enum 的默认值必须落在选项里，否则注册表自检会拒绝整份 manifest
  if (p.type == ParamType::Enum && !p.options.empty()) {
    bool found = false;
    for (const auto& o : p.options) {
      if (o.value == p.def.stringValue()) found = true;
    }
    if (!found) p.def = Value::text(p.options.front().value);
  }
  return p;
}

OperatorDesc synthesizeOperator(const SubgraphDef& def, const std::string& opId) {
  OperatorDesc op;
  op.id = opId;
  op.version = def.version.empty() ? std::string("1.0.0") : def.version;
  op.label = def.name.empty() ? def.id : def.name;
  op.category = def.category.empty() ? std::string("Subgraph") : def.category;
  op.keywords = def.keywords;
  op.doc = def.doc;
  for (const SubInput& in : def.inputs) {
    op.inputs.push_back(Port{in.name, in.type, in.label, in.doc, !in.to.empty()});
  }
  for (const SubOutput& o : def.outputs) {
    op.outputs.push_back(Port{o.name, o.type, o.label, o.doc, true});
  }
  for (const SubParam& p : def.params) op.params.push_back(paramFromDecl(p.name, p.decl));
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &subgraphStub;
  return op;
}

const SubgraphDef* findSubgraphDef(const RawGraph& graph, const std::string& opId) {
  if (opId.rfind(kSubPrefix, 0) == 0) {
    auto it = graph.subgraphs.find(opId.substr(std::string(kSubPrefix).size()));
    return it == graph.subgraphs.end() ? nullptr : &it->second;
  }
  if (opId.rfind(kLibraryIdPrefix, 0) == 0) return Library::instance().find(opId);
  return nullptr;
}

namespace {

class Expander {
 public:
  Expander(const RawGraph& in, RawGraph& out, Diagnostics& diags)
      : in_(in), out_(out), diags_(diags) {}

  bool run() {
    std::vector<std::string> stack;
    std::vector<RawNode> nodes = in_.nodes;
    applyGraphParams(nodes);
    expandLevel(nodes, in_.edges, std::string(), false, stack);
    return ok_;
  }

 private:
  void fail(const std::string& nodeId, const char* code, const std::string& message,
            const std::string& paramPath = {}, const std::string& portName = {}) {
    diags_.error(nodeId, Phase::Validate, code, message, paramPath, portName);
    ok_ = false;
  }

  /// 一个提升参数的值写进它绑定的每个节点。子图外参（F4）与顶层图参数（J7）共用这一段：
  /// graphParam 非空 = 来自顶层参数，记进 graphParams；否则记进 boundParams。
  /// 找不到的目标节点在这里静默跳过 —— 子图定义解析时已经挡过，顶层参数由调用方先查。
  static void writeBinds(std::vector<RawNode>& nodes, const std::map<std::string, std::size_t>& byId,
                         const std::vector<std::pair<std::string, std::string>>& binds,
                         const nlohmann::json& value, const std::string* graphParam) {
    for (const auto& bind : binds) {
      auto n = byId.find(bind.first);
      if (n == byId.end()) continue;
      RawNode& target = nodes[n->second];
      target.params[bind.second] = value;
      if (graphParam) {
        target.graphParams[bind.second] = *graphParam;
        target.boundParams.erase(bind.second);
      } else {
        // 记一笔「这个值是外层表单灌进来的」，`lyflow params` 的 source=bound 靠它。
        target.boundParams.insert(bind.second);
        target.graphParams.erase(bind.second);
      }
    }
  }

  /// 一个顶层参数的取值按它自己的规格查（P1.2）：类型、硬限位、options。老格式没写 type
  /// 的跳过这一步 —— 它的规格就是被绑定的那个节点参数，照旧由 buildPlan 规整。
  /// 不合法只报诊断、记 paramValuesOk，不打断展开：绑定照常写下去，节点那一层的规整与
  /// 其余诊断照常出来（编辑期校验要一次看到全部问题），buildPlan 最后不给可运行的计划。
  void checkGraphParamValues() {
    for (const GraphParam& gp : in_.params) {
      auto typeIt = gp.decl.find("type");
      if (typeIt == gp.decl.end()) continue;
      Param spec = paramFromDecl(gp.name, gp.decl);
      // enum 没给 options（手写的半份规格）：只能查「是字符串」，查选项会把每个值都判成非法
      if (spec.type == ParamType::Enum && spec.options.empty()) spec.type = ParamType::String;
      const std::string who =
          "图参数 '" + gp.name + "'" + (spec.label.empty() ? std::string(" ") : "（" + spec.label + "）");
      auto check = [&](const nlohmann::json& value, const char* which) {
        Value v;
        std::string message;
        if (!coerceParam(spec, value, v, message) || !checkRange(spec, v, message)) {
          diags_.error("", Phase::Validate, "bad_param", who + which + "：" + message, gp.name);
          out_.paramValuesOk = false;
        }
      };
      check(gp.decl.at("default"), "的 default");
      if (gp.given) check(*gp.given, "传入的值");
    }
  }

  /// 顶层图参数（J7）。与子图外参唯一的不同是「一处定义」：目标节点里又显式写了
  /// 同一个参数、或者两个顶层参数绑同一个目标，都是 param_conflict，而不是悄悄覆盖。
  /// 目标参数在算子上存不存在要 manifest 才知道，留给 buildPlan（那里报 unknown_bind）。
  void applyGraphParams(std::vector<RawNode>& nodes) {
    checkGraphParamValues();
    std::map<std::string, std::size_t> byId;
    for (std::size_t i = 0; i < nodes.size(); ++i) byId[nodes[i].id] = i;
    std::map<std::pair<std::string, std::string>, std::string> owner;
    bool clean = true;
    for (const GraphParam& gp : in_.params) {
      for (const auto& bind : gp.binds) {
        auto n = byId.find(bind.first);
        if (n == byId.end()) {
          fail("", "unknown_bind",
               "顶层参数 '" + gp.name + "' 绑到不存在的节点 '" + bind.first + "'", gp.name);
          clean = false;
          continue;
        }
        auto taken = owner.emplace(bind, gp.name);
        if (!taken.second) {
          fail(bind.first, "param_conflict",
               "参数 '" + bind.second + "' 同时被顶层参数 '" + taken.first->second + "' 与 '" +
                   gp.name + "' 绑定",
               bind.second);
          clean = false;
          continue;
        }
        if (nodes[n->second].params.contains(bind.second)) {
          fail(bind.first, "param_conflict",
               "参数 '" + bind.second + "' 由顶层参数 '" + gp.name +
                   "' 绑定，节点里不能再显式写值（一处定义）",
               bind.second);
          clean = false;
        }
      }
    }
    if (!clean) return;
    for (const GraphParam& gp : in_.params) {
      writeBinds(nodes, byId, gp.binds, gp.value(), &gp.name);
    }
  }

  static bool resolveSource(const std::map<std::string, Boundary>& boundaries,
                            const std::string& prefix, const std::string& node,
                            const std::string& port, Endpoint& out) {
    auto it = boundaries.find(node);
    if (it == boundaries.end()) {
      out = Endpoint{prefix + node, port};
      return true;
    }
    auto o = it->second.outputs.find(port);
    if (o == it->second.outputs.end()) return false;
    out = o->second;
    return true;
  }

  static bool resolveTargets(const std::map<std::string, Boundary>& boundaries,
                             const std::string& prefix, const std::string& node,
                             const std::string& port, std::vector<Endpoint>& out) {
    auto it = boundaries.find(node);
    if (it == boundaries.end()) {
      out.push_back(Endpoint{prefix + node, port});
      return true;
    }
    auto i = it->second.inputs.find(port);
    if (i == it->second.inputs.end()) return false;
    out = i->second;
    return true;
  }

  Boundary expandDef(const SubgraphDef& def, const RawNode& host, const std::string& hostId,
                     const std::string& prefix, bool bypass, std::vector<std::string>& stack) {
    std::vector<RawNode> nodes = def.nodes;
    std::map<std::string, std::size_t> byId;
    for (std::size_t i = 0; i < nodes.size(); ++i) byId[nodes[i].id] = i;

    // 外参覆盖内参（F4）。外面没给值就用声明里的默认值 —— 「表单上看到什么就是里面用什么」。
    std::set<std::string> declared;
    for (const SubParam& p : def.params) {
      declared.insert(p.name);
      nlohmann::json value;
      auto given = host.params.find(p.name);
      if (given != host.params.end()) {
        value = *given;
      } else {
        auto def2 = p.decl.find("default");
        if (def2 == p.decl.end()) continue;
        value = *def2;
      }
      // 宿主节点的这个外参本身由顶层参数灌进来时，来源一路传下去：
      // 里面的节点报 source=graph，而不是只报到子图这一层。
      auto viaGraph = host.graphParams.find(p.name);
      writeBinds(nodes, byId, p.binds, value,
                 viaGraph == host.graphParams.end() ? nullptr : &viaGraph->second);
    }
    for (auto it = host.params.begin(); it != host.params.end(); ++it) {
      if (declared.count(it.key())) continue;
      auto viaGraph = host.graphParams.find(it.key());
      if (viaGraph != host.graphParams.end()) {
        fail(hostId, "unknown_bind",
             "顶层参数 '" + viaGraph->second + "' 绑到子图 '" + def.id + "' 没有的参数 '" +
                 it.key() + "'",
             it.key());
      } else {
        fail(hostId, "unknown_param", "子图 '" + def.id + "' 没有参数 '" + it.key() + "'",
             it.key());
      }
    }

    auto boundaries = expandLevel(nodes, def.edges, prefix, bypass, stack);

    Boundary b;
    for (const SubInput& in : def.inputs) {
      std::vector<Endpoint> eps;
      for (const auto& t : in.to) {
        std::vector<Endpoint> resolved;
        if (!resolveTargets(boundaries, prefix, t.first, t.second, resolved)) {
          fail(hostId, "unknown_port",
               "子图 '" + def.id + "' 的输入 " + in.name + " 落到不存在的端口 " + t.first + "." +
                   t.second,
               {}, in.name);
          continue;
        }
        eps.insert(eps.end(), resolved.begin(), resolved.end());
      }
      b.inputs[in.name] = std::move(eps);
    }
    for (const SubOutput& o : def.outputs) {
      Endpoint ep;
      if (!resolveSource(boundaries, prefix, o.node, o.port, ep)) {
        fail(hostId, "unknown_port",
             "子图 '" + def.id + "' 的输出 " + o.name + " 来自不存在的端口 " + o.node + "." + o.port,
             {}, o.name);
        continue;
      }
      b.outputs[o.name] = ep;
    }
    return b;
  }

  std::map<std::string, Boundary> expandLevel(const std::vector<RawNode>& nodes,
                                              const std::vector<RawEdge>& edges,
                                              const std::string& prefix, bool bypass,
                                              std::vector<std::string>& stack) {
    std::map<std::string, Boundary> boundaries;
    for (const RawNode& node : nodes) {
      const std::string full = prefix + node.id;
      const SubgraphDef* def = findSubgraphDef(in_, node.op);
      if (!def) {
        if (node.op.rfind(kSubPrefix, 0) == 0 || node.op.rfind(kLibraryIdPrefix, 0) == 0) {
          fail(full, "unknown_op", "找不到子图定义 '" + node.op + "'");
          continue;
        }
        RawNode copy = node;
        copy.id = full;
        // 静音一个子图节点 = 整棵子树都静音（E5 的透传语义逐层成立）
        copy.bypass = node.bypass || bypass;
        out_.nodes.push_back(std::move(copy));
        continue;
      }
      if (std::find(stack.begin(), stack.end(), node.op) != stack.end()) {
        fail(full, "recursive_subgraph", "子图 '" + node.op + "' 直接或间接引用了自己");
        continue;
      }
      if (static_cast<int>(stack.size()) >= kMaxSubgraphDepth) {
        fail(full, "recursive_subgraph",
             "子图嵌套超过 " + std::to_string(kMaxSubgraphDepth) + " 层");
        continue;
      }
      stack.push_back(node.op);
      boundaries[node.id] =
          expandDef(*def, node, full, full + kPathSeparator, bypass || node.bypass, stack);
      stack.pop_back();
    }

    for (const RawEdge& e : edges) {
      Endpoint from;
      if (!resolveSource(boundaries, prefix, e.fromNode, e.fromPort, from)) {
        fail(prefix + e.fromNode, "unknown_port",
             "子图节点没有输出端口 '" + e.fromPort + "'", {}, e.fromPort);
        continue;
      }
      std::vector<Endpoint> targets;
      if (!resolveTargets(boundaries, prefix, e.toNode, e.toPort, targets)) {
        fail(prefix + e.toNode, "unknown_port", "子图节点没有输入端口 '" + e.toPort + "'", {},
             e.toPort);
        continue;
      }
      for (std::size_t i = 0; i < targets.size(); ++i) {
        RawEdge oe;
        oe.id = prefix + e.id + (i == 0 ? std::string() : "#" + std::to_string(i));
        oe.fromNode = from.node;
        oe.fromPort = from.port;
        oe.toNode = targets[i].node;
        oe.toPort = targets[i].port;
        out_.edges.push_back(std::move(oe));
      }
    }
    return boundaries;
  }

  const RawGraph& in_;
  RawGraph& out_;
  Diagnostics& diags_;
  bool ok_ = true;
};

}  // namespace

bool expandGraph(const RawGraph& in, RawGraph& out, Diagnostics& diags) {
  out.id = in.id;
  out.nodes.clear();
  out.edges.clear();
  // 图输出直接写展开后的路径 id（F2 的 `父/子` 形式），所以原样带过来。
  out.outputs = in.outputs;
  Expander expander(in, out, diags);
  return expander.run();
}

}  // namespace lyflow::exec
