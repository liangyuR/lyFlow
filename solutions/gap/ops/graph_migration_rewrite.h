#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lyflow/registry.h"

namespace lyflow::packs::gap::graph_migration_rules::detail {
struct GapSignal {
  Json value, context, feature;
  std::string kind, featureKind;
  std::string featureField;
  Json choice;
};
inline Json endpoint(const std::string& node, const std::string& port) {
  return {{"node", node}, {"port", port}};
}
class GapRewrite {
public:
  GapRewrite(Json& graph, const Json& rule, Json* outer)
      : graph_(graph), rule_(rule), outer_(outer), prefix_(rule.at("adapterIdPrefix")) {
    for (const auto& value : rule.at("component").at("nodes"))
      selected_.insert(value.get<std::string>());
    for (const auto& node : graph.at("nodes")) {
      const auto id = node.at("id").get<std::string>();
      occupied_.insert(id);
      if (selected_.count(id))
        originals_[id] = node;
    }
    for (const auto& edge : graph.at("edges")) {
      edgeIds_.insert(edge.at("id").get<std::string>());
      if (selected_.count(edge.at("to").at("node").get<std::string>())) {
        const auto key = edge.at("to").dump();
        require(incoming_.emplace(key, edge).second, "GAP升级拒绝同一输入端口的多重连线: " + key);
      }
    }
  }
  void run() {
    std::set<std::string> complete;
    while (complete.size() != originals_.size()) {
      bool progress = false;
      for (const auto& entry : originals_) {
        if (complete.count(entry.first))
          continue;
        if (entry.second.at("op") == "gap.result_bundle" &&
            complete.size() + 1 != originals_.size())
          continue;
        bool ready = true;
        for (const auto& edge : incoming_)
          if (edge.second.at("to").at("node") == entry.first &&
              !complete.count(edge.second.at("from").at("node").get<std::string>()))
            ready = false;
        if (!ready)
          continue;
        rewrite(entry.second);
        complete.insert(entry.first);
        progress = true;
      }
      require(progress, "GAP已知导入模板必须为无环完整计算图");
    }
    for (const auto& edge : incoming_)
      require(consumed_.count(edge.first),
              "GAP模板存在未知输入或未消费旧连线: " + edge.second.at("id").get<std::string>());
    rewriteOutputs(graph_);
    rewriteBindings(graph_, false);
    if (outer_) {
      rewriteProjectedOutputs(*outer_);
      rewriteBindings(*outer_, true);
    }
    Json nodes = Json::array(), edges = Json::array();
    for (const auto& old : graph_.at("nodes")) {
      const auto id = old.at("id").get<std::string>();
      nodes.push_back(selected_.count(id) ? replacements_.at(id) : old);
    }
    for (auto& node : additions_)
      nodes.push_back(std::move(node));
    for (const auto& old : graph_.at("edges"))
      if (!selected_.count(old.at("from").at("node").get<std::string>()))
        edges.push_back(old);
    for (auto& edge : edges_)
      edges.push_back(std::move(edge));
    graph_["nodes"] = std::move(nodes);
    graph_["edges"] = std::move(edges);
  }

private:
  GapSignal signal(const Json& ref) const {
    const auto found = signals_.find(ref.dump());
    require(found != signals_.end(), "GAP模板使用未知旧输出端口: " + ref.dump());
    return found->second;
  }
  bool has(const std::string& id, const char* port) const {
    return incoming_.count(endpoint(id, port).dump()) != 0;
  }
  GapSignal input(const std::string& id, const char* port, bool required = true) {
    const auto key = endpoint(id, port).dump();
    const auto found = incoming_.find(key);
    if (found == incoming_.end()) {
      require(!required, "GAP模板缺少必要旧输入: " + id + "." + port);
      return {};
    }
    consumed_.insert(key);
    return signal(found->second.at("from"));
  }
  void wire(const Json& from, const std::string& to, const std::string& port) {
    require(from.is_object(), "GAP升级缺少原子来源或几何上下文: " + to + "." + port);
    const auto key = endpoint(to, port).dump();
    require(wired_.insert(key).second, "GAP升级产生重复输入: " + key);
    std::string id;
    do {
      id = prefix_ + "_edge_" + std::to_string(nextEdge_++);
    } while (!edgeIds_.insert(id).second);
    edges_.push_back({{"id", id}, {"from", from}, {"to", endpoint(to, port)}});
  }
  GapSignal pass(const std::string& id, const char* port, const char* target = nullptr,
                 bool required = true) {
    auto value = input(id, port, required);
    if (value.value.is_object())
      wire(value.value, id, target ? target : port);
    return value;
  }
  void boundInput(const std::string& id, const char* port, const char* target = nullptr,
                  bool required = true, const char* context = nullptr) {
    const auto value = pass(id, port, target, required);
    if (value.value.is_object())
      wire(value.context, id, context ? context : std::string(target ? target : port) + "Context");
  }
  static std::string version(const std::string& op) {
    const auto* desc = ensureRegistry().find(op);
    require(desc != nullptr, "GAP升级需要当前运行库提供算子: " + op);
    return desc->version;
  }
  std::string add(const std::string& op, const Json& params = Json::object()) {
    const auto id = prefix_ + "_node_" + std::to_string(nextNode_++);
    require(occupied_.insert(id).second, "GAP新增节点ID已被占用: " + id);
    additions_.push_back({{"id", id}, {"op", op}, {"opVersion", version(op)}, {"params", params}});
    return id;
  }
  void replace(const Json& old, const std::string& op, const Json& params) {
    auto value = old;
    value["op"] = op;
    value["opVersion"] = version(op);
    value["params"] = params;
    replacements_[old.at("id").get<std::string>()] = std::move(value);
  }
  void output(const std::string& id, const std::string& oldPort, const std::string& newPort,
              const std::string& kind, const std::string& context = {},
              const Json& feature = Json(), const std::string& featureKind = {}) {
    signals_[endpoint(id, oldPort).dump()] = {endpoint(id, newPort),
                                              context.empty() ? Json() : endpoint(id, context),
                                              feature, kind, featureKind};
  }
  void allowed(const Json& params, std::initializer_list<const char*> fields) const {
    const std::set<std::string> names(fields.begin(), fields.end());
    for (auto item = params.begin(); item != params.end(); ++item)
      require(names.count(item.key()), "GAP模板存在不支持的参数，拒绝静默丢弃: " + item.key());
  }
  Json fileParameters(const Json& old) const {
    Json p = old;
    p.erase("dropNonFinite");
    const auto& a = rule_.at("profiles").at("primary");
    const auto& b = rule_.at("profiles").at("secondary");
    for (const auto* field : {"unit", "fileFrame", "sensorFrame"})
      p[field] = a.at(field);
    p["handedness"] = a.at("fileHandedness");
    p["primarySourceId"] = a.at("sourceId");
    p["secondarySourceId"] = b.at("sourceId");
    return p;
  }
  void rename(const std::string& id, const std::string& before, const std::string& after,
              bool scaling = false) {
    params_[id + "." + before] = {id + "." + after, scaling};
  }
  void rewriteOutputs(Json& owner) {
    if (!owner.contains("outputs"))
      return;
    require(owner["outputs"].is_object() || owner["outputs"].is_array(),
            "GAP图outputs必须是完整图对象或子图声明数组");
    for (auto& value : owner["outputs"]) {
      auto& ref = owner["outputs"].is_array() ? value.at("from") : value;
      require(ref.is_object() && ref.contains("node") && ref.contains("port"),
              "GAP图输出声明缺少明确端点");
      if (!selected_.count(ref["node"].get<std::string>()))
        continue;
      const auto mapped = signal(endpoint(ref["node"], ref["port"]));
      ref["node"] = mapped.value["node"];
      ref["port"] = mapped.value["port"];
    }
  }
  void rewriteBindings(Json& owner, bool nested) {
    if (!owner.contains("params"))
      return;
    require(owner["params"].is_object() || owner["params"].is_array(),
            "GAP图参数声明必须是图对象或子图声明数组");
    for (auto& parameter : owner["params"])
      if (parameter.is_object() && parameter.contains("binds")) {
        require(parameter["binds"].is_array(), "GAP参数binds必须是数组");
        for (auto& binding : parameter["binds"]) {
          std::string key, head;
          if (binding.is_string())
            key = binding.get<std::string>();
          else if (binding.is_object() && binding.contains("node") && binding.contains("param"))
            key = text(binding, "node") + "." + text(binding, "param");
          else
            throw std::invalid_argument("GAP不支持未知参数绑定表示");
          if (nested) {
            const auto slash = key.rfind('/');
            if (slash == std::string::npos)
              continue;
            head = key.substr(0, slash + 1);
            const auto instance = head.substr(0, head.size() - 1);
            require(instance.find('/') == std::string::npos,
                    "GAP外层绑定暂只支持直接共享子图实例，拒绝猜测多层实例路径");
            bool belongs = false;
            for (const auto& node : owner.at("nodes"))
              if (node.at("id") == instance)
                belongs = node.value("op", "") ==
                          "sub:" + rule_.at("scope").at("subgraph").get<std::string>();
            if (!belongs)
              continue;
            throw std::invalid_argument(
                "GAP图级参数不能直接绑定子图内部投影；请先声明并绑定子图提升参数: " + key);
          }
          const auto oldDot = key.rfind('.');
          const auto oldId = key.substr(0, oldDot), oldField = key.substr(oldDot + 1);
          if (!selected_.count(oldId))
            continue;
          require(!originals_.at(oldId).value("params", Json::object()).contains(oldField),
                  "GAP原节点参数同时存在显式值和图级绑定，须先解决param_conflict: " + key);
          const auto found = params_.find(key);
          if (found == params_.end()) {
            require(!replacements_.at(oldId).value("params", Json::object()).contains(oldField),
                    "GAP转换器新增声明不能由旧图参数覆盖，须显式重建绑定: " + key);
            continue;
          }
          require(!found->second.second, "GAP动态参数涉及单位/枚举转换，须显式重建绑定: " + key);
          const auto newDot = found->second.first.rfind('.');
          const auto targetId = found->second.first.substr(0, newDot),
                     targetField = found->second.first.substr(newDot + 1);
          replacements_.at(targetId)["params"].erase(targetField);
          if (binding.is_string())
            binding = head + found->second.first;
          else {
            const auto split = found->second.first.rfind('.');
            binding["node"] = found->second.first.substr(0, split);
            binding["param"] = found->second.first.substr(split + 1);
          }
        }
      }
  }
  void rewriteProjectedOutputs(Json& owner) {
    if (!owner.contains("outputs"))
      return;
    require(owner.at("outputs").is_object(), "GAP外层图输出必须为GraphDoc对象");
    for (auto& value : owner["outputs"]) {
      if (!value.is_object() || !value.contains("node") || !value.contains("port") ||
          !value.at("node").is_string())
        throw std::invalid_argument("GAP外层输出缺少明确投影端点");
      const auto path = value.at("node").get<std::string>();
      const auto slash = path.find('/');
      if (slash == std::string::npos)
        continue;
      const auto instance = path.substr(0, slash);
      bool belongs = false;
      for (const auto& node : owner.at("nodes"))
        if (node.at("id") == instance)
          belongs =
              node.value("op", "") == "sub:" + rule_.at("scope").at("subgraph").get<std::string>();
      if (!belongs)
        continue;
      const auto child = path.substr(slash + 1);
      require(child.find('/') == std::string::npos, "GAP输出暂不支持跨多层共享子图投影");
      if (!selected_.count(child))
        continue;
      const auto mapped = signal(endpoint(child, value.at("port")));
      value["node"] = instance + "/" + mapped.value.at("node").get<std::string>();
      value["port"] = mapped.value.at("port");
    }
  }
  void location(const std::string& id) {
    const auto maker = add("gap.make_location");
    wire(endpoint(id, "scan"), maker, "scan");
    wire(endpoint(id, "rois"), maker, "rois");
    output(id, "scan", "scan", "scan", {}, endpoint(maker, "location"), "location");
    output(id, "rois", "rois", "rois", {}, endpoint(maker, "location"), "location");
  }
  void fineSources() {
    if (!loadId_.empty())
      return;
    for (const auto& item : originals_) {
      const auto op = item.second.at("op").get<std::string>();
      if (op == "gap.load_profile_pair")
        loadId_ = item.first;
      if (op == "gap.camera_guard")
        guardId_ = item.first;
      if (op == "util.merge")
        mergedRoot_ = endpoint(item.first, "cloud");
    }
    require(!loadId_.empty() && mergedRoot_.is_object(), "GAP细粒度缺少原始双路来源或唯一全量合并");
    for (const auto& item : originals_)
      if (item.second.at("op") == "filter.radius_outlier") {
        const auto found = incoming_.find(endpoint(item.first, "cloud").dump());
        require(found != incoming_.end() && found->second.at("from") == mergedRoot_,
                "GAP细粒度只支持全量合并后的单次半径去噪");
        mergedRoot_ = endpoint(item.first, "cloud");
      }
    const auto sensor = guardId_.empty() ? loadId_ : guardId_;
    for (const auto& item : originals_)
      if (item.second.at("op") == "gap.to_measurement_frame") {
        const auto found = incoming_.find(endpoint(item.first, "cloud").dump());
        require(found != incoming_.end() && found->second.at("from").at("node") == sensor,
                "GAP换轴必须直接来自当前已声明传感器或guard路径");
        const auto port = found->second.at("from").at("port").get<std::string>();
        require(port == "primary" || port == "secondary", "GAP换轴缺少明确逻辑角色");
        auto& frame = frames_[port == "primary" ? 0 : 1];
        require(frame.empty(), "GAP每逻辑角色只能有一个原始换轴");
        frame = item.first;
      }
    require(!frames_[0].empty() && !frames_[1].empty(), "GAP细粒度缺少原始双路换轴证据");
  }
  void scanSources(const std::string& target) {
    fineSources();
    const auto sensor = guardId_.empty() ? loadId_ : guardId_;
    wire(endpoint(sensor, "primary"), target, "sensorPrimary");
    wire(endpoint(sensor, "secondary"), target, "sensorSecondary");
    wire(endpoint(loadId_, "sourceInfo"), target, "sourceInfo");
    if (!guardId_.empty())
      wire(endpoint(guardId_, "quality"), target, "guardQuality");
  }
  Json rawScene() {
    if (!rawScene_.is_null())
      return rawScene_;
    fineSources();
    const auto node = add("gap.make_scan_pair", {{"stage", "raw"}});
    scanSources(node);
    wire(endpoint(frames_[0], "profile"), node, "primary");
    wire(endpoint(frames_[1], "profile"), node, "secondary");
    wire(endpoint(mergedRoot_.at("node"), "profile"), node, "merged");
    rawScene_ = endpoint(node, "scan");
    return rawScene_;
  }
  Json sceneForMerged(const Json& original) {
    fineSources();
    if (original == mergedRoot_)
      return rawScene();
    std::string match;
    for (const auto& item : originals_)
      if (item.second.at("op") == "gap.make_scan_pair") {
        const auto found = incoming_.find(endpoint(item.first, "merged").dump());
        if (found != incoming_.end() && found->second.at("from") == original) {
          require(match.empty(), "GAP裁剪合并对应多个不同场景，拒绝推测");
          match = item.first;
        }
      }
    require(!match.empty(), "GAP裁剪合并缺少完整双路场景构造证据");
    return endpoint(match, "scan");
  }
  Json lineFeature(const std::string& id) {
    const auto node = add("gap.make_line_feature");
    for (const auto* field : {"profile", "line", "lineContext", "innerEnd", "pointContext",
                              "inliers", "weightedInliers", "quality"})
      wire(endpoint(id, field), node, field);
    return endpoint(node, "feature");
  }
  void attachProfileScene(const std::string& id, const std::string& port) {
    for (const auto& item : originals_)
      if (item.second.at("op") == "gap.make_scan_pair")
        for (const auto* role : {"primary", "secondary", "merged"}) {
          const auto found = incoming_.find(endpoint(item.first, role).dump());
          if (found != incoming_.end() && found->second.at("from") == endpoint(id, port)) {
            auto& value = signals_.at(endpoint(id, port).dump());
            require(value.feature.is_null() || value.feature == endpoint(item.first, "scan"),
                    "GAP同一旧轮廓被多个不同场景复用，拒绝猜测fallback完整来源");
            value.feature = endpoint(item.first, "scan");
            value.featureKind = "scan_pair";
            value.featureField = role;
          }
        }
  }
  void fallback(const Json& old) {
    const auto id = old.at("id").get<std::string>();
    const auto a = input(id, "a"), b = input(id, "b");
    require(a.kind == b.kind && !a.featureKind.empty() && a.featureKind == b.featureKind &&
                a.feature.is_object() && b.feature.is_object(),
            "GAP旧fallback无法证明数值、上下文与来源属于同一原子生产者: " + id);
    const auto pair = a.featureKind + a.feature.dump() + b.feature.dump();
    Json selected;
    std::string split;
    const auto existing = fallbacks_.find(pair);
    if (existing == fallbacks_.end()) {
      replace(old, "flow.fallback", Json::object());
      wire(a.feature, id, "a");
      wire(b.feature, id, "b");
      selected = endpoint(id, "out");
      split = add("gap.split_" + a.featureKind);
      wire(selected, split,
           a.featureKind == "location"    ? "location"
           : a.featureKind == "scan_pair" ? "scan"
           : a.featureKind == "roi_set"   ? "rois"
                                          : "feature");
      fallbacks_[pair] = selected;
    } else {
      selected = existing->second;
      split = id;
      replace(old, "gap.split_" + a.featureKind, Json::object());
      wire(selected, split,
           a.featureKind == "location"    ? "location"
           : a.featureKind == "scan_pair" ? "scan"
           : a.featureKind == "roi_set"   ? "rois"
                                          : "feature");
    }
    signals_[endpoint(id, "choice").dump()] = {
        endpoint(selected.at("node"), "choice"), {}, {}, "record", {}};
    const auto field = [&](const GapSignal& value) {
      if (value.kind == "scan" || value.kind == "rois")
        return value.kind;
      if (value.kind == "box" || value.kind == "profile") {
        require(!value.featureField.empty() && value.featureField == b.featureField,
                "GAP fallback角色必须严格匹配");
        return value.featureField;
      }
      if (value.kind == "line")
        return std::string("line");
      if (value.kind == "point")
        return a.featureKind == "line_feature" ? std::string("innerEnd") : std::string("point");
      if (value.kind == "quality")
        return std::string("quality");
      throw std::invalid_argument("GAP未知fallback输出角色: " + id);
    }(a);
    GapSignal result{endpoint(split, field), {}, selected, a.kind, a.featureKind};
    if ((a.kind == "scan" && a.featureKind == "scan_pair") ||
        (a.kind == "rois" && a.featureKind == "roi_set"))
      result.value = selected;
    if (a.kind == "line" || a.kind == "point")
      result.context = endpoint(split, a.kind == "line" ? "lineContext" : "pointContext");
    if (a.kind == "box")
      result.context = endpoint(split, field + "Context");
    result.featureField = a.featureField;
    result.choice = endpoint(selected.at("node"), "choice");
    if (a.kind == "box")
      boxChoices_[field] = result.choice;
    signals_[endpoint(id, "out").dump()] = std::move(result);
  }
  void rewrite(const Json& old);
  Json& graph_;
  const Json& rule_;
  Json* outer_;
  std::string prefix_;
  std::size_t nextNode_ = 0, nextEdge_ = 0;
  std::set<std::string> selected_, occupied_, edgeIds_, consumed_, wired_;
  std::map<std::string, Json> originals_, incoming_, replacements_, fallbacks_;
  std::map<std::string, GapSignal> signals_;
  std::map<std::string, std::pair<std::string, bool>> params_;
  std::vector<Json> additions_, edges_;
  std::string loadId_, guardId_;
  std::array<std::string, 2> frames_;
  Json mergedRoot_, rawScene_;
  std::map<std::string, Json> boxChoices_;
  std::map<std::string, std::array<GapSignal, 2>> measurementFeatures_;
};
inline void GapRewrite::rewrite(const Json& old) {
  const auto id = old.at("id").get<std::string>(), op = old.at("op").get<std::string>();
  auto p = old.value("params", Json::object());
  if (op == "flow.fallback") {
    require(p.empty(), "GAP已知fallback模板不接受自定义参数");
    fallback(old);
    return;
  }
  if (op == "gap.load_profile_pair") {
    allowed(p, {"source", "dir", "primaryFile", "secondaryFile", "primaryPrefix", "secondaryPrefix",
                "layout", "dropNonFinite"});
    replace(old, op, fileParameters(p));
    rename(id, "dropNonFinite", "dropNonFinite", true);
    for (const auto* role : {"primary", "secondary"})
      output(id, role, role, "sensor");
    output(id, "sourceInfo", "sourceInfo", "record");
    return;
  }
  if (op == "gap.read_scan") {
    allowed(p, {"source", "dir", "primaryFile", "secondaryFile", "primaryPrefix", "secondaryPrefix",
                "layout", "removeOutliers", "outlierRadiusMm", "outlierNeighbors"});
    p = fileParameters(p);
    p["measurementFrame"] = rule_.at("measurement").at("frame");
    if (has(id, "primary") || has(id, "secondary")) {
      const auto a = pass(id, "primary"), b = pass(id, "secondary");
      require(a.kind == "sensor" && b.kind == "sensor", "GAP读入只接受已核验原始传感器路径");
      const auto from =
          incoming_.at(endpoint(id, "primary").dump()).at("from").at("node").get<std::string>();
      const auto fromB =
          incoming_.at(endpoint(id, "secondary").dump()).at("from").at("node").get<std::string>();
      require(from == fromB, "GAP双路读入必须来自同一已知配对或guard生产者");
      const auto& producer = originals_.at(from);
      if (producer.at("op") == "gap.camera_guard") {
        wire(endpoint(from, "quality"), id, "guardQuality");
        const auto originalLoad =
            incoming_.at(endpoint(from, "primary").dump()).at("from").at("node").get<std::string>();
        wire(endpoint(originalLoad, "sourceInfo"), id, "sourceInfo");
      } else {
        require(producer.at("op") == "gap.load_profile_pair", "GAP未知传感器来源生产者");
        wire(endpoint(from, "sourceInfo"), id, "sourceInfo");
      }
    }
    replace(old, op, p);
    output(id, "scan", "scan", "scan");
    return;
  }
  if (op == "gap.camera_guard") {
    allowed(p, {"onDisagree", "maxDeltaMm", "sampleStep", "halfWindow", "minSamples"});
    const auto a = pass(id, "primary"), b = pass(id, "secondary");
    require(a.kind == "sensor" && b.kind == "sensor", "GAP guard必须位于原始传感器路径");
    if (has(id, "box"))
      boundInput(id, "box");
    replace(old, op, p);
    for (const auto* role : {"primary", "secondary"})
      output(id, role, role, "sensor");
    output(id, "quality", "quality", "record");
    return;
  }
  if (op == "gap.to_measurement_frame") {
    allowed(p, {});
    require(pass(id, "cloud", "profile").kind == "sensor", "GAP换轴必须消费完整原始传感器Profile");
    replace(old, "profile.remap_axes",
            {{"axes", "XZY"},
             {"signs", {1, 1, 1}},
             {"targetFrame", rule_.at("measurement").at("frame")}});
    output(id, "cloud", "profile", "profile");
    fineSources();
    const auto load = originals_.at(loadId_).value("params", Json::object());
    if (load.value("dropNonFinite", true)) {
      const auto compact = add("profile.filter_validity", {{"mode", "compact"}});
      wire(endpoint(id, "profile"), compact, "profile");
      signals_[endpoint(id, "cloud").dump()] = {
          endpoint(compact, "profile"), {}, {}, "profile", {}};
    }
    return;
  }
  if (op == "gap.drop_non_finite" || op == "filter.radius_outlier") {
    if (op == "gap.drop_non_finite")
      allowed(p, {});
    else
      allowed(p, {"radius", "minNeighbors", "keepOrganized"});
    require(!p.value("keepOrganized", false), "GAP细粒度旧半径去噪只支持压缩原槽语义");
    p.erase("keepOrganized");
    rename(id, "keepOrganized", "keepOrganized", true);
    replace(old, op == "gap.drop_non_finite" ? "profile.filter_validity" : "profile.radius_outlier",
            op == "gap.drop_non_finite" ? Json{{"mode", "compact"}} : p);
    pass(id, "cloud", "profile");
    output(id, "cloud", "profile", "profile");
    return;
  }
  if (op == "util.merge") {
    allowed(p, {});
    fineSources();
    replace(old, "profile.merge", {{"channelPolicy", "intersection"}});
    pass(id, "a");
    pass(id, "b");
    const auto a = incoming_.at(endpoint(id, "a").dump()).at("from");
    const auto b = incoming_.at(endpoint(id, "b").dump()).at("from");
    auto frame = [&](Json ref) {
      auto producer = originals_.at(ref.at("node").get<std::string>());
      if (producer.at("op") == "gap.drop_non_finite")
        ref = incoming_.at(endpoint(producer.at("id"), "cloud").dump()).at("from");
      return ref.at("node");
    };
    require(frame(a) == frames_[1] && frame(b) == frames_[0],
            "GAP已知合并必须secondary先、primary后，不能猜测逻辑角色");
    output(id, "cloud", "profile", "profile");
    return;
  }
  if (op == "gap.overall_roi") {
    allowed(p, {"roi", "mode", "usingCamera"});
    replace(old, op, p);
    pass(id, "primary");
    pass(id, "secondary");
    output(id, "box", "box", "box", "context");
    return;
  }
  if (op == "gap.datum_window") {
    allowed(p, {"side", "startMm", "lengthMm", "heightMm"});
    replace(old, op, p);
    boundInput(id, "anchor");
    if (has(id, "heightAnchor"))
      boundInput(id, "heightAnchor", nullptr, true, "heightContext");
    output(id, "box", "box", "box", "context");
    return;
  }
  if (op == "filter.crop_box2d") {
    allowed(p, {"bounds", "invert"});
    require(!p.value("invert", false), "GAP已知场景不支持反向裁剪模板");
    rename(id, "invert", "invert", true);
    rename(id, "bounds", "bounds");
    replace(
        old, "profile.crop_box",
        {{"bounds", p.value("bounds", "closed")}, {"mode", "compact"}, {"emptyPolicy", "allow"}});
    pass(id, "cloud", "profile");
    boundInput(id, "box");
    output(id, "cloud", "profile", "profile");
    attachProfileScene(id, "cloud");
    return;
  }
  if (op == "gap.make_scan_pair") {
    allowed(p, {});
    replace(old, op, {{"stage", "processed"}});
    scanSources(id);
    for (const auto* role : {"primary", "secondary", "merged"})
      pass(id, role);
    output(id, "scan", "scan", "scan", {}, endpoint(id, "scan"), "scan_pair");
    return;
  }
  if (op == "gap.split_scan_pair" || op == "gap.split_roi_set") {
    allowed(p, {});
    replace(old, op, p);
    if (op == "gap.split_scan_pair") {
      pass(id, "scan");
      for (const auto* role : {"primary", "secondary", "merged"})
        output(id, role, role, "profile");
    } else {
      pass(id, "rois");
      for (const auto* role : {"datum", "target", "seamLeft", "seamRight", "seam"})
        output(id, role, role, "box", std::string(role) + "Context");
    }
    return;
  }
  if (op == "gap.load_template") {
    allowed(p, {"dir", "left", "right", "layout"});
    rename(id, "layout", "layout", true);
    if (rule_.contains("templates"))
      for (const auto* name : {"unit", "fileFrame", "templateFrame", "handedness"})
        p[name] = rule_.at("templates").at(name);
    replace(old, op, p);
    output(id, "left", "left", "template");
    output(id, "right", "right", "template");
    return;
  }
  if (op == "gap.align_template") {
    const auto cloud = input(id, "cloud"), left = input(id, "tplLeft"),
               right = input(id, "tplRight");
    require(cloud.kind == "profile" && left.kind == "template" && right.kind == "template" &&
                left.value.at("node") == right.value.at("node"),
            "GAP配准需要同一完整模板对及已声明量测轮廓");
    wire(sceneForMerged(incoming_.at(endpoint(id, "cloud").dump()).at("from")), id, "scan");
    wire(endpoint(left.value.at("node"), "templates"), id, "templates");
    replace(old, op, p);
    output(id, "alignment", "alignment", "alignment");
    return;
  }
  if (op == "gap.select_alignment") {
    allowed(p, {"minScore"});
    replace(old, op, p);
    for (const auto* name : {"a", "b", "c", "d"})
      if (has(id, name))
        pass(id, name);
    require(has(id, "a"), "GAP模板选择缺少首个候选");
    output(id, "alignment", "alignment", "alignment");
    return;
  }
  if (op == "gap.business_rois" || op == "gap.roi_from_labels") {
    replace(old, op, p);
    if (op == "gap.business_rois")
      pass(id, "alignment");
    else {
      fineSources();
      const auto a = input(id, "primary"), b = input(id, "secondary");
      const auto sensor = guardId_.empty() ? loadId_ : guardId_;
      require(a.value == endpoint(sensor, "primary") && b.value == endpoint(sensor, "secondary"),
              "GAP模型区域源必须与实际原始双路sensor路径一致");
      wire(rawScene(), id, "scan");
      pass(id, "labels");
      if (has(id, "backdrop"))
        pass(id, "backdrop");
    }
    for (const auto* role : {"flushBase", "flushRef", "gapLeft", "gapRight", "seam"}) {
      output(id, role, role, "box", std::string(role) + "Context", endpoint(id, "rois"), "roi_set");
      signals_.at(endpoint(id, role).dump()).featureField =
          std::string(role) == "flushBase"  ? "datum"
          : std::string(role) == "flushRef" ? "target"
          : std::string(role) == "gapLeft"  ? "seamLeft"
          : std::string(role) == "gapRight" ? "seamRight"
                                            : "seam";
    }
    output(id, "rois", "rois", "rois", {}, endpoint(id, "rois"), "roi_set");
    return;
  }
  if (op == "gap.make_roi_set") {
    allowed(p, {"source", "datumSide"});
    std::string producer;
    for (const auto* role : {"datum", "target", "seamLeft", "seamRight"}) {
      const auto box = input(id, role);
      const auto original =
          incoming_.at(endpoint(id, role).dump()).at("from").at("node").get<std::string>();
      require(producer.empty() || producer == original,
              "GAP业务区域四框必须来自同一已知区域生产者");
      producer = original;
      wire(box.value, id, role);
      wire(box.context, id, std::string(role) + "Context");
    }
    const auto& original = originals_.at(producer);
    require(original.at("op") == "gap.business_rois" || original.at("op") == "gap.roi_from_labels",
            "GAP旧区域集合缺少运行时完整业务信息生产者");
    const bool fromTemplate = original.at("op") == "gap.business_rois";
    const auto source = p.value("source", std::string("template"));
    require(source == (fromTemplate ? "template" : "model"),
            "GAP旧区域source覆盖与实际生产者不一致，拒绝静默改写来源声明");
    const auto side = p.value("datumSide", std::string("auto"));
    const auto producerSide =
        fromTemplate
            ? original.value("params", Json::object()).value("datumSide", std::string("auto"))
            : std::string("auto");
    require((side == "auto" || side == "left" || side == "right") && side == producerSide,
            "GAP旧区域datumSide覆盖无法证明与实际生产者一致，须显式整理侧声明");
    require(!fromTemplate || (!bound(graph_, producer, "datumSide") &&
                              !(outer_ && bound(*outer_, producer, "datumSide",
                                                rule_.at("scope").at("subgraph")))),
            "GAP区域侧的动态生产者覆盖不能与旧make_roi_set静态声明混用");
    require(!fromTemplate || has(id, "alignment"),
            "GAP模板区域缺少旧make_roi_set实际配准来源，无法证明source/自动侧声明等价");
    const auto split = add("gap.split_roi_set");
    wire(endpoint(producer, "rois"), split, "rois");
    wire(endpoint(split, "info"), id, "info");
    if (has(id, "alignment")) {
      const auto alignment = input(id, "alignment");
      require(original.at("op") == "gap.business_rois" &&
                  alignment.value ==
                      signal(incoming_.at(endpoint(producer, "alignment").dump()).at("from")).value,
              "GAP区域集合的配准记录与实际业务框来源不一致");
    }
    if (has(id, "overall"))
      boundInput(id, "overall");
    if (has(id, "cropStatus"))
      pass(id, "cropStatus");
    replace(old, op, {{"metadataMode", "update_metadata"}});
    rename(id, "source", "source", true);
    rename(id, "datumSide", "datumSide", true);
    output(id, "rois", "rois", "rois", {}, endpoint(id, "rois"), "roi_set");
    return;
  }
  if (op == "gap.profile_tensor") {
    allowed(p, {});
    fineSources();
    require(!originals_.at(loadId_).value("params", Json::object()).value("dropNonFinite", true),
            "GAP模型需要明确保留原始槽，拒绝已启用或未声明dropNonFinite=false的旧来源");
    replace(old, op, p);
    require(pass(id, "primary").kind == "sensor" && pass(id, "secondary").kind == "sensor",
            "GAP模型张量只能使用原始sensor槽");
    output(id, "tensor", "tensor", "tensor");
    return;
  }
  if (op == "ml.onnx_run") {
    allowed(p, {"modelPath"});
    replace(old, "gap.infer_profile", p);
    const auto tensor = pass(id, "input");
    const auto producer =
        incoming_.at(endpoint(id, "input").dump()).at("from").at("node").get<std::string>();
    require(tensor.kind == "tensor" && originals_.at(producer).at("op") == "gap.profile_tensor",
            "GAP推理必须消费已核验双路profile_tensor");
    output(id, "output", "output", "tensor");
    return;
  }
  if (op == "gap.labels_from_logits") {
    allowed(p, {});
    replace(old, op, p);
    pass(id, "tensor");
    const auto inference =
        incoming_.at(endpoint(id, "tensor").dump()).at("from").at("node").get<std::string>();
    require(originals_.at(inference).at("op") == "ml.onnx_run", "GAP标签只接受实际已知推理路径");
    const auto tensor = incoming_.at(endpoint(inference, "input").dump()).at("from");
    for (const auto* role : {"primary", "secondary"})
      wire(signal(incoming_.at(endpoint(tensor.at("node"), role).dump()).at("from")).value, id,
           role);
    output(id, "labels", "labels", "labels");
    return;
  }
  if (op == "gap.labels_to_cloud") {
    allowed(p, {"row"});
    rename(id, "row", "row", true);
    replace(old, op, p);
    input(id, "cloud");
    fineSources();
    const auto row = p.value("row", std::string("primary"));
    require(row == "primary" || row == "secondary", "GAP着色行必须为primary或secondary");
    require(incoming_.at(endpoint(id, "cloud").dump()).at("from") ==
                endpoint(frames_[row == "primary" ? 0 : 1], "cloud"),
            "GAP标签着色必须直接对应原始完整换轴行，不能猜测压缩槽");
    wire(rawScene(), id, "scan");
    pass(id, "labels");
    output(id, "cloud", "cloud", "cloud");
    return;
  }
  if (op == "gap.roll_anchored_crop") {
    const auto primary = input(id, "primary"), secondary = input(id, "secondary"),
               merged = input(id, "merged");
    const auto scan = add("gap.make_scan_pair", {{"stage", "processed"}});
    scanSources(scan);
    const std::array<GapSignal, 3> values{primary, secondary, merged};
    const std::array<const char*, 3> roles{"primary", "secondary", "merged"};
    for (std::size_t i = 0; i < 3; ++i)
      wire(values[i].value, scan, roles[i]);
    replace(old, op, p);
    wire(endpoint(scan, "scan"), id, "scan");
    boundInput(id, "gapLeft");
    boundInput(id, "gapRight");
    const auto split = add("gap.split_scan_pair");
    wire(endpoint(id, "scan"), split, "scan");
    for (const auto* role : roles) {
      signals_[endpoint(id, role).dump()] = {
          endpoint(split, role), {}, endpoint(id, "scan"), "profile", "scan_pair"};
      signals_.at(endpoint(id, role).dump()).featureField = role;
    }
    output(id, "window", "window", "box", "windowContext");
    output(id, "status", "status", "record");
    return;
  }
  if (op == "gap.fit_line") {
    allowed(p, {"distThresh", "minInliers", "segmentPoints", "lineType", "endpoints", "dirMode",
                "dirNominalDeg", "dirTolDeg"});
    p["distanceThreshold"] = p.value("distThresh", .1) / 1000.0;
    p.erase("distThresh");
    rename(id, "distThresh", "distanceThreshold", true);
    replace(old, "profile.fit_line_robust", p);
    pass(id, "cloud", "profile");
    boundInput(id, "box");
    boundInput(id, "toward");
    if (has(id, "refLine"))
      boundInput(id, "refLine");
    bool reference = false;
    for (const auto& edge : incoming_)
      if (edge.second.at("from") == endpoint(id, "innerEnd"))
        reference = true;
    Json feature;
    const auto featureKind = reference ? "reference_feature" : "line_feature";
    if (reference) {
      const auto maker = add("gap.make_reference_feature", {{"method", "line_end"}});
      for (const auto* field : {"profile", "line", "lineContext", "pointContext", "quality"})
        wire(endpoint(id, field), maker, field);
      wire(endpoint(id, "innerEnd"), maker, "point");
      wire(endpoint(id, "indexMap"), maker, "selection");
      feature = endpoint(maker, "feature");
    } else
      feature = lineFeature(id);
    output(id, "line", "line", "line", "lineContext", feature, featureKind);
    output(id, "innerEnd", "innerEnd", "point", "pointContext", feature, featureKind);
    output(id, "quality", "quality", "quality", {}, feature, featureKind);
    return;
  }
  if (op == "gap.selected_point" || op == "gap.nearest_to_line") {
    allowed(p, {});
    const auto profile = input(id, "cloud");
    require(profile.kind == "profile", "GAP选点需要已声明真实轮廓");
    replace(old, op == "gap.selected_point" ? "profile.nearest_sample" : "profile.nearest_to_line",
            p);
    wire(profile.value, id, "profile");
    if (op == "gap.selected_point") {
      const auto box = input(id, "box");
      const auto anchor = add("geometry.box_anchor", {{"anchor", "minimum"}});
      wire(box.value, anchor, "box");
      const auto binding = add("profile.bind_geometry",
                               {{"frame", rule_.at("measurement").at("frame")}, {"plane", "XY"}});
      wire(profile.value, binding, "profile");
      wire(endpoint(anchor, "point"), binding, "geometry");
      wire(endpoint(binding, "geometry"), id, "anchor");
      wire(endpoint(binding, "context"), id, "anchorContext");
    } else
      boundInput(id, "line");
    const auto feature =
        add("gap.make_reference_feature",
            {{"method", op == "gap.selected_point" ? "selected_point" : "nearest_point"}});
    for (const auto* field : {"profile", "point", "pointContext", "selection"})
      wire(endpoint(id, field), feature, field);
    output(id, "point", "point", "point", "pointContext", endpoint(feature, "feature"),
           "reference_feature");
    return;
  }
  if (op == "gap.locate_template" || op == "gap.locate_model") {
    require(pass(id, "scan").kind == "scan", "GAP定位必须消费完整ScanPair来源");
    if (op == "gap.locate_model")
      allowed(p, {"modelPath", "cropEnabled", "cropHalfWidth", "cropHalfHeight",
                  "cropMaxRollBoxHeight", "cropMinPointsKept", "cropUsingCamera"});
    else {
      std::set<std::string> names{
          "templateDir",      "overallRoi",      "overallMode",    "overallCamera", "datumSide",
          "initialPoseMode",  "maxMatchingDist", "maxFitnessDist", "maxIterations", "normalKnn",
          "minScore",         "bidirection",     "globalCoarse",   "successGuide",  "segRoi",
          "trustTranslation", "trustRotation",   "degenerateRatio"};
      for (int k = 1; k <= 4; ++k)
        for (const auto* suffix : {"Enabled", "Id", "Left", "Right", "DatumRoi", "TargetRoi",
                                   "SeamLeftRoi", "SeamRightRoi", "RoisFrom"})
          names.insert("template" + std::to_string(k) + suffix);
      for (auto it = p.begin(); it != p.end(); ++it)
        require(names.count(it.key()), "GAP旧模板定位参数不受支持: " + it.key());
      if (rule_.contains("templates"))
        for (const auto& fields : {std::make_pair("unit", "templateUnit"),
                                   std::make_pair("fileFrame", "templateFileFrame"),
                                   std::make_pair("templateFrame", "templateFrame"),
                                   std::make_pair("handedness", "templateHandedness")})
          p[fields.second] = rule_.at("templates").at(fields.first);
    }
    replace(old, op, p);
    location(id);
    if (op == "gap.locate_template")
      output(id, "alignment", "alignment", "alignment");
    return;
  }
  if (op == "gap.role_line" || op == "gap.ref_point" || op == "gap.datum_direction") {
    require(pass(id, "scan").kind == "scan" && pass(id, "rois").kind == "rois",
            "GAP角色拟合需要完整ScanPair/RoiSet");
    allowed(p, {"role", "cloud", "method", "distThresh", "minInliers", "segmentPoints", "lineType",
                "endpoints", "dirMode", "dirNominalDeg", "dirTolDeg", "startMm", "lengthMm",
                "heightMm"});
    if (has(id, "refLine"))
      boundInput(id, "refLine");
    if (has(id, "baseLine"))
      boundInput(id, "baseLine");
    replace(old, op, p);
    const auto kind = op == "gap.ref_point" ? "reference_feature" : "line_feature";
    const auto feature = endpoint(id, "feature");
    output(id, "line", "line", "line", "lineContext", feature, kind);
    output(id, op == "gap.ref_point" ? "point" : "innerEnd",
           op == "gap.ref_point" ? "point" : "innerEnd", "point", "pointContext", feature, kind);
    output(id, "quality", "quality", "quality", {}, feature, kind);
    if (op == "gap.datum_direction")
      output(id, "window", "window", "box", "windowContext");
    return;
  }
  if (op == "gap.seam_circles" || op == "gap.fit_gap_circles") {
    if (op == "gap.seam_circles")
      require(pass(id, "scan").kind == "scan" && pass(id, "rois").kind == "rois",
              "GAP双圆需要同源场景");
    else {
      for (const auto* role : {"primary", "secondary", "merged"})
        pass(id, role);
      boundInput(id, "boxLeft");
      boundInput(id, "boxRight");
    }
    if (has(id, "refLine"))
      boundInput(id, "refLine");
    if (has(id, "refLineRight"))
      boundInput(id, "refLineRight");
    replace(old, op, p);
    Json feature = endpoint(id, "feature");
    if (op == "gap.fit_gap_circles") {
      const auto maker = add("gap.make_circle_pair");
      for (const auto* side : {"left", "right"})
        for (const auto* suffix : {"", "Context", "Profile", "Inliers"})
          wire(endpoint(id, std::string(side) + suffix), maker, std::string(side) + suffix);
      wire(endpoint(id, "quality"), maker, "quality");
      feature = endpoint(maker, "feature");
    }
    for (const auto* side : {"left", "right"})
      output(id, side, side, "circle", std::string(side) + "Context", feature, "circle_pair");
    output(id, "quality", "quality", "quality", {}, feature, "circle_pair");
    return;
  }
  if (op == "gap.flush") {
    allowed(p, {"scale", "offset", "signed"});
    measurementFeatures_[id] = {input(id, "baseLine"), input(id, "refPoint")};
    Json target{{"gain", p.value("scale", 1.0)},
                {"offsetMm", p.value("offset", 0.0)},
                {"signed", p.value("signed", true)},
                {"signConvention", "canonical_axis"},
                {"outputUnit", "mm"}};
    replace(old, "metrology.flush_point_line", target);
    boundInput(id, "baseLine");
    boundInput(id, "refPoint");
    rename(id, "scale", "gain");
    rename(id, "offset", "offsetMm");
    rename(id, "signed", "signed");
    for (const auto* name : {"value", "foot", "segment", "baseLine"})
      output(id, name, name,
             std::string(name) == "value"  ? "measurement"
             : std::string(name) == "foot" ? "point"
                                           : "line",
             std::string(name) == "value" ? "" : std::string(name) + "Context");
    return;
  }
  if (op == "gap.gap") {
    allowed(p, {"definition", "offset"});
    measurementFeatures_[id] = {input(id, "left"), input(id, "right")};
    require(measurementFeatures_.at(id)[0].feature == measurementFeatures_.at(id)[1].feature,
            "GAP旧间隙的双圆不是同一个完整原子拟合结果，拒绝混配质量来源");
    const auto definition = p.value("definition", std::string("B"));
    require(definition == "A" || definition == "B", "GAP旧圆间隙定义必须为A或B");
    replace(old, "metrology.circle_gap",
            {{"definition", definition == "A" ? "datum_tangents" : "center_line"},
             {"offsetMm", p.value("offset", 0.0)},
             {"outputUnit", "mm"}});
    boundInput(id, "left");
    boundInput(id, "right");
    if (has(id, "baseLine"))
      boundInput(id, "baseLine");
    rename(id, "definition", "definition", true);
    rename(id, "offset", "offsetMm");
    output(id, "value", "value", "measurement");
    output(id, "segment", "segment", "line", "segmentContext");
    return;
  }
  if (op == "gap.point_offset") {
    allowed(p, {"absDx", "absDy", "scaleDx", "scaleDy", "dxOffset", "dyOffset"});
    replace(old, "metrology.point_offset",
            {{"absDx", p.value("absDx", false)},
             {"absDy", p.value("absDy", false)},
             {"gainDx", p.value("scaleDx", 1.0)},
             {"gainDy", p.value("scaleDy", 1.0)},
             {"dxOffsetMm", p.value("dxOffset", 0.0)},
             {"dyOffsetMm", p.value("dyOffset", 0.0)},
             {"outputUnit", "mm"}});
    boundInput(id, "a");
    boundInput(id, "b");
    rename(id, "absDx", "absDx");
    rename(id, "absDy", "absDy");
    for (const auto& fields :
         {std::make_pair("scaleDx", "gainDx"), std::make_pair("scaleDy", "gainDy"),
          std::make_pair("dxOffset", "dxOffsetMm"), std::make_pair("dyOffset", "dyOffsetMm")})
      rename(id, fields.first, fields.second);
    for (const auto* field : {"dx", "dy", "distance"})
      output(id, field, field, "measurement");
    output(id, "segment", "segment", "line", "segmentContext");
    return;
  }
  if (op == "gap.judge") {
    allowed(p, {"nominal", "upper", "lower"});
    pass(id, "value");
    replace(old, op, p);
    output(id, "value", "value", "measurement");
    return;
  }
  if (op == "gap.result_bundle") {
    replace(old, op, p);
    const auto measuredFrom = [&](const char* port) -> const std::array<GapSignal, 2>& {
      const auto source =
          incoming_.at(endpoint(id, port).dump()).at("from").at("node").get<std::string>();
      const auto found = measurementFeatures_.find(source);
      require(found != measurementFeatures_.end(), "GAP结果缺少实际测量几何的生产者证据");
      return found->second;
    };
    const auto& flushFeatures = measuredFrom("flush");
    const auto& circleFeatures = measuredFrom("gap");
    for (const auto& mapping :
         {std::make_pair("fitBase", &flushFeatures[0]), std::make_pair("fitRef", &flushFeatures[1]),
          std::make_pair("fits", &circleFeatures[0])})
      if (has(id, mapping.first)) {
        const auto quality = input(id, mapping.first);
        require(quality.feature.is_object() && quality.feature == mapping.second->feature,
                std::string("GAP质量与实际测量几何未选择同一原子特征，拒绝跨生产者混配: ") +
                    mapping.first);
      }
    const auto scan = input(id, "scan"), rois = input(id, "rois");
    if (rule_.at("component").at("style") == "fine" &&
        (scan.choice.is_object() || rois.choice.is_object())) {
      require(scan.choice.is_object() && rois.choice.is_object(),
              "GAP旧细粒度定位必须同时声明实际scan与roi的fallback选择");
      const auto location = add("gap.make_location");
      wire(scan.value, location, "scan");
      wire(rois.value, location, "rois");
      wire(scan.choice, location, "scanChoice");
      wire(rois.choice, location, "roiChoice");
      for (const auto& choice : boxChoices_)
        if (choice.first != "seam")
          wire(choice.second, location, choice.first + "Choice");
      const auto split = add("gap.split_location");
      wire(endpoint(location, "location"), split, "location");
      wire(endpoint(split, "scan"), id, "scan");
      wire(endpoint(split, "rois"), id, "rois");
    } else {
      wire(scan.value, id, "scan");
      wire(rois.value, id, "rois");
    }
    for (const auto* name : {"gap", "flush", "fits", "fitBase", "fitRef", "fallback"})
      if (has(id, name)) {
        const auto value = pass(id, name);
        const auto port = std::string(name) == "fitBase"  ? "baseFeature"
                          : std::string(name) == "fitRef" ? "refFeature"
                          : std::string(name) == "fits"   ? "circleFeature"
                                                          : "";
        const auto expected = std::string(name) == "fitBase"  ? "line_feature"
                              : std::string(name) == "fitRef" ? "reference_feature"
                                                              : "circle_pair";
        if (*port && value.featureKind == expected)
          wire(value.feature, id, port);
      }
    output(id, "bundle", "bundle", "record");
    return;
  }
  throw std::invalid_argument("GAP整图升级尚未实现该旧模板节点的完整转换: " + id + " " + op);
}
}
