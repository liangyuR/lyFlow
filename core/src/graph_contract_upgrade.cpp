#include "lyflow/graph_contract_upgrade.h"

#include <algorithm>
#include <set>
#include <map>
#include <tuple>
#include <vector>
#include <stdexcept>

namespace lyflow {
namespace {
using Json = nlohmann::json;

void require(bool condition, const char* why) {
  if (!condition)
    throw std::invalid_argument(why);
}

std::string text(const Json& object, const char* field) {
  require(object.is_object() && object.contains(field) && object[field].is_string(),
          "契约升级字段必须为字符串");
  const auto result = object[field].get<std::string>();
  require(!result.empty() && result.size() <= 1024 && result.find('\0') == std::string::npos,
          "契约升级字段必须非空且不超过1024字节");
  return result;
}

void keys(const Json& object, std::set<std::string> expected) {
  require(object.is_object() && object.size() == expected.size(), "契约升级字段数量不符");
  for (const auto& item : object.items())
    require(expected.count(item.key()) != 0, "契约升级包含未知字段");
}

std::size_t nodeIndex(const Json& nodes, const std::string& id) {
  std::size_t result = nodes.size();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].value("id", std::string()) == id) {
      require(result == nodes.size(), "契约升级引用了重名节点");
      result = i;
    }
  }
  return result;
}

std::size_t edgeIndex(const Json& edges, const Json& from, const Json& to) {
  std::size_t result = edges.size();
  for (std::size_t i = 0; i < edges.size(); ++i) {
    if (edges[i].value("from", Json()) == from && edges[i].value("to", Json()) == to) {
      require(result == edges.size(), "契约升级不能处理重复连线");
      result = i;
    }
  }
  return result;
}

void enabled(const Json& node) {
  require(!node.contains("bypass") || (node["bypass"].is_boolean() && !node["bypass"].get<bool>()),
          "契约升级节点必须启用，不能旁路单位或坐标契约转换");
}

void upgradeDtsProfile(Json& graph, const Json& rule, const Json* outer = nullptr) {
  auto fields = std::set<std::string>{"kind", "source", "profile", "adapterId"};
  if (rule.contains("scope"))
    fields.insert("scope");
  keys(rule, fields);
  const auto& source = rule.at("source");
  keys(source, {"node", "op", "opVersion", "port"});
  require(text(source, "op") == "dts.profile_in" && text(source, "opVersion") == "1.0.0" &&
              text(source, "port") == "profile",
          "DTS轮廓升级需要已知dts.profile_in@1.0.0:profile来源证据");
  const auto& profile = rule.at("profile");
  keys(profile, {"frame", "sourceId", "unit", "plane", "handedness", "sampleOrder"});
  text(profile, "frame");
  text(profile, "sourceId");
  require(
      text(profile, "unit") == "mm" && text(profile, "plane") == "XZ" &&
          text(profile, "sampleOrder") == "acquisition" &&
          (text(profile, "handedness") == "right" || text(profile, "handedness") == "reflected"),
      "DTS v1轮廓必须明确声明毫米/XZ/采集序及手性");
  const auto sourceId = text(source, "node");
  const auto adapterId = text(rule, "adapterId");
  require(sourceId.find('/') == std::string::npos,
          "嵌套DTS来源须先内联为可审查图，再显式升级；不能猜测共享库实例");
  auto& nodes = graph["nodes"];
  auto& edges = graph["edges"];
  const auto si = nodeIndex(nodes, sourceId);
  require(si < nodes.size() && nodes[si].value("op", "") == "dts.profile_in" &&
              nodes[si].value("opVersion", "") == "1.0.0",
          "DTS轮廓来源节点或精确v1版本不符");
  enabled(nodes[si]);
  require(nodeIndex(nodes, adapterId) == nodes.size(), "DTS Profile适配ID已占用，拒绝重复转换");
  std::map<std::string, std::string> chain{{"dts.profile_in", sourceId}};
  auto discover = [&](const std::string& fromOp, const char* fromPort, const char* targetOp,
                      const char* targetPort) {
    const auto from = chain.at(fromOp);
    std::string found;
    for (const auto& edge : edges) {
      if (edge["from"].value("node", "") != from || edge["from"].value("port", "") != fromPort ||
          edge["to"].value("port", "") != targetPort)
        continue;
      const auto id = text(edge["to"], "node");
      const auto i = nodeIndex(nodes, id);
      if (i < nodes.size() && nodes[i].value("op", "") == targetOp) {
        require(found.empty(), "同源DTS链含多个候选，必须分别显式选择完整链");
        found = id;
      }
    }
    require(!found.empty(), "DTS轮廓升级需要完整同源8节点消费链，缺失或外部来源不能猜测");
    chain[targetOp] = found;
  };
  discover("dts.profile_in", "profile", "dts.profile_clean", "profile");
  discover("dts.profile_clean", "clean", "dts.split_faces", "clean");
  discover("dts.split_faces", "faces", "dts.seal_dome", "faces");
  discover("dts.seal_dome", "dome", "dts.pick_metal", "dome");
  discover("dts.pick_metal", "pick", "dts.seal_root", "pick");
  discover("dts.seal_root", "root", "dts.flush", "root");
  discover("dts.flush", "flush", "dts.profile_bundle", "flush");
  using Link = std::tuple<const char*, const char*, const char*, const char*>;
  const std::vector<Link> links = {{"dts.profile_in", "profile", "dts.profile_clean", "profile"},
                                   {"dts.profile_clean", "clean", "dts.split_faces", "clean"},
                                   {"dts.profile_clean", "clean", "dts.seal_dome", "clean"},
                                   {"dts.profile_clean", "clean", "dts.pick_metal", "clean"},
                                   {"dts.profile_clean", "clean", "dts.seal_root", "clean"},
                                   {"dts.split_faces", "faces", "dts.seal_dome", "faces"},
                                   {"dts.split_faces", "faces", "dts.pick_metal", "faces"},
                                   {"dts.split_faces", "faces", "dts.seal_root", "faces"},
                                   {"dts.split_faces", "faces", "dts.profile_bundle", "faces"},
                                   {"dts.seal_dome", "dome", "dts.pick_metal", "dome"},
                                   {"dts.seal_dome", "dome", "dts.seal_root", "dome"},
                                   {"dts.seal_dome", "dome", "dts.profile_bundle", "dome"},
                                   {"dts.pick_metal", "pick", "dts.seal_root", "pick"},
                                   {"dts.pick_metal", "pick", "dts.profile_bundle", "pick"},
                                   {"dts.pick_metal", "metal", "dts.flush", "metal"},
                                   {"dts.pick_metal", "metal", "dts.profile_bundle", "metal"},
                                   {"dts.seal_root", "root", "dts.flush", "root"},
                                   {"dts.seal_root", "root", "dts.profile_bundle", "root"},
                                   {"dts.flush", "flush", "dts.profile_bundle", "flush"},
                                   {"dts.flush", "foot", "dts.profile_bundle", "foot"}};
  Json expected = Json::array();
  auto endpoint = [](const std::string& id, const std::string& port) {
    return Json{{"node", id}, {"port", port}};
  };
  std::set<std::string> ids;
  for (const auto& [op, id] : chain) {
    const auto i = nodeIndex(nodes, id);
    require(nodes[i].value("opVersion", "") == "1.0.0", "DTS消费链必须全部精确为1.0.0");
    enabled(nodes[i]);
    require(!nodes[i].contains("params") || nodes[i]["params"].is_object(),
            "DTS节点参数必须为对象");
    ids.insert(id);
  }
  for (const auto& [from, fp, to, tp] : links) {
    const auto f = endpoint(chain.at(from), fp), t = endpoint(chain.at(to), tp);
    require(edgeIndex(edges, f, t) < edges.size(), "DTS消费链连线不完整或混合来源");
    expected.push_back({{"from", f}, {"to", t}});
  }
  std::vector<std::size_t> bridges;
  for (const auto& edge : edges) {
    if (!ids.count(edge["from"].value("node", "")) && !ids.count(edge["to"].value("node", "")))
      continue;
    if (std::any_of(expected.begin(), expected.end(), [&](const Json& link) {
          return edge["from"] == link["from"] && edge["to"] == link["to"];
        }))
      continue;
    const auto fromId = edge["from"].value("node", "");
    if (fromId == sourceId && !ids.count(edge["to"].value("node", "")))
      continue;
    const auto bi = nodeIndex(nodes, edge["to"].value("node", ""));
    if (ids.count(fromId) && bi < nodes.size() &&
        nodes[bi].value("op", "") == "dts.geometry_to_metric") {
      const auto& bridge = nodes[bi];
      enabled(bridge);
      const auto op = nodes[nodeIndex(nodes, fromId)].value("op", "");
      const auto port = edge["from"].value("port", "");
      require(
          bridge.value("opVersion", "") == "1.0.0" && edge["to"].value("port", "") == "geometry" &&
              bridge.value("params", Json()).value("sourceContract", "") == "dts.geometry.v1" &&
              bridge["params"].value("sourceOp", "") == op &&
              ((op == "dts.seal_dome" && port == "apex") ||
               (op == "dts.pick_metal" && port == "metal") ||
               (op == "dts.seal_root" && port == "point") || (op == "dts.flush" && port == "foot")),
          "已有DTS几何桥的版本/来源/端口证据不符");
      bridges.push_back(bi);
      continue;
    }
    throw std::invalid_argument("DTS消费链存在未声明外部消费边: " + edge.dump() +
                                "；请先显式适配或移除外部消费边");
  }
  auto bound = [&](const std::string& id, const std::string& name) {
    const auto hasBinding = [&](const Json& owner) {
      if (!owner.contains("params"))
        return false;
      for (const auto& parameter : owner["params"]) {
        if (!parameter.is_object() || !parameter.contains("binds") ||
            !parameter["binds"].is_array())
          continue;
        for (const auto& bind : parameter["binds"])
          if ((bind.is_string() &&
               (bind.get<std::string>() == id + "." + name ||
                (outer && bind.get<std::string>().size() > id.size() + name.size() + 2 &&
                 bind.get<std::string>().compare(
                     bind.get<std::string>().size() - id.size() - name.size() - 2,
                     id.size() + name.size() + 2, "/" + id + "." + name) == 0))) ||
              (bind.is_object() && bind.value("node", "") == id && bind.value("param", "") == name))
            return true;
      }
      return false;
    };
    return hasBinding(graph) || (outer && hasBinding(*outer));
  };
  for (const auto& [op, id] : chain) {
    if (op == "dts.profile_in")
      continue;
    auto& node = nodes[nodeIndex(nodes, id)];
    if (!node.contains("params"))
      node["params"] = Json::object();
    if (op == "dts.profile_clean") {
      node["op"] = "profile.clean";
      const Json defaults{{"smoothPoints", 5},
                          {"minIntensity", 0.5},
                          {"minValidPoints", 16},
                          {"mode", "preserve_slots"}};
      for (const auto& item : defaults.items())
        if (!node["params"].contains(item.key()) && !bound(id, item.key()))
          node["params"][item.key()] = item.value();
    } else if (op == "dts.split_faces") {
      node["op"] = "profile.split_faces";
      const Json defaults{{"gapMm", 1.0},    {"jumpMm", 1.5},    {"minPoints", 15},
                          {"fitLenMm", 5.0}, {"maxRmsMm", 0.05}, {"angleBreakDeg", 6.0},
                          {"minLenMm", 3.0}};
      for (const auto& item : defaults.items())
        if (!node["params"].contains(item.key()) && !bound(id, item.key()))
          node["params"][item.key()] = item.value();
    } else {
      node["opVersion"] = "2.0.0";
    }
  }
  for (const auto i : bridges) {
    nodes[i]["opVersion"] = "2.0.0";
    nodes[i]["params"]["sourceContract"] = "dts.geometry.v2";
  }
  const auto oldTarget = endpoint(chain.at("dts.profile_clean"), "profile");
  const auto ai = endpoint(adapterId, "cloud");
  for (auto& edge : edges)
    if (edge["from"] == endpoint(sourceId, "profile") && edge["to"] == oldTarget)
      edge["from"] = endpoint(adapterId, "profile");
  auto addEdge = [&](const std::string& id, const Json& from, const Json& to) {
    for (const auto& edge : edges)
      require(edge.value("id", "") != id, "DTS升级新连线ID已占用");
    edges.push_back({{"id", id}, {"from", from}, {"to", to}});
  };
  addEdge("contract_" + adapterId + "_in", endpoint(sourceId, "profile"), ai);
  addEdge("contract_" + adapterId + "_foot", endpoint(chain.at("dts.flush"), "footInfo"),
          endpoint(chain.at("dts.profile_bundle"), "footInfo"));
  nodes.push_back({{"id", adapterId},
                   {"op", "profile.from_cloud"},
                   {"opVersion", "1.0.0"},
                   {"params", profile}});
}
} // namespace

bool upgradeGraphContracts(const Json& graph, const Json& request, Json& upgraded, Json& report,
                           GraphContractUpgradeError& error) {
  error = {};
  try {
    keys(request, {"schemaVersion", "rules"});
    require(request["schemaVersion"].is_number_integer() && request["schemaVersion"] == 1,
            "未知契约升级版本，拒绝自动推断");
    require(graph.is_object() && graph.contains("schemaVersion") &&
                graph["schemaVersion"].is_number_integer() && graph["schemaVersion"] == 1,
            "契约升级只支持明确的GraphDoc schemaVersion=1");
    require(request["rules"].is_array() && request["rules"].size() <= 1024,
            "契约升级rules必须为最多1024项的数组");
    require(graph.contains("nodes") && graph["nodes"].is_array() && graph.contains("edges") &&
                graph["edges"].is_array(),
            "契约升级需要完整的nodes/edges数组");
    const bool profileUpgrade =
        std::any_of(request["rules"].begin(), request["rules"].end(), [](const Json& r) {
          return r.is_object() && r.value("kind", "") == "dtsProfile.v1";
        });
    if (profileUpgrade) {
      if (!graph.contains("contractUpgrade") && graph.contains("meta") &&
          graph["meta"].is_object() && graph["meta"].contains("contractUpgradeHistory") &&
          graph["meta"]["contractUpgradeHistory"].is_array()) {
        for (const auto& entry : graph["meta"]["contractUpgradeHistory"])
          if (entry.is_object() && entry.value("request", Json()) == request) {
            upgraded = graph;
            report = Json::array();
            error = {};
            return true;
          }
      }
      Json candidate = graph;
      Json priorRules = Json::array();
      for (const auto& rule : request["rules"])
        if (rule.value("kind", "") != "dtsProfile.v1")
          priorRules.push_back(rule);
      if (!priorRules.empty()) {
        Json priorReport;
        const Json priorRequest{{"schemaVersion", 1}, {"rules", priorRules}};
        if (!upgradeGraphContracts(graph, priorRequest, candidate, priorReport, error))
          return false;
      }
      for (const auto& rule : request["rules"])
        if (rule.value("kind", "") == "dtsProfile.v1") {
          error.nodeId = rule.at("source").value("node", "");
          if (rule.contains("scope")) {
            keys(rule["scope"], {"subgraph"});
            const auto scope = text(rule["scope"], "subgraph");
            require(candidate.contains("subgraphs") && candidate["subgraphs"].is_object() &&
                        candidate["subgraphs"].contains(scope) &&
                        candidate["subgraphs"][scope].is_object(),
                    "显式DTS子图作用域不存在；库图请直接迁移原始图资产");
            auto& level = candidate["subgraphs"][scope];
            require(level.contains("nodes") && level["nodes"].is_array() &&
                        level.contains("edges") && level["edges"].is_array(),
                    "显式DTS子图必须包含完整nodes/edges");
            upgradeDtsProfile(level, rule, &candidate);
          } else {
            upgradeDtsProfile(candidate, rule);
          }
        }
      require(!candidate.contains("meta") || candidate["meta"].is_object(),
              "契约升级历史需要对象meta");
      auto& meta = candidate["meta"];
      if (meta.is_null())
        meta = Json::object();
      auto& history = meta["contractUpgradeHistory"];
      require(history.is_null() || history.is_array(), "契约升级历史必须为数组");
      if (history.is_null())
        history = Json::array();
      Json sources = Json::array();
      for (const auto& rule : request["rules"])
        sources.push_back(rule.at("source"));
      const Json entry{{"schemaVersion", 1}, {"request", request}, {"appliedSources", sources}};
      if (std::find(history.begin(), history.end(), entry) == history.end())
        history.push_back(entry);
      candidate.erase("contractUpgrade");
      const Json update{{"contractUpgradeReplacement", {{"before", graph}, {"after", candidate}}}};
      report = Json::array({{{"nodeId", ""},
                             {"op", ""},
                             {"opVersion", ""},
                             {"params", Json::object()},
                             {"source", sources},
                             {"edits",
                              {{"removeEdges", Json::array()},
                               {"addNodes", Json::array()},
                               {"addEdges", Json::array()}}},
                             {"graphUpdate", update}}});
      upgraded = std::move(candidate);
      error = {};
      return true;
    }
    Json candidate = graph;
    Json changes = Json::array();
    Json requirements = Json::array();
    auto& nodes = candidate["nodes"];
    auto& edges = candidate["edges"];
    std::set<std::string> adapterIds;
    for (const auto& rule : request["rules"]) {
      error.nodeId.clear();
      if (rule.is_object() && rule.contains("target") && rule["target"].is_object())
        error.nodeId = rule["target"].value("node", std::string());
      const auto kind = text(rule, "kind");
      keys(rule, kind == "planeCalib.v1"
                     ? std::set<std::string>{"kind", "source", "target", "adapterId", "missingUnit",
                                             "imageFrame", "planeFrame"}
                     : std::set<std::string>{"kind", "source", "target", "adapterId"});
      const auto& source = rule.at("source");
      const auto& target = rule.at("target");
      keys(source, {"node", "op", "opVersion", "port"});
      keys(target, {"node", "port"});
      const auto sourceId = text(source, "node");
      const auto sourceOp = text(source, "op");
      const auto sourceVersion = text(source, "opVersion");
      const auto sourcePort = text(source, "port");
      const auto targetId = text(target, "node");
      error.nodeId = targetId;
      text(target, "port");
      const auto adapterId = text(rule, "adapterId");
      require(adapterIds.insert(adapterId).second && adapterId != sourceId && adapterId != targetId,
              "契约升级adapterId必须独立且唯一");
      const auto si = nodeIndex(nodes, sourceId), ti = nodeIndex(nodes, targetId);
      require(si < nodes.size() && ti < nodes.size(), "契约升级源或目标节点不存在");
      text(nodes[ti], "op");
      require(!nodes[ti].contains("params") || nodes[ti]["params"].is_object(),
              "契约升级目标节点参数必须为对象");
      require(nodes[si].value("op", std::string()) == sourceOp &&
                  nodes[si].value("opVersion", std::string()) == sourceVersion &&
                  sourceVersion == "1.0.0",
              "契约升级要求精确匹配已知源算子版本，缺失或未知版本不能猜测");
      std::string adapterOp, port;
      Json params;
      if (kind == "planeCalib.v1") {
        require((sourceOp == "image.load_calib" || sourceOp == "image.board_calib") &&
                    sourcePort == "calib",
                "PlaneCalib升级只接受已知v1标定生产者的calib端口");
        const auto unit = text(rule, "missingUnit");
        require(unit == "m" || unit == "mm", "旧PlaneCalib必须明确声明缺失单位m或mm");
        params = {{"missingUnit", unit},
                  {"imageFrame", text(rule, "imageFrame")},
                  {"planeFrame", text(rule, "planeFrame")}};
        adapterOp = "image.upgrade_plane_calib";
        port = "calib";
      } else if (kind == "dtsGeometry.v1") {
        require((sourceOp == "dts.seal_dome" && sourcePort == "apex") ||
                    (sourceOp == "dts.pick_metal" && sourcePort == "metal") ||
                    (sourceOp == "dts.seal_root" && sourcePort == "point") ||
                    (sourceOp == "dts.flush" && sourcePort == "foot"),
                "旧DTS毫米几何只接受明确的v1点/线端口，不转换Measurement或未知来源");
        params = {{"sourceContract", "dts.geometry.v1"}, {"sourceOp", sourceOp}};
        adapterOp = "dts.geometry_to_metric";
        port = "geometry";
      } else {
        throw std::invalid_argument("未知契约升级规则，拒绝自动推断");
      }
      const Json from{{"node", sourceId}, {"port", sourcePort}};
      const Json adapterEndpoint{{"node", adapterId}, {"port", port}};
      const Json inserted{
          {"id", adapterId}, {"op", adapterOp}, {"opVersion", "1.0.0"}, {"params", params}};
      requirements.push_back(
          {{"source", source}, {"target", target}, {"adapter", inserted}, {"port", port}});
      const auto existing = nodeIndex(nodes, adapterId);
      const auto oldEdge = edgeIndex(edges, from, target);
      if (existing < nodes.size()) {
        require(!nodes[existing].contains("bypass") || (nodes[existing]["bypass"].is_boolean() &&
                                                        !nodes[existing]["bypass"].get<bool>()),
                "已有契约适配节点必须启用，不能旁路单位或坐标契约转换");
        require(nodes[existing].value("op", std::string()) == adapterOp &&
                    nodes[existing].value("opVersion", std::string()) == "1.0.0" &&
                    nodes[existing].value("params", Json()) == params && oldEdge == edges.size() &&
                    edgeIndex(edges, from, adapterEndpoint) < edges.size() &&
                    edgeIndex(edges, adapterEndpoint, target) < edges.size(),
                "已有适配节点与原版本证据或升级拓扑不符，拒绝重复转换");
        continue;
      }
      require(oldEdge < edges.size(), "契约升级必须精确命中用户指定的原消费连线");
      const Json removed = edges[oldEdge];
      edges.erase(edges.begin() + static_cast<Json::difference_type>(oldEdge));
      const Json inputEdge{
          {"id", "contract_" + adapterId + "_in"}, {"from", from}, {"to", adapterEndpoint}};
      for (const auto& edge : edges)
        require(edge.value("id", std::string()) != inputEdge["id"],
                "契约升级新增连线ID与现有连线冲突");
      Json outputEdge = removed;
      outputEdge["from"] = adapterEndpoint;
      nodes.push_back(inserted);
      edges.push_back(inputEdge);
      edges.push_back(outputEdge);
      changes.push_back({{"nodeId", targetId},
                         {"op", nodes[ti].at("op")},
                         {"opVersion", nodes[ti].value("opVersion", "")},
                         {"params", nodes[ti].value("params", Json::object())},
                         {"source", source},
                         {"edits",
                          {{"removeEdges", Json::array({removed})},
                           {"addNodes", Json::array({inserted})},
                           {"addEdges", Json::array({inputEdge, outputEdge})}}}});
    }
    require(!candidate.contains("meta") || candidate["meta"].is_object(),
            "契约升级历史需要对象类型meta");
    auto& meta = candidate["meta"];
    if (meta.is_null())
      meta = Json::object();
    require(!meta.contains("contractUpgradeHistory") || meta["contractUpgradeHistory"].is_array(),
            "契约升级历史必须是数组，不能覆盖已有未知数据");
    auto& history = meta["contractUpgradeHistory"];
    if (history.is_null())
      history = Json::array();
    Json sources = Json::array();
    for (const auto& rule : request["rules"])
      sources.push_back(rule["source"]);
    const Json entry{{"schemaVersion", 1}, {"request", request}, {"appliedSources", sources}};
    if (std::find(history.begin(), history.end(), entry) == history.end())
      history.push_back(entry);
    candidate.erase("contractUpgrade");
    if (changes.empty() && graph.contains("contractUpgrade"))
      changes.push_back({{"nodeId", ""},
                         {"op", ""},
                         {"opVersion", ""},
                         {"params", Json::object()},
                         {"source", Json::object()},
                         {"edits",
                          {{"removeEdges", Json::array()},
                           {"addNodes", Json::array()},
                           {"addEdges", Json::array()}}}});
    if (!changes.empty())
      changes.back()["graphUpdate"] = {
          {"contractUpgradeRetirement",
           {{"request", request}, {"historyEntry", entry}, {"requirements", requirements}}}};
    upgraded = std::move(candidate);
    report = std::move(changes);
    error = {};
    return true;
  } catch (const std::exception& e) {
    error.message = e.what();
    return false;
  }
}
} // namespace lyflow
