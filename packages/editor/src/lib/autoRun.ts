// 自动运行管所有改参数的方式（2026-10-04 拍板，修订 ADR-0011）：以前只有拖滑块 / 数字框松手补一次正式运行，
// 敲回车、下拉框、勾选框、↺、右键粘贴值、恢复这组参数、撤销都不跑，画面停在上一次的结果上。
// 这里只放纯函数：一步提交前后，哪些节点的参数（有效值）、静音，哪些图参数的取值变了。调度在 lib/preview。

import { listGraphNodes } from "./findNodes";
import { splitBind } from "./graphParams";
import { effectiveParams, valueEquals } from "./params";
import { levelOf } from "./subgraph";
import type { GraphDoc } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

export interface ParamEditTargets {
  /** 参数（有效值）或静音变了的节点，完整 id。子图定义里改的：每个实例各一个。 */
  nodes: string[];
  /** 取值（default 叠上当前配方）变了、现在还在的图参数。 */
  graphParams: string[];
}

/** 一步提交前后改了哪些会影响结果的参数。增删节点、换算子、连线这些结构上的改动不算（要跑请按 F5）；
 *  稀疏存储里写上缺省值、挪节点、改名、折叠也不算。paramsBefore / paramsAfter 是前后的图参数取值（runParamsOf 那一份）。 */
export function paramEditTargets(
  before: GraphDoc,
  after: GraphDoc,
  paramsBefore: Readonly<Record<string, unknown>> | undefined,
  paramsAfter: Readonly<Record<string, unknown>> | undefined,
  ops: ReadonlyMap<string, OperatorDesc>,
): ParamEditTargets {
  // 谁提供参数变了（纳入配方、绑定 / 解绑、删图参数、改名、提升成子图参数）：那个参数的值是搬家，不是改动 —— 节点上的显式值被删、
  // 图参数凭空多出来，逐个比会当成改了、白跑一次。只跳过提供者变了的那几个参数；同一步里别的参数（跳回历史时跨过一次纳入配方）照样算
  const providedBefore = providers(before);
  const providedAfter = providers(after);
  const moved = (scope: string, node: string, param: string): boolean => {
    const key = `${scope}|${node}.${param}`;
    return providedBefore.get(key) !== providedAfter.get(key);
  };
  const nodes: string[] = [];
  if (before !== after) {
    const was = new Map(listGraphNodes(before, ops).map((e) => [e.id, e]));
    for (const now of listGraphNodes(after, ops)) {
      const old = was.get(now.id);
      if (!old || old.opId !== now.opId) continue;
      if (old.muted !== now.muted) {
        nodes.push(now.id);
        continue;
      }
      const a = levelOf(after, now.path).nodes.find((n) => n.id === now.localId);
      const b = levelOf(before, old.path).nodes.find((n) => n.id === old.localId);
      if (!a || !b || a.params === b.params) continue;
      const op = ops.get(now.opId);
      if (!op) continue;
      const pa = effectiveParams(op, a);
      const pb = effectiveParams(op, b);
      const scope = now.path.length > 0 ? now.path[now.path.length - 1]!.subgraphId : "";
      if (op.params.some((p) => !moved(scope, now.localId, p.name) && !valueEquals(pa[p.name], pb[p.name]))) nodes.push(now.id);
    }
  }
  // 图参数：前后都在、绑的还是那几个，取值变了才算（新纳入的、删掉的、改了绑定的是搬家）
  const graphParams = Object.keys(after.params ?? {}).filter((name) => {
    const was = before.params?.[name];
    const now = after.params?.[name];
    return !!was && !!now && valueEquals(was.binds, now.binds) && !valueEquals(paramsBefore?.[name], paramsAfter?.[name]);
  });
  return { nodes, graphParams };
}

/** 「节点.参数」由谁提供：顶层的 "|a.k" → 图参数名；子图定义里的 "<子图 id>|in.k" → 提升出来的子图参数名。 */
function providers(doc: GraphDoc): Map<string, string> {
  const out = new Map<string, string>();
  for (const [name, gp] of Object.entries(doc.params ?? {})) for (const b of gp.binds) out.set(`|${b}`, name);
  for (const [id, def] of Object.entries(doc.subgraphs ?? {})) {
    for (const p of def.params ?? []) for (const b of p.binds) out.set(`${id}|${b.node}.${b.param}`, p.name);
  }
  return out;
}

/** 只补跑过的：节点（子图节点看它里面的）这张图里已经有过状态、不是 idle。从空白开始拼、新接的分支还没跑过时，
 *  改参数不替人开第一次运行 —— 与拖框、切配方同一条。图参数看它绑着的那几个节点里有没有跑过的。 */
export function autoRunTargetsReady(
  doc: GraphDoc,
  edits: ParamEditTargets,
  states: ReadonlyMap<string, { state: string }>,
): ParamEditTargets {
  const ran = (id: string): boolean => {
    for (const [key, n] of states) {
      if ((key === id || key.startsWith(`${id}/`)) && n.state !== "idle") return true;
    }
    return false;
  };
  const boundRan = (name: string): boolean =>
    (doc.params?.[name]?.binds ?? []).some((b) => {
      const node = splitBind(b)?.node;
      return node !== undefined && ran(node);
    });
  return { nodes: edits.nodes.filter(ran), graphParams: edits.graphParams.filter(boundRan) };
}
