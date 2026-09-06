// docs/http-transport.md 的最小实现，给 `pnpm e2e:http` 当后端。
// 每个请求起一次 `lyflow` CLI —— 桩服务器不常驻 core，所以缓存统计恒为 0，
// 取点云会重跑一次图。真正的业务服务应该用 core/include/lyflow/client.hpp 常驻。

import { spawn } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";

import { encodeCloudFromPcd } from "./pcd.mjs";
import { WsHub } from "./ws.mjs";

// ------------------------------------------------------------------ 配置

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (!a.startsWith("--")) continue;
    const key = a.slice(2);
    const next = argv[i + 1];
    if (next && !next.startsWith("--")) {
      out[key] = next;
      i += 1;
    } else {
      out[key] = "1";
    }
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));
const PORT = Number(args.port ?? process.env.LYFLOW_HTTP_PORT ?? 8787);
const TOKEN = args.token ?? process.env.LYFLOW_HTTP_TOKEN ?? "";
const ROOT = path.resolve(args.root ?? process.env.LYFLOW_HTTP_ROOT ?? process.cwd());
const CLI = path.resolve(
  args.cli ??
    process.env.LYFLOW_CLI ??
    path.join(process.cwd(), "bridge", "target", "debug", "lyflow.exe"),
);
const SCRATCH = fs.mkdtempSync(path.join(os.tmpdir(), "lyflow-http-"));

// ------------------------------------------------------------------ CLI

function runCli(argv, { onLine } = {}) {
  return new Promise((resolve) => {
    const child = spawn(CLI, argv, { cwd: ROOT });
    let stdout = "";
    let stderr = "";
    let pending = "";
    const lines = [];
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk) => {
      stdout += chunk;
      pending += chunk;
      let nl;
      while ((nl = pending.indexOf("\n")) >= 0) {
        const line = pending.slice(0, nl).trim();
        pending = pending.slice(nl + 1);
        if (!line) continue;
        lines.push(line);
        onLine?.(line);
      }
    });
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk) => {
      stderr += chunk;
    });
    child.on("error", (e) => resolve({ code: -1, lines, stdout, stderr: String(e), child }));
    child.on("close", (code) => {
      const rest = pending.trim();
      if (rest) {
        lines.push(rest);
        onLine?.(rest);
      }
      resolve({ code, lines, stdout, stderr, child });
    });
  });
}

/** 起 CLI 并立刻把 child 交出来，调用方自己等 done（run 要能中途 cancel）。 */
function startCli(argv, onLine) {
  const child = spawn(CLI, argv, { cwd: ROOT });
  let pending = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (chunk) => {
    pending += chunk;
    let nl;
    while ((nl = pending.indexOf("\n")) >= 0) {
      const line = pending.slice(0, nl).trim();
      pending = pending.slice(nl + 1);
      if (line) onLine(line);
    }
  });
  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk) => {
    stderr += chunk;
  });
  const done = new Promise((resolve) => {
    child.on("close", (code) => {
      const rest = pending.trim();
      if (rest) onLine(rest);
      resolve({ code, stderr });
    });
    child.on("error", (e) => resolve({ code: -1, stderr: String(e) }));
  });
  return { child, done };
}

function parseJson(line) {
  try {
    return JSON.parse(line);
  } catch {
    return null;
  }
}

function lastJson(lines) {
  for (let i = lines.length - 1; i >= 0; i -= 1) {
    const v = parseJson(lines[i]);
    if (v !== null) return v;
  }
  return null;
}

function isExecutionEvent(value) {
  return (
    value !== null &&
    typeof value === "object" &&
    typeof value.kind === "string" &&
    typeof value.runId === "string" &&
    typeof value.seq === "number"
  );
}

// ---------------------------------------------------------------- 工作区

let scratchSeq = 0;

function scratchFile(suffix) {
  scratchSeq += 1;
  return path.join(SCRATCH, `g${process.pid}-${scratchSeq}${suffix}`);
}

/** 图文档落成临时文件。graphPath 只用来定 base-dir，相对路径参数照它解析。 */
function materialize(doc, graphPath) {
  const baseDir = graphPath ? path.dirname(resolveWorkspace(graphPath)) : ROOT;
  const file = scratchFile(".lyflow.json");
  fs.writeFileSync(file, JSON.stringify(doc), "utf8");
  return { file, baseDir };
}

/** 工作区外的路径一律拒绝：桩服务器也是服务器，不该被当成任意文件读写口。 */
function resolveWorkspace(p) {
  const full = path.resolve(ROOT, p);
  const rel = path.relative(ROOT, full);
  if (rel.startsWith("..") || path.isAbsolute(rel)) {
    throw Object.assign(new Error(`路径不在工作区里：${p}`), { status: 403 });
  }
  return full;
}

// -------------------------------------------------------------------- 状态

const hub = new WsHub();
const runs = new Map();
const recent = [];
let manifestCache = null;

async function manifest() {
  if (manifestCache) return manifestCache;
  const r = await runCli(["manifest"]);
  const v = lastJson(r.lines);
  if (!v) throw new Error(`lyflow manifest 失败：${r.stderr}`);
  manifestCache = v;
  return v;
}

async function coreInfo() {
  const m = await manifest();
  return {
    version: (m.generatedBy ?? "unknown").split("/")[1] ?? "unknown",
    operatorCount: m.operators.length,
    typeCount: m.types.length,
    generation: 0,
    hotReload: false,
  };
}

async function startRun(body) {
  const { file, baseDir } = materialize(body.doc, body.graphPath ?? null);
  const argv = ["run", file, "--base-dir", baseDir, "--outputs"];
  for (const t of body.targets ?? []) argv.push("--to", t);
  if (body.mode === "preview") {
    argv.push("--preview");
    if (body.previewMaxPoints) argv.push("--preview-points", String(body.previewMaxPoints));
  }

  const state = {
    graphFile: file,
    baseDir,
    targets: body.targets ?? [],
    events: [],
    outputs: {},
    cancelled: false,
    lastSeq: -1,
  };

  let resolveId;
  const runId = new Promise((resolve) => {
    resolveId = resolve;
  });

  const { child, done } = startCli(argv, (line) => {
    const value = parseJson(line);
    if (value === null) return;
    if (isExecutionEvent(value)) {
      if (!state.id) {
        state.id = value.runId;
        runs.set(state.id, state);
        resolveId(state.id);
      }
      state.events.push(value);
      state.lastSeq = value.seq;
      hub.broadcast(value);
      return;
    }
    // 不带 runId/seq 的那些：校验诊断，或者 --outputs 的最后一行
    if (Array.isArray(value) || value.severity) state.diagnostics = value;
    else state.outputs = value;
  });

  state.child = child;
  state.done = done.then((r) => {
    if (state.cancelled && state.id) {
      const event = {
        schemaVersion: 1,
        runId: state.id,
        seq: state.lastSeq + 1,
        kind: "run_finished",
        status: "cancelled",
      };
      state.events.push(event);
      hub.broadcast(event);
    }
    return r;
  });

  const finished = state.done.then(() => null);
  const id = await Promise.race([runId, finished]);
  if (!id) {
    const detail = state.diagnostics ? JSON.stringify(state.diagnostics) : "校验失败，没有执行";
    throw Object.assign(new Error(detail), { status: 400 });
  }
  return { runId: id };
}

function outputInfoOf(state, nodeId) {
  let latest = null;
  for (const e of state.events) {
    if (e.kind === "node_state" && e.nodeId === nodeId && e.stats?.outputs) latest = e.stats.outputs;
  }
  return latest ?? [];
}

async function cloudOf(state, nodeId, port, maxPoints) {
  const pcd = scratchFile(".pcd");
  const r = await runCli([
    "dump",
    state.graphFile,
    `${nodeId}:${port}`,
    pcd,
    "--base-dir",
    state.baseDir,
    "--format",
    "ascii",
  ]);
  if (!fs.existsSync(pcd)) {
    throw Object.assign(new Error(`取不到 ${nodeId}:${port} 的点云：${r.stderr.trim()}`), {
      status: 404,
    });
  }
  try {
    return encodeCloudFromPcd(pcd, maxPoints);
  } finally {
    fs.rmSync(pcd, { force: true });
  }
}

// -------------------------------------------------------------------- HTTP

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", reject);
  });
}

function send(res, status, value) {
  const body = value === undefined ? "" : JSON.stringify(value);
  res.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": Buffer.byteLength(body),
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Headers": "Authorization, Content-Type",
    "Access-Control-Allow-Methods": "GET, POST, PUT, DELETE, OPTIONS",
  });
  res.end(body);
}

function authorized(req) {
  if (!TOKEN) return true;
  return req.headers.authorization === `Bearer ${TOKEN}`;
}

async function route(req, res, url) {
  const p = url.pathname;
  const q = url.searchParams;
  const json = async () => JSON.parse((await readBody(req)).toString("utf8") || "{}");
  const runOf = (id) => {
    const state = runs.get(id);
    if (!state) throw Object.assign(new Error(`没有这次运行：${id}`), { status: 404 });
    return state;
  };

  if (req.method === "GET" && p === "/lyflow/manifest") return send(res, 200, await manifest());
  if (req.method === "GET" && p === "/lyflow/core-info") return send(res, 200, await coreInfo());

  if (req.method === "POST" && p === "/lyflow/validate") {
    const body = await json();
    const { file, baseDir } = materialize(body.doc, body.graphPath ?? null);
    const r = await runCli(["validate", file, "--base-dir", baseDir]);
    return send(res, 200, lastJson(r.lines) ?? []);
  }

  if (req.method === "POST" && p === "/lyflow/plan") {
    const body = await json();
    const { file, baseDir } = materialize(body.doc, body.graphPath ?? null);
    const argv = ["plan", file, "--base-dir", baseDir];
    for (const t of body.targets ?? []) argv.push("--to", t);
    const r = await runCli(argv);
    return send(res, 200, lastJson(r.lines) ?? []);
  }

  if (req.method === "POST" && p === "/lyflow/run") return send(res, 200, await startRun(await json()));

  if (req.method === "POST" && p === "/lyflow/cancel") {
    const { runId } = await json();
    const state = runs.get(runId);
    if (state?.child && state.child.exitCode === null) {
      state.cancelled = true;
      state.child.kill();
    }
    return send(res, 200, {});
  }

  let m = /^\/lyflow\/runs\/([^/]+)\/outputs$/.exec(p);
  if (req.method === "GET" && m) {
    const state = runOf(decodeURIComponent(m[1]));
    await state.done;
    return send(res, 200, state.outputs ?? {});
  }

  m = /^\/lyflow\/runs\/([^/]+)\/nodes\/([^/]+)\/outputs$/.exec(p);
  if (req.method === "GET" && m) {
    const state = runOf(decodeURIComponent(m[1]));
    await state.done;
    return send(res, 200, outputInfoOf(state, decodeURIComponent(m[2])));
  }

  m = /^\/lyflow\/runs\/([^/]+)\/clouds\/([^/]+)\/([^/]+)$/.exec(p);
  if (req.method === "GET" && m) {
    const state = runOf(decodeURIComponent(m[1]));
    await state.done;
    const buf = await cloudOf(
      state,
      decodeURIComponent(m[2]),
      decodeURIComponent(m[3]),
      Number(q.get("maxPoints") ?? 0),
    );
    res.writeHead(200, {
      "Content-Type": "application/octet-stream",
      "Content-Length": buf.length,
      "Access-Control-Allow-Origin": "*",
    });
    return res.end(buf);
  }

  if (p === "/lyflow/cache") {
    // 桩服务器每次请求都是新进程，进程间没有共享缓存
    if (req.method === "DELETE") return send(res, 200, {});
    return send(res, 200, {
      entries: 0,
      bytes: 0,
      budgetBytes: 0,
      hits: 0,
      misses: 0,
      evictions: 0,
    });
  }

  if (req.method === "GET" && p === "/lyflow/library") {
    return send(res, 200, { dirs: [], count: 0, problems: [] });
  }
  if (req.method === "POST" && p === "/lyflow/library/refresh") {
    manifestCache = null;
    return send(res, 200, {
      status: { dirs: [], count: 0, problems: [] },
      manifest: await manifest(),
    });
  }
  if (req.method === "POST" && p === "/lyflow/library/save") {
    throw Object.assign(new Error("桩服务器不支持保存到库"), { status: 501 });
  }

  if (req.method === "POST" && p === "/lyflow/import") {
    const body = await json();
    const file = scratchFile(".import");
    fs.writeFileSync(file, body.text ?? "", "utf8");
    const argv = ["import", file, "--kind", body.kind];
    if (body.baseDir) argv.push("--base-dir", resolveWorkspace(body.baseDir));
    const r = await runCli(argv);
    const v = lastJson(r.lines);
    if (r.code !== 0 || !v || Array.isArray(v)) {
      throw Object.assign(new Error(v ? JSON.stringify(v) : r.stderr.trim()), { status: 400 });
    }
    return send(res, 200, v);
  }

  if (p === "/lyflow/files/graph") {
    const file = resolveWorkspace(q.get("path") ?? "");
    if (req.method === "GET") {
      const doc = JSON.parse(fs.readFileSync(file, "utf8"));
      return send(res, 200, { doc, migrations: [] });
    }
    if (req.method === "PUT") {
      const body = await json();
      fs.mkdirSync(path.dirname(file), { recursive: true });
      fs.writeFileSync(file, `${JSON.stringify(body.doc, null, 2)}\n`, "utf8");
      return send(res, 200, {});
    }
  }

  if (p === "/lyflow/files/backup" || p === "/lyflow/files/backup/status") {
    const file = `${resolveWorkspace(q.get("path") ?? "")}~`;
    if (p.endsWith("/status")) {
      const main = file.slice(0, -1);
      const exists = fs.existsSync(file);
      const backupModified = exists ? fs.statSync(file).mtimeMs : null;
      const fileModified = fs.existsSync(main) ? fs.statSync(main).mtimeMs : null;
      return send(res, 200, {
        exists,
        newer: exists && fileModified !== null && backupModified > fileModified,
        backupModified,
        fileModified,
      });
    }
    if (req.method === "GET") {
      return send(res, 200, { doc: JSON.parse(fs.readFileSync(file, "utf8")), migrations: [] });
    }
    if (req.method === "PUT") {
      const body = await json();
      fs.writeFileSync(file, JSON.stringify(body.doc), "utf8");
      return send(res, 200, {});
    }
    if (req.method === "DELETE") {
      fs.rmSync(file, { force: true });
      return send(res, 200, {});
    }
  }

  if (req.method === "PUT" && p === "/lyflow/files/bytes") {
    const file = resolveWorkspace(q.get("path") ?? "");
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, await readBody(req));
    return send(res, 200, {});
  }

  if (p === "/lyflow/recent") {
    if (req.method === "GET") return send(res, 200, recent);
    if (req.method === "POST") {
      const { path: entry } = await json();
      const at = Date.now();
      const kept = recent.filter((r) => r.path !== entry);
      recent.length = 0;
      recent.push({ path: entry, openedAt: at }, ...kept.slice(0, 19));
      return send(res, 200, recent);
    }
  }

  return send(res, 404, { error: `没有这个端点：${req.method} ${p}` });
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url ?? "/", `http://${req.headers.host ?? "localhost"}`);
  if (req.method === "OPTIONS") return send(res, 204, undefined);
  if (!authorized(req)) return send(res, 401, { error: "缺 Authorization: Bearer <token>" });
  route(req, res, url).catch((e) => {
    if (res.headersSent) return;
    send(res, e.status ?? 500, { error: e instanceof Error ? e.message : String(e) });
  });
});

hub.attach(server, "/lyflow/events", (protocols) => {
  if (!TOKEN) return true;
  return protocols.includes(`lyflow-token.${TOKEN}`);
});

server.listen(PORT, "127.0.0.1", () => {
  console.log(`lyflow test-server: http://127.0.0.1:${PORT}`);
  console.log(`  工作区 ${ROOT}`);
  console.log(`  CLI    ${CLI}`);
  console.log(`  鉴权   ${TOKEN ? "开（Bearer token）" : "关"}`);
});

function shutdown() {
  hub.closeAll();
  for (const state of runs.values()) state.child?.kill();
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 500).unref();
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
process.on("message", (m) => {
  if (m === "shutdown") shutdown();
});
