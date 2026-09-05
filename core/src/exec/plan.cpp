#include "exec/plan.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "exec/hash.h"
#include "lyflow/json_writer.h"

namespace lyflow::exec {
namespace {

// ------------------------------------------------------------------- semver

struct Semver {
  int major = 0, minor = 0, patch = 0;
  bool valid = false;
};

Semver parseSemver(const std::string& s) {
  Semver v;
  int parts[3] = {0, 0, 0};
  std::size_t idx = 0, start = 0;
  for (std::size_t i = 0; i <= s.size() && idx < 3; ++i) {
    if (i == s.size() || s[i] == '.') {
      if (i == start) return v;
      try {
        parts[idx++] = std::stoi(s.substr(start, i - start));
      } catch (...) {
        return v;
      }
      start = i + 1;
    } else if (s[i] < '0' || s[i] > '9') {
      return v;
    }
  }
  if (idx != 3) return v;
  v.major = parts[0];
  v.minor = parts[1];
  v.patch = parts[2];
  v.valid = true;
  return v;
}

// -------------------------------------------------------------- 参数规整

/// 把 GraphDoc 里的原始 JSON 值按声明类型规整成 Value。
/// 失败时返回 false 并给出人话原因 —— 这句话会原样出现在参数框的悬浮提示里。
bool coerceParam(const Param& p, const nlohmann::json& j, Value& out, std::string& message) {
  auto numbers = [&](std::size_t n, bool allowAlt, std::size_t alt) -> bool {
    if (!j.is_array()) {
      message = "应当是长度 " + std::to_string(n) + " 的数字数组";
      return false;
    }
    if (j.size() != n && !(allowAlt && j.size() == alt)) {
      message = "数组长度应当是 " + std::to_string(n) + "，实际是 " + std::to_string(j.size());
      return false;
    }
    std::vector<double> v;
    v.reserve(j.size());
    for (const auto& e : j) {
      if (!e.is_number()) {
        message = "数组元素必须都是数字";
        return false;
      }
      const double d = e.get<double>();
      if (!std::isfinite(d)) {
        message = "数组元素必须都是有限的数字";
        return false;
      }
      v.push_back(d);
    }
    out = Value::vec(std::move(v));
    return true;
  };

  switch (p.type) {
    case ParamType::Bool:
      if (!j.is_boolean()) { message = "应当是 true / false"; return false; }
      out = Value::boolean(j.get<bool>());
      return true;

    case ParamType::Int: {
      if (!j.is_number()) { message = "应当是整数"; return false; }
      const double d = j.get<double>();
      if (std::floor(d) != d) { message = "应当是整数，实际带小数"; return false; }
      out = Value::integer(static_cast<std::int64_t>(d));
      return true;
    }

    case ParamType::Float: {
      if (!j.is_number()) { message = "应当是数字"; return false; }
      const double d = j.get<double>();
      // 手改过的文件里 1e400 会被解析成 inf，而 inf 通得过任何 min/max 检查，
      // 一路流进算子变成 NaN 结果，最后表现成「3D 视图一片空白」。
      if (!std::isfinite(d)) { message = "必须是有限的数字"; return false; }
      out = Value::number(d);
      return true;
    }

    case ParamType::Vec2f:     return numbers(2, false, 0);
    case ParamType::Vec3f:     return numbers(3, false, 0);
    case ParamType::Vec4f:     return numbers(4, false, 0);
    case ParamType::Color:     return numbers(3, true, 4);
    case ParamType::Transform: return numbers(16, false, 0);

    case ParamType::Enum: {
      if (!j.is_string()) { message = "应当是选项里的字符串"; return false; }
      const std::string v = j.get<std::string>();
      for (const auto& o : p.options) {
        if (o.value == v) { out = Value::text(v); return true; }
      }
      message = "'" + v + "' 不在选项里";
      return false;
    }

    case ParamType::Flags:
      if (!j.is_number_integer()) { message = "应当是整数位掩码"; return false; }
      out = Value::integer(j.get<std::int64_t>());
      return true;

    case ParamType::String:
    case ParamType::Text:
    case ParamType::Path:
      if (!j.is_string()) { message = "应当是字符串"; return false; }
      out = Value::text(j.get<std::string>());
      return true;

    case ParamType::Curve:
      // 曲线编辑器是 P2，形态未定。原样存成字符串，至少不丢数据。
      out = Value::text(j.dump());
      return true;
  }
  message = "未知参数类型";
  return false;
}

/// 硬边界。softMin/softMax 只影响滑块范围，手输超出是合法的，所以这里不查。
bool checkRange(const Param& p, const Value& v, std::string& message) {
  auto one = [&](double d) {
    if (p.min && d < *p.min) {
      message = "不能小于 " + jsonNumber(*p.min);
      return false;
    }
    if (p.max && d > *p.max) {
      message = "不能大于 " + jsonNumber(*p.max);
      return false;
    }
    return true;
  };
  switch (v.kind()) {
    case Value::Kind::Int:   return one(static_cast<double>(v.intValue()));
    case Value::Kind::Float: return one(v.floatValue());
    case Value::Kind::FloatVec:
      for (double d : v.vecValue()) {
        if (!one(d)) return false;
      }
      return true;
    default:
      return true;
  }
}

// ------------------------------------------------------------------ 类型兼容

constexpr const char* kAnyType = "Any";

bool typesCompatible(const Registry& r, const std::string& from, const std::string& to) {
  if (from == to) return true;
  if (from == "Any" || to == "Any") return true;
  const PortType* t = r.findType(from);
  if (!t) return false;
  return std::find(t->castableTo.begin(), t->castableTo.end(), to) != t->castableTo.end();
}

const Port* findPort(const std::vector<Port>& ports, const std::string& name) {
  for (const auto& p : ports) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const Migration* findMigration(const OperatorDesc& op, int fromMajor) {
  for (const Migration& m : op.migrations) {
    if (m.fromMajor == fromMajor && m.apply) return &m;
  }
  return nullptr;
}

// ------------------------------------------------------------------ 参数联动

bool valueEquals(const Value& a, const Value& b) {
  if (a.kind() != b.kind()) return false;
  switch (a.kind()) {
    case Value::Kind::Null:     return true;
    case Value::Kind::Bool:     return a.boolValue() == b.boolValue();
    case Value::Kind::Int:      return a.intValue() == b.intValue();
    case Value::Kind::Float:    return a.floatValue() == b.floatValue();
    case Value::Kind::String:   return a.stringValue() == b.stringValue();
    case Value::Kind::FloatVec: return a.vecValue() == b.vecValue();
  }
  return false;
}

/// 条件未设置视为成立。引用了不存在的参数也视为成立 —— 那是 Registry::validate()
/// 的活，校验期不该因为算子描述的笔误把用户的图判成非法。
bool conditionHolds(const Condition& c, const ParamMap& params) {
  if (!c.isSet()) return true;
  auto it = params.find(c.param);
  if (it == params.end()) return true;
  if (!c.eq.isNull()) return valueEquals(it->second, c.eq);
  if (!c.in.empty()) {
    for (const Value& v : c.in) {
      if (valueEquals(it->second, v)) return true;
    }
    return false;
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------------- Any 推导

const std::string& effectiveType(const std::unordered_map<std::string, std::string>& resolved,
                                 const std::string& port, const std::string& declared) {
  auto it = resolved.find(port);
  return it == resolved.end() ? declared : it->second;
}

// ------------------------------------------------------- 规范化参数 JSON

std::string canonicalParamsJson(const ParamMap& params) {
  // 键排序：cacheKey 必须与「参数在文件里的书写顺序」无关，
  // 否则同一张图存两次就可能算出两个不同的键。
  std::vector<const std::string*> keys;
  keys.reserve(params.size());
  for (const auto& kv : params) keys.push_back(&kv.first);
  std::sort(keys.begin(), keys.end(),
            [](const std::string* a, const std::string* b) { return *a < *b; });

  JsonWriter w;
  w.setIndent(0);
  w.beginObject();
  for (const std::string* k : keys) {
    const Value& v = params.at(*k);
    w.key(*k);
    switch (v.kind()) {
      case Value::Kind::Null:   w.valueNull(); break;
      case Value::Kind::Bool:   w.value(v.boolValue()); break;
      case Value::Kind::Int:    w.value(v.intValue()); break;
      case Value::Kind::Float:  w.value(v.floatValue()); break;
      case Value::Kind::String: w.value(v.stringValue()); break;
      case Value::Kind::FloatVec:
        w.beginArray();
        for (double d : v.vecValue()) w.value(d);
        w.endArray();
        break;
    }
  }
  w.endObject();
  return w.str();
}

// ------------------------------------------------------------------ buildPlan

bool buildPlan(const Registry& registry, const RawGraph& graph, const BuildOptions& options,
               Plan& out, Diagnostics& diags) {
  out.runId = options.runId;
  out.nodes.clear();
  out.ok = false;

  const std::size_t n = graph.nodes.size();
  std::unordered_map<std::string, std::size_t> indexById;
  for (std::size_t i = 0; i < n; ++i) indexById[graph.nodes[i].id] = i;

  // -- 逐节点校验（D5：不早退）--------------------------------------------
  struct Prepared {
    const OperatorDesc* op = nullptr;
    ParamMap params;
    bool valid = true;
    std::vector<Diagnostic> errors;
  };
  std::vector<Prepared> prepared(n);

  auto fail = [&](std::size_t i, std::string code, std::string message,
                  std::string paramPath = {}, std::string portName = {}) {
    Status s = Status::Error(Phase::Validate, std::move(code), std::move(message),
                             std::move(paramPath), std::move(portName));
    prepared[i].valid = false;
    prepared[i].errors.push_back(Diagnostic{graph.nodes[i].id, Severity::Error, s});
    diags.add(graph.nodes[i].id, s, Severity::Error);
  };

  for (std::size_t i = 0; i < n; ++i) {
    const RawNode& rn = graph.nodes[i];
    const OperatorDesc* op = registry.find(rn.op);
    if (!op) {
      fail(i, "unknown_op", "当前 core 没有注册算子 '" + rn.op + "'");
      continue;
    }
    prepared[i].op = op;

    // -- 别名重定向与迁移（E3）。先算出「该用哪份参数」，再拿它去做常规校验。
    nlohmann::json params = rn.params;
    std::vector<std::string> notes;
    bool migrated = false;
    if (op->id != rn.op) {
      notes.push_back("算子 '" + rn.op + "' 已重命名为 '" + op->id + "'");
      migrated = true;
    }

    const Semver current = parseSemver(op->version);
    if (!rn.opVersion.empty()) {
      const Semver saved = parseSemver(rn.opVersion);
      if (saved.valid && current.valid) {
        if (saved.major > current.major) {
          fail(i, "version_mismatch",
               "此节点存于 " + op->id + " v" + rn.opVersion + "，比当前的 v" + op->version +
                   " 还新，无法降级");
        } else if (saved.major < current.major) {
          bool chainOk = true;
          for (int m = saved.major; m < current.major && chainOk; ++m) {
            const Migration* step = findMigration(*op, m);
            if (!step) { chainOk = false; break; }
            try {
              nlohmann::json next = step->apply(params);
              if (!next.is_object()) { chainOk = false; break; }
              params = std::move(next);
            } catch (...) {
              chainOk = false;
              break;
            }
            notes.push_back("v" + std::to_string(m) + " → v" + std::to_string(m + 1) +
                            "：参数已按迁移规则改写");
          }
          if (!chainOk) {
            fail(i, "version_mismatch",
                 "此节点存于 " + op->id + " v" + rn.opVersion + "，当前是 v" + op->version +
                     "（主版本不同，而算子没有提供完整的迁移链）");
          } else {
            migrated = true;
          }
        } else if (saved.minor != current.minor) {
          diags.warn(rn.id, Phase::Validate, "version_mismatch",
                     "此节点存于 v" + rn.opVersion + "，当前是 v" + op->version +
                         "，默认值可能已变更");
        }
      }
    }
    if (migrated && prepared[i].valid) {
      MigrationPlan plan;
      plan.op = op->id;
      plan.opVersion = op->version;
      plan.paramsJson = params.dump();
      plan.notes = notes;
      diags.migration(rn.id, "节点已从 " + rn.op + " v" +
                                 (rn.opVersion.empty() ? std::string("?") : rn.opVersion) +
                                 " 迁移到 " + op->id + " v" + op->version,
                      std::move(plan));
    }

    // 参数：先查未知键，再逐个规整 + 查范围。
    for (auto it = params.begin(); it != params.end(); ++it) {
      if (!findParam(*op, it.key())) {
        fail(i, "unknown_param", "算子没有参数 '" + it.key() + "'", it.key());
      }
    }
    for (const Param& p : op->params) {
      Value v = p.def;
      auto it = params.find(p.name);
      if (it != params.end()) {
        std::string message;
        if (!coerceParam(p, *it, v, message)) {
          fail(i, "bad_param", (p.label.empty() ? p.name : p.label) + "：" + message, p.name);
          v = p.def;  // 用默认值占位，后面的检查还能继续跑
        }
      }
      std::string rangeMessage;
      if (!checkRange(p, v, rangeMessage)) {
        fail(i, "bad_param", (p.label.empty() ? p.name : p.label) + "：" + rangeMessage, p.name);
      }
      prepared[i].params[p.name] = std::move(v);
    }
    // 必填只对**可见**的参数成立（1.6）：被 visibleWhen 藏起来的 path 参数
    // 用户根本没机会选文件，为它标红只会让人找不到那个红框在哪。
    for (const Param& p : op->params) {
      if (p.type != ParamType::Path) continue;
      if (!conditionHolds(p.visibleWhen, prepared[i].params)) continue;
      const Value& v = prepared[i].params[p.name];
      if (v.kind() == Value::Kind::String && v.stringValue().empty()) {
        fail(i, "bad_param", (p.label.empty() ? p.name : p.label) + "：还没有选择文件", p.name);
      }
    }
  }

  // -- 边：端口存在性 -------------------------------------------------------
  std::vector<std::vector<InputBinding>> inputsOf(n);
  std::vector<std::unordered_map<std::string, int>> consumersOf(n);
  std::vector<std::set<std::string>> connectedInputs(n);

  struct Wire {
    std::size_t fi = 0, ti = 0;
    const Port* outPort = nullptr;
    const Port* inPort = nullptr;
    const RawEdge* edge = nullptr;
  };
  std::vector<Wire> wires;
  wires.reserve(graph.edges.size());

  for (const RawEdge& e : graph.edges) {
    const std::size_t fi = indexById.at(e.fromNode);
    const std::size_t ti = indexById.at(e.toNode);
    const OperatorDesc* fromOp = prepared[fi].op;
    const OperatorDesc* toOp = prepared[ti].op;
    if (!fromOp || !toOp) continue;  // unknown_op 已经报过，不再叠加噪声

    const Port* outPort = findPort(fromOp->outputs, e.fromPort);
    const Port* inPort = findPort(toOp->inputs, e.toPort);
    if (!outPort) {
      fail(fi, "unknown_port", "算子没有输出端口 '" + e.fromPort + "'", {}, e.fromPort);
      continue;
    }
    if (!inPort) {
      fail(ti, "unknown_port", "算子没有输入端口 '" + e.toPort + "'", {}, e.toPort);
      continue;
    }
    wires.push_back(Wire{fi, ti, outPort, inPort, &e});
  }

  // -- Any 推导（E6）。一个节点的全部 Any 端口共用一个类型变量 —— reroute 与
  // debug view 这类透传算子正是这个语义，别的算子干脆别声明多个 Any。
  std::vector<std::string> anyType(n);
  auto concreteType = [&](std::size_t node, const Port* port) -> const std::string& {
    return port->type == kAnyType ? anyType[node] : port->type;
  };
  for (std::size_t round = 0; round <= n; ++round) {
    bool changed = false;
    for (const Wire& w : wires) {
      const std::string& fromT = concreteType(w.fi, w.outPort);
      const std::string& toT = concreteType(w.ti, w.inPort);
      if (!fromT.empty() && toT.empty() && w.inPort->type == kAnyType) {
        anyType[w.ti] = fromT;
        changed = true;
      } else if (!toT.empty() && fromT.empty() && w.outPort->type == kAnyType) {
        anyType[w.fi] = toT;
        changed = true;
      }
    }
    if (!changed) break;
  }

  // -- 类型兼容（按推导后的实际类型）+ 装配输入绑定 -------------------------
  for (const Wire& w : wires) {
    const RawEdge& e = *w.edge;
    const std::string& fromT = concreteType(w.fi, w.outPort);
    const std::string& toT = concreteType(w.ti, w.inPort);
    // 推导不出来的一端仍是 Any，不报错：脚本生成的图里孤立的 reroute 很常见。
    const bool bothKnown = !fromT.empty() && !toT.empty();
    if (bothKnown && !typesCompatible(registry, fromT, toT)) {
      fail(w.ti, "type_mismatch",
           "类型不匹配：" + fromT + " → " + toT + "（端口 " + e.toPort + "）", {}, e.toPort);
      continue;
    }
    connectedInputs[w.ti].insert(e.toPort);
    inputsOf[w.ti].push_back(InputBinding{e.toPort, static_cast<int>(w.fi), e.fromPort});
    consumersOf[w.fi][e.fromPort] += 1;
  }

  for (std::size_t i = 0; i < n; ++i) {
    if (!prepared[i].op) continue;
    for (const Port& p : prepared[i].op->inputs) {
      if (p.required && !connectedInputs[i].count(p.name)) {
        fail(i, "missing_input", "必填输入端口 '" + (p.label.empty() ? p.name : p.label) +
                                     "' 没有连线", {}, p.name);
      }
    }
  }

  // -- 拓扑排序（Kahn）。有环是整图级失败：排不出顺序就没法跑。--------------
  std::vector<int> indegree(n, 0);
  std::vector<std::vector<std::size_t>> downstream(n);
  for (const RawEdge& e : graph.edges) {
    const std::size_t fi = indexById.at(e.fromNode);
    const std::size_t ti = indexById.at(e.toNode);
    indegree[ti] += 1;
    downstream[fi].push_back(ti);
  }
  std::vector<std::size_t> order;
  order.reserve(n);
  std::vector<std::size_t> queue;
  for (std::size_t i = 0; i < n; ++i) {
    if (indegree[i] == 0) queue.push_back(i);
  }
  // 用下标顺序而不是 set：同一张图两次编译必须得到同一个拓扑序，
  // 否则 cacheKey 稳定但事件顺序会飘，测试就没法断言。
  std::size_t head = 0;
  std::vector<int> level(n, 0);
  while (head < queue.size()) {
    const std::size_t id = queue[head++];
    order.push_back(id);
    for (std::size_t next : downstream[id]) {
      level[next] = std::max(level[next], level[id] + 1);
      if (--indegree[next] == 0) queue.push_back(next);
    }
  }
  if (order.size() != n) {
    for (std::size_t i = 0; i < n; ++i) {
      if (indegree[i] > 0) {
        diags.error(graph.nodes[i].id, Phase::Compile, "cycle", "此节点在一个环上，无法排出执行顺序");
      }
    }
    return false;
  }

  // -- Run to node：只保留目标的上游闭包 -----------------------------------
  std::vector<bool> keep(n, true);
  if (!options.targets.empty()) {
    keep.assign(n, false);
    std::vector<std::size_t> stack;
    for (const std::string& t : options.targets) {
      // 精确匹配优先；匹配不到时按路径前缀收编整棵子图（F2）。
      auto it = indexById.find(t);
      if (it != indexById.end()) {
        stack.push_back(it->second);
        continue;
      }
      const std::string prefix = t + "/";
      std::size_t matched = 0;
      for (std::size_t i = 0; i < n; ++i) {
        if (graph.nodes[i].id.compare(0, prefix.size(), prefix) == 0) {
          stack.push_back(i);
          matched += 1;
        }
      }
      if (matched == 0) {
        diags.error("", Phase::Compile, "unknown_node", "Run to node 的目标不存在: " + t);
        return false;
      }
    }
    std::vector<std::vector<std::size_t>> upstream(n);
    for (const RawEdge& e : graph.edges) {
      upstream[indexById.at(e.toNode)].push_back(indexById.at(e.fromNode));
    }
    while (!stack.empty()) {
      const std::size_t id = stack.back();
      stack.pop_back();
      if (keep[id]) continue;
      keep[id] = true;
      for (std::size_t up : upstream[id]) stack.push_back(up);
    }
  }

  // -- 装配 Plan ------------------------------------------------------------
  std::vector<int> planIndex(n, -1);
  for (std::size_t id : order) {
    if (!keep[id]) continue;
    planIndex[id] = static_cast<int>(out.nodes.size());
    PlanNode pn;
    pn.id = graph.nodes[id].id;
    pn.op = prepared[id].op;
    pn.level = level[id];
    pn.valid = prepared[id].valid;
    pn.bypass = graph.nodes[id].bypass;
    pn.errors = prepared[id].errors;
    pn.params = std::move(prepared[id].params);
    if (pn.op && !anyType[id].empty()) {
      for (const Port& p : pn.op->inputs) {
        if (p.type == kAnyType) pn.inputTypes[p.name] = anyType[id];
      }
      for (const Port& p : pn.op->outputs) {
        if (p.type == kAnyType) pn.outputTypes[p.name] = anyType[id];
      }
    }
    for (const InputBinding& b : inputsOf[id]) {
      InputBinding remapped = b;
      remapped.fromNode = planIndex[static_cast<std::size_t>(b.fromNode)];
      pn.inputs.push_back(std::move(remapped));
    }
    // 消费者数只统计留在计划里的下游 —— Run to node 时被裁掉的下游不算数
    for (const auto& kv : consumersOf[id]) pn.consumers[kv.first] = 0;
    out.nodes.push_back(std::move(pn));
  }
  for (const RawEdge& e : graph.edges) {
    const int fi = planIndex[indexById.at(e.fromNode)];
    const int ti = planIndex[indexById.at(e.toNode)];
    if (fi < 0 || ti < 0) continue;
    out.nodes[static_cast<std::size_t>(fi)].consumers[e.fromPort] += 1;
  }

  // 并行调度的依赖计数（E2）。按**输入绑定**去重而不是按边：类型不兼容的边
  // 已经被丢掉了，照着边算会让下游永远等一个不会来的上游。
  for (std::size_t i = 0; i < out.nodes.size(); ++i) {
    std::set<int> ups;
    for (const InputBinding& b : out.nodes[i].inputs) {
      if (b.fromNode >= 0) ups.insert(b.fromNode);
    }
    out.nodes[i].upstream.assign(ups.begin(), ups.end());
    for (int u : ups) out.nodes[static_cast<std::size_t>(u)].downstream.push_back(static_cast<int>(i));
  }

  // -- cacheKey：内容寻址（D4）--------------------------------------------
  for (PlanNode& pn : out.nodes) {
    if (!pn.op || !pn.valid) continue;
    Hasher h;
    // 预览命名空间：preview 的结果与正式结果永不互相命中（F5）。
    h.add(options.cacheNamespace);
    h.add(pn.op->id);
    h.add(pn.op->version);
    // bypass 改变的是结果本身，所以必须进键 —— 不然静音再取消静音会拿到旧结果。
    h.add(pn.bypass ? std::string("bypass") : std::string());
    h.add(canonicalParamsJson(pn.params));
    if (pn.op->externalKey) {
      ParamView view(pn.params, options.baseDir);
      h.add(pn.op->externalKey(view));
    } else {
      h.add(std::string{});
    }
    // 上游按端口名排序：输入的书写顺序不该影响键
    std::vector<const InputBinding*> sorted;
    for (const auto& b : pn.inputs) sorted.push_back(&b);
    std::sort(sorted.begin(), sorted.end(),
              [](const InputBinding* a, const InputBinding* b) { return a->port < b->port; });
    for (const InputBinding* b : sorted) {
      h.add(b->port);
      h.add(b->fromPort);
      h.add(b->fromNode >= 0 ? out.nodes[static_cast<std::size_t>(b->fromNode)].cacheKey
                             : std::string{});
    }
    // 不确定性算子（比如带随机种子却不暴露 seed 的）永远不该命中缓存
    if (!pn.op->capabilities.deterministic) h.add(options.runId);
    pn.cacheKey = h.hex();
  }

  out.ok = true;
  return true;
}

}  // namespace lyflow::exec
