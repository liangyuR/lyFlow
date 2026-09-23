// manifest store —— **算子描述存这里，不存进节点**：节点只记 op id、渲染时现查。
// 这是 M3 热重载的前提；快照进节点热重载就永久失效（docs/operator-manifest.md）。

import { create } from "zustand";

import { transport, type TransportKind } from "../transport";
import type {
  CoreInfo,
  OperatorDesc,
  OperatorManifestBundle,
  PortType,
  SnippetDesc,
} from "../types/manifest";

export type LoadStatus = "idle" | "loading" | "ready" | "error";

interface ManifestState {
  status: LoadStatus;
  error: string | null;
  bundle: OperatorManifestBundle | null;
  coreInfo: CoreInfo | null;
  transportKind: TransportKind;

  /** 按 id 索引，避免每次渲染节点都线性扫一遍。 */
  operatorsById: Map<string, OperatorDesc>;
  typesByName: Map<string, PortType>;

  /** 用户目录里的片段（m8-plan L14），宿主扫描出来的。算子包随附的在 bundle.snippets。 */
  userSnippets: SnippetDesc[];
  snippetProblems: string[];

  load: () => Promise<void>;
  /** 热重载换代（ADR-0009）：整份替换，当前 doc 一个字都不动。 */
  replaceBundle: (bundle: OperatorManifestBundle, generation: number) => void;
  /** 重扫用户片段目录。宿主不支持时什么都不做。 */
  loadUserSnippets: () => Promise<void>;
}

function index(bundle: OperatorManifestBundle) {
  const operatorsById = new Map<string, OperatorDesc>();
  for (const op of bundle.operators) {
    operatorsById.set(op.id, op);
    // 别名一并索引：算子重命名后老图里的 op id 仍然查得到。
    for (const alias of op.aliases ?? []) operatorsById.set(alias, op);
  }
  const typesByName = new Map<string, PortType>();
  for (const t of bundle.types) typesByName.set(t.name, t);
  // 每个声明了的 Bundle kind 也登记成一个类型名（`Bundle<kind>`），颜色取类型表里的 Bundle：
  // 端口着色、连线校验都按类型名查表，这样不必在每个查表的地方另认一遍这种写法。
  const bundleType = typesByName.get("Bundle");
  for (const b of bundle.bundles ?? []) {
    const name = `Bundle<${b.kind}>`;
    const doc = b.label ?? b.doc;
    typesByName.set(name, {
      name,
      color: bundleType?.color ?? "#c8a86b",
      ...(doc !== undefined ? { doc } : {}),
    });
  }
  return { operatorsById, typesByName };
}

export const useManifestStore = create<ManifestState>((set, get) => ({
  status: "idle",
  error: null,
  bundle: null,
  coreInfo: null,
  // 真正的种类在 load 里补 —— store 建起来时宿主还没装上传输。
  transportKind: "static",
  operatorsById: new Map(),
  typesByName: new Map(),
  userSnippets: [],
  snippetProblems: [],

  load: async () => {
    set({ status: "loading", error: null, transportKind: transport.kind });
    try {
      const [bundle, coreInfo] = await Promise.all([
        transport.getManifest(),
        transport.getCoreInfo(),
      ]);
      set({ status: "ready", bundle, coreInfo, ...index(bundle) });
    } catch (e) {
      set({ status: "error", error: e instanceof Error ? e.message : String(e) });
      return;
    }
    await get().loadUserSnippets();
  },

  loadUserSnippets: async () => {
    if (typeof transport.listSnippets !== "function") return;
    try {
      const scan = await transport.listSnippets();
      set({ userSnippets: scan.snippets, snippetProblems: scan.problems });
    } catch (e) {
      set({ userSnippets: [], snippetProblems: [e instanceof Error ? e.message : String(e)] });
    }
  },

  replaceBundle: (bundle, generation) => {
    set((prev) => ({
      status: "ready",
      error: null,
      bundle,
      coreInfo: prev.coreInfo
        ? {
            ...prev.coreInfo,
            operatorCount: bundle.operators.length,
            typeCount: bundle.types.length,
            generation,
          }
        : prev.coreInfo,
      ...index(bundle),
    }));
  },
}));

/** doc 里引用了但当前 core 没有的算子。热重载删掉一个算子之后，
 *  这些节点保留在 doc 里显示成「算子缺失」，可删可等（1.5）。 */
export function missingOperators(ops: readonly string[]): Set<string> {
  const known = useManifestStore.getState().operatorsById;
  const out = new Set<string>();
  for (const id of ops) {
    if (!known.has(id)) out.add(id);
  }
  return out;
}

const NO_SNIPPETS: SnippetDesc[] = [];
let snippetCache: { pack: SnippetDesc[] | undefined; user: SnippetDesc[]; all: SnippetDesc[] } | null =
  null;

/** 片段库的全部条目：算子包随附的在前，用户目录的在后；同 id 时用户的那份覆盖包里的。 */
export function allSnippets(
  pack: SnippetDesc[] | undefined,
  user: SnippetDesc[],
): SnippetDesc[] {
  if (snippetCache && snippetCache.pack === pack && snippetCache.user === user) {
    return snippetCache.all;
  }
  const byId = new Map<string, SnippetDesc>();
  for (const s of pack ?? NO_SNIPPETS) byId.set(s.id, s);
  for (const s of user) byId.set(s.id, s);
  const all = [...byId.values()];
  snippetCache = { pack, user, all };
  return all;
}

export function useSnippets(): SnippetDesc[] {
  const pack = useManifestStore((s) => s.bundle?.snippets);
  const user = useManifestStore((s) => s.userSnippets);
  return allSnippets(pack, user);
}

export function findSnippet(id: string): SnippetDesc | undefined {
  const s = useManifestStore.getState();
  return allSnippets(s.bundle?.snippets, s.userSnippets).find((x) => x.id === id);
}

/** 按 op id 现查算子描述。节点渲染必须走这里，不要缓存结果。 */
export function useOperator(id: string): OperatorDesc | undefined {
  return useManifestStore((s) => s.operatorsById.get(id));
}

/** 端口类型的颜色。查不到时给个显眼的灰，比静默失败好。 */
export function usePortColor(typeName: string): string {
  return useManifestStore((s) => s.typesByName.get(typeName)?.color ?? "#6b7280");
}
