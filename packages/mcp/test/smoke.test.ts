import assert from "node:assert/strict";
import { spawn, spawnSync, type ChildProcess } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

import { findRepoRoot } from "../src/repo.js";

const ROOT = findRepoRoot();
const CLI = ROOT ? path.join(ROOT, "bridge", "target", "debug", "lyflow.exe") : "";
const TEST_SERVER = ROOT
  ? path.join(ROOT, "packages", "editor", "test-server", "server.mjs")
  : "";

function entryPoint(): string | null {
  const here = path.dirname(fileURLToPath(import.meta.url));
  const pkg = path.resolve(here, "..", "..");
  for (const candidate of [
    path.join(pkg, "dist", "index.js"),
    path.join(pkg, "build-test", "src", "index.js"),
  ]) {
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

const ENTRY = entryPoint();

/**
 * 冒烟图用的是标准包的 filter.voxel_grid（ADR-0014）。只有纯平台构建（LYFLOW_STD_PACKS=0）的 CLI
 * 里没有它才 skip；标准包没关就照跑 —— 那时没有它是构建坏了，validate_graph 那一步会挂。
 */
function stdPacksExcluded(): boolean {
  if (process.env["LYFLOW_STD_PACKS"] !== "0") return false;
  const r = spawnSync(CLI, ["manifest"], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  const line = (r.stdout ?? "").split(/\r?\n/).find((l) => l.startsWith("{")) ?? "{}";
  const operators = (JSON.parse(line) as { operators?: { id: string }[] }).operators ?? [];
  return !operators.some((o) => o.id === "filter.voxel_grid");
}

function skipReason(): string | false {
  if (!ROOT) return "找不到仓库根，跳过集成冒烟";
  if (!fs.existsSync(CLI)) return `没有 ${CLI}（先 cargo build --bin lyflow），跳过集成冒烟`;
  if (!fs.existsSync(TEST_SERVER)) return `没有 ${TEST_SERVER}，跳过集成冒烟`;
  if (!ENTRY) return "还没构建出 dist/index.js 或 build-test/src/index.js，跳过集成冒烟";
  if (stdPacksExcluded()) {
    return "纯平台构建（LYFLOW_STD_PACKS=0）没有标准包，冒烟图里的 filter.voxel_grid 跑不了，跳过集成冒烟";
  }
  return false;
}

function freePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.on("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const address = server.address();
      const port = typeof address === "object" && address ? address.port : 0;
      server.close(() => resolve(port));
    });
  });
}

async function waitFor(url: string, timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let last = "还没开始";
  while (Date.now() < deadline) {
    try {
      const res = await fetch(url);
      if (res.ok) return;
      last = `HTTP ${res.status}`;
    } catch (e) {
      last = e instanceof Error ? e.message : String(e);
    }
    await new Promise((r) => setTimeout(r, 200));
  }
  throw new Error(`等不到 ${url}：${last}`);
}

const DOC = {
  schemaVersion: 1,
  id: "mcp-smoke",
  name: "mcp 冒烟",
  nodes: [
    { id: "gen", op: "gen.synthetic", params: { pointCount: 5000, seed: 7 } },
    { id: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
  ],
  edges: [{ id: "e1", from: { node: "gen", port: "cloud" }, to: { node: "voxel", port: "cloud" } }],
  outputs: { thinned: { node: "voxel", port: "cloud" } },
};

function payload(result: unknown): Record<string, unknown> {
  const content = (result as { content?: { type: string; text?: string }[] }).content ?? [];
  return JSON.parse(content.map((c) => c.text ?? "").join("")) as Record<string, unknown>;
}

const reason = skipReason();
if (reason) console.log(`# skip: ${reason}`);

test(
  "test-server + @lyflow/mcp：算子 → 校验 → 运行 → 点云统计",
  { skip: reason, timeout: 180000 },
  async () => {
    const port = await freePort();
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), "lyflow-mcp-smoke-"));
    const graphFile = path.join(workspace, "smoke.lyflow.json");
    fs.writeFileSync(graphFile, JSON.stringify(DOC, null, 2), "utf8");

    let server: ChildProcess | null = null;
    let client: Client | null = null;
    try {
      server = spawn(
        process.execPath,
        [TEST_SERVER, "--port", String(port), "--root", workspace, "--cli", CLI],
        { stdio: ["ignore", "pipe", "pipe"] },
      );
      server.stderr?.setEncoding("utf8");
      server.stderr?.on("data", (c: string) => process.stderr.write(`[test-server] ${c}`));
      await waitFor(`http://127.0.0.1:${port}/lyflow/manifest`, 90000);

      const transport = new StdioClientTransport({
        command: process.execPath,
        args: [ENTRY as string],
        env: {
          ...(process.env as Record<string, string>),
          LYFLOW_HTTP_BASE: `http://127.0.0.1:${port}`,
          LYFLOW_CLI: CLI,
        },
        stderr: "inherit",
      });
      client = new Client({ name: "smoke", version: "0.0.0" });
      await client.connect(transport);

      const operators = payload(
        await client.callTool({ name: "list_operators", arguments: { query: "synthetic" } }),
      );
      const rows = operators["operators"] as { id: string; doc: string }[];
      assert.ok(rows.some((r) => r.id === "gen.synthetic"), JSON.stringify(rows));

      const op = payload(await client.callTool({ name: "get_operator", arguments: { id: "gen.synthetic" } }));
      assert.equal(op["id"], "gen.synthetic");
      assert.ok(Array.isArray(op["params"]));
      assert.ok(Array.isArray(op["outputs"]));

      const validated = payload(
        await client.callTool({ name: "validate_graph", arguments: { graphPath: graphFile } }),
      );
      assert.equal(validated["ok"], true, JSON.stringify(validated));

      const run = payload(
        await client.callTool({ name: "run_graph", arguments: { graphPath: graphFile } }),
      );
      // ADR-0022：status 三态来自 core 的 summary，nodes 是 id -> 收尾状态的对象
      assert.equal(run["status"], "ok", JSON.stringify(run));
      assert.equal(run["runStatus"], "ok", JSON.stringify(run));
      const nodes = run["nodes"] as Record<string, { state: string; outputsAvailable: boolean }>;
      assert.equal(nodes["gen"]?.state, "done", JSON.stringify(nodes));
      assert.equal(nodes["gen"]?.outputsAvailable, true);
      const outputs = run["outputs"] as Record<
        string,
        { state: string; node: string; port: string; elementCount?: number }
      >;
      assert.equal(outputs["thinned"]?.state, "value", JSON.stringify(outputs));
      assert.equal(outputs["thinned"]?.node, "voxel");
      assert.deepEqual(run["decisions"], {});
      assert.deepEqual(run["contractViolations"], []);

      const runId = run["runId"] as string;
      const ports = payload(
        await client.callTool({
          name: "get_node_outputs",
          arguments: { runId, nodeId: "voxel" },
        }),
      );
      assert.ok(Array.isArray(ports["outputs"]));

      const summary = payload(
        await client.callTool({
          name: "summarize_output",
          arguments: { runId, nodeId: "voxel", port: "cloud", head: 3 },
        }),
      );
      assert.equal(summary["kind"], "cloud", JSON.stringify(summary));
      const pointCount = summary["pointCount"] as number;
      assert.ok(pointCount > 0 && pointCount <= 5000, `pointCount=${pointCount}`);
      const bbox = summary["bbox"] as { min: number[]; max: number[] };
      for (let i = 0; i < 3; i += 1) {
        assert.ok(Number.isFinite(bbox.min[i]) && Number.isFinite(bbox.max[i]));
        assert.ok((bbox.max[i] as number) >= (bbox.min[i] as number));
      }
      assert.ok(
        (bbox.max[0] as number) - (bbox.min[0] as number) > 0,
        `x 方向没有跨度：${JSON.stringify(bbox)}`,
      );
      const channels = summary["channels"] as Record<string, { mean: number }>;
      for (const axis of ["x", "y", "z"]) {
        const stat = channels[axis];
        assert.ok(stat && Number.isFinite(stat.mean), `${axis} 的统计不对：${JSON.stringify(stat)}`);
      }
      assert.equal((summary["head"] as unknown[]).length, Math.min(3, pointCount));
    } finally {
      await client?.close().catch(() => undefined);
      server?.kill();
      fs.rmSync(workspace, { recursive: true, force: true });
    }
  },
);

/** 配方（param-recipe P4.2 / 验收 28）用的图：count 绑 gen.pointCount，leaf 绑 voxel.leafSize。 */
const RECIPE_DOC = {
  schemaVersion: 1,
  id: "mcp-recipe-smoke",
  name: "mcp 配方冒烟",
  params: {
    count: { type: "int", default: 5000, binds: ["gen.pointCount"], min: 1, max: 100000 },
    leaf: { type: "vec3f", default: [0.02, 0.02, 0.02], binds: ["voxel.leafSize"], min: 0.001, max: 1 },
  },
  nodes: [
    { id: "gen", op: "gen.synthetic", params: { seed: 11 } },
    { id: "voxel", op: "filter.voxel_grid" },
  ],
  edges: [{ id: "e1", from: { node: "gen", port: "cloud" }, to: { node: "voxel", port: "cloud" } }],
  outputs: { thinned: { node: "voxel", port: "cloud" } },
};

function cliJsonLines(args: string[]): { code: number; lines: Record<string, unknown>[] } {
  const r = spawnSync(CLI, args, { encoding: "utf8" });
  const lines = (r.stdout ?? "")
    .split(/\r?\n/)
    .filter((l) => l.startsWith("{"))
    .map((l) => JSON.parse(l) as Record<string, unknown>);
  return { code: r.status ?? -1, lines };
}

test(
  "配方：list_recipes → run_graph recipe（与 lyflow run --recipe 结果一致）→ 失配的配方被拦 → get_params recipe",
  { skip: reason, timeout: 180000 },
  async () => {
    const port = await freePort();
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), "lyflow-mcp-recipe-"));
    const graphFile = path.join(workspace, "配方冒烟.lyflow.json");
    fs.writeFileSync(graphFile, JSON.stringify(RECIPE_DOC, null, 2), "utf8");
    // 图的规格摘要问 CLI 要（算法只有 Rust 与编辑器两份，这里不写第三份）
    const probe = cliJsonLines(["recipes", graphFile, "--json"]);
    const digest = probe.lines.find((l) => l["kind"] === "recipe_dir")?.["specDigest"];
    assert.match(String(digest), /^sha256:[0-9a-f]{64}$/);
    const dir = path.join(workspace, "配方冒烟.recipes");
    fs.mkdirSync(dir);
    const writeRecipe = (name: string, values: Record<string, unknown>): string => {
      const file = path.join(dir, `${name}.lyflow-recipe.json`);
      const body = {
        schemaVersion: 1,
        name,
        graph: { id: RECIPE_DOC.id, specDigest: digest },
        values,
        updatedAt: "2026-09-25T00:00:00.000Z",
      };
      fs.writeFileSync(file, JSON.stringify(body, null, 2), "utf8");
      return file;
    };
    const fileA = writeRecipe("车型A", { count: 3000, leaf: [0.05, 0.05, 0.05] });
    writeRecipe("坏", { count: 0, nope: 1 });
    fs.writeFileSync(
      path.join(dir, "index.json"),
      JSON.stringify({ default: "车型A", order: ["车型A", "坏"] }),
      "utf8",
    );

    let server: ChildProcess | null = null;
    let client: Client | null = null;
    try {
      server = spawn(
        process.execPath,
        [TEST_SERVER, "--port", String(port), "--root", workspace, "--cli", CLI],
        { stdio: ["ignore", "pipe", "pipe"] },
      );
      server.stderr?.setEncoding("utf8");
      server.stderr?.on("data", (c: string) => process.stderr.write(`[test-server] ${c}`));
      await waitFor(`http://127.0.0.1:${port}/lyflow/manifest`, 90000);
      const transport = new StdioClientTransport({
        command: process.execPath,
        args: [ENTRY as string],
        env: {
          ...(process.env as Record<string, string>),
          LYFLOW_HTTP_BASE: `http://127.0.0.1:${port}`,
          LYFLOW_CLI: CLI,
        },
        stderr: "inherit",
      });
      client = new Client({ name: "smoke-recipe", version: "0.0.0" });
      await client.connect(transport);

      const listed = payload(
        await client.callTool({ name: "list_recipes", arguments: { graphPath: graphFile } }),
      );
      assert.equal(listed["count"], 2, JSON.stringify(listed));
      assert.equal(listed["default"], "车型A");
      const recipes = listed["recipes"] as {
        name: string;
        file: string;
        default: boolean;
        values: number;
        runnable: boolean;
        mismatches: Record<string, number>;
        items: { kind: string; param: string | null; fixLabel: string }[];
      }[];
      assert.deepEqual(
        recipes.map((r) => r.name),
        ["车型A", "坏"],
      );
      assert.equal(recipes[0]?.runnable, true);
      assert.equal(recipes[0]?.default, true);
      assert.deepEqual(recipes[0]?.items, []);
      assert.equal(recipes[1]?.runnable, false);
      assert.deepEqual(recipes[1]?.mismatches, { extra: 1, type: 0, range: 1, spec: 0 });
      assert.deepEqual(
        recipes[1]?.items.map((m) => [m.kind, m.param, m.fixLabel]),
        [
          ["range", "count", "夹到限位：1"],
          ["extra", "nope", "删除这个值"],
        ],
      );

      // 按配方 A 跑：取值经信封的 params 交给后端；与 CLI 的 run --recipe 是同一个结果
      const run = payload(
        await client.callTool({
          name: "run_graph",
          arguments: { graphPath: graphFile, recipe: recipes[0]?.file },
        }),
      );
      assert.equal(run["status"], "ok", JSON.stringify(run));
      assert.deepEqual(run["recipe"], { name: "车型A", file: fileA, values: 2 });
      const outputsOf = async (nodeId: string) =>
        payload(
          await client!.callTool({
            name: "get_node_outputs",
            arguments: { runId: run["runId"], nodeId },
          }),
        )["outputs"] as { port: string; elementCount?: number }[];
      const genPorts = await outputsOf("gen");
      assert.equal(genPorts.find((p) => p.port === "cloud")?.elementCount, 3000, JSON.stringify(genPorts));
      const viaMcp = (await outputsOf("voxel")).find((p) => p.port === "cloud")?.elementCount;

      const direct = cliJsonLines(["run", graphFile, "--recipe", fileA, "--outputs"]);
      assert.equal(direct.code, 0);
      const cliOutputs = direct.lines[direct.lines.length - 1] as Record<string, { elementCount?: number }>;
      assert.equal(viaMcp, cliOutputs["thinned"]?.elementCount, JSON.stringify(cliOutputs));
      // 基础下点数不同：真的是配方在起作用
      const base = cliJsonLines(["run", graphFile, "--outputs"]);
      const baseOutputs = base.lines[base.lines.length - 1] as Record<string, { elementCount?: number }>;
      assert.notEqual(baseOutputs["thinned"]?.elementCount, viaMcp);

      // 失配的配方：不碰后端，报错里带条目
      const blocked = await client.callTool({
        name: "run_graph",
        arguments: { graphPath: graphFile, recipe: recipes[1]?.file },
      });
      assert.equal(blocked.isError, true);
      const why = payload(blocked);
      assert.match(String(why["error"]), /配方「坏」有 2 处失配，不能运行/);
      assert.equal((why["recipe"] as { blocking: number }).blocking, 2);

      // 内联的图同样认 recipe（MCP 把它落成临时文件交给 CLI）
      const inline = payload(
        await client.callTool({ name: "run_graph", arguments: { graph: RECIPE_DOC, recipe: fileA } }),
      );
      assert.equal(inline["status"], "ok", JSON.stringify(inline));

      const params = payload(
        await client.callTool({
          name: "get_params",
          arguments: { graphPath: graphFile, recipe: fileA, only: "graph" },
        }),
      );
      const rows = params["params"] as { node: string; param: string; value: unknown; graphParam?: string }[];
      assert.deepEqual(
        rows.map((r) => [r.node, r.param, r.value, r.graphParam]),
        [
          ["gen", "pointCount", 3000, "count"],
          ["voxel", "leafSize", [0.05, 0.05, 0.05], "leaf"],
        ],
      );
      const badParams = await client.callTool({
        name: "get_params",
        arguments: { graphPath: graphFile, recipe: recipes[1]?.file },
      });
      assert.equal(badParams.isError, true);
      assert.match(payload(badParams)["stderr"] as string, /\[越界\] count/);
    } finally {
      await client?.close().catch(() => undefined);
      server?.kill();
      fs.rmSync(workspace, { recursive: true, force: true });
    }
  },
);
