// DTS 旧图的离线迁移规则（lyflow migrate）：
//   dtsProfile.v1  —— dts.profile_in 起的整条 v1 毫米/XZ 消费链改成原生 Profile 链；
//   dtsGeometry.v1 —— 在一条 v1 毫米几何连线上插入 dts.geometry_to_metric。
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "dts_ops.h"
#include "lyflow/graph_migration.h"

namespace lyflow::dts {
namespace {
using Json = nlohmann::json;
using namespace lyflow::graph_migration;

Json endpoint(const std::string& node, const std::string& port) {
  return Json{{"node", node}, {"port", port}};
}

/// owner（整图或外层图）里是否有图参数绑定到 node.param。outer 非空时也认 <实例>/<node>.<param>。
bool bound(const Json& owner, const std::string& node, const std::string& param, bool nested) {
  const auto direct = node + "." + param;
  const auto suffix = "/" + direct;
  for (const auto& parameter : owner.value("params", Json::array())) {
    if (!parameter.is_object())
      continue;
    for (const auto& bind : parameter.value("binds", Json::array())) {
      if (bind.is_object() && bind.value("node", "") == node && bind.value("param", "") == param)
        return true;
      if (!bind.is_string())
        continue;
      const auto& text = bind.get_ref<const std::string&>();
      if (text == direct || (nested && text.size() > suffix.size() &&
                             text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0))
        return true;
    }
  }
  return false;
}

void migrateProfile(Json& graph, const Json& rule, Json* outer) {
  const auto& source = rule.at("source");
  keys(source, {"node", "op", "opVersion", "port"});
  require(text(source, "op") == "dts.profile_in" && text(source, "opVersion") == "1.0.0" &&
              text(source, "port") == "profile",
          "dtsProfile.v1 的来源必须是 dts.profile_in@1.0.0 的 profile 端口");
  const auto& profile = rule.at("profile");
  keys(profile, {"frame", "sourceId", "unit", "plane", "handedness", "sampleOrder"});
  text(profile, "frame");
  text(profile, "sourceId");
  require(profile["unit"] == "mm" && profile["plane"] == "XZ" &&
              profile["sampleOrder"] == "acquisition" &&
              (profile["handedness"] == "right" || profile["handedness"] == "reflected"),
          "DTS v1 轮廓必须声明 unit=mm、plane=XZ、sampleOrder=acquisition 及手性");
  const auto sourceId = text(source, "node");
  const auto adapterId = text(rule, "adapterId");
  auto& nodes = graph["nodes"];
  auto& edges = graph["edges"];
  const auto si = nodeIndex(nodes, sourceId);
  require(si < nodes.size() && nodes[si].value("op", "") == "dts.profile_in" &&
              nodes[si].value("opVersion", "") == "1.0.0",
          "来源节点 " + sourceId + " 不是 dts.profile_in@1.0.0");
  requireEnabled(nodes[si]);
  require(nodeIndex(nodes, adapterId) == nodes.size(), "适配节点 ID 已被占用: " + adapterId);

  std::map<std::string, std::string> chain{{"dts.profile_in", sourceId}};
  auto discover = [&](const char* fromOp, const char* fromPort, const char* targetOp,
                      const char* targetPort) {
    std::string found;
    for (const auto& edge : edges) {
      if (edge["from"].value("node", "") != chain.at(fromOp) ||
          edge["from"].value("port", "") != fromPort || edge["to"].value("port", "") != targetPort)
        continue;
      const auto id = edge["to"].value("node", "");
      const auto i = nodeIndex(nodes, id);
      if (i < nodes.size() && nodes[i].value("op", "") == targetOp) {
        require(found.empty(), std::string("同一来源有多个 ") + targetOp + "，请分别迁移每条链");
        found = id;
      }
    }
    require(!found.empty(), std::string("DTS 消费链不完整：找不到 ") + targetOp);
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
  std::set<std::string> ids;
  for (const auto& [op, id] : chain) {
    const auto& node = nodes[nodeIndex(nodes, id)];
    require(node.value("opVersion", "") == "1.0.0", "DTS 消费链必须全部是 1.0.0: " + id);
    requireEnabled(node);
    ids.insert(id);
  }
  Json expected = Json::array();
  for (const auto& [from, fromPort, to, toPort] : links) {
    const auto f = endpoint(chain.at(from), fromPort), t = endpoint(chain.at(to), toPort);
    require(edgeIndex(edges, f, t) < edges.size(),
            "DTS 消费链缺少连线 " + f.dump() + " → " + t.dump());
    expected.push_back({{"from", f}, {"to", t}});
  }

  // 链外消费边只允许 dts.geometry_to_metric@1.0.0 几何桥（随链一起升到 v2）。
  std::vector<std::size_t> bridges;
  for (const auto& edge : edges) {
    const auto fromId = edge["from"].value("node", "");
    const auto toId = edge["to"].value("node", "");
    if ((!ids.count(fromId) && !ids.count(toId)) || (fromId == sourceId && !ids.count(toId)) ||
        std::any_of(expected.begin(), expected.end(), [&](const Json& link) {
          return edge["from"] == link["from"] && edge["to"] == link["to"];
        }))
      continue;
    const auto bi = nodeIndex(nodes, toId);
    const auto sourceOp = ids.count(fromId) ? nodes[nodeIndex(nodes, fromId)].value("op", "") : "";
    const auto port = edge["from"].value("port", "");
    const bool bridge =
        bi < nodes.size() && nodes[bi].value("op", "") == "dts.geometry_to_metric" &&
        nodes[bi].value("opVersion", "") == "1.0.0" && edge["to"].value("port", "") == "geometry" &&
        nodes[bi].value("params", Json()).value("sourceContract", "") == "dts.geometry.v1" &&
        nodes[bi]["params"].value("sourceOp", "") == sourceOp &&
        ((sourceOp == "dts.seal_dome" && port == "apex") ||
         (sourceOp == "dts.pick_metal" && port == "metal") ||
         (sourceOp == "dts.seal_root" && port == "point") ||
         (sourceOp == "dts.flush" && port == "foot"));
    require(bridge, "DTS 消费链有外部消费边 " + edge.dump() + "，请先适配或删除它");
    requireEnabled(nodes[bi]);
    bridges.push_back(bi);
  }

  const auto defaultsOf = [](const std::string& op) {
    if (op == "dts.profile_clean")
      return Json{{"smoothPoints", 5},
                  {"minIntensity", 0.5},
                  {"minValidPoints", 16},
                  {"mode", "preserve_slots"}};
    return Json{{"gapMm", 1.0},     {"jumpMm", 1.5},        {"minPoints", 15}, {"fitLenMm", 5.0},
                {"maxRmsMm", 0.05}, {"angleBreakDeg", 6.0}, {"minLenMm", 3.0}};
  };
  for (const auto& [op, id] : chain) {
    if (op == "dts.profile_in")
      continue;
    auto& node = nodes[nodeIndex(nodes, id)];
    if (op == "dts.profile_clean" || op == "dts.split_faces") {
      node["op"] = op == "dts.profile_clean" ? "profile.clean" : "profile.split_faces";
      auto& params = node["params"];
      if (!params.is_object())
        params = Json::object();
      const auto defaults = defaultsOf(op);
      for (const auto& item : defaults.items())
        if (!params.contains(item.key()) && !bound(graph, id, item.key(), outer != nullptr) &&
            !(outer && bound(*outer, id, item.key(), true)))
          params[item.key()] = item.value();
    } else {
      node["opVersion"] = "2.0.0";
    }
  }
  for (const auto i : bridges) {
    nodes[i]["opVersion"] = "2.0.0";
    nodes[i]["params"]["sourceContract"] = "dts.geometry.v2";
  }

  const auto oldTarget = endpoint(chain.at("dts.profile_clean"), "profile");
  for (auto& edge : edges)
    if (edge["from"] == endpoint(sourceId, "profile") && edge["to"] == oldTarget)
      edge["from"] = endpoint(adapterId, "profile");
  for (const auto* suffix : {"_in", "_foot"})
    require(std::none_of(edges.begin(), edges.end(),
                         [&](const Json& edge) {
                           return edge.value("id", "") == "contract_" + adapterId + suffix;
                         }),
            "迁移要新增的连线 ID 已被占用: contract_" + adapterId + suffix);
  edges.push_back({{"id", "contract_" + adapterId + "_in"},
                   {"from", endpoint(sourceId, "profile")},
                   {"to", endpoint(adapterId, "cloud")}});
  edges.push_back({{"id", "contract_" + adapterId + "_foot"},
                   {"from", endpoint(chain.at("dts.flush"), "footInfo")},
                   {"to", endpoint(chain.at("dts.profile_bundle"), "footInfo")}});
  nodes.push_back({{"id", adapterId},
                   {"op", "profile.from_cloud"},
                   {"opVersion", "1.0.0"},
                   {"params", profile}});
}

void migrateGeometry(Json& graph, const Json& rule, Json*) {
  keys(rule, {"kind", "source", "target", "adapterId"});
  const auto& source = rule.at("source");
  const auto op = text(source, "op");
  const auto port = text(source, "port");
  require((op == "dts.seal_dome" && port == "apex") ||
              (op == "dts.pick_metal" && port == "metal") ||
              (op == "dts.seal_root" && port == "point") || (op == "dts.flush" && port == "foot"),
          "dtsGeometry.v1 只接受 v1 的点/线几何端口: " + op + "." + port);
  require(text(source, "opVersion") == "1.0.0", "dtsGeometry.v1 的来源必须是 1.0.0");
  insertEdgeAdapter(graph, rule, "dts.geometry_to_metric", "geometry",
                    {{"sourceContract", "dts.geometry.v1"}, {"sourceOp", op}});
}
} // namespace

void registerGraphMigrations(Registry& r) {
  r.addGraphMigration({"dtsProfile.v1", {}, migrateProfile});
  r.addGraphMigration({"dtsGeometry.v1", {}, migrateGeometry});
  const std::string hint =
      "运行 lyflow migrate <图> --profile-source <dts.profile_in 节点> --profile-frame <frame> "
      "--profile-source-id <sourceId> --write；子图来源写 <定义ID>/<节点ID>";
  r.addRetiredOperator("dts.profile_clean", hint);
  r.addRetiredOperator("dts.split_faces", hint);
}
} // namespace lyflow::dts
