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

/** 信封里的 params（编辑器合成的「default + 当前配方覆盖」，param-recipe K3）→ CLI 的
 *  `--param <名字>=<json>`。CLI 按 JSON 解析值，与 C ABI 的 params_json 给出同一个结果。 */
function paramArgs(params) {
  if (!params || typeof params !== "object") return [];
  return Object.entries(params).flatMap(([name, value]) => ["--param", `${name}=${JSON.stringify(value)}`]);
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

// ---------------------------------------------------------------- 配方文件
// param-recipe P3.2，与 Tauri 的五个命令（bridge/src/commands.rs「配方文件」）同一套约束，外加「只在工作区里」：
// 配方目录（名字以 .recipes 结尾）里的 *.lyflow-recipe.json、index.json、autosave~.json 可读写删，
// *.lyflow-recipe.json 之间可改名；任意位置（工作区内）的 *.lyflow-recipe.json 可读写（导入 / 导出）。

const RECIPE_EXT = ".lyflow-recipe.json";
const RECIPE_AUX = ["index.json", "autosave~.json"];

const endsWithCi = (s, suffix) => s.length > suffix.length && s.toLowerCase().endsWith(suffix);
const isRecipeFile = (name) => endsWithCi(name, RECIPE_EXT);
const isRecipeAux = (name) => RECIPE_AUX.includes(name.toLowerCase());
const isRecipeDir = (dir) => endsWithCi(path.basename(dir), ".recipes");

function refuse(message) {
  return Object.assign(new Error(message), { status: 403 });
}

/** access = "rw"（读写：配方目录里的三种，或任意位置的配方文件）| "dir"（删、改名：只在配方目录里）。 */
function recipePath(p, access) {
  if (String(p).split(/[\\/]/).includes("..")) throw refuse(`路径里不能有 ..：${p}`);
  const full = resolveWorkspace(p);
  const name = path.basename(full);
  const inDir = isRecipeDir(path.dirname(full));
  const ok = access === "rw" ? isRecipeFile(name) || (inDir && isRecipeAux(name)) : inDir && (isRecipeFile(name) || isRecipeAux(name));
  if (!ok) throw refuse(`这里只能读写配方文件（*${RECIPE_EXT}，或 *.recipes/ 里的 index.json、autosave~.json）：${p}`);
  return full;
}

// -------------------------------------------------------------------- 状态

const hub = new WsHub();
const runs = new Map();
const recent = [];
let manifestCache = null;

/** 跑出过输出的 cacheKey。桩服务器不常驻 core、没有结果仓，单节点运行（docs/node-run-plan.md R6）
 *  的「上游有没有结果」只能照事件记账：run_started 给出 id → cacheKey，节点 done / skipped 且
 *  输出可取就记一笔。 */
const produced = new Set();
/** cacheKey → 那一次的 stats.outputs。R7 挂上的节点这次没有事件，getOutputInfo 就从这里取。 */
const producedInfo = new Map();

function notePlanNodes(state, nodes) {
  for (const n of nodes ?? []) state.keys.set(n.id, n.cacheKey);
}

function noteEvent(state, value) {
  if (value.kind === "run_started" || value.kind === "plan_extended") notePlanNodes(state, value.nodes);
  if (value.kind !== "node_state") return;
  if (["done", "skipped", "error", "cancelled"].includes(value.state)) state.touched.add(value.nodeId);
  if (value.state !== "done" && value.state !== "skipped") return;
  if (value.stats?.outputsAvailable === false) return;
  const key = state.keys.get(value.nodeId);
  if (!key) return;
  produced.add(key);
  if (value.stats?.outputs) producedInfo.set(key, value.stats.outputs);
}

/** R7 的最小语义：全图计划里这次没有收场事件、但 cacheKey 跑出过结果的节点算「挂上」。
 *  桩取点云本来就是按图重跑一遍 CLI，不看 runId，所以挂不挂只影响 attached 与 getOutputInfo。 */
function attachUnplanned(state) {
  const attached = [];
  for (const [id, key] of state.fullKeys ?? []) {
    if (state.touched.has(id) || !produced.has(key)) continue;
    attached.push(id);
    state.attachedInfo.set(id, producedInfo.get(key) ?? []);
  }
  return attached;
}

/** isolate 的 id 语义与 targets 相同：精确命中，或落在某个子图节点之下。 */
const inIsolate = (isolate, id) => isolate.some((t) => id === t || id.startsWith(`${t}/`));

let stubSeq = 0;

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

/** 单节点运行的最小语义（R6）：上游有没有当前 cacheKey 的结果，照 produced 记账判；缺就不起 CLI，
 *  直接回一对 run_started / run_finished(upstream_not_ready)，与 core 开跑前失败时的事件同形。
 *  齐了就按 --to 跑 —— CLI 没有 isolate 开关（R6 不扩到 CLI），进程之间也不共享缓存，所以
 *  上游会被桩重算一遍；编辑器只认 isolate 里节点的事件，界面上看不出差别。 */
async function isolateVerdict(body, file, baseDir) {
  const isolate = body.isolate;
  const argv = ["plan", file, "--base-dir", baseDir, ...paramArgs(body.params)];
  for (const t of isolate) argv.push("--to", t);
  const r = await runCli(argv);
  const plan = lastJson(r.lines);
  // 编不出计划（图有错）：交给真正的运行去报诊断
  if (!Array.isArray(plan) || plan.some((n) => typeof n.cacheKey !== "string")) return null;
  const missing = plan.filter(
    (n) => !inIsolate(isolate, n.nodeId) && !n.lazy && !n.bypass && !produced.has(n.cacheKey),
  );
  return { plan, missing, fullKeys: await fullKeysOf(file, baseDir, body.params) };
}

/** 全图的 cacheKey（R7 / 修订一 V2）：计划外节点「现在该有的键」。 */
async function fullKeysOf(file, baseDir, params) {
  const whole = lastJson((await runCli(["plan", file, "--base-dir", baseDir, ...paramArgs(params)])).lines);
  return new Map(
    Array.isArray(whole)
      ? whole.filter((n) => typeof n.cacheKey === "string").map((n) => [n.nodeId, n.cacheKey])
      : [],
  );
}

function rejectNotReady(body, verdict) {
  stubSeq += 1;
  const runId = `stub-isolate-${Date.now()}-${stubSeq}`;
  const eager = verdict.plan.filter((n) => !n.lazy);
  const diagnostics = verdict.missing.map((n) => ({
    nodeId: n.nodeId,
    severity: "error",
    phase: "execute",
    code: "upstream_not_ready",
    message: `上游 ${n.nodeId} 还没有可用结果，先运行它或运行到此`,
  }));
  const at = new Date().toISOString();
  const started = {
    schemaVersion: 1, runId, seq: 0, at, kind: "run_started",
    nodeCount: eager.length, maxParallel: 1, mode: "full",
    plan: eager.map((n) => n.nodeId), targets: body.isolate, isolate: body.isolate, force: body.force ?? [],
    nodes: eager.map((n) => ({ id: n.nodeId, cacheKey: n.cacheKey, level: n.level })),
  };
  const first = diagnostics[0];
  const finished = {
    schemaVersion: 1, runId, seq: 1, at, kind: "run_finished", status: "error", durationMs: 0,
    error: { phase: first.phase, code: first.code, message: first.message },
    diagnostics,
  };
  const state = {
    id: runId, events: [started, finished], outputs: {}, cancelled: false, lastSeq: 1,
    keys: new Map(), done: Promise.resolve({ code: 1 }),
    touched: new Set(), fullKeys: verdict.fullKeys, attachedInfo: new Map(),
  };
  finished.attached = attachUnplanned(state);
  runs.set(runId, state);
  hub.broadcast(started);
  hub.broadcast(finished);
  return { runId };
}

async function startRun(body) {
  const { file, baseDir } = materialize(body.doc, body.graphPath ?? null);
  const isolate = Array.isArray(body.isolate) && body.isolate.length > 0 ? body.isolate : null;
  if (isolate && body.mode === "preview") {
    throw Object.assign(new Error("预览模式不能与「只运行此节点」（isolate）同时使用"), { status: 400 });
  }
  let verdict = null;
  if (isolate) {
    verdict = await isolateVerdict(body, file, baseDir);
    if (verdict && verdict.missing.length > 0) return rejectNotReady(body, verdict);
  }
  const argv = ["run", file, "--base-dir", baseDir, "--outputs", ...paramArgs(body.params)];
  const scope = isolate ?? body.targets ?? [];
  for (const t of scope) argv.push("--to", t);
  // 带 targets 的运行都要挂结果（修订一 V2），先把全图的键拿到
  const fullKeys =
    verdict?.fullKeys ?? (scope.length > 0 ? await fullKeysOf(file, baseDir, body.params) : null);
  // force（V1）不用管：桩每次都是新进程、没有跨运行的缓存，本来就是真算
  if (body.mode === "preview") {
    argv.push("--preview");
    if (body.previewMaxPoints) argv.push("--preview-points", String(body.previewMaxPoints));
  }

  const state = {
    graphFile: file,
    baseDir,
    // 取点云是按图重跑一遍 CLI（cloudOf）：这次的图参数取值得跟着，否则重跑的是 default
    params: body.params ?? null,
    targets: isolate ?? body.targets ?? [],
    events: [],
    outputs: {},
    cancelled: false,
    lastSeq: -1,
    keys: new Map(),
    touched: new Set(),
    fullKeys,
    attachedInfo: new Map(),
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
      // CLI 不认 isolate / force：把这次的范围补回 run_started，编辑器靠它认出单节点运行
      if (isolate && value.kind === "run_started") {
        value.isolate = isolate;
        value.targets = isolate;
      }
      if (value.kind === "run_started") value.force = body.force ?? [];
      noteEvent(state, value);
      // 进程里的 core 也会算 attached，但它的结果仓是这一个进程的、永远是空的：按记账覆盖
      if (fullKeys && value.kind === "run_finished") value.attached = attachUnplanned(state);
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

/** run summary（ADR-0022）就挂在最后一条 run_finished 上，不必再起一次 CLI。 */
function summaryOf(state) {
  for (let i = state.events.length - 1; i >= 0; i -= 1) {
    const e = state.events[i];
    if (e.kind !== "run_finished") continue;
    return e.summary && typeof e.summary === "object" ? e.summary : null;
  }
  return null;
}

function outputInfoOf(state, nodeId) {
  let latest = state.attachedInfo?.get(nodeId) ?? null;
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
    ...paramArgs(state.params),
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
    const r = await runCli(["validate", file, "--base-dir", baseDir, ...paramArgs(body.params)]);
    return send(res, 200, lastJson(r.lines) ?? []);
  }

  if (req.method === "POST" && p === "/lyflow/plan") {
    const body = await json();
    const { file, baseDir } = materialize(body.doc, body.graphPath ?? null);
    const argv = ["plan", file, "--base-dir", baseDir, ...paramArgs(body.params)];
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

  // ADR-0022：与 run_finished 事件里那个 summary 是同一个对象。
  // 还没有（run 没结束，或者接的是老 core）就是 404 —— 不能压成 {}。
  m = /^\/lyflow\/runs\/([^/]+)\/summary$/.exec(p);
  if (req.method === "GET" && m) {
    const state = runOf(decodeURIComponent(m[1]));
    await state.done;
    const summary = summaryOf(state);
    if (!summary) {
      return send(res, 404, { error: "这次运行没有 run summary（core 的 ABI < v9？）" });
    }
    return send(res, 200, summary);
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

  m = /^\/lyflow\/runs\/([^/]+)\/tensors\/([^/]+)\/([^/]+)$/.exec(p);
  if (req.method === "GET" && m) {
    throw Object.assign(
      new Error("桩服务器不支持取张量（没有常驻结果仓），见 docs/http-transport.md"),
      { status: 501 },
    );
  }

  m = /^\/lyflow\/runs\/([^/]+)\/indices\/([^/]+)\/([^/]+)$/.exec(p);
  if (req.method === "GET" && m) {
    throw Object.assign(
      new Error("桩服务器不支持取下标（没有常驻结果仓），见 docs/http-transport.md"),
      { status: 501 },
    );
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

  if (req.method === "GET" && p === "/lyflow/files/recipes") {
    const raw = q.get("dir") ?? "";
    if (raw.split(/[\\/]/).includes("..")) throw refuse(`路径里不能有 ..：${raw}`);
    const dir = resolveWorkspace(raw);
    if (!isRecipeDir(dir)) throw refuse(`不是配方目录（名字要以 .recipes 结尾）：${raw}`);
    if (!fs.existsSync(dir) || !fs.statSync(dir).isDirectory()) return send(res, 200, { exists: false, files: [] });
    const files = fs
      .readdirSync(dir, { withFileTypes: true })
      .filter((e) => e.isFile() && (isRecipeFile(e.name) || isRecipeAux(e.name)))
      .map((e) => ({ name: e.name, modified: Math.round(fs.statSync(path.join(dir, e.name)).mtimeMs) }))
      .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    return send(res, 200, { exists: true, files });
  }

  if (p === "/lyflow/files/recipe") {
    if (req.method === "GET") {
      const file = recipePath(q.get("path") ?? "", "rw");
      return send(res, 200, { text: fs.readFileSync(file, "utf8") });
    }
    if (req.method === "PUT") {
      const file = recipePath(q.get("path") ?? "", "rw");
      const { text } = await json();
      let value;
      try {
        value = JSON.parse(text);
      } catch (e) {
        throw Object.assign(new Error(`内容不是合法 JSON：${e.message}`), { status: 400 });
      }
      if (typeof value !== "object" || value === null || Array.isArray(value)) {
        throw Object.assign(new Error("内容应当是一个 JSON 对象"), { status: 400 });
      }
      fs.mkdirSync(path.dirname(file), { recursive: true });
      const tmp = `${file}.tmp~`;
      fs.writeFileSync(tmp, text, "utf8");
      fs.renameSync(tmp, file);
      return send(res, 200, {});
    }
    if (req.method === "DELETE") {
      fs.rmSync(recipePath(q.get("path") ?? "", "dir"), { force: true });
      return send(res, 200, {});
    }
  }

  if (req.method === "POST" && p === "/lyflow/files/recipe/rename") {
    const body = await json();
    const from = recipePath(body.from ?? "", "dir");
    const to = recipePath(body.to ?? "", "dir");
    if (!isRecipeFile(path.basename(from)) || !isRecipeFile(path.basename(to))) {
      throw refuse("只有配方文件（*.lyflow-recipe.json）能改名");
    }
    if (path.dirname(from) !== path.dirname(to)) throw refuse("改名只能在同一个配方目录里");
    if (fs.existsSync(to) && from.toLowerCase() !== to.toLowerCase()) {
      throw Object.assign(new Error(`${body.to} 已经存在`), { status: 409 });
    }
    fs.renameSync(from, to);
    return send(res, 200, {});
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
