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
import type { ExecutionEvent, GraphDiagnostic, OutputInfo } from "../types/execution";

export type TransportKind = "tauri" | "static";

export type Unlisten = () => void;

export interface Transport {
  readonly kind: TransportKind;
  getManifest(): Promise<OperatorManifestBundle>;
  getCoreInfo(): Promise<CoreInfo>;
  saveGraph(path: string, doc: GraphDoc): Promise<void>;
  loadGraph(path: string): Promise<GraphDoc>;

  /** 权威校验（C++ 侧），返回全部诊断。 */
  validateGraph(doc: GraphDoc, graphPath: string | null): Promise<GraphDiagnostic[]>;
  /** 启动一次运行，返回 runId。状态走 `onExecutionEvent`。 */
  runGraph(doc: GraphDoc, graphPath: string | null, targets?: string[]): Promise<string>;
  cancelRun(runId: string): Promise<void>;
  getOutputInfo(runId: string, nodeId: string): Promise<OutputInfo[]>;
  /** 点云走二进制，绝不 JSON（ADR-0006）。 */
  getOutputCloud(
    runId: string,
    nodeId: string,
    port: string,
    maxPoints: number,
  ): Promise<ArrayBuffer>;
  onExecutionEvent(cb: (e: ExecutionEvent) => void): Promise<Unlisten>;
}

/** 浏览器模式下所有执行相关的入口都走这里。 */
function browserOnly(what: string): never {
  throw new Error(`浏览器模式不能${what} —— 计算在 C++ 里，请在 Tauri 里运行（pnpm dev）`);
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
  async validateGraph(doc, graphPath) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<GraphDiagnostic[]>("validate_graph", { doc, graphPath });
  },
  async runGraph(doc, graphPath, targets) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<string>("run_graph", { doc, graphPath, targets: targets ?? null });
  },
  async cancelRun(runId) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<void>("cancel_run", { runId });
  },
  async getOutputInfo(runId, nodeId) {
    const { invoke } = await import("@tauri-apps/api/core");
    return invoke<OutputInfo[]>("get_output_info", { runId, nodeId });
  },
  async getOutputCloud(runId, nodeId, port, maxPoints) {
    const { invoke } = await import("@tauri-apps/api/core");
    // Tauri 的 ipc::Response 在 JS 侧到手是 ArrayBuffer；某些版本给的是
    // Uint8Array，两种都接住 —— 差别只在一次 .buffer。
    const raw = await invoke<ArrayBuffer | Uint8Array>("get_output_cloud", {
      runId,
      nodeId,
      port,
      maxPoints,
    });
    if (raw instanceof ArrayBuffer) return raw;
    const view = raw as Uint8Array;
    return view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength) as ArrayBuffer;
  },
  async onExecutionEvent(cb) {
    const { listen } = await import("@tauri-apps/api/event");
    return listen<ExecutionEvent>("execution-event", (e) => cb(e.payload));
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
  async validateGraph() {
    return browserOnly("做权威校验");
  },
  async runGraph() {
    return browserOnly("运行");
  },
  async cancelRun() {
    return browserOnly("运行");
  },
  async getOutputInfo() {
    return browserOnly("取运行结果");
  },
  async getOutputCloud() {
    return browserOnly("取运行结果");
  },
  async onExecutionEvent() {
    // 这个不抛：界面启动时无条件订阅一次，浏览器模式下静默给个空的取消函数
    // 比让整个 App 挂在一个 useEffect 里强。真去点运行才会拿到上面那条错误。
    return () => {};
  },
};

export const transport: Transport = inTauri() ? tauriTransport : staticTransport;
