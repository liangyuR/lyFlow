#pragma once
//
// GraphDoc 的 C++ 侧解析结果。
//
// 这是执行流水线的第一站：
//   graph_json ──parse──► RawGraph ──validate──► Diagnostics ──compile──► Plan
//
// 解析阶段**不抛异常**。GraphDoc 可能来自手改的文件、脚本、旧版本客户端
// （docs/architecture.md：C++ 不能信任传进来的 GraphDoc），所以「重复 id」
// 「边指向不存在的节点」这类结构性问题也是诊断，和类型不匹配一样回到前端标红，
// 而不是让整次运行以一句「JSON 解析失败」收场。
//
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lyflow/status.h"

namespace lyflow::exec {

struct RawNode {
  std::string id;
  std::string op;
  std::string opVersion;
  /// M2 只解析不使用：bypass 是执行语义（不是 UI 状态），M3 实现透传。
  bool bypass = false;
  /// 稀疏参数，原样保留 JSON —— 报错信息里要能说出用户到底填了什么。
  nlohmann::json params = nlohmann::json::object();
};

struct RawEdge {
  std::string id;
  std::string fromNode, fromPort;
  std::string toNode, toPort;
};

struct RawGraph {
  std::string id;
  std::vector<RawNode> nodes;
  std::vector<RawEdge> edges;
};

/// 解析并做结构校验。返回 false 表示图不可用（诊断已写进 diags）。
bool parseGraph(const std::string& json, RawGraph& out, Diagnostics& diags);

}  // namespace lyflow::exec
