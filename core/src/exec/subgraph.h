#pragma once
// 子图展开（ADR-0010 / F1）。compile 之前把 `sub:` 与 `lib.` 节点递归替换成平图，
// 执行器、缓存、事件对子图一无所知。
#include <string>
#include <vector>

#include "exec/graph.h"
#include "lyflow/manifest.h"
#include "lyflow/registry.h"
#include "lyflow/status.h"

namespace lyflow::exec {

/// 展开的深度上限。超过多半是配置错了而不是真有 32 层。
constexpr int kMaxSubgraphDepth = 32;

/// 路径分隔符（F2）。localId 的字符集里没有它，所以拼出来的路径可以反解。
constexpr char kPathSeparator = '/';

/// 一份参数声明（JSON：manifest 的 param 形态，不含 name）→ Param。子图提升参数与
/// 顶层图参数的规格（P1.1）共用。声明是数据不是代码：认不出的 type 退回 float，坏字段忽略。
Param paramFromDecl(const std::string& name, const nlohmann::json& decl);

/// 子图定义 → 临时 OperatorDesc。端口与参数来自 inputs/outputs/params，
/// compute 是一个只会在「展开漏了」时才被调到的桩。
OperatorDesc synthesizeOperator(const SubgraphDef& def, const std::string& opId);

/// 节点的 op 是不是一个子图引用。是的话把定义指针写进 out。
const SubgraphDef* findSubgraphDef(const RawGraph& graph, const std::string& opId);

/// 递归展开成平图。失败时诊断已写进 diags，out 的内容不可用。
bool expandGraph(const RawGraph& in, RawGraph& out, Diagnostics& diags);

}  // namespace lyflow::exec
