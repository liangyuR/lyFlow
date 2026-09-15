import assert from "node:assert/strict";
import { spawn, type ChildProcess } from "node:child_process";
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

function skipReason(): string | false {
  if (!ROOT) return "找不到仓库根，跳过集成冒烟";
  if (!fs.existsSync(CLI)) return `没有 ${CLI}（先 cargo build --bin lyflow），跳过集成冒烟`;
  if (!fs.existsSync(TEST_SERVER)) return `没有 ${TEST_SERVER}，跳过集成冒烟`;
  if (!ENTRY) return "还没构建出 dist/index.js 或 build-test/src/index.js，跳过集成冒烟";
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
      assert.equal(run["status"], "ok", JSON.stringify(run));
      const nodes = run["nodes"] as { id: string; state: string; elementCount: number | null }[];
      const gen = nodes.find((n) => n.id === "gen");
      assert.equal(gen?.state, "done");
      assert.equal(gen?.elementCount, 5000);
      const outputs = run["outputs"] as Record<string, { node: string; port: string }>;
      assert.equal(outputs["thinned"]?.node, "voxel");

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
