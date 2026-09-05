#pragma once
// GraphDoc 的 C++ 侧解析结果：graph_json →parse→ RawGraph →validate→ 诊断 →compile→ Plan。
// 解析阶段不抛异常，结构性问题也是诊断（C++ 不能信任传进来的 GraphDoc）。
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
