#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>

#include <nlohmann/json.hpp>

namespace lyflow {
class Registry;

struct GraphMigrationError {
  std::string nodeId;
  std::string message;
};

/// 按 request（{"schemaVersion":1,"rules":[...]}）逐条执行已注册的图迁移规则，结果写进 migrated。
/// 规则带 {"scope":{"subgraph":<定义ID>}} 时作用于该子图定义。只由 `lyflow migrate` 离线调用；
/// 失败返回 false 并填写 error，migrated 内容未定义。
bool migrateGraph(const Registry& registry, const nlohmann::json& graph,
                  const nlohmann::json& request, nlohmann::json& migrated,
                  GraphMigrationError& error);

/// 迁移规则实现共用的小工具，不满足前提时抛 std::invalid_argument。
namespace graph_migration {
void require(bool condition, const std::string& why);
/// object[field] 必须是非空字符串。
std::string text(const nlohmann::json& object, const char* field);
/// object 必须是对象，且字段恰好是 names。
void keys(const nlohmann::json& object, std::initializer_list<const char*> names);
/// 按 id 找节点下标，找不到返回 nodes.size()。
std::size_t nodeIndex(const nlohmann::json& nodes, const std::string& id);
/// 按两端找连线下标，找不到返回 edges.size()。
std::size_t edgeIndex(const nlohmann::json& edges, const nlohmann::json& from,
                      const nlohmann::json& to);
/// 契约转换不能被旁路。
void requireEnabled(const nlohmann::json& node);
/// 在 rule.source → rule.target 这条连线上插入适配节点 rule.adapterId（adapterOp@1.0.0，
/// 输入输出端口都叫 adapterPort）。源节点必须正是 rule.source.op@rule.source.opVersion。
void insertEdgeAdapter(nlohmann::json& level, const nlohmann::json& rule,
                       const std::string& adapterOp, const std::string& adapterPort,
                       nlohmann::json params);
} // namespace graph_migration
} // namespace lyflow
