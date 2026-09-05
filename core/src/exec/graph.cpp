#include "exec/graph.h"

#include <set>

namespace lyflow::exec {
namespace {

std::string asString(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

}  // namespace

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
      if (!jn.is_object()) {
        diags.error("", Phase::Validate, "bad_input", "nodes 里有一项不是对象");
        continue;
      }
      RawNode node;
      node.id = asString(jn, "id");
      node.op = asString(jn, "op");
      node.opVersion = asString(jn, "opVersion");
      if (node.id.empty()) {
        diags.error("", Phase::Validate, "bad_input", "存在 id 为空的节点");
        continue;
      }
      if (!nodeIds.insert(node.id).second) {
        diags.error(node.id, Phase::Validate, "duplicate_id", "节点 id 重复: " + node.id);
        continue;
      }
      if (node.op.empty()) {
        diags.error(node.id, Phase::Validate, "unknown_op", "节点没有声明 op");
        continue;
      }
      auto bypassIt = jn.find("bypass");
      if (bypassIt != jn.end() && bypassIt->is_boolean()) node.bypass = bypassIt->get<bool>();

      auto paramsIt = jn.find("params");
      if (paramsIt != jn.end() && paramsIt->is_object()) node.params = *paramsIt;

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
      if (!je.is_object()) {
        diags.error("", Phase::Validate, "bad_input", "edges 里有一项不是对象");
        continue;
      }
      RawEdge edge;
      edge.id = asString(je, "id");
      auto from = je.find("from");
      auto to = je.find("to");
      if (from == je.end() || to == je.end() || !from->is_object() || !to->is_object()) {
        diags.error("", Phase::Validate, "bad_input", "边 " + edge.id + " 缺少 from/to");
        continue;
      }
      edge.fromNode = asString(*from, "node");
      edge.fromPort = asString(*from, "port");
      edge.toNode = asString(*to, "node");
      edge.toPort = asString(*to, "port");

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

  return !diags.hasErrors();
}

}  // namespace lyflow::exec
