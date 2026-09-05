#pragma once
// 校验 + 编译。D5：一次返回全部诊断，不在第一个错误处早退。
// 校验错误是节点级的 —— 只有有环或 JSON 读不了才是整图级失败。
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "exec/graph.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow/status.h"

namespace lyflow::exec {

struct InputBinding {
  std::string port;        ///< 本节点的输入端口名
  int fromNode = -1;       ///< 上游在 Plan::nodes 里的下标
  std::string fromPort;
};

struct PlanNode {
  std::string id;
  const OperatorDesc* op = nullptr;   ///< 未知算子时为 nullptr
  int level = 0;                      ///< 同层可并行。M2 不用，M3 的并行执行靠它
  bool valid = true;                  ///< false = 该节点自身校验没过，执行时直接标 error
  std::vector<Diagnostic> errors;     ///< 该节点的全部阻塞性诊断（D5）
  ParamMap params;                    ///< 已合并默认值
  std::vector<InputBinding> inputs;
  /// 输出端口 -> 下游消费者数。M2 只是记着，M3 的按需释放靠它。
  std::unordered_map<std::string, int> consumers;
  std::string cacheKey;
};

struct Plan {
  std::string runId;
  std::vector<PlanNode> nodes;  ///< 拓扑序
  bool ok = false;              ///< false = 整图级失败（有环 / 解析失败 / 目标不存在）
};

struct BuildOptions {
  std::string runId;
  std::filesystem::path baseDir;
  /// Run to node：只保留这些节点的上游闭包。空 = 跑全图。
  std::vector<std::string> targets;
};

/// 校验 + 编译。诊断（含 warning）全部写进 diags；节点级诊断同时挂在 PlanNode 上。
bool buildPlan(const Registry& registry, const RawGraph& graph, const BuildOptions& options,
               Plan& out, Diagnostics& diags);

/// 参数规范化后的 JSON（键排序、浮点稳定）。cacheKey 与测试都用它。
std::string canonicalParamsJson(const ParamMap& params);

}  // namespace lyflow::exec
