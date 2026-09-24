#include "exec/graph.h"

#include <algorithm>
#include <set>

#include "lyflow/manifest.h"

namespace lyflow::exec {
namespace {

std::string asString(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

/// 读一个节点。返回 false 表示这一项根本不成形（原因写进 why）。
bool readNode(const nlohmann::json& jn, RawNode& node, std::string& why) {
  if (!jn.is_object()) {
    why = "不是对象";
    return false;
  }
  node.id = asString(jn, "id");
  node.op = asString(jn, "op");
  node.opVersion = asString(jn, "opVersion");
  if (node.id.empty()) {
    why = "id 为空";
    return false;
  }
  if (node.op.empty()) {
    why = "没有声明 op";
    return false;
  }
  auto bypassIt = jn.find("bypass");
  if (bypassIt != jn.end() && bypassIt->is_boolean()) node.bypass = bypassIt->get<bool>();
  auto paramsIt = jn.find("params");
  if (paramsIt != jn.end() && paramsIt->is_object()) node.params = *paramsIt;
  return true;
}

bool readEdge(const nlohmann::json& je, RawEdge& edge, std::string& why) {
  if (!je.is_object()) {
    why = "不是对象";
    return false;
  }
  edge.id = asString(je, "id");
  auto from = je.find("from");
  auto to = je.find("to");
  if (from == je.end() || to == je.end() || !from->is_object() || !to->is_object()) {
    why = "缺少 from/to";
    return false;
  }
  edge.fromNode = asString(*from, "node");
  edge.fromPort = asString(*from, "port");
  edge.toNode = asString(*to, "node");
  edge.toPort = asString(*to, "port");
  return true;
}

void readStringList(const nlohmann::json& j, const char* key, std::vector<std::string>& out) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_array()) return;
  for (const auto& e : *it) {
    if (e.is_string()) out.push_back(e.get<std::string>());
  }
}

/// 一个顶层参数的声明。binds 写成 "节点.参数"，按最后一个 '.' 切：参数名里没有点。
bool parseGraphParam(const std::string& name, const nlohmann::json& j, GraphParam& out,
                     std::string& error) {
  if (name.empty()) {
    error = "params 里有一个空名字";
    return false;
  }
  if (!j.is_object()) {
    error = "顶层参数 '" + name + "' 不是 { default, binds } 对象";
    return false;
  }
  if (!j.contains("default")) {
    error = "顶层参数 '" + name + "' 缺少 default";
    return false;
  }
  auto typeIt = j.find("type");
  if (typeIt != j.end()) {
    ParamType ignored;
    if (!typeIt->is_string() || !parseParamType(typeIt->get<std::string>(), ignored)) {
      error = "顶层参数 '" + name + "' 的 type 不是 manifest 的参数类型名";
      return false;
    }
  }
  auto bindsIt = j.find("binds");
  if (bindsIt == j.end() || !bindsIt->is_array()) {
    error = "顶层参数 '" + name + "' 缺少 binds 数组";
    return false;
  }
  out.name = name;
  out.decl = j;
  for (const auto& b : *bindsIt) {
    const std::string spec = b.is_string() ? b.get<std::string>() : std::string();
    const auto dot = spec.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == spec.size()) {
      error = "顶层参数 '" + name + "' 的绑定写法是 \"节点.参数\"，收到 " +
              (b.is_string() ? spec : b.dump());
      return false;
    }
    out.binds.push_back({spec.substr(0, dot), spec.substr(dot + 1)});
  }
  return true;
}

}  // namespace

bool applyGraphParamValues(const nlohmann::json& values, RawGraph& graph, Diagnostics& diags) {
  if (values.is_null()) return true;
  if (!values.is_object()) {
    diags.error("", Phase::Validate, "bad_input", "顶层参数的取值必须是 JSON 对象 { 名字: 值 }");
    return false;
  }
  bool ok = true;
  for (auto it = values.begin(); it != values.end(); ++it) {
    auto gp = std::find_if(graph.params.begin(), graph.params.end(),
                           [&](const GraphParam& p) { return p.name == it.key(); });
    if (gp == graph.params.end()) {
      diags.error("", Phase::Validate, "unknown_param", "图没有声明顶层参数 '" + it.key() + "'",
                  it.key());
      ok = false;
      continue;
    }
    // 不覆盖 decl 的 default：图里写的那个同样要按规格查（P1.2），报错时也分得清是谁的错
    gp->given = it.value();
  }
  return ok;
}

bool parseSubgraphDef(const nlohmann::json& j, const std::string& id, SubgraphDef& out,
                      std::string& error) {
  if (!j.is_object()) {
    error = "子图 '" + id + "' 不是对象";
    return false;
  }
  out.id = id;
  out.name = asString(j, "name");
  if (out.name.empty()) out.name = asString(j, "label");
  if (out.name.empty()) out.name = id;
  out.doc = asString(j, "doc");
  out.category = asString(j, "category");
  const std::string version = asString(j, "version");
  if (!version.empty()) out.version = version;
  readStringList(j, "keywords", out.keywords);

  std::set<std::string> nodeIds;
  auto nodesIt = j.find("nodes");
  if (nodesIt != j.end() && nodesIt->is_array()) {
    for (const auto& jn : *nodesIt) {
      RawNode node;
      std::string why;
      if (!readNode(jn, node, why)) {
        error = "子图 '" + id + "' 的节点有问题：" + why;
        return false;
      }
      if (!nodeIds.insert(node.id).second) {
        error = "子图 '" + id + "' 里节点 id 重复: " + node.id;
        return false;
      }
      out.nodes.push_back(std::move(node));
    }
  }

  std::set<std::pair<std::string, std::string>> occupied;
  auto edgesIt = j.find("edges");
  if (edgesIt != j.end() && edgesIt->is_array()) {
    for (const auto& je : *edgesIt) {
      RawEdge edge;
      std::string why;
      if (!readEdge(je, edge, why)) {
        error = "子图 '" + id + "' 的边有问题：" + why;
        return false;
      }
      if (!nodeIds.count(edge.fromNode) || !nodeIds.count(edge.toNode)) {
        error = "子图 '" + id + "' 的边 " + edge.id + " 指向不存在的节点";
        return false;
      }
      if (!occupied.insert({edge.toNode, edge.toPort}).second) {
        error = "子图 '" + id + "' 的输入端口 " + edge.toNode + "." + edge.toPort + " 上有多条边";
        return false;
      }
      out.edges.push_back(std::move(edge));
    }
  }

  auto inputsIt = j.find("inputs");
  if (inputsIt != j.end() && inputsIt->is_array()) {
    for (const auto& ji : *inputsIt) {
      SubInput in;
      in.name = asString(ji, "name");
      in.type = asString(ji, "type");
      in.label = asString(ji, "label");
      in.doc = asString(ji, "doc");
      if (in.name.empty() || in.type.empty()) {
        error = "子图 '" + id + "' 的输入缺少 name/type";
        return false;
      }
      auto toIt = ji.find("to");
      if (toIt != ji.end() && toIt->is_array()) {
        for (const auto& jt : *toIt) {
          const std::string node = asString(jt, "node");
          const std::string port = asString(jt, "port");
          if (!nodeIds.count(node)) {
            error = "子图 '" + id + "' 的输入 " + in.name + " 落到不存在的节点 " + node;
            return false;
          }
          in.to.push_back({node, port});
        }
      }
      out.inputs.push_back(std::move(in));
    }
  }

  auto outputsIt = j.find("outputs");
  if (outputsIt != j.end() && outputsIt->is_array()) {
    for (const auto& jo : *outputsIt) {
      SubOutput o;
      o.name = asString(jo, "name");
      o.type = asString(jo, "type");
      o.label = asString(jo, "label");
      o.doc = asString(jo, "doc");
      auto fromIt = jo.find("from");
      if (fromIt != jo.end() && fromIt->is_object()) {
        o.node = asString(*fromIt, "node");
        o.port = asString(*fromIt, "port");
      }
      if (o.name.empty() || o.type.empty() || !nodeIds.count(o.node)) {
        error = "子图 '" + id + "' 的输出 " + o.name + " 缺少 name/type 或来源节点不存在";
        return false;
      }
      out.outputs.push_back(std::move(o));
    }
  }

  auto paramsIt = j.find("params");
  if (paramsIt != j.end() && paramsIt->is_array()) {
    for (const auto& jp : *paramsIt) {
      SubParam p;
      p.name = asString(jp, "name");
      if (p.name.empty()) {
        error = "子图 '" + id + "' 有一个没有 name 的参数";
        return false;
      }
      p.decl = jp;
      auto bindsIt = jp.find("binds");
      if (bindsIt != jp.end() && bindsIt->is_array()) {
        for (const auto& jb : *bindsIt) {
          const std::string node = asString(jb, "node");
          const std::string param = asString(jb, "param");
          if (!nodeIds.count(node)) {
            error = "子图 '" + id + "' 的参数 " + p.name + " 绑到不存在的节点 " + node;
            return false;
          }
          p.binds.push_back({node, param});
        }
      }
      out.params.push_back(std::move(p));
    }
  }
  return true;
}

bool parseGraph(const std::string& json, RawGraph& out, Diagnostics& diags) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(json);
  } catch (const std::exception& e) {
    diags.error("", Phase::Validate, "io", std::string("GraphDoc 不是合法 JSON: ") + e.what());
    return false;
  }
  if (!doc.is_object()) {
    diags.error("", Phase::Validate, "io", "GraphDoc 顶层不是对象");
    return false;
  }

  {
    auto it = doc.find("schemaVersion");
    if (it != doc.end() && it->is_number_integer() && it->get<int>() != 1) {
      diags.error("", Phase::Validate, "bad_input",
                  "schemaVersion " + std::to_string(it->get<int>()) + " 无法识别（本版本支持 1）");
      return false;
    }
  }

  out.id = asString(doc, "id");

  // -- 节点 -----------------------------------------------------------------
  std::set<std::string> nodeIds;
  auto nodesIt = doc.find("nodes");
  if (nodesIt != doc.end() && nodesIt->is_array()) {
    for (const auto& jn : *nodesIt) {
      RawNode node;
      std::string why;
      if (!readNode(jn, node, why)) {
        const std::string id = jn.is_object() ? asString(jn, "id") : std::string();
        const char* code = why == "没有声明 op" ? "unknown_op" : "bad_input";
        diags.error(id, Phase::Validate, code, "节点有问题：" + why);
        continue;
      }
      if (!nodeIds.insert(node.id).second) {
        diags.error(node.id, Phase::Validate, "duplicate_id", "节点 id 重复: " + node.id);
        continue;
      }
      // x / ui / groups 一律忽略：后端不关心坐标（ADR-0002）
      out.nodes.push_back(std::move(node));
    }
  }

  // -- 边 -------------------------------------------------------------------
  std::set<std::string> edgeIds;
  std::set<std::pair<std::string, std::string>> occupiedInputs;
  auto edgesIt = doc.find("edges");
  if (edgesIt != doc.end() && edgesIt->is_array()) {
    for (const auto& je : *edgesIt) {
      RawEdge edge;
      std::string why;
      if (!readEdge(je, edge, why)) {
        diags.error("", Phase::Validate, "bad_input", "边有问题：" + why);
        continue;
      }
      if (!edge.id.empty() && !edgeIds.insert(edge.id).second) {
        diags.error("", Phase::Validate, "duplicate_id", "边 id 重复: " + edge.id);
        continue;
      }
      if (!nodeIds.count(edge.fromNode)) {
        diags.error("", Phase::Validate, "unknown_node",
                    "边 " + edge.id + " 的源节点不存在: " + edge.fromNode);
        continue;
      }
      if (!nodeIds.count(edge.toNode)) {
        diags.error("", Phase::Validate, "unknown_node",
                    "边 " + edge.id + " 的目标节点不存在: " + edge.toNode);
        continue;
      }
      // 输入端口是单连接（docs/graph-doc.md）。把这条挡在编译前，否则
      // 「哪条边赢」就成了隐式的求值顺序，是 bug 温床。
      if (!occupiedInputs.insert({edge.toNode, edge.toPort}).second) {
        diags.error(edge.toNode, Phase::Validate, "multi_input",
                    "输入端口 " + edge.toPort + " 上有多条边（输入端口是单连接）", {},
                    edge.toPort);
        continue;
      }
      out.edges.push_back(std::move(edge));
    }
  }

  // -- 图级命名输出（ADR-0017）。节点/端口是否存在留给 buildPlan：这里还没展开子图。
  auto outputsIt = doc.find("outputs");
  if (outputsIt != doc.end()) {
    if (!outputsIt->is_object()) {
      diags.error("", Phase::Validate, "bad_input", "outputs 必须是对象 { 名字: {node, port} }");
    } else {
      for (auto it = outputsIt->begin(); it != outputsIt->end(); ++it) {
        if (it.key().empty()) {
          diags.error("", Phase::Validate, "bad_input", "outputs 里有一个空名字");
          continue;
        }
        if (!it.value().is_object()) {
          diags.error("", Phase::Validate, "bad_input",
                      "图输出 '" + it.key() + "' 不是 {node, port} 对象");
          continue;
        }
        GraphOutput o;
        o.name = it.key();
        o.node = asString(it.value(), "node");
        o.port = asString(it.value(), "port");
        if (o.node.empty() || o.port.empty()) {
          diags.error("", Phase::Validate, "bad_input",
                      "图输出 '" + o.name + "' 缺少 node 或 port");
          continue;
        }
        out.outputs.push_back(std::move(o));
      }
    }
  }

  // -- 顶层图参数（m7-plan J7）。绑定目标存不存在留给展开阶段：那里才分得清子图实例。
  auto paramsIt = doc.find("params");
  if (paramsIt != doc.end()) {
    if (!paramsIt->is_object()) {
      diags.error("", Phase::Validate, "bad_input",
                  "params 必须是对象 { 名字: { default, binds, ... } }");
    } else {
      for (auto it = paramsIt->begin(); it != paramsIt->end(); ++it) {
        std::string error;
        GraphParam gp;
        if (!parseGraphParam(it.key(), it.value(), gp, error)) {
          diags.error("", Phase::Validate, "bad_input", error);
          continue;
        }
        out.params.push_back(std::move(gp));
      }
    }
  }

  // -- 子图定义（F3）。结构性问题在这里一次报完，展开阶段就不用再防了。
  auto subsIt = doc.find("subgraphs");
  if (subsIt != doc.end() && subsIt->is_object()) {
    for (auto it = subsIt->begin(); it != subsIt->end(); ++it) {
      SubgraphDef def;
      std::string error;
      if (!parseSubgraphDef(it.value(), it.key(), def, error)) {
        diags.error("", Phase::Validate, "bad_input", error);
        continue;
      }
      out.subgraphs[it.key()] = std::move(def);
    }
  }

  return !diags.hasErrors();
}

}  // namespace lyflow::exec
