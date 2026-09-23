// 实时校验（m8-plan L16）：doc 一变就 debounce 调一次 core 的 validate（M7 的 OperatorDesc::validate
// 钩子在里面），诊断按节点挂到画布与 Inspector 上。判定全在 C++，这里只存结论 —— 与 stale 同一条路
// （ADR-0007），前端不重新实现任何一条校验。

import { create } from "zustand";

import { pathPrefix, type SubPath } from "../lib/subgraph";
import { transport } from "../transport";
import { useUiStore } from "./ui";
import type { GraphDiagnostic } from "../types/execution";
import type { GraphDoc } from "../types/graph";

/** 拖框时每帧都改参数，攒一下再问；比 plan 的 150 ms 稍长，拖动停下来之后一眨眼就出结果。 */
const DEBOUNCE_MS = 200;

interface ValidationState {
  /** 事件里的 nodeId 是**路径**（子图内部是 `outer/inner`），键原样。只收 error / warning，不收迁移。 */
  byNode: ReadonlyMap<string, GraphDiagnostic[]>;
  /** 最近一次校验的时刻（performance.now）。验收脚本拿它量「拖完到标红」的延迟。 */
  at: number;
  set(diags: GraphDiagnostic[]): void;
  reset(): void;
}

const EMPTY: ReadonlyMap<string, GraphDiagnostic[]> = new Map();

export const useValidationStore = create<ValidationState>((set) => ({
  byNode: EMPTY,
  at: 0,
  set(diags) {
    const byNode = new Map<string, GraphDiagnostic[]>();
    for (const d of diags) {
      if (d.kind === "migration" || !d.nodeId) continue;
      const list = byNode.get(d.nodeId);
      if (list) list.push(d);
      else byNode.set(d.nodeId, [d]);
    }
    set({ byNode: byNode.size === 0 ? EMPTY : byNode, at: performance.now() });
  },
  reset() {
    set({ byNode: EMPTY, at: 0 });
  },
}));

let timer: ReturnType<typeof setTimeout> | null = null;
let ticket = 0;

/** doc 变了就重新校验。晚发的那次赢，与 schedulePlan 同一套 debounce + ticket。 */
export function scheduleValidate(doc: GraphDoc, graphPath: string | null): void {
  if (transport.kind === "static") return;
  if (timer) clearTimeout(timer);
  timer = setTimeout(() => {
    timer = null;
    void requestValidate(doc, graphPath);
  }, DEBOUNCE_MS);
}

export async function requestValidate(doc: GraphDoc, graphPath: string | null): Promise<void> {
  if (transport.kind === "static") return;
  const mine = ++ticket;
  try {
    const diags = await transport.validateGraph(doc, graphPath);
    if (mine !== ticket) return;
    useValidationStore.getState().set(diags);
  } catch {
    // 校验本身失败（core 换代中、结构坏了）不是编辑期该弹窗的事：清掉旧结论，别留幽灵红框
    if (mine === ticket) useValidationStore.getState().reset();
  }
}

let cache: {
  path: SubPath;
  byNode: ReadonlyMap<string, GraphDiagnostic[]>;
  result: Map<string, GraphDiagnostic[]>;
} | null = null;

/** 当前层级的「本地 id → 诊断」。子图节点收它内部全部节点的诊断。 */
export function validationAt(
  path: SubPath,
  byNode: ReadonlyMap<string, GraphDiagnostic[]>,
): ReadonlyMap<string, GraphDiagnostic[]> {
  if (cache && cache.path === path && cache.byNode === byNode) return cache.result;
  const prefix = pathPrefix(path);
  const result = new Map<string, GraphDiagnostic[]>();
  for (const [id, list] of byNode) {
    if (prefix && !id.startsWith(prefix)) continue;
    const rest = id.slice(prefix.length);
    if (!rest) continue;
    const slash = rest.indexOf("/");
    const local = slash < 0 ? rest : rest.slice(0, slash);
    const merged = result.get(local);
    if (merged) merged.push(...list);
    else result.set(local, [...list]);
  }
  cache = { path, byNode, result };
  return result;
}

const NONE: GraphDiagnostic[] = [];

/** 某节点在**当前层级**的校验诊断（error 与 warning 都在，调用方自己挑）。 */
export function useNodeValidation(localId: string): GraphDiagnostic[] {
  const path = useUiStore((s) => s.path);
  return useValidationStore((s) => validationAt(path, s.byNode).get(localId) ?? NONE);
}

export function errorsOf(diags: readonly GraphDiagnostic[]): GraphDiagnostic[] {
  return diags.filter((d) => d.severity === "error");
}
