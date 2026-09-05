//
// 传输层抽象。
//
// 存在的唯一理由：让 UI 代码不知道自己跑在 Tauri 里还是浏览器里。
// 浏览器模式下用 dump 出来的静态 manifest，可以不启动 Tauri、不重编 C++
// 就迭代界面 —— 这个反馈循环的差别是秒级 vs 分钟级。
//
// 但它必须显眼：状态栏会明确标出当前是哪种 transport，
// 否则迟早有人对着一份三天前 dump 的 manifest 调半天。
//

import type { CoreInfo, OperatorManifestBundle } from "../types/manifest";
import type { GraphDoc } from "../types/graph";

export type TransportKind = "tauri" | "static";

export interface Transport {
  readonly kind: TransportKind;
  getManifest(): Promise<OperatorManifestBundle>;
  getCoreInfo(): Promise<CoreInfo>;
  saveGraph(path: string, doc: GraphDoc): Promise<void>;
  loadGraph(path: string): Promise<GraphDoc>;
}

function inTauri(): boolean {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

const tauriTransport: Transport = {
  kind: "tauri",
  async getManifest() {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<OperatorManifestBundle>("get_manifest");
  },
  async getCoreInfo() {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<CoreInfo>("get_core_info");
  },
  async saveGraph(path, doc) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("save_graph", { path, doc });
  },
  async loadGraph(path) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<GraphDoc>("load_graph", { path });
  },
};

/**
 * 浏览器模式：读 public/manifest.dev.json。
 * 用 `pnpm --dir app run dump-manifest` 之类的方式刷新它（见 scripts/）。
 */
const staticTransport: Transport = {
  kind: "static",
  async getManifest() {
    const res = await fetch("/manifest.dev.json");
    if (!res.ok) {
      throw new Error(
        `读不到 /manifest.dev.json (${res.status})。` +
          `浏览器模式需要先 dump 一份：scripts/dump-manifest.ps1`,
      );
    }
    return (await res.json()) as OperatorManifestBundle;
  },
  async getCoreInfo() {
    const m = await this.getManifest();
    return {
      version: (m.generatedBy ?? "unknown").split("/")[1] ?? "unknown",
      operatorCount: m.operators.length,
      typeCount: m.types.length,
    };
  },
  async saveGraph() {
    throw new Error("浏览器模式不能存盘，请在 Tauri 里运行");
  },
  async loadGraph() {
    throw new Error("浏览器模式不能读盘，请在 Tauri 里运行");
  },
};

export const transport: Transport = inTauri() ? tauriTransport : staticTransport;
