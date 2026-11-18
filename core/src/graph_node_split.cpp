#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "lyflow/graph_migration.h"

namespace lyflow::graph_migration {
namespace {

using Json = nlohmann::json;
using Locate = std::function<std::optional<std::string>(const std::string&)>;

Json reference(const std::string& node, const std::string& port) {
  return {{"node", node}, {"port", port}};
}

const std::vector<SplitPort>& route(const SplitRoutes& routes, const std::string& node,
                                    const std::string& name, const char* label) {
  const auto found = routes.find(name);
  require(found != routes.end(),
          "节点 " + node + " 的旧" + label + " " + name + " 无法迁移，请检查该端口或参数的用法");
  return found->second;
}

Json rewriteRefs(const Json& ref, const char* key, const SplitRoutes& routes, const Locate& locate,
                 const char* label) {
  if (ref.is_string()) {
    const auto value = ref.get<std::string>();
    const auto dot = value.rfind('.');
    const auto node = value.substr(0, dot);
    const auto prefix = locate(node);
    if (!prefix || dot == std::string::npos)
      return Json::array({ref});
    Json result = Json::array();
    for (const auto& to : route(routes, node, value.substr(dot + 1), label))
      result.push_back(*prefix + to.node + "." + to.port);
    return result;
  }
  const auto node = ref.value("node", std::string());
  const auto prefix = locate(node);
  if (!prefix)
    return Json::array({ref});
  Json result = Json::array();
  for (const auto& to : route(routes, node, text(ref, key), label)) {
    auto next = ref;
    next["node"] = *prefix + to.node;
    next[key] = to.port;
    result.push_back(std::move(next));
  }
  return result;
}

void rewriteList(Json& refs, const char* key, const SplitRoutes& routes, const Locate& locate,
                 const char* label) {
  Json result = Json::array();
  for (const auto& ref : refs)
    for (auto& next : rewriteRefs(ref, key, routes, locate, label))
      result.push_back(std::move(next));
  refs = std::move(result);
}

void rewriteOutput(Json& ref, const SplitRoutes& outputs, const Locate& locate) {
  auto matches = rewriteRefs(ref, "port", outputs, locate, "输出");
  require(matches.size() == 1, "输出没有唯一的迁移映射: " + ref.dump());
  ref = std::move(matches.front());
}

void rewriteInterface(Json& doc, const SplitRoutes& inputs, const SplitRoutes& outputs,
                      const SplitRoutes& params, const Locate& locate) {
  if (auto found = doc.find("inputs"); found != doc.end() && found->is_array())
    for (auto& input : *found)
      if (auto to = input.find("to"); to != input.end())
        rewriteList(*to, "port", inputs, locate, "输入");
  if (auto found = doc.find("outputs"); found != doc.end()) {
    if (found->is_array()) {
      for (auto& output : *found)
        rewriteOutput(output.at("from"), outputs, locate);
    } else {
      for (auto& output : *found)
        rewriteOutput(output, outputs, locate);
    }
  }
  if (auto found = doc.find("params"); found != doc.end())
    for (auto& parameter : *found)
      if (auto binds = parameter.find("binds"); binds != parameter.end()) {
        rewriteList(*binds, "param", params, locate, "参数");
        require(!binds->empty(), "图参数没有可迁移的绑定，请改用新算子的参数");
      }
}

std::optional<std::string> prefixIn(const Json& graph, const Json& definitions, const Json& target,
                                    const std::string& path, const std::string& node) {
  const Json* scope = &graph;
  std::size_t begin = 0;
  for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', begin)) {
    const auto& nodes = scope->at("nodes");
    const auto index = nodeIndex(nodes, path.substr(begin, slash - begin));
    if (index == nodes.size())
      return std::nullopt;
    const auto op = nodes[index].value("op", std::string());
    if (op.compare(0, 4, "sub:") != 0)
      return std::nullopt;
    const auto definition = definitions.find(op.substr(4));
    if (definition == definitions.end())
      return std::nullopt;
    scope = &*definition;
    begin = slash + 1;
  }
  if (scope != &target || path.substr(begin) != node)
    return std::nullopt;
  return path.substr(0, begin);
}

} // namespace

void splitRuleKeys(const Json& rule, std::initializer_list<const char*> names) {
  if (!rule.contains("scope")) {
    keys(rule, names);
    return;
  }
  auto copy = rule;
  copy.erase("scope");
  keys(copy, names);
  keys(rule["scope"], {"subgraph"});
}

const Json& splitSource(const Json& level, const Json& rule, const char* oldOp) {
  const auto& source = rule.at("source");
  keys(source, {"node"});
  const auto id = text(source, "node");
  const auto& nodes = level.at("nodes");
  const auto index = nodeIndex(nodes, id);
  require(index < nodes.size() && nodes[index].value("op", "") == oldOp &&
              nodes[index].value("opVersion", "1.0.0") == "1.0.0",
          "来源节点 " + id + " 不是旧版 " + oldOp);
  requireEnabled(nodes[index]);
  return nodes[index];
}

Json requireSplitMigration(const Json&) {
  throw std::invalid_argument(
      "该算子已拆分为多个节点，请运行 lyflow migrate --request <文件> --write");
}

bool inputConnected(const Json& level, const std::string& node, const std::string& port) {
  const auto target = reference(node, port);
  for (const auto& edge : level.at("edges"))
    if (edge.at("to") == target)
      return true;
  for (const auto& input : level.value("inputs", Json::array()))
    for (const auto& to : input.value("to", Json::array()))
      if (to == target)
        return true;
  return false;
}

std::vector<const Json*> parameterBindings(const Json& level, const Json* outer,
                                           const std::string& node, const std::string& param) {
  std::vector<const Json*> matches;
  auto collect = [&](const Json& doc, const Locate& locate) {
    const auto params = doc.find("params");
    if (params == doc.end())
      return;
    for (const auto& declaration : *params) {
      for (const auto& bind : declaration.value("binds", Json::array())) {
        if (bind.is_string()) {
          const auto& value = bind.get_ref<const std::string&>();
          const auto dot = value.rfind('.');
          if (dot == std::string::npos || value.substr(dot + 1) != param ||
              !locate(value.substr(0, dot)))
            continue;
        } else if (bind.value("param", std::string()) != param ||
                   !locate(bind.value("node", std::string()))) {
          continue;
        }
        matches.push_back(&declaration);
        break;
      }
    }
  };
  collect(level, [&](const std::string& path) -> std::optional<std::string> {
    return path == node ? std::optional<std::string>("") : std::nullopt;
  });
  if (outer) {
    const auto& definitions = outer->at("subgraphs");
    const auto collectNested = [&](const Json& doc) {
      collect(doc, [&](const std::string& path) {
        return prefixIn(doc, definitions, level, path, node);
      });
    };
    collectNested(*outer);
    for (const auto& def : definitions)
      if (&def != &level)
        collectNested(def);
  }
  return matches;
}

void splitNode(Json& level, Json* outer, const std::string& nodeId, Json replacements,
               const SplitRoutes& inputs, const SplitRoutes& outputs, const SplitRoutes& params,
               Json links) {
  auto& nodes = level.at("nodes");
  const auto index = nodeIndex(nodes, nodeId);
  require(index < nodes.size(), "找不到要拆分的节点 " + nodeId);
  const auto& old = nodes[index];
  requireEnabled(old);
  require(replacements.is_array() && !replacements.empty(), "拆分需要至少一个新节点");
  std::set<std::string> ids;
  for (const auto& node : replacements) {
    const auto id = text(node, "id");
    require(ids.insert(id).second && (id == nodeId || nodeIndex(nodes, id) == nodes.size()),
            "拆分节点 ID 已被占用: " + id);
  }
  auto uiIndex = nodeIndex(replacements, nodeId);
  if (uiIndex == replacements.size())
    uiIndex = 0;
  if (old.contains("ui") && !replacements[uiIndex].contains("ui"))
    replacements[uiIndex]["ui"] = old["ui"];
  const auto oldParams = old.value("params", Json::object());
  for (const auto& [name, value] : oldParams.items())
    for (const auto& to : route(params, nodeId, name, "参数"))
      replacements.at(nodeIndex(replacements, to.node))["params"][to.port] = value;

  const Locate local = [&](const std::string& path) -> std::optional<std::string> {
    return path == nodeId ? std::optional<std::string>("") : std::nullopt;
  };
  rewriteInterface(level, inputs, outputs, params, local);
  if (outer) {
    auto& definitions = outer->at("subgraphs");
    auto rewrite = [&](Json& doc) {
      const Locate nested = [&](const std::string& path) {
        return prefixIn(doc, definitions, level, path, nodeId);
      };
      rewriteInterface(doc, inputs, outputs, params, nested);
    };
    rewrite(*outer);
    for (auto& def : definitions)
      if (&def != &level)
        rewrite(def);
  }

  auto& edges = level.at("edges");
  auto next = Json::array();
  std::set<std::string> edgeIds;
  for (const auto& edge : edges)
    edgeIds.insert(edge.value("id", std::string()));
  std::size_t sequence = 0;
  auto addId = [&](Json& edge) {
    std::string id;
    do {
      id = nodeId + "_split_" + std::to_string(++sequence);
    } while (!edgeIds.insert(id).second);
    edge["id"] = std::move(id);
  };
  for (auto edge : edges) {
    rewriteOutput(edge.at("from"), outputs, local);
    const auto targets = rewriteRefs(edge.at("to"), "port", inputs, local, "输入");
    require(!targets.empty(), "旧输入没有迁移映射: " + edge.at("to").dump());
    for (std::size_t i = 0; i < targets.size(); ++i) {
      edge["to"] = targets[i];
      if (i)
        addId(edge);
      next.push_back(edge);
    }
  }
  for (auto& link : links) {
    addId(link);
    next.push_back(std::move(link));
  }
  edges = std::move(next);
  const auto place = nodes.erase(nodes.begin() + static_cast<Json::difference_type>(index));
  nodes.insert(place, replacements.begin(), replacements.end());
}

} // namespace lyflow::graph_migration
