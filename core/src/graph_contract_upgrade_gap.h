#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace lyflow::gap_contract_upgrade {
namespace detail {
using Json = nlohmann::json;
inline void require(bool condition, const std::string& why) {
  if (!condition)
    throw std::invalid_argument(why);
}
inline void keys(const Json& object, std::initializer_list<const char*> names) {
  require(object.is_object() && object.size() == names.size(), "GAP升级字段数量不符");
  for (const auto* name : names)
    require(object.contains(name), std::string("GAP升级缺少字段: ") + name);
}
inline std::string text(const Json& object, const char* field) {
  require(object.is_object() && object.contains(field) && object[field].is_string(),
          std::string("GAP升级字段必须为字符串: ") + field);
  const auto result = object[field].get<std::string>();
  require(!result.empty() && result.size() <= 1024 && result.find('\0') == std::string::npos,
          std::string("GAP升级字段必须非空、不含NUL且最多1024字节: ") + field);
  return result;
}
inline const std::map<std::string, std::string>& versions() {
  static const std::map<std::string, std::string> result{{"gap.load_profile_pair", "1.0.0"},
                                                         {"gap.to_measurement_frame", "1.0.0"},
                                                         {"gap.drop_non_finite", "1.0.0"},
                                                         {"gap.profile_tensor", "1.0.0"},
                                                         {"gap.labels_from_logits", "1.0.0"},
                                                         {"gap.roi_from_labels", "1.2.0"},
                                                         {"gap.labels_to_cloud", "1.0.0"},
                                                         {"gap.roll_anchored_crop", "1.1.0"},
                                                         {"gap.datum_window", "1.0.0"},
                                                         {"gap.overall_roi", "1.0.0"},
                                                         {"gap.load_template", "1.0.0"},
                                                         {"gap.align_template", "1.1.0"},
                                                         {"gap.select_alignment", "1.0.0"},
                                                         {"gap.business_rois", "1.2.0"},
                                                         {"gap.fit_line", "1.1.1"},
                                                         {"gap.selected_point", "1.0.0"},
                                                         {"gap.nearest_to_line", "1.0.0"},
                                                         {"gap.fit_gap_circles", "1.4.0"},
                                                         {"gap.flush", "1.1.1"},
                                                         {"gap.gap", "1.0.1"},
                                                         {"gap.point_offset", "1.0.1"},
                                                         {"gap.camera_guard", "1.0.0"},
                                                         {"gap.judge", "1.0.0"},
                                                         {"gap.result_bundle", "2.1.0"},
                                                         {"gap.make_scan_pair", "1.0.0"},
                                                         {"gap.split_scan_pair", "1.0.0"},
                                                         {"gap.make_roi_set", "1.1.0"},
                                                         {"gap.split_roi_set", "1.0.0"},
                                                         {"gap.read_scan", "1.1.0"},
                                                         {"gap.locate_template", "2.1.0"},
                                                         {"gap.locate_model", "1.0.0"},
                                                         {"gap.role_line", "1.0.0"},
                                                         {"gap.ref_point", "1.0.0"},
                                                         {"gap.seam_circles", "1.2.0"},
                                                         {"gap.datum_direction", "1.0.0"},
                                                         {"geometry.box_2d", "1.0.0"},
                                                         {"ml.onnx_run", "1.0.0"},
                                                         {"filter.radius_outlier", "1.0.0"},
                                                         {"util.merge", "1.0.0"},
                                                         {"filter.crop_box2d", "1.0.0"},
                                                         {"flow.fallback", "1.0.0"}};
  return result;
}
inline void declaration(const Json& value) {
  keys(value, {"sourceId", "unit", "fileFrame", "sensorFrame", "fileHandedness"});
  for (const auto* name : {"sourceId", "unit", "fileFrame", "sensorFrame", "fileHandedness"})
    text(value, name);
  require(value["unit"] == "m", "GAP旧图数值契约为米，拒绝在自动升级时推测或纠正旧单位");
  require(value["fileHandedness"] == "right" || value["fileHandedness"] == "reflected",
          "GAP文件手性必须明确为right或reflected");
}
inline void enabled(const Json& node) {
  require(!node.contains("bypass") || (node["bypass"].is_boolean() && node["bypass"] == false),
          "GAP升级组件节点必须启用，不支持旁路或未知旁路状态");
}
inline bool bound(const Json& owner, const std::string& node, const char* field,
                  bool nested = false) {
  if (!owner.contains("params") || (!owner["params"].is_object() && !owner["params"].is_array()))
    return false;
  const auto direct = node + "." + field;
  const auto suffix = "/" + direct;
  for (const auto& parameter : owner["params"])
    if (parameter.is_object() && parameter.contains("binds") && parameter["binds"].is_array())
      for (const auto& binding : parameter["binds"]) {
        if (binding.is_string()) {
          const auto& text = binding.get_ref<const std::string&>();
          if (text == direct ||
              (nested && text.size() > suffix.size() &&
               text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0))
            return true;
        } else if (binding.is_object() && binding.value("node", "") == node &&
                   binding.value("param", "") == field)
          return true;
      }
  return false;
}
} // namespace detail

inline void validate(const nlohmann::json& graph, const nlohmann::json& rule,
                     const nlohmann::json* outer = nullptr) {
  using namespace detail;
  auto shape = rule;
  if (shape.contains("scope")) {
    keys(shape["scope"], {"subgraph"});
    text(shape["scope"], "subgraph");
    shape.erase("scope");
  }
  if (shape.contains("templates")) {
    const auto& t = shape.at("templates");
    keys(t, {"unit", "fileFrame", "templateFrame", "handedness"});
    text(t, "fileFrame");
    text(t, "templateFrame");
    require(t.at("unit") == "m", "GAP旧模板数值契约为米，不能自动猜测模板单位转换");
    require(t.at("handedness") == "right" || t.at("handedness") == "reflected",
            "GAP模板文件手性必须明确声明");
    shape.erase("templates");
  }
  keys(shape, {"kind", "source", "profiles", "measurement", "alreadyAligned", "component",
               "adapterIdPrefix"});
  require(rule["kind"] == "gapProfile.v1", "未知GAP整图升级版本");
  keys(rule["source"], {"node", "op", "opVersion", "port"});
  const auto sourceId = text(rule["source"], "node");
  require(sourceId.find('/') == std::string::npos,
          "GAP来源必须属于明确作用域，实例投影路径不能代替共享子图定义");
  const auto sourceOp = text(rule["source"], "op");
  const auto sourceVersion = text(rule["source"], "opVersion");
  const auto sourcePort = text(rule["source"], "port");
  require((sourceOp == "gap.read_scan" && sourceVersion == "1.1.0" && sourcePort == "scan") ||
              (sourceOp == "gap.load_profile_pair" && sourceVersion == "1.0.0" &&
               sourcePort == "primary"),
          "GAP升级需要精确read_scan@1.1.0:scan或load_profile_pair@1.0.0:primary来源证据");
  require(rule["alreadyAligned"].is_boolean() && rule["alreadyAligned"] == true,
          "GAP升级必须显式声明两路已处于共同坐标空间；此声明不证明硬件标定");
  keys(rule["profiles"], {"primary", "secondary"});
  const auto& a = rule["profiles"]["primary"];
  const auto& b = rule["profiles"]["secondary"];
  declaration(a);
  declaration(b);
  for (const auto* field : {"unit", "fileFrame", "sensorFrame", "fileHandedness"})
    require(a[field] == b[field], std::string("双路源声明不符合当前成对IO的共同字段: ") + field);
  keys(rule["measurement"], {"frame", "plane"});
  const auto frame = text(rule["measurement"], "frame");
  require(rule["measurement"]["plane"] == "XY" && frame != a["sensorFrame"],
          "GAP量测必须明确为不同于传感器帧的XY空间");
  const auto prefix = text(rule, "adapterIdPrefix");
  require(prefix.size() <= 128 && prefix.find('/') == std::string::npos &&
              prefix.find('.') == std::string::npos,
          "GAP新增节点前缀最多128字节且不能含实例路径或参数分隔符");
  keys(rule["component"], {"style", "nodes"});
  require(rule["component"]["style"] == "blocks" || rule["component"]["style"] == "fine",
          "GAP升级组件必须明确blocks或fine模板");
  const auto& selected = rule["component"]["nodes"];
  require(selected.is_array() && !selected.empty() && selected.size() <= 4096,
          "GAP升级需要明确1到4096项的完整组件节点");
  require(graph.contains("nodes") && graph["nodes"].is_array() && graph.contains("edges") &&
              graph["edges"].is_array(),
          "GAP整图升级需要完整nodes/edges");
  std::map<std::string, const Json*> nodes;
  for (const auto& node : graph["nodes"]) {
    const auto id = text(node, "id");
    require(nodes.emplace(id, &node).second, "GAP升级拒绝重名节点: " + id);
  }
  std::set<std::string> ids;
  for (const auto& entry : selected) {
    require(entry.is_string(), "GAP组件节点引用必须为字符串");
    const auto id = entry.get<std::string>();
    require(ids.insert(id).second && nodes.count(id), "GAP组件节点重复或不存在: " + id);
    const auto& node = *nodes.at(id);
    const auto op = text(node, "op");
    const auto version = text(node, "opVersion");
    const auto found = versions().find(op);
    require(found != versions().end() && found->second == version,
            "GAP组件算子不在PR66精确版本白名单: " + id + " " + op + "@" + version);
    require(!node.contains("params") || node["params"].is_object(), "GAP组件参数必须为对象: " + id);
    enabled(node);
    require(id.rfind(prefix + "_", 0) != 0, "GAP新增节点前缀已被组件占用: " + id);
  }
  require(ids.count(sourceId), "GAP来源锚点不属于显式组件");
  const auto& source = *nodes.at(sourceId);
  require(source["op"] == sourceOp && source["opVersion"] == sourceVersion,
          "GAP来源锚点实际版本与请求不符");
  const auto params = source.value("params", Json::object());
  const auto mode = params.value("source", std::string("dir"));
  require(mode == "files" || mode == "dir",
          "GAP旧裸云注入无法证明原槽；请先提供完整Profile或重新读取原始文件");
  const auto layout = params.value("layout", std::string("sensor"));
  require(layout == "sensor" || layout == "profile", "GAP文件布局必须为已知sensor/profile声明");
  require(!bound(graph, sourceId, "layout") && !(outer && bound(*outer, sourceId, "layout", true)),
          "GAP来源布局不能由未固定的动态参数改变，须先固定已声明文件布局");
  const auto sourceHand = a.at("fileHandedness").get<std::string>();
  const auto measurementHand = layout == "profile"     ? sourceHand
                               : sourceHand == "right" ? std::string("reflected")
                                                       : std::string("right");
  for (const auto& entry : selected) {
    const auto& node = *nodes.at(entry.get<std::string>());
    if (node.at("op") != "gap.locate_template" && node.at("op") != "gap.load_template")
      continue;
    const auto templateParams = node.value("params", Json::object());
    const auto templateLayout = templateParams.value("layout", std::string("profile"));
    require(templateLayout == "profile" || templateLayout == "sensor", "GAP模板布局必须为已知声明");
    const auto templateHand = rule.contains("templates")
                                  ? rule.at("templates").at("handedness").get<std::string>()
                                  : std::string("reflected");
    const auto targetHand = templateLayout == "profile" ? templateHand
                            : templateHand == "right"   ? std::string("reflected")
                                                        : std::string("right");
    require(
        targetHand == measurementHand,
        "GAP模板默认软件手性与量测声明不一致；请提供匹配实际文件的templates显式声明，不能猜测反射");
  }
  if (mode == "files") {
    for (const auto* field : {"primaryFile", "secondaryFile"})
      if (!bound(graph, sourceId, field) && !(outer && bound(*outer, sourceId, field, true)))
        text(params, field);
  } else if (!bound(graph, sourceId, "dir") && !(outer && bound(*outer, sourceId, "dir", true)))
    text(params, "dir");
  std::set<std::string> edgeIds, links;
  for (const auto& edge : graph["edges"]) {
    const auto id = text(edge, "id");
    require(edgeIds.insert(id).second, "GAP升级拒绝重复连线ID: " + id);
    require(edge.contains("from") && edge.contains("to"), "GAP连线缺少端点: " + id);
    keys(edge["from"], {"node", "port"});
    keys(edge["to"], {"node", "port"});
    const auto from = text(edge["from"], "node"), to = text(edge["to"], "node");
    const auto fp = text(edge["from"], "port"), tp = text(edge["to"], "port");
    require(nodes.count(from) && nodes.count(to), "GAP连线引用不存在节点: " + id);
    require(links.insert(edge["from"].dump() + "->" + edge["to"].dump()).second,
            "GAP升级拒绝重复连接: " + id);
    if (ids.count(from) != ids.count(to))
      throw std::invalid_argument("GAP组件存在未声明外部消费或来源边: " + id + " " + from + "." +
                                  fp + " -> " + to + "." + tp);
    if (to == sourceId && (tp == "primary" || tp == "secondary"))
      throw std::invalid_argument("GAP文件来源被裸云输入覆盖，不能推测原槽: " + id);
  }
  require(rule.dump().size() <= 128 * 1024, "GAP升级请求超过128KiB预算");
}

} // namespace lyflow::gap_contract_upgrade

#include "graph_contract_upgrade_gap_rewrite.h"

namespace lyflow::gap_contract_upgrade {
inline bool applied(const nlohmann::json& graph, const nlohmann::json& rule) {
  try {
    const auto* level = &graph;
    if (rule.contains("scope"))
      level = &graph.at("subgraphs").at(rule.at("scope").at("subgraph").get<std::string>());
    const auto source = rule.at("source").at("node");
    bool found = false;
    for (const auto& id : rule.at("component").at("nodes")) {
      const auto node = std::find_if(level->at("nodes").begin(), level->at("nodes").end(),
                                     [&](const auto& value) { return value.at("id") == id; });
      if (node == level->at("nodes").end())
        return false;
      const auto op = node->at("op").template get<std::string>();
      const auto version = node->at("opVersion").template get<std::string>();
      if (id == source)
        found = op == rule.at("source").at("op") && version == "2.0.0";
      const std::map<std::string, std::string> known{{"gap.load_profile_pair", "2.0.0"},
                                                     {"gap.read_scan", "2.0.0"},
                                                     {"gap.camera_guard", "2.0.0"},
                                                     {"gap.locate_template", "3.0.0"},
                                                     {"gap.locate_model", "2.0.0"},
                                                     {"gap.role_line", "2.0.0"},
                                                     {"gap.ref_point", "2.0.0"},
                                                     {"gap.datum_direction", "2.0.0"},
                                                     {"gap.seam_circles", "2.0.0"},
                                                     {"gap.split_location", "1.0.0"},
                                                     {"gap.split_line_feature", "1.0.0"},
                                                     {"gap.split_reference_feature", "1.0.0"},
                                                     {"gap.result_bundle", "3.0.0"},
                                                     {"gap.judge", "1.0.0"},
                                                     {"metrology.flush_point_line", "1.0.0"},
                                                     {"metrology.circle_gap", "1.0.0"},
                                                     {"metrology.point_offset", "1.0.0"},
                                                     {"profile.remap_axes", "1.0.0"},
                                                     {"profile.filter_validity", "1.0.0"},
                                                     {"profile.merge", "1.0.0"},
                                                     {"profile.radius_outlier", "1.0.0"},
                                                     {"profile.crop_box", "1.0.0"},
                                                     {"profile.fit_line_robust", "1.0.0"},
                                                     {"profile.nearest_sample", "1.0.0"},
                                                     {"profile.nearest_to_line", "1.0.0"},
                                                     {"gap.overall_roi", "2.0.0"},
                                                     {"gap.datum_window", "2.0.0"},
                                                     {"gap.make_scan_pair", "2.0.0"},
                                                     {"gap.split_scan_pair", "2.0.0"},
                                                     {"gap.make_roi_set", "2.0.0"},
                                                     {"gap.split_roi_set", "2.0.0"},
                                                     {"gap.load_template", "2.0.0"},
                                                     {"gap.align_template", "2.0.0"},
                                                     {"gap.select_alignment", "2.0.0"},
                                                     {"gap.business_rois", "2.0.0"},
                                                     {"gap.profile_tensor", "2.0.0"},
                                                     {"gap.infer_profile", "1.0.0"},
                                                     {"gap.labels_from_logits", "2.0.0"},
                                                     {"gap.roi_from_labels", "2.0.0"},
                                                     {"gap.labels_to_cloud", "2.0.0"},
                                                     {"gap.roll_anchored_crop", "2.0.0"},
                                                     {"gap.fit_gap_circles", "2.0.0"},
                                                     {"flow.fallback", "1.0.0"}};
      const auto expected = known.find(op);
      if (expected == known.end() || expected->second != version)
        return false;
    }
    return found;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}
inline void upgrade(nlohmann::json& graph, const nlohmann::json& rule,
                    nlohmann::json* outer = nullptr) {
  validate(graph, rule, outer);
  std::map<std::string, int> counts;
  for (const auto& id : rule.at("component").at("nodes"))
    for (const auto& node : graph.at("nodes"))
      if (node.at("id") == id)
        ++counts[node.at("op").get<std::string>()];
  const bool blocks = rule.at("component").at("style") == "blocks";
  detail::require((blocks ? counts["gap.read_scan"] == 1 && counts["gap.load_profile_pair"] <= 1 &&
                                counts["gap.seam_circles"] == 1
                          : counts["gap.read_scan"] == 0 && counts["gap.load_profile_pair"] == 1 &&
                                counts["gap.to_measurement_frame"] == 2 &&
                                counts["util.merge"] == 1 && counts["filter.radius_outlier"] <= 1 &&
                                counts["gap.fit_gap_circles"] == 1) &&
                      counts["gap.camera_guard"] <= 1 && counts["gap.locate_model"] <= 1 &&
                      counts["gap.locate_template"] <= 1 && counts["gap.flush"] == 1 &&
                      counts["gap.gap"] == 1 && counts["gap.result_bundle"] == 1,
                  "GAP组件不是已知完整StandardGap模板");
  detail::GapRewrite(graph, rule, outer).run();
}
}
