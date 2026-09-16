import type {
  CoreInfo,
  Diagnostic,
  ExecutionEvent,
  ManifestBundle,
  OutputInfo,
  RunEnvelope,
  RunOutputs,
} from "./types.js";

export class HttpError extends Error {
  readonly status: number;
  readonly body: string;
  constructor(status: number, message: string, body: string) {
    super(message);
    this.name = "HttpError";
    this.status = status;
    this.body = body;
  }
}

export interface RunWaitResult {
  runId: string;
  events: ExecutionEvent[];
  timedOut: boolean;
}

function eventsUrlOf(base: string): string {
  const url = new URL(base);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  url.pathname = `${url.pathname.replace(/\/+$/, "")}/lyflow/events`;
  url.search = "";
  return url.toString();
}

export class LyFlowHttp {
  readonly base: string;
  readonly token: string | undefined;

  #manifest: ManifestBundle | null = null;
  #generation: number | null = null;

  constructor(base: string, token?: string | undefined) {
    this.base = base.replace(/\/+$/, "");
    this.token = token;
  }

  #headers(extra?: Record<string, string>): Record<string, string> {
    const h: Record<string, string> = { ...extra };
    if (this.token) h["Authorization"] = `Bearer ${this.token}`;
    return h;
  }

  async request(path: string, init: RequestInit = {}): Promise<Response> {
    let res: Response;
    try {
      res = await fetch(`${this.base}${path}`, {
        ...init,
        headers: this.#headers(init.headers as Record<string, string> | undefined),
      });
    } catch (e) {
      throw new Error(
        `连不上 ${this.base}${path}：${e instanceof Error ? e.message : String(e)}`,
      );
    }
    if (!res.ok) {
      const text = await res.text().catch(() => "");
      let message = text;
      try {
        const parsed = JSON.parse(text) as { error?: unknown };
        if (typeof parsed.error === "string") message = parsed.error;
      } catch {
        message = text;
      }
      throw new HttpError(
        res.status,
        message || `${init.method ?? "GET"} ${path} 失败（HTTP ${res.status}）`,
        text,
      );
    }
    return res;
  }

  async getJson<T>(path: string): Promise<T> {
    const res = await this.request(path);
    return (await res.json()) as T;
  }

  async sendJson<T>(method: string, path: string, body?: unknown): Promise<T> {
    const res = await this.request(path, {
      method,
      ...(body === undefined
        ? {}
        : { headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }),
    });
    const text = await res.text();
    return (text ? (JSON.parse(text) as T) : (undefined as T));
  }

  coreInfo(): Promise<CoreInfo> {
    return this.getJson<CoreInfo>("/lyflow/core-info");
  }

  async manifest(): Promise<ManifestBundle> {
    let generation: number | null = null;
    try {
      const info = await this.coreInfo();
      generation = typeof info.generation === "number" ? info.generation : 0;
    } catch {
      generation = null;
    }
    if (this.#manifest && (generation === null || generation === this.#generation)) {
      return this.#manifest;
    }
    const bundle = await this.getJson<ManifestBundle>("/lyflow/manifest");
    this.#manifest = bundle;
    this.#generation = generation;
    return bundle;
  }

  validate(envelope: { doc: unknown; graphPath: string | null }): Promise<Diagnostic[]> {
    return this.sendJson<Diagnostic[]>("POST", "/lyflow/validate", envelope);
  }

  plan(envelope: {
    doc: unknown;
    graphPath: string | null;
    targets: string[] | null;
  }): Promise<unknown[]> {
    return this.sendJson<unknown[]>("POST", "/lyflow/plan", envelope);
  }

  runOutputs(runId: string): Promise<RunOutputs> {
    return this.getJson<RunOutputs>(`/lyflow/runs/${encodeURIComponent(runId)}/outputs`);
  }

  nodeOutputs(runId: string, nodeId: string): Promise<OutputInfo[]> {
    return this.getJson<OutputInfo[]>(
      `/lyflow/runs/${encodeURIComponent(runId)}/nodes/${encodeURIComponent(nodeId)}/outputs`,
    );
  }

  async cloud(
    runId: string,
    nodeId: string,
    port: string,
    maxPoints: number,
  ): Promise<ArrayBuffer> {
    const res = await this.request(
      `/lyflow/runs/${encodeURIComponent(runId)}/clouds/${encodeURIComponent(nodeId)}/` +
        `${encodeURIComponent(port)}?maxPoints=${maxPoints}`,
    );
    return res.arrayBuffer();
  }

  async #openEvents(timeoutMs: number): Promise<WebSocket> {
    const protocols = this.token
      ? ["lyflow.v1", `lyflow-token.${this.token}`]
      : ["lyflow.v1"];
    const url = eventsUrlOf(this.base);
    const socket = new WebSocket(url, protocols);
    await new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        reject(new Error(`等不到事件流 ${url} 握手完成（${timeoutMs} ms）`));
      }, timeoutMs);
      socket.addEventListener(
        "open",
        () => {
          clearTimeout(timer);
          resolve();
        },
        { once: true },
      );
      socket.addEventListener(
        "error",
        () => {
          clearTimeout(timer);
          reject(new Error(`连不上事件流 ${url}`));
        },
        { once: true },
      );
    });
    return socket;
  }

  async runAndWait(envelope: RunEnvelope, timeoutMs: number): Promise<RunWaitResult> {
    const socket = await this.#openEvents(Math.min(timeoutMs, 15000));
    const frames: ExecutionEvent[] = [];
    let runId: string | null = null;
    let finished = false;
    let settle: () => void = () => {};
    const done = new Promise<void>((resolve) => {
      settle = resolve;
    });

    const onMessage = (e: MessageEvent) => {
      if (typeof e.data !== "string") return;
      let frame: ExecutionEvent;
      try {
        frame = JSON.parse(e.data) as ExecutionEvent;
      } catch {
        return;
      }
      if (frame.kind === "manifest_updated" || frame.kind === "core_reload_failed") return;
      frames.push(frame);
      if (runId !== null && frame.runId === runId && frame.kind === "run_finished") {
        finished = true;
        settle();
      }
    };
    socket.addEventListener("message", onMessage);

    try {
      const started = await this.sendJson<{ runId: string }>("POST", "/lyflow/run", envelope);
      runId = started.runId;
      if (frames.some((f) => f.runId === runId && f.kind === "run_finished")) {
        finished = true;
        settle();
      }
      const timer = setTimeout(() => settle(), timeoutMs);
      await done;
      clearTimeout(timer);
      return {
        runId,
        events: frames.filter((f) => f.runId === runId),
        timedOut: !finished,
      };
    } finally {
      socket.removeEventListener("message", onMessage);
      socket.close();
    }
  }
}
