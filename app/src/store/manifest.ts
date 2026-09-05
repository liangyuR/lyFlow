//
// manifest store。
//
// **算子描述存在这里，不存进节点。**
//
// 这是 M0 必须做对、后期改代价很大的一个决定（见 docs/operator-manifest.md
// 结尾）。节点只记 op id，渲染时按 id 现查。这样 M3 的热重载才可能：
// C++ 重编译后推一份新 manifest 进这个 store，所有节点的外观和参数表单
// 自动跟着变，当前打开的图不需要重置。
//
// 如果反过来在创建节点时把 manifest 快照进节点里，热重载就永久失效了。
//

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
