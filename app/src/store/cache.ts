// 缓存与计划 store（ADR-0007）。**判定全在 C++**：这里只存 plan_graph 的结论，
// 一个字节的推导都不做 —— 前端算不出 IO 算子的 externalKey，自己推一定会错。

import { create } from "zustand";

import { transport } from "../transport";
import type { CacheStats, PlanNode } from "../types/execution";
import type { GraphDoc } from "../types/graph";

/** doc 每次变都重新编译一次代价太大，攒一下再问。 */
const DEBOUNCE_MS = 150;

interface CacheState {
  /** 最近一次 plan_graph 的结果，按 nodeId 索引。 */
  plan: Map<string, PlanNode>;
  /** 上一次运行开始时每个节点的 cacheKey（来自 run_started.nodes）。 */
  ranWith: Map<string, string>;
  stats: CacheStats | null;
  /** 编译失败（图当前不合法）时为 true，stale 标记按「无从判断」处理。 */
  unavailable: boolean;

  setPlan(nodes: PlanNode[]): void;
  setRanWith(nodes: { id: string; cacheKey: string }[]): void;
  setStats(stats: CacheStats | null): void;
  reset(): void;
}

export const useCacheStore = create<CacheState>((set) => ({
  plan: new Map(),
  ranWith: new Map(),
  stats: null,
  unavailable: false,

  setPlan(nodes) {
    set({
      plan: new Map(nodes.map((n) => [n.nodeId, n])),
      unavailable: nodes.length === 0,
    });
  },
  setRanWith(nodes) {
    set({ ranWith: new Map(nodes.map((n) => [n.id, n.cacheKey])) });
  },
  setStats(stats) {
    set({ stats });
  },
  reset() {
    set({ plan: new Map(), ranWith: new Map(), unavailable: false });
  },
}));

/** 某个节点是不是 stale：这次编译出来的 cacheKey 与上次运行时不同，且现在没有缓存。
 *  两个条件缺一不可 —— 只看 key 变化的话，改回原值也会一直标着红。 */
export function isNodeStale(nodeId: string): boolean {
  const { plan, ranWith } = useCacheStore.getState();
  const node = plan.get(nodeId);
  if (!node) return false;
  const previous = ranWith.get(nodeId);
  if (previous === undefined) return false;
  return node.cacheKey !== previous && !node.cached;
}

/** 下一次运行要真算的节点数。工具栏的「将重算 N 个节点」就是它。 */
export function pendingRecompute(): number {
  let n = 0;
  for (const node of useCacheStore.getState().plan.values()) {
    if (!node.cached) n += 1;
  }
  return n;
}

export function useStaleNodeIds(): ReadonlySet<string> {
  const plan = useCacheStore((s) => s.plan);
  const ranWith = useCacheStore((s) => s.ranWith);
  const out = new Set<string>();
  for (const [id, node] of plan) {
    const previous = ranWith.get(id);
    if (previous !== undefined && node.cacheKey !== previous && !node.cached) out.add(id);
  }
  return out;
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let value = bytes / 1024;
  let i = 0;
  while (value >= 1024 && i < units.length - 1) {
    value /= 1024;
    i += 1;
  }
  return `${value.toFixed(value >= 100 ? 0 : 1)} ${units[i]}`;
}

// 编译请求 -------------------------------------------------------------------

let timer: ReturnType<typeof setTimeout> | null = null;
let ticket = 0;

/** doc 变了就重新编译一次。debounce + ticket：拖滑块时每帧一次编译既浪费，
 *  回复顺序也不保证等于发起顺序，晚发的那次必须赢。 */
export function schedulePlan(doc: GraphDoc, graphPath: string | null): void {
  if (transport.kind !== "tauri") return;
  if (timer) clearTimeout(timer);
  timer = setTimeout(() => {
    void requestPlan(doc, graphPath);
  }, DEBOUNCE_MS);
}

export async function requestPlan(doc: GraphDoc, graphPath: string | null): Promise<void> {
  if (transport.kind !== "tauri") return;
  const mine = ++ticket;
  try {
    const nodes = await transport.planGraph(doc, graphPath);
    if (mine !== ticket) return;
    useCacheStore.getState().setPlan(nodes);
  } catch {
    // 编译不出来（图有错、core 换代中）不是异常路径，标记一下就够了
    if (mine === ticket) useCacheStore.getState().setPlan([]);
  }
}

export async function refreshCacheStats(): Promise<void> {
  if (transport.kind !== "tauri") return;
  try {
    useCacheStore.getState().setStats(await transport.cacheStats());
  } catch {
    useCacheStore.getState().setStats(null);
  }
}

export async function clearCache(): Promise<void> {
  await transport.clearCache();
  useCacheStore.getState().setRanWith([]);
  await refreshCacheStats();
}
