import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import type { Config } from "./config.js";
import type { LyFlowHttp } from "./http.js";
import { resolveGraph } from "./graph.js";
import { paramsArgv } from "./argv.js";
import { runCli } from "./cli.js";
import { ArtifactStore } from "./artifacts.js";
import { object } from "./summary.js";

export function digest(value: unknown): string {
  const canonical = (v: unknown): unknown => Array.isArray(v) ? v.map(canonical)
    : v !== null && typeof v === "object" ? Object.fromEntries(Object.entries(object(v)).sort(([a], [b]) => a.localeCompare(b)).map(([k, x]) => [k, canonical(x)])) : v;
  return crypto.createHash("sha256").update(JSON.stringify(canonical(value)) ?? "null").digest("hex");
}

export async function environment(config: Config, http: LyFlowHttp): Promise<Record<string, unknown>> {
  const remote = await http.manifest();
  const remoteInfo = await http.coreInfo();
  const local = config.cli ? await runCli(config, ["manifest"]) : null;
  const manifest = local?.lines.find((x) => Array.isArray(x.value["operators"]))?.value ?? null;
  const info = config.cli ? await runCli(config, ["info"]) : null;
  const cliInfo = info?.lines.find((x) => x.value["kind"] === "core_info")?.value ?? null;
  const remoteHash = digest(remote);
  const localHash = manifest ? digest(manifest) : null;
  const sameManifest = localHash === remoteHash;
  const a = object(remoteInfo)["buildFingerprint"], b = cliInfo?.["buildFingerprint"];
  return {
    http: { base: config.httpBase, ...remoteInfo, manifestHash: remoteHash },
    cli: { executable: config.cli ?? null, available: local?.code === 0, manifestHash: localHash, ...cliInfo },
    compatible: sameManifest, buildVerified: typeof a === "string" && a === b,
    warnings: [
      ...(!config.cli ? ["LYFLOW_CLI 未配置，批评估与参数查询不可用"] : []),
      ...(manifest && !sameManifest ? ["HTTP 与 CLI 的 manifest 不一致，单图与批评估不可比较"] : []),
      ...(!(typeof a === "string" && a === b) ? ["未验证 HTTP/CLI 构建指纹相同；manifest 一致也不能证明算法实现相同"] : []),
    ],
    packs: [...new Set(remote.operators.map((o) => o.pack ?? "core"))],
    workDir: config.workDir,
  };
}

export function registerInspectionTools(server: McpServer, config: Config, http: LyFlowHttp, artifacts: ArtifactStore): void {
  const ok = (v: Record<string, unknown>) => ({ content: [{ type: "text" as const, text: JSON.stringify(v) }], structuredContent: v });
  const fail = (e: unknown) => ({ content: [{ type: "text" as const, text: JSON.stringify({ error: e instanceof Error ? e.message : String(e) }) }], isError: true });
  server.registerTool("get_environment", {
    title: "检查执行环境", description: "核对 HTTP/CLI 算子清单、构建指纹与启用的包；不返回鉴权凭据。",
    inputSchema: {}, annotations: { readOnlyHint: true },
  }, async () => { try { return ok(await environment(config, http)); } catch (e) { return fail(e); } });

  server.registerTool("inspect_graph", {
    title: "检查图与调参入口",
    description: "图拓扑、core 生效参数、绑定、单位、参数角色、外部文件和算子运行能力。取值来自 lyflow params，不自行合并默认值。",
    inputSchema: {
      graph: z.record(z.unknown()).optional(), graphPath: z.string().optional(), baseDir: z.string().optional(),
      graphParams: z.record(z.unknown()).optional(), set: z.record(z.unknown()).optional(), recipe: z.string().optional(),
    }, annotations: { readOnlyHint: true },
  }, async (args, extra) => {
    try {
      if (!config.cli) throw new Error("inspect_graph 的生效参数需要配置 LYFLOW_CLI");
      const graph = resolveGraph(args);
      let file = args.graphPath;
      if (!file) {
        const dir = path.join(config.workDir, `inspect-${crypto.randomUUID()}`);
        fs.mkdirSync(dir, { recursive: true });
        file = path.join(dir, "graph.lyflow.json");
        fs.writeFileSync(file, JSON.stringify(graph.doc));
      }
      const argv = paramsArgv({ graphPath: file, baseDir: args.baseDir, recipe: args.recipe, graphParams: args.graphParams,
        set: args.graphPath ? Object.entries(args.set ?? {}).map(([k, v]) => `${k}=${JSON.stringify(v)}`) : undefined });
      const result = await runCli(config, argv, { signal: extra.signal });
      if (result.code !== 0) throw new Error(`lyflow params 退出码 ${result.code}: ${result.stderr}`);
      const local = await runCli(config,["manifest"],{signal:extra.signal});
      const manifest = local.lines[0]?.value as unknown as import("./types.js").ManifestBundle;
      if (local.code !== 0 || !Array.isArray(manifest?.operators)) throw new Error("CLI manifest 不可读");
      const definitions = object(graph.doc["params"]);
      const params = result.lines.map((line) => {
        const row = line.value;
        const op = manifest.operators.find((o) => o.id === row["op"]);
        const spec = object(op?.params?.find((p) => object(p)["name"] === row["param"]));
        return { ...spec, ...row, role: spec["tuningRole"] ?? (spec["type"] === "path" ? "input" : "unspecified"),
          editPath: row["graphParam"] ? `graphParams.${row["graphParam"]}` : `set.${row["node"]}.${row["param"]}`,
          unitResolution:spec["unitSource"] ? {source:spec["unitSource"],resolved:null,requiresRun:true} : {fixed:row["unit"] ?? spec["unit"] ?? null} };
      });
      const root = args.baseDir ? path.resolve(args.baseDir) : path.dirname(path.resolve(file));
      const dependencies = params.filter((p) => object(p)["type"] === "path" && typeof object(p)["value"] === "string" && object(p)["value"] !== "").map((p) => {
        const o = object(p), localPath = path.resolve(root, String(o["value"]));
        return { node: o["node"], param: o["param"], value: o["value"], scope: "cli", localPath, localExists: fs.existsSync(localPath) };
      });
      return ok({ id: graph.doc.id, graphHash: digest(graph.doc), graphParams: definitions,
        nodes: graph.doc.nodes.map((n) => ({ id: n.id, op: n.op, capabilities: manifest.operators.find((o) => o.id === n.op)?.capabilities ?? {} })),
        edges: graph.doc.edges ?? [], outputs: graph.doc.outputs ?? {}, params, dependencies,
        artifact: artifacts.put({ graph: graph.doc, effectiveParams: params }),
        note: "localExists 只检查 CLI 文件系统；HTTP 远端路径由后端校验。质量规格 acceptance 默认锁定。" });
    } catch (e) { return fail(e); }
  });
}
