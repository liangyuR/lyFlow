#pragma once
// 校验 + 编译。D5：一次返回全部诊断，不在第一个错误处早退。
// 校验错误是节点级的 —— 只有有环或 JSON 读不了才是整图级失败。
#include <filesystem>
#include <map>
#include <set>
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
  bool acceptsError = false;
  bool lazy = false;
};

struct PlanNode {
  std::string id;
  const OperatorDesc* op = nullptr;   ///< 未知算子时为 nullptr
  int level = 0;                      ///< 同层可并行，就绪队列按它排序
  bool valid = true;                  ///< false = 该节点自身校验没过，执行时直接标 error
  bool bypass = false;                ///< 静音：不调 compute，输出从输入透传（E5）
  /// 只被惰性端口依赖：不进初始就绪队列，被 demand 时才调度（ADR-0016）。
  bool deferred = false;
  /// 该节点的输出由 RunOptions::inputs 注入：不调 compute（ADR-0017）。
  bool provided = false;
  /// 由宿主注入的**输入**端口（m8-plan L18）：compute 照常调，这些端口的值取注入数据。
  std::set<std::string> injectedInputs;
  /// 在 BuildOptions::isolate 里（按路径前缀展开后）：它的上游只许命中缓存（node-run R2）。
  /// 它自己照常查缓存 —— 修订一 V1 起 isolate 不再隐含强制重算。
  bool isolated = false;
  /// 在 BuildOptions::force 里（按路径前缀展开后）：跳过缓存查找、强制执行、结果覆盖写回（修订一 V1）。
  bool forced = false;
  std::vector<Diagnostic> errors;     ///< 该节点的全部阻塞性诊断（D5）
  ParamMap params;                    ///< 已合并默认值（迁移之后的）
  /// 图里**显式写了**的参数键，**跑完迁移之后**的那一份。`lyflow params` 的
  /// source 靠它分开「用户填的」与「合进来的默认值」。不能回头去看 RawNode：
  /// 迁移会改名（v1 的 count 在 v2 叫 keepCount），那时两边对不上。
  std::set<std::string> explicitParams;
  /// explicitParams 里由子图提升参数灌进来的那些（ADR-0010 F4）。展开之后
  /// 「用户在这个节点上填的」和「外层子图表单灌进来的」在 params 里长得一模一样。
  std::set<std::string> boundParams;
  /// explicitParams 里由顶层图参数（J7）灌进来的那些 → 顶层参数名。
  std::map<std::string, std::string> graphParams;
  std::vector<InputBinding> inputs;
  /// 输出端口 -> 下游消费者数。
  std::unordered_map<std::string, int> consumers;
  std::string cacheKey;

  /// Any 端口沿连线推导出的具体类型（E6）。仍为 Any 的端口不在表里。
  std::unordered_map<std::string, std::string> inputTypes;
  std::unordered_map<std::string, std::string> outputTypes;

  /// 并行调度用：去重后的上下游下标（E2 的依赖计数）。
  std::vector<int> upstream;
  std::vector<int> downstream;
};

/// 编译好的图级输出。node 是 Plan::nodes 的下标，-1 = 该节点不在本次计划里。
struct PlanOutput {
  std::string name;
  std::string nodeId;
  std::string port;
  int node = -1;
};

struct Plan {
  std::string runId;
  std::vector<PlanNode> nodes;  ///< 拓扑序
  std::vector<PlanOutput> outputs;
  bool ok = false;              ///< false = 整图级失败（有环 / 解析失败 / 目标不存在）
};

struct BuildOptions {
  std::string runId;
  std::filesystem::path baseDir;
  /// Run to node：只保留这些节点的上游闭包。空 = 跑全图。
  /// 目标按路径前缀匹配：给一个子图节点的 id 等于给它展开后的全部内部节点（F2）。
  std::vector<std::string> targets;
  /// 只运行这些节点（node-run R1）：id 语义与 targets 相同，子图节点按路径前缀展开。
  /// 编译照常保留 targets 的上游闭包（cacheKey 要靠它算），这里只负责给命中的节点标 isolated；
  /// 「targets 取同一组 id」由调用方保证（执行器在 Run 的构造里做）。
  std::vector<std::string> isolate;
  /// 强制重算这些节点（修订一 V1），id 语义同 targets。可与 targets / isolate / preview 任意组合；
  /// 不在本次计划里的 id 没有效果（它根本不跑）。
  std::vector<std::string> force;
  /// 非空时混进每个 cacheKey，把预览结果关进独立的缓存命名空间（F5）。
  std::string cacheNamespace;
  /// 被注入的节点 id → 注入数据的摘要。进 cacheKey（输出注入与输入注入都进）。
  std::unordered_map<std::string, std::string> providedDigest;
  /// 被注入的节点 id → 注入的端口名。编译期按算子声明分两种：名字是输出端口就是
  /// 整节点注入（provided，ADR-0017），只是输入端口就是输入注入（m8-plan L18）。
  std::unordered_map<std::string, std::set<std::string>> injectedPorts;
};

/// 校验 + 编译。诊断（含 warning 与迁移）全部写进 diags；节点级诊断同时挂在 PlanNode 上。
bool buildPlan(const Registry& registry, const RawGraph& graph, const BuildOptions& options,
               Plan& out, Diagnostics& diags);

/// 参数规范化后的 JSON（键排序、浮点稳定）。cacheKey 与测试都用它。
std::string canonicalParamsJson(const ParamMap& params);

/// 端口声明类型 + 推导表 → 实际类型。推导不出来时返回声明类型（可能是 "Any"）。
const std::string& effectiveType(const std::unordered_map<std::string, std::string>& resolved,
                                 const std::string& port, const std::string& declared);

}  // namespace lyflow::exec
