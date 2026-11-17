#include "lyflow/graph_migration.h"

#include <stdexcept>

#include "lyflow/registry.h"

namespace lyflow {
using Json = nlohmann::json;

namespace graph_migration {
void require(bool condition, const std::string& why) {
  if (!condition)
    throw std::invalid_argument(why);
}

std::string text(const Json& object, const char* field) {
  require(object.is_object() && object.contains(field) && object[field].is_string() &&
              !object[field].get_ref<const std::string&>().empty(),
          std::string("迁移规则字段必须是非空字符串: ") + field);
  return object[field].get<std::string>();
}

void keys(const Json& object, std::initializer_list<const char*> names) {
  require(object.is_object() && object.size() == names.size(), "迁移规则字段数量不符");
  for (const auto* name : names)
    require(object.contains(name), std::string("迁移规则缺少字段: ") + name);
}

std::size_t nodeIndex(const Json& nodes, const std::string& id) {
  for (std::size_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].value("id", std::string()) == id)
      return i;
  return nodes.size();
}

std::size_t edgeIndex(const Json& edges, const Json& from, const Json& to) {
  for (std::size_t i = 0; i < edges.size(); ++i)
    if (edges[i].value("from", Json()) == from && edges[i].value("to", Json()) == to)
      return i;
  return edges.size();
}

void requireEnabled(const Json& node) {
  require(!node.value("bypass", false),
          "节点 " + node.value("id", std::string()) + " 处于旁路状态，不能旁路单位或坐标契约转换");
}

void insertEdgeAdapter(Json& level, const Json& rule, const std::string& adapterOp,
                       const std::string& adapterPort, Json params) {
  const auto& source = rule.at("source");
  const auto& target = rule.at("target");
  keys(source, {"node", "op", "opVersion", "port"});
  keys(target, {"node", "port"});
  const auto sourceId = text(source, "node");
  const auto adapterId = text(rule, "adapterId");
  auto& nodes = level["nodes"];
  auto& edges = level["edges"];
  const auto si = nodeIndex(nodes, sourceId);
  require(si < nodes.size() && nodes[si].value("op", "") == text(source, "op") &&
              nodes[si].value("opVersion", "") == text(source, "opVersion"),
          "源节点 " + sourceId + " 不是 " + text(source, "op") + "@" + text(source, "opVersion"));
  require(nodeIndex(nodes, text(target, "node")) < nodes.size(),
          "目标节点不存在: " + text(target, "node"));
  require(nodeIndex(nodes, adapterId) == nodes.size(), "适配节点 ID 已被占用: " + adapterId);
  const Json from{{"node", sourceId}, {"port", text(source, "port")}};
  const auto oldEdge = edgeIndex(edges, from, target);
  require(oldEdge < edges.size(),
          "找不到要插入适配节点的连线 " + from.dump() + " → " + target.dump());
  const Json adapter{{"node", adapterId}, {"port", adapterPort}};
  auto output = edges[oldEdge];
  output["from"] = adapter;
  edges.erase(edges.begin() + static_cast<Json::difference_type>(oldEdge));
  edges.push_back({{"id", "contract_" + adapterId + "_in"}, {"from", from}, {"to", adapter}});
  edges.push_back(std::move(output));
  nodes.push_back({{"id", adapterId},
                   {"op", adapterOp},
                   {"opVersion", "1.0.0"},
                   {"params", std::move(params)}});
}
} // namespace graph_migration

namespace {
std::string ruleNode(const Json& rule) {
  for (const char* field : {"source", "target"})
    if (rule.is_object() && rule.contains(field) && rule[field].is_object())
      return rule[field].value("node", std::string());
  return {};
}
} // namespace

bool migrateGraph(const Registry& registry, const Json& graph, const Json& request, Json& migrated,
                  GraphMigrationError& error) {
  using namespace graph_migration;
  error = {};
  try {
    keys(request, {"schemaVersion", "rules"});
    require(request["schemaVersion"] == 1, "未知的迁移请求版本");
    require(request["rules"].is_array(), "迁移请求的 rules 必须是数组");
    migrated = graph;
    require(migrated.is_object() && migrated.value("nodes", Json()).is_array() &&
                migrated.value("edges", Json()).is_array(),
            "迁移需要包含 nodes 与 edges 的图");
    for (const auto& rule : request["rules"]) {
      error.nodeId = ruleNode(rule);
      const auto kind = text(rule, "kind");
      const auto* migration = registry.findGraphMigration(kind);
      require(migration != nullptr, "没有注册迁移规则 " + kind + "，需要启用提供它的包");
      if (!rule.contains("scope")) {
        migration->fn(migrated, rule, nullptr);
        continue;
      }
      const auto subgraph = text(rule["scope"], "subgraph");
      auto& definitions = migrated["subgraphs"];
      require(definitions.is_object() && definitions.contains(subgraph) &&
                  definitions[subgraph].value("nodes", Json()).is_array() &&
                  definitions[subgraph].value("edges", Json()).is_array(),
              "子图定义不存在或缺少 nodes/edges: " + subgraph + "；库图请直接迁移原始图资产");
      migration->fn(definitions[subgraph], rule, &migrated);
    }
    migrated.erase("contractUpgrade");
    error = {};
    return true;
  } catch (const std::exception& e) {
    error.message = e.what();
    return false;
  }
}
} // namespace lyflow
