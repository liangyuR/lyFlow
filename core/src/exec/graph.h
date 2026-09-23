#pragma once
// GraphDoc 的 C++ 侧解析结果：graph_json →parse→ RawGraph →expand→ 平图 →validate→ 诊断 →compile→ Plan。
// 解析阶段不抛异常，结构性问题也是诊断（C++ 不能信任传进来的 GraphDoc）。
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lyflow/status.h"

namespace lyflow::exec {

struct RawNode {
  std::string id;
  std::string op;
  std::string opVersion;
  /// 静音：不调 compute，输出从类型兼容的输入透传（E5）。
  bool bypass = false;
  /// 稀疏参数，原样保留 JSON —— 报错信息里要能说出用户到底填了什么。
  nlohmann::json params = nlohmann::json::object();
  /// 展开子图时由提升参数（ADR-0010 F4）写进来的那些键。展开之后 params 里
  /// 「用户在这个节点上填的」和「外层子图表单灌进来的」长得一模一样，
  /// 而 `lyflow params` 的 source 要分得开这两件事，所以记一笔。
  std::set<std::string> boundParams;
  /// 由顶层图参数（m7-plan J7）写进来的键 → 是哪个顶层参数。与 boundParams 同一个用途，
  /// 另记名字是因为 `lyflow params` 要说出「这个值归哪个顶层参数管」。
  std::map<std::string, std::string> graphParams;
};

struct RawEdge {
  std::string id;
  std::string fromNode, fromPort;
  std::string toNode, toPort;
};

/// 子图的一个输入：外面接进来的一条边，可扇出到多个内部端口（F3）。
struct SubInput {
  std::string name, type, label, doc;
  std::vector<std::pair<std::string, std::string>> to;
};

/// 子图的一个输出：只能来自一个内部端口。
struct SubOutput {
  std::string name, type, label, doc;
  std::string node, port;
};

/// 提升出来的对外参数（F4）。一个外参可绑多个内参。
struct SubParam {
  std::string name;
  /// 原始声明，合成 OperatorDesc 时按 manifest 的 param 形态读。
  nlohmann::json decl = nlohmann::json::object();
  std::vector<std::pair<std::string, std::string>> binds;
};

/// 子图定义。GraphDoc 的 `subgraphs` 与库文件 `*.lyflow-op.json` 共用这一份结构。
struct SubgraphDef {
  std::string id;
  std::string name, doc, category, version = "1.0.0";
  std::vector<std::string> keywords;
  std::vector<RawNode> nodes;
  std::vector<RawEdge> edges;
  std::vector<SubInput> inputs;
  std::vector<SubOutput> outputs;
  std::vector<SubParam> params;
};

/// 顶层图参数（m7-plan J7）。语义与子图的提升参数相同：展开期把值写进被绑定的节点参数。
/// 宿主运行期给的值（CLI `--param`、C ABI `params_json`）取代 default。
struct GraphParam {
  std::string name;
  /// 原始声明 { type?, default, binds, doc? }。
  nlohmann::json decl = nlohmann::json::object();
  std::vector<std::pair<std::string, std::string>> binds;
};

/// 图级命名输出（ADR-0017）。宿主只认名字，不认节点 id。
struct GraphOutput {
  std::string name;
  std::string node;
  std::string port;
};

struct RawGraph {
  std::string id;
  std::vector<RawNode> nodes;
  std::vector<RawEdge> edges;
  /// 图内定义的子图，键是 subgraphId（节点用 `sub:<id>` 引用）。
  std::map<std::string, SubgraphDef> subgraphs;
  /// 按名字升序（JSON 对象键的顺序），展开后仍用路径 id 指节点。
  std::vector<GraphOutput> outputs;
  /// 按名字升序。
  std::vector<GraphParam> params;
};

/// 解析并做结构校验。返回 false 表示图不可用（诊断已写进 diags）。
bool parseGraph(const std::string& json, RawGraph& out, Diagnostics& diags);

/// 把宿主给的顶层参数值（JSON 对象 名字→值）盖到 default 上。未声明的名字报
/// unknown_param。values 为 null 或空对象时什么都不做。
bool applyGraphParamValues(const nlohmann::json& values, RawGraph& graph, Diagnostics& diags);

/// 从一份 JSON 对象读一个子图定义。失败时填 error 并返回 false。
bool parseSubgraphDef(const nlohmann::json& j, const std::string& id, SubgraphDef& out,
                      std::string& error);

}  // namespace lyflow::exec
