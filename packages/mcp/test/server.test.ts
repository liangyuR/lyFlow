import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";

import { MISSING_BASE, loadConfig } from "../src/config.js";
import { createServer } from "../src/server.js";

const TOOLS = [
  "list_operators",
  "get_operator",
  "list_port_types",
  "validate_graph",
  "plan_graph",
  "run_graph",
  "get_node_outputs",
  "summarize_output",
  "eval",
  "perturb",
  "diff_graphs",
  "get_params",
  "patch_graph",
];

async function connect(env: NodeJS.ProcessEnv): Promise<Client> {
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  const server = createServer(loadConfig(env));
  await server.connect(serverTransport);
  const client = new Client({ name: "test", version: "0.0.0" });
  await client.connect(clientTransport);
  return client;
}

function text(result: unknown): string {
  const content = (result as { content?: { type: string; text?: string }[] }).content ?? [];
  return content.map((c) => c.text ?? "").join("");
}

test("LYFLOW_HTTP_BASE 是必填的，缺了就说清楚", () => {
  assert.throws(() => loadConfig({}), new RegExp("LYFLOW_HTTP_BASE"));
  assert.match(MISSING_BASE, /LYFLOW_HTTP_BASE/);
});

test("配置项默认值", () => {
  const config = loadConfig({ LYFLOW_HTTP_BASE: "http://127.0.0.1:8787/" });
  assert.equal(config.httpBase, "http://127.0.0.1:8787");
  assert.equal(config.token, undefined);
  assert.equal(config.cli, undefined);
  assert.equal(config.workDir, path.join(os.tmpdir(), "lyflow-mcp"));
});

test("工具面就是这 13 个，输入 schema 的必填项对得上", async () => {
  const client = await connect({ LYFLOW_HTTP_BASE: "http://127.0.0.1:1" });
  const listed = await client.listTools();
  assert.deepEqual(
    listed.tools.map((t) => t.name).sort(),
    [...TOOLS].sort(),
  );

  const byName = new Map(listed.tools.map((t) => [t.name, t]));
  const required = (name: string): string[] =>
    ((byName.get(name)?.inputSchema as { required?: string[] } | undefined)?.required ?? []).sort();
  const props = (name: string): string[] =>
    Object.keys(
      (byName.get(name)?.inputSchema as { properties?: Record<string, unknown> } | undefined)
        ?.properties ?? {},
    ).sort();

  assert.deepEqual(required("get_operator"), ["id"]);
  assert.deepEqual(required("list_operators"), []);
  assert.deepEqual(props("list_operators"), ["category", "pack", "query"]);
  assert.deepEqual(required("summarize_output"), ["nodeId", "port", "runId"]);
  assert.deepEqual(props("summarize_output"), ["head", "maxPoints", "nodeId", "port", "runId"]);
  assert.deepEqual(required("run_graph"), []);
  assert.deepEqual(props("run_graph"), [
    "baseDir",
    "graph",
    "graphPath",
    "mode",
    "set",
    "targets",
    "timeoutMs",
  ]);
  assert.deepEqual(required("eval"), ["graphPath", "metric"]);
  assert.deepEqual(required("perturb"), ["after", "axis", "graphPath", "metric", "region"]);
  assert.deepEqual(required("diff_graphs"), ["a", "b"]);
  assert.deepEqual(required("get_params"), ["graphPath"]);
  assert.deepEqual(props("get_params"), ["baseDir", "graphPath", "node", "only", "set"]);
  assert.deepEqual(required("patch_graph"), ["graphPath"]);
  assert.deepEqual(props("patch_graph"), [
    "addNode",
    "baseDir",
    "dryRun",
    "graphPath",
    "out",
    "removeNode",
    "rewire",
    "set",
  ]);
  await client.close();
});

test("resource 清单里有 manifest、三份 schema、两篇文档与图样例", async () => {
  const client = await connect({ LYFLOW_HTTP_BASE: "http://127.0.0.1:1" });
  const listed = await client.listResources();
  const uris = listed.resources.map((r) => r.uri);
  for (const uri of [
    "lyflow://manifest",
    "lyflow://schema/operator-manifest",
    "lyflow://schema/graph-doc",
    "lyflow://schema/execution-event",
    "lyflow://examples/graph",
    "lyflow://docs/agent-tuning",
    "lyflow://docs/http-transport",
  ]) {
    assert.ok(uris.includes(uri), `缺 ${uri}，实际有 ${uris.join(", ")}`);
  }
  await client.close();
});

test("没配 LYFLOW_CLI 时 eval / perturb / diff_graphs 给一句说得清的错", async () => {
  const client = await connect({ LYFLOW_HTTP_BASE: "http://127.0.0.1:1" });
  for (const [name, args] of [
    ["eval", { graphPath: "g.json", metric: ["outputs.gap"] }],
    [
      "perturb",
      { graphPath: "g.json", after: "n:cloud", region: {}, axis: "x=0:1:2", metric: ["outputs.gap"] },
    ],
    ["diff_graphs", { a: "a.json", b: "b.json" }],
  ] as const) {
    const result = await client.callTool({ name, arguments: args });
    assert.equal(result.isError, true, name);
    assert.match(text(result), /LYFLOW_CLI/, name);
  }
  await client.close();
});

test("后端连不上时工具报错而不是挂住", async () => {
  const client = await connect({ LYFLOW_HTTP_BASE: "http://127.0.0.1:1" });
  const result = await client.callTool({ name: "list_operators", arguments: {} });
  assert.equal(result.isError, true);
  assert.match(text(result), /连不上/);
  await client.close();
});

test("图给得不对时 run_graph 不去碰后端", async () => {
  const client = await connect({ LYFLOW_HTTP_BASE: "http://127.0.0.1:1" });
  const result = await client.callTool({
    name: "run_graph",
    arguments: {
      graph: { schemaVersion: 1, id: "g", nodes: [{ id: "a", op: "x" }], edges: [] },
      set: { "nope.p": 1 },
    },
  });
  assert.equal(result.isError, true);
  assert.match(text(result), /没有节点 nope/);
  await client.close();
});

test("图缺 GraphDoc 必填字段时当场说清楚，而不是让后端回一个空诊断数组", async () => {
  const client = await connect({ LYFLOW_HTTP_BASE: "http://127.0.0.1:1" });
  const result = await client.callTool({
    name: "validate_graph",
    arguments: { graph: { schemaVersion: 1, nodes: [{ id: "a", op: "x" }] } },
  });
  assert.equal(result.isError, true);
  assert.match(text(result), /id/);
  assert.match(text(result), /edges/);
  await client.close();
});
