import { McpServer, ResourceTemplate } from "@modelcontextprotocol/sdk/server/mcp.js";

import type { LyFlowHttp } from "./http.js";
import { findRepoRoot, listPacks, readRepoFile } from "./repo.js";

interface FileResource {
  uri: string;
  name: string;
  title: string;
  description: string;
  mimeType: string;
  relative: string;
}

const FILES: FileResource[] = [
  {
    uri: "lyflow://schema/operator-manifest",
    name: "schema-operator-manifest",
    title: "算子清单 schema",
    description: "OperatorManifestBundle 的 JSON Schema。",
    mimeType: "application/json",
    relative: "schema/operator-manifest.schema.json",
  },
  {
    uri: "lyflow://schema/graph-doc",
    name: "schema-graph-doc",
    title: "图文档 schema",
    description: "GraphDoc 的 JSON Schema —— 写图之前先看它。",
    mimeType: "application/json",
    relative: "schema/graph-doc.schema.json",
  },
  {
    uri: "lyflow://schema/execution-event",
    name: "schema-execution-event",
    title: "执行事件 schema",
    description: "ExecutionEvent 与诊断的 JSON Schema。",
    mimeType: "application/json",
    relative: "schema/execution-event.schema.json",
  },
  {
    uri: "lyflow://examples/graph",
    name: "example-graph",
    title: "图文档样例",
    description: "一份合法的 GraphDoc，含图级命名输出。",
    mimeType: "application/json",
    relative: "schema/examples/graph.example.lyflow.json",
  },
  {
    uri: "lyflow://docs/agent-tuning",
    name: "docs-agent-tuning",
    title: "调参工作法",
    description: "eval / perturb / 留出集的工作法，含两个真实的切错案例。",
    mimeType: "text/markdown",
    relative: "docs/agent-tuning.md",
  },
  {
    uri: "lyflow://docs/http-transport",
    name: "docs-http-transport",
    title: "HTTP 传输契约",
    description: "/lyflow/* 的 REST + WebSocket 契约。",
    mimeType: "text/markdown",
    relative: "docs/http-transport.md",
  },
];

export function registerResources(server: McpServer, http: LyFlowHttp): void {
  const root = findRepoRoot();

  server.registerResource(
    "manifest",
    "lyflow://manifest",
    {
      title: "算子清单",
      description: "后端当前这一份完整 OperatorManifestBundle。",
      mimeType: "application/json",
    },
    async (uri) => {
      const bundle = await http.manifest();
      return {
        contents: [
          { uri: uri.href, mimeType: "application/json", text: JSON.stringify(bundle) },
        ],
      };
    },
  );

  for (const file of FILES) {
    server.registerResource(
      file.name,
      file.uri,
      { title: file.title, description: file.description, mimeType: file.mimeType },
      (uri) => ({
        contents: [
          { uri: uri.href, mimeType: file.mimeType, text: readRepoFile(root, file.relative) },
        ],
      }),
    );
  }

  server.registerResource(
    "pack-readme",
    new ResourceTemplate("lyflow://packs/{name}/readme", {
      list: () => ({
        resources: listPacks(root).map((name) => ({
          uri: `lyflow://packs/${name}/readme`,
          name: `pack-${name}`,
          title: `${name} 包的 README`,
          mimeType: "text/markdown",
        })),
      }),
    }),
    { title: "算子包 README", description: "packs/<name>/README.md。", mimeType: "text/markdown" },
    (uri, variables) => {
      const name = String(variables["name"] ?? "");
      if (!/^[A-Za-z0-9._-]+$/.test(name)) throw new Error(`不是一个包名：${name}`);
      return {
        contents: [
          {
            uri: uri.href,
            mimeType: "text/markdown",
            text: readRepoFile(root, `packs/${name}/README.md`),
          },
        ],
      };
    },
  );
}
