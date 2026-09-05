// manifest store —— **算子描述存这里，不存进节点**：节点只记 op id、渲染时现查。
// 这是 M3 热重载的前提；快照进节点热重载就永久失效（docs/operator-manifest.md）。

import { create } from "zustand";

import { transport, type TransportKind } from "../transport";
import type {
  CoreInfo,
  OperatorDesc,
  OperatorManifestBundle,
  PortType,
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

  load: () => Promise<void>;
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
  return { operatorsById, typesByName };
}

export const useManifestStore = create<ManifestState>((set) => ({
  status: "idle",
  error: null,
  bundle: null,
  coreInfo: null,
  transportKind: transport.kind,
  operatorsById: new Map(),
  typesByName: new Map(),

  load: async () => {
    set({ status: "loading", error: null });
    try {
      const [bundle, coreInfo] = await Promise.all([
        transport.getManifest(),
        transport.getCoreInfo(),
      ]);
      set({ status: "ready", bundle, coreInfo, ...index(bundle) });
    } catch (e) {
      set({ status: "error", error: e instanceof Error ? e.message : String(e) });
    }
  },
}));

/** 按 op id 现查算子描述。节点渲染必须走这里，不要缓存结果。 */
export function useOperator(id: string): OperatorDesc | undefined {
  return useManifestStore((s) => s.operatorsById.get(id));
}

/** 端口类型的颜色。查不到时给个显眼的灰，比静默失败好。 */
export function usePortColor(typeName: string): string {
  return useManifestStore((s) => s.typesByName.get(typeName)?.color ?? "#6b7280");
}
