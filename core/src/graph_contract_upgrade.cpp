#include "lyflow/graph_contract_upgrade.h"

#include <algorithm>
#include <set>
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
