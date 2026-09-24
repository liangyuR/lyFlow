import type { CoreInfo, OperatorManifestBundle } from "../types/manifest";
import type { GraphDoc } from "../types/graph";
import type {
  CacheStats,
  ExecutionEvent,
  GraphDiagnostic,
  OutputInfo,
  RunOutputs,
  PlanNode,
} from "../types/execution";
import type {
  BackupStatus,
  CoreReloadFailed,
  LibraryMeta,
  LibraryRefresh,
  LibraryStatus,
  LoadedGraph,
  ManifestUpdated,
  RecentEntry,
  RunOptions,
  Transport,
  TransportKind,
  Unlisten,
} from "./types";

/** WebSocket 上除 ExecutionEvent 以外的两种控制帧。见 docs/http-transport.md。 */
const MANIFEST_UPDATED = "manifest_updated";
const CORE_RELOAD_FAILED = "core_reload_failed";

/** 断线后的重连退避，毫秒。 */
const RECONNECT_MS = [200, 500, 1000, 2000, 4000];

type Listener = (frame: Record<string, unknown>) => void;

function joinUrl(base: string, path: string): string {
  return `${base.replace(/\/+$/, "")}${path}`;
}

/** 走 HTTP/WebSocket 后端的传输。契约见 docs/http-transport.md，
 *  与 C ABI v8 一一对应；阶段 B 的业务服务照它实现即可。 */
export interface HttpTransportOptions {
  /** 事件流的地址。宿主的 WebSocket 不在 HTTP 端口上时给这一项；
   *  缺省是 baseUrl 换成 ws(s) 之后加 `/lyflow/events`。 */
  eventsUrl?: string;
  /** 握手时请求的子协议。缺省是 `lyflow.v1` 加上带 token 的那一项，也是契约的正路。
   *  历史兼容：后端的 WebSocket 库不会回 `Sec-WebSocket-Protocol` 时（浏览器会因此拒绝握手）
   *  给 `[]`，并把鉴权放进 `eventsUrl` 自己。新后端不要走这一条。 */
  eventsProtocols?: string[];
}

export class HttpTransport implements Transport {
  readonly kind: TransportKind = "http";
  readonly baseUrl: string;
  readonly token: string | undefined;
  readonly eventsUrl: string;
  readonly eventsProtocols: string[] | undefined;

  #socket: WebSocket | null = null;
  #listeners = new Set<Listener>();
  #closed = false;
  #attempt = 0;

  constructor(baseUrl: string, token?: string, options?: HttpTransportOptions) {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
    this.token = token;
    this.eventsUrl =
      options?.eventsUrl ?? joinUrl(this.baseUrl, "/lyflow/events").replace(/^http/, "ws");
    this.eventsProtocols = options?.eventsProtocols;
  }

  // ---- 底层 ---------------------------------------------------------------

  #headers(extra?: Record<string, string>): Record<string, string> {
    const h: Record<string, string> = { ...extra };
    if (this.token) h["Authorization"] = `Bearer ${this.token}`;
    return h;
  }

  async #request(path: string, init: RequestInit = {}): Promise<Response> {
    const res = await fetch(joinUrl(this.baseUrl, path), {
      ...init,
      headers: this.#headers(init.headers as Record<string, string> | undefined),
    });
    if (!res.ok) {
      const text = await res.text().catch(() => "");
      let message = text;
      try {
        const parsed = JSON.parse(text) as { error?: string };
        if (typeof parsed.error === "string") message = parsed.error;
      } catch {
        /* 不是 JSON 就原样用 */
      }
      throw new Error(message || `${init.method ?? "GET"} ${path} 失败（HTTP ${res.status}）`);
    }
    return res;
  }

  async #get<T>(path: string): Promise<T> {
    const res = await this.#request(path);
    return (await res.json()) as T;
  }

  async #send<T>(method: string, path: string, body?: unknown): Promise<T> {
    const res = await this.#request(path, {
      method,
      headers: body === undefined ? {} : { "Content-Type": "application/json" },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
    });
    if (res.status === 204) return undefined as T;
    const text = await res.text();
    return (text ? JSON.parse(text) : undefined) as T;
  }

  // ---- 描述 ---------------------------------------------------------------

  getManifest(): Promise<OperatorManifestBundle> {
    return this.#get<OperatorManifestBundle>("/lyflow/manifest");
  }
  getCoreInfo(): Promise<CoreInfo> {
    return this.#get<CoreInfo>("/lyflow/core-info");
  }

  // ---- 校验、计划、执行 -----------------------------------------------------

  validateGraph(
    doc: GraphDoc,
    graphPath: string | null,
    params?: Record<string, unknown>,
  ): Promise<GraphDiagnostic[]> {
    return this.#send<GraphDiagnostic[]>("POST", "/lyflow/validate", {
      doc,
      graphPath,
      params: params ?? null,
    });
  }

  async planGraph(
    doc: GraphDoc,
    graphPath: string | null,
    targets?: string[],
    params?: Record<string, unknown>,
  ): Promise<PlanNode[]> {
    const out = await this.#send<PlanNode[] | GraphDiagnostic[]>("POST", "/lyflow/plan", {
      doc,
      graphPath,
      targets: targets ?? null,
      params: params ?? null,
    });
    // 与 Tauri 一致：校验没过时后端返回诊断数组，靠 cacheKey 区分（ADR-0007）
    const items = out as { cacheKey?: unknown }[];
    if (items.some((n) => typeof n.cacheKey !== "string")) return [];
    return out as PlanNode[];
  }

  async runGraph(
    doc: GraphDoc,
    graphPath: string | null,
    options?: RunOptions,
  ): Promise<string> {
    const out = await this.#send<{ runId: string }>("POST", "/lyflow/run", {
      doc,
      graphPath,
      targets: options?.targets ?? null,
      isolate: options?.isolate ?? null,
      force: options?.force ?? null,
      mode: options?.mode ?? "full",
      previewMaxPoints: options?.previewMaxPoints ?? null,
      previewBudgetMs: options?.previewBudgetMs ?? null,
      sceneId: options?.sceneId ?? null,
      params: options?.params ?? null,
    });
    return out.runId;
  }

  async cancelRun(runId: string): Promise<void> {
    await this.#send<void>("POST", "/lyflow/cancel", { runId });
  }

  getOutputInfo(runId: string, nodeId: string): Promise<OutputInfo[]> {
    return this.#get<OutputInfo[]>(
      `/lyflow/runs/${encodeURIComponent(runId)}/nodes/${encodeURIComponent(nodeId)}/outputs`,
    );
  }

  getRunOutputs(runId: string): Promise<RunOutputs> {
    return this.#get<RunOutputs>(`/lyflow/runs/${encodeURIComponent(runId)}/outputs`);
  }

  async getOutputCloud(
    runId: string,
    nodeId: string,
    port: string,
    maxPoints: number,
  ): Promise<ArrayBuffer> {
    const res = await this.#request(
      `/lyflow/runs/${encodeURIComponent(runId)}/clouds/${encodeURIComponent(nodeId)}/` +
        `${encodeURIComponent(port)}?maxPoints=${maxPoints}`,
    );
    return res.arrayBuffer();
  }

  async getOutputTensor(
    runId: string,
    nodeId: string,
    port: string,
    offset: number,
    count: number,
  ): Promise<ArrayBuffer> {
    const res = await this.#request(
      `/lyflow/runs/${encodeURIComponent(runId)}/tensors/${encodeURIComponent(nodeId)}/` +
        `${encodeURIComponent(port)}?offset=${offset}&count=${count}`,
    );
    return res.arrayBuffer();
  }

  async getOutputIndices(
    runId: string,
    nodeId: string,
    port: string,
    offset: number,
    count: number,
  ): Promise<ArrayBuffer> {
    const res = await this.#request(
      `/lyflow/runs/${encodeURIComponent(runId)}/indices/${encodeURIComponent(nodeId)}/` +
        `${encodeURIComponent(port)}?offset=${offset}&count=${count}`,
    );
    return res.arrayBuffer();
  }

  clearCache(): Promise<void> {
    return this.#send<void>("DELETE", "/lyflow/cache");
  }
  cacheStats(): Promise<CacheStats> {
    return this.#get<CacheStats>("/lyflow/cache");
  }

  // ---- 事件（WebSocket）----------------------------------------------------

  /** 三个 on* 共用一条连接：后端只暴露一个 `/lyflow/events`。 */
  #ensureSocket(): void {
    if (this.#socket || this.#closed) return;
    // token 走子协议而不是查询串：查询串会进日志和 Referer
    const protocols =
      this.eventsProtocols ??
      (this.token ? ["lyflow.v1", `lyflow-token.${this.token}`] : ["lyflow.v1"]);
    const socket =
      protocols.length > 0 ? new WebSocket(this.eventsUrl, protocols) : new WebSocket(this.eventsUrl);
    this.#socket = socket;
    socket.addEventListener("open", () => {
      this.#attempt = 0;
    });
    socket.addEventListener("message", (e: MessageEvent) => {
      if (typeof e.data !== "string") return;
      let frame: Record<string, unknown>;
      try {
        frame = JSON.parse(e.data) as Record<string, unknown>;
      } catch {
        return;
      }
      for (const fn of [...this.#listeners]) fn(frame);
    });
    socket.addEventListener("close", () => {
      this.#socket = null;
      if (this.#closed || this.#listeners.size === 0) return;
      const delay = RECONNECT_MS[Math.min(this.#attempt, RECONNECT_MS.length - 1)] ?? 4000;
      this.#attempt += 1;
      setTimeout(() => this.#ensureSocket(), delay);
    });
    socket.addEventListener("error", () => {
      /* close 会紧跟着来，重连逻辑放那里一处 */
    });
  }

  #subscribe(fn: Listener): Unlisten {
    this.#listeners.add(fn);
    this.#ensureSocket();
    return () => {
      this.#listeners.delete(fn);
    };
  }

  async onExecutionEvent(cb: (e: ExecutionEvent) => void): Promise<Unlisten> {
    return this.#subscribe((frame) => {
      const kind = frame["kind"];
      if (kind === MANIFEST_UPDATED || kind === CORE_RELOAD_FAILED) return;
      cb(frame as unknown as ExecutionEvent);
    });
  }

  async onManifestUpdated(cb: (e: ManifestUpdated) => void): Promise<Unlisten> {
    return this.#subscribe((frame) => {
      if (frame["kind"] !== MANIFEST_UPDATED) return;
      cb(frame as unknown as ManifestUpdated);
    });
  }

  async onCoreReloadFailed(cb: (e: CoreReloadFailed) => void): Promise<Unlisten> {
    return this.#subscribe((frame) => {
      if (frame["kind"] !== CORE_RELOAD_FAILED) return;
      cb(frame as unknown as CoreReloadFailed);
    });
  }

  /** 宿主卸载编辑器时调用，停掉重连。 */
  dispose(): void {
    this.#closed = true;
    this.#listeners.clear();
    this.#socket?.close();
    this.#socket = null;
  }

  // ---- 工作区文件 ----------------------------------------------------------

  async saveGraph(path: string, doc: GraphDoc): Promise<void> {
    await this.#send<void>("PUT", `/lyflow/files/graph?path=${encodeURIComponent(path)}`, { doc });
  }
  loadGraph(path: string): Promise<LoadedGraph> {
    return this.#get<LoadedGraph>(`/lyflow/files/graph?path=${encodeURIComponent(path)}`);
  }
  async writeBackup(path: string, doc: GraphDoc): Promise<void> {
    await this.#send<void>("PUT", `/lyflow/files/backup?path=${encodeURIComponent(path)}`, { doc });
  }
  backupStatus(path: string): Promise<BackupStatus> {
    return this.#get<BackupStatus>(`/lyflow/files/backup/status?path=${encodeURIComponent(path)}`);
  }
  readBackup(path: string): Promise<LoadedGraph> {
    return this.#get<LoadedGraph>(`/lyflow/files/backup?path=${encodeURIComponent(path)}`);
  }
  async discardBackup(path: string): Promise<void> {
    await this.#send<void>("DELETE", `/lyflow/files/backup?path=${encodeURIComponent(path)}`);
  }
  async writeFileBytes(path: string, contents: Uint8Array): Promise<void> {
    await this.#request(`/lyflow/files/bytes?path=${encodeURIComponent(path)}`, {
      method: "PUT",
      headers: { "Content-Type": "application/octet-stream" },
      body: contents as unknown as BodyInit,
    });
  }
  getRecentFiles(): Promise<RecentEntry[]> {
    return this.#get<RecentEntry[]>("/lyflow/recent");
  }
  pushRecentFile(path: string): Promise<RecentEntry[]> {
    return this.#send<RecentEntry[]>("POST", "/lyflow/recent", { path });
  }

  // ---- 库算子与导入 --------------------------------------------------------

  getLibraryStatus(): Promise<LibraryStatus> {
    return this.#get<LibraryStatus>("/lyflow/library");
  }
  refreshLibrary(): Promise<LibraryRefresh> {
    return this.#send<LibraryRefresh>("POST", "/lyflow/library/refresh");
  }
  saveAsLibrary(doc: GraphDoc, subgraphId: string, meta: LibraryMeta): Promise<LibraryStatus> {
    return this.#send<LibraryStatus>("POST", "/lyflow/library/save", { doc, subgraphId, meta });
  }
  importGraph(kind: string, text: string, baseDir: string | null): Promise<GraphDoc> {
    return this.#send<GraphDoc>("POST", "/lyflow/import", { kind, text, baseDir });
  }
}
