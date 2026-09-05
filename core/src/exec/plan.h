#pragma once
//
// 校验 + 编译。
//
// 两条设计要点：
//
// **D5：校验一次返回全部诊断。** 走完所有节点、所有参数再回来，而不是撞到第一个
// 错误就 return。前端因此能一次把所有红框标出来，用户不用「改一个 → 重跑 →
// 又一个」地挤牙膏。这件事事后改要动 C++/Rust/前端三层，所以第一天就做对。
//
// **校验错误是节点级的，不是整图级的。** 一个节点参数填错，其余节点照常执行，
// 只有它的下游标 cancelled/upstream_failed。这是「一次看到所有能看到的」——
// 整图中止的话，用户改完这个错还得再跑一遍才知道后面还有没有问题。
// 只有排不出拓扑序（有环）或者 JSON 根本读不了才是整图级失败。
//
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
