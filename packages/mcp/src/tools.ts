import fs from "node:fs";
import path from "node:path";

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { evalArgv, perturbArgv } from "./argv.js";
import { DEFAULT_CLI_TIMEOUT_MS, runCli, stderrTail } from "./cli.js";
import { decodeCloud, summarizeCloud } from "./cloud.js";
import type { Config } from "./config.js";
import { resolveGraph } from "./graph.js";
import { HttpError, LyFlowHttp } from "./http.js";
import { parseDiagnostics, summarizeOutputs, summarizeRun } from "./run.js";
import { firstSentence, matches, nearest } from "./text.js";
import type { OutputInfo } from "./types.js";

const MAX_FAILURES = 20;
const DEFAULT_RUN_TIMEOUT_MS = 300000;
const DEFAULT_MAX_POINTS = 200000;
const DEFAULT_HEAD = 8;

type ToolResult = {
  content: { type: "text"; text: string }[];
  isError?: boolean;
};

function ok(value: unknown): ToolResult {
  return { content: [{ type: "text", text: JSON.stringify(value) }] };
}

function bad(message: string, extra?: Record<string, unknown>): ToolResult {
  const body = extra ? { error: message, ...extra } : { error: message };
  return { content: [{ type: "text", text: JSON.stringify(body) }], isError: true };
}

function failed(e: unknown): ToolResult {
  if (e instanceof HttpError) return bad(e.message, { httpStatus: e.status });
  return bad(e instanceof Error ? e.message : String(e));
}

const graphInput = {
  graph: z.record(z.unknown()).optional().describe("内联的 GraphDoc。与 graphPath 二选一"),
  graphPath: z
    .string()
    .optional()
    .describe("本地图文件路径。MCP 进程读它并把 doc 发给后端；与 graph 二选一"),
  baseDir: z
    .string()
    .optional()
    .describe("后端解析相对路径参数的基准目录，相对后端工作区根"),
};

const cliSamples = {
  samplesPath: z.string().optional().describe("样本集 JSON Lines 的路径"),
  samplesGlob: z.string().optional().describe("样本文件通配符，要配 bind"),
  bind: z.string().optional().describe("<节点>.<参数>，samplesGlob 把每个文件绑到它上面"),
};

function workRun(config: Config, prefix: string): string {
  const dir = path.join(
    config.workDir,
    `${prefix}-${new Date().toISOString().replace(/[:.]/g, "-")}-${process.pid}`,
  );
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}

export function registerTools(server: McpServer, config: Config, http: LyFlowHttp): void {
  server.registerTool(
    "list_operators",
    {
      title: "列出算子",
      description:
        "按包、分类、关键词过滤算子，每个只给一行。先用它找到 id，再用 get_operator 看细节。",
      inputSchema: {
        pack: z.string().optional().describe("只看这个算子包，例如 gap"),
        category: z.string().optional().describe("分类前缀，例如 滤波 或 gap"),
        query: z.string().optional().describe("在 id / label / keywords / doc 上做子串匹配"),
      },
    },
    async (args) => {
      try {
        const bundle = await http.manifest();
        const rows = bundle.operators
          .filter((op) => (args.pack ? op.pack === args.pack : true))
          .filter((op) => (args.category ? (op.category ?? "").startsWith(args.category) : true))
          .filter((op) =>
            args.query
              ? matches(args.query, [op.id, op.label, op.doc, ...(op.keywords ?? [])])
              : true,
          )
          .map((op) => {
            const row: Record<string, unknown> = {
              id: op.id,
              label: op.label,
              category: op.category,
              doc: firstSentence(op.doc),
            };
            if (op.pack) row["pack"] = op.pack;
            return row;
          });
        return ok({ count: rows.length, total: bundle.operators.length, operators: rows });
      } catch (e) {
        return failed(e);
      }
    },
  );

  server.registerTool(
    "get_operator",
    {
      title: "看一个算子",
      description:
        "算子的全量描述，含 preconditions（什么时候它整个不该用）。找不到时给最接近的几个 id。",
      inputSchema: { id: z.string().describe("算子 id，例如 gap.notch_width") },
    },
    async (args) => {
      try {
        const bundle = await http.manifest();
        const op =
          bundle.operators.find((o) => o.id === args.id) ??
          bundle.operators.find((o) => (o.aliases ?? []).includes(args.id));
        if (!op) {
          return bad(`没有算子 ${args.id}`, {
            nearest: nearest(
              args.id,
              bundle.operators.map((o) => o.id),
              3,
            ),
          });
        }
        return ok(op);
      } catch (e) {
        return failed(e);
      }
    },
  );

  server.registerTool(
    "list_port_types",
    {
      title: "列出端口类型",
      description: "端口类型表：名字、颜色、可隐式转换到哪些类型、一句说明。",
      inputSchema: {},
    },
    async () => {
      try {
        const bundle = await http.manifest();
        return ok({ types: bundle.types });
      } catch (e) {
        return failed(e);
      }
    },
  );

  server.registerTool(
    "validate_graph",
    {
      title: "校验图",
      description: "图合法时返回空数组，否则原样返回诊断。跑之前先校验。",
      inputSchema: graphInput,
    },
    async (args) => {
      try {
        const g = resolveGraph(args);
        const diagnostics = await http.validate({ doc: g.doc, graphPath: g.graphPath });
        return ok({ diagnostics, ok: Array.isArray(diagnostics) && diagnostics.length === 0 });
      } catch (e) {
        return failed(e);
      }
    },
  );

  server.registerTool(
    "plan_graph",
    {
      title: "编译计划",
      description: "每个节点的 cacheKey、缓存命中、层级。校验没过时返回的是诊断数组。",
      inputSchema: {
        ...graphInput,
        targets: z.array(z.string()).optional().describe("只编译到这些节点的上游闭包"),
      },
    },
    async (args) => {
      try {
        const g = resolveGraph(args);
        const plan = await http.plan({
          doc: g.doc,
          graphPath: g.graphPath,
          targets: args.targets ?? null,
        });
        return ok({ plan });
      } catch (e) {
        return failed(e);
      }
    },
  );

  server.registerTool(
    "run_graph",
    {
      title: "跑一次图",
      description:
        "跑完整张图并等 run_finished。返回图级命名输出、每个节点的状态与耗时、诊断。" +
        "不返回点云 —— 要看点云走 summarize_output。",
      inputSchema: {
        ...graphInput,
        targets: z.array(z.string()).optional().describe("只跑到这些节点"),
        set: z
          .record(z.unknown())
          .optional()
          .describe('发送前改节点参数，键是 "<节点>.<参数>"，语义与 CLI --set 相同'),
        mode: z.enum(["full", "preview"]).optional().describe("preview 下源算子先抽稀"),
        timeoutMs: z.number().int().positive().optional().describe(`等 run_finished 的上限，默认 ${DEFAULT_RUN_TIMEOUT_MS}`),
      },
    },
    async (args) => {
      let graph;
      try {
        graph = resolveGraph(args);
      } catch (e) {
        return failed(e);
      }
      try {
        const result = await http.runAndWait(
          {
            doc: graph.doc,
            graphPath: graph.graphPath,
            targets: args.targets ?? null,
            mode: args.mode ?? "full",
            previewMaxPoints: null,
            previewBudgetMs: null,
            sceneId: null,
          },
          args.timeoutMs ?? DEFAULT_RUN_TIMEOUT_MS,
        );
        const summary = summarizeRun(result.events);
        let outputs: unknown = {};
        try {
          outputs = summarizeOutputs(await http.runOutputs(result.runId));
        } catch {
          outputs = {};
        }
        return ok({
          runId: result.runId,
          status: result.timedOut ? "timeout" : summary.status,
          durationMs: summary.durationMs,
          outputs,
          nodes: summary.nodes,
          diagnostics: summary.diagnostics,
        });
      } catch (e) {
        if (e instanceof HttpError && e.status === 400) {
          const diagnostics = parseDiagnostics(e.body) ?? parseDiagnostics(e.message);
          return ok({
            runId: null,
            status: "invalid",
            durationMs: null,
            outputs: {},
            nodes: [],
            diagnostics: diagnostics ?? [{ message: e.message }],
          });
        }
        return failed(e);
      }
    },
  );

  server.registerTool(
    "get_node_outputs",
    {
      title: "看一个节点的输出端口",
      description: "某次运行里该节点每个输出端口的类型、元素数、字节数与非点云的值。",
      inputSchema: {
        runId: z.string(),
        nodeId: z.string(),
      },
    },
    async (args) => {
      try {
        return ok({ outputs: await http.nodeOutputs(args.runId, args.nodeId) });
      } catch (e) {
        return failed(e);
      }
    },
  );

  server.registerTool(
    "summarize_output",
    {
      title: "把一个输出压成统计量",
      description:
        "点云给点数、包围盒、每通道 min/max/mean 与前几个点；张量给形状与统计量；" +
        "其余类型原样给值。统计在 MCP 进程里算，不搬点云给调用方。",
      inputSchema: {
        runId: z.string(),
        nodeId: z.string(),
        port: z.string(),
        maxPoints: z
          .number()
          .int()
          .nonnegative()
          .optional()
          .describe(`取点云时的抽稀上限，0 表示不抽稀，默认 ${DEFAULT_MAX_POINTS}`),
        head: z.number().int().nonnegative().optional().describe(`前几个值，默认 ${DEFAULT_HEAD}`),
      },
    },
    async (args) => {
      try {
        const infos = await http.nodeOutputs(args.runId, args.nodeId);
        const info = infos.find((o) => o.port === args.port);
        if (!info) {
          return bad(`节点 ${args.nodeId} 上没有输出端口 ${args.port}`, {
            ports: infos.map((o) => o.port),
          });
        }
        const head = args.head ?? DEFAULT_HEAD;
        const base = {
          runId: args.runId,
          nodeId: args.nodeId,
          port: args.port,
          type: info.type,
          elementCount: info.elementCount ?? null,
        };
        if (info.type === "PointCloud") {
          const maxPoints = args.maxPoints ?? DEFAULT_MAX_POINTS;
          const buffer = await http.cloud(args.runId, args.nodeId, args.port, maxPoints);
          const summary = summarizeCloud(decodeCloud(buffer), head);
          return ok({ ...base, maxPoints, ...summary });
        }
        return ok({ ...base, ...nonCloudSummary(info) });
      } catch (e) {
        return failed(e);
      }
    },
  );

  server.registerTool(
    "eval",
    {
      title: "样本集 × 参数组 → 指标 → 统计",
      description:
        "起本地 lyflow eval。只回统计与失败清单，逐行的 eval_row 落盘给路径。" +
        "读 summary 先看 ok/n 与 failCodes，再看 std。",
      inputSchema: {
        graphPath: z.string().describe("图文件路径，CLI 直接读它"),
        ...cliSamples,
        params: z
          .array(z.record(z.unknown()))
          .optional()
          .describe('显式参数组列表，例如 [{"n_fit.distThresh":0.1}]'),
        param: z
          .array(z.string())
          .optional()
          .describe("轴展开，<节点>.<参数>=<起>:<止>:<档数>"),
        metric: z.array(z.string()).min(1).describe("值路径，例如 outputs.gap"),
        holdout: z.string().optional().describe("<tag>=<value>，带这个标签的样本进 holdout 组"),
        groupBy: z.string().optional().describe("按这个标签键分组报数"),
        baseDir: z.string().optional(),
        set: z.array(z.string()).optional().describe("<节点>.<参数>=<json>，与 CLI --set 相同"),
        noCache: z.boolean().optional(),
      },
    },
    async (args) => {
      if (!config.cli) return bad(CLI_MISSING);
      let dir: string;
      let argv: string[];
      try {
        dir = workRun(config, "eval");
        let paramsFile: string | null = null;
        if (args.params && args.params.length > 0) {
          paramsFile = path.join(dir, "paramsets.json");
          fs.writeFileSync(paramsFile, JSON.stringify(args.params), "utf8");
        }
        argv = evalArgv(args, paramsFile);
      } catch (e) {
        return failed(e);
      }

      const result = await runCli(config, argv);
      const rowsPath = path.join(dir, "rows.jsonl");
      const rows = result.lines.filter((l) => l.value["kind"] === "eval_row");
      fs.writeFileSync(rowsPath, rows.map((l) => l.text).join("\n") + (rows.length ? "\n" : ""), "utf8");

      const summaries = result.lines
        .filter((l) => l.value["kind"] === "eval_summary")
        .map((l) => l.value);
      const failures = rows
        .filter((l) => l.value["status"] !== "ok")
        .map((l) => ({
          sample: l.value["sample"],
          paramSet: l.value["paramSet"],
          status: l.value["status"],
          errors: l.value["errors"],
        }));

      return ok({
        exitCode: result.code,
        timedOut: result.timedOut,
        argv,
        summaries,
        rowCount: rows.length,
        rowsPath,
        failureCount: failures.length,
        failures: failures.slice(0, MAX_FAILURES),
        failuresTruncated: failures.length > MAX_FAILURES,
        stderrTail: stderrTail(result.stderr),
        ...(result.code === 4 || result.spawnError !== null ? { stderr: result.stderr } : {}),
      });
    },
  );

  server.registerTool(
    "perturb",
    {
      title: "合成位移测灵敏度",
      description:
        "起本地 lyflow perturb：在某个端口后插 edit.translate_region，对位移做轴扫描，" +
        "报 d(指标)/d(位移)。std 小不等于测对了缝 —— 调参之前先跑这个。",
      inputSchema: {
        graphPath: z.string(),
        after: z.string().describe("<节点>:<端口>，在它后面插扰动算子"),
        region: z
          .record(z.unknown())
          .describe('几何选区，{"kind":"halfspace","point":[..],"normal":[..]} 或 kind=box'),
        axis: z.string().describe("<x|y|z>=<起>:<止>:<档数>，单位是米；要跨过 0 才抓得到 signFold"),
        ...cliSamples,
        metric: z.array(z.string()).min(1),
        expect: z.number().optional().describe("期望斜率。点云是米、Measurement 是毫米，所以常写 1000"),
        tolerance: z.number().optional(),
        baseDir: z.string().optional(),
        set: z.array(z.string()).optional(),
      },
    },
    async (args) => {
      if (!config.cli) return bad(CLI_MISSING);
      let dir: string;
      let argv: string[];
      try {
        dir = workRun(config, "perturb");
        argv = perturbArgv(args);
      } catch (e) {
        return failed(e);
      }

      const result = await runCli(config, argv);
      const rowsPath = path.join(dir, "rows.jsonl");
      fs.writeFileSync(
        rowsPath,
        result.lines.map((l) => l.text).join("\n") + (result.lines.length ? "\n" : ""),
        "utf8",
      );

      const summaries = result.lines
        .filter((l) => l.value["kind"] === "perturb_summary")
        .map((l) => l.value);
      const samples = result.lines.filter((l) => l.value["kind"] === "perturb_sample");
      const failures = samples.filter((l) => l.value["pass"] === false).map((l) => l.value);

      return ok({
        exitCode: result.code,
        timedOut: result.timedOut,
        argv,
        summaries,
        sampleCount: samples.length,
        rowsPath,
        failureCount: failures.length,
        failures: failures.slice(0, MAX_FAILURES),
        failuresTruncated: failures.length > MAX_FAILURES,
        stderrTail: stderrTail(result.stderr),
        ...(result.code === 4 || result.spawnError !== null ? { stderr: result.stderr } : {}),
      });
    },
  );

  server.registerTool(
    "diff_graphs",
    {
      title: "比两张图",
      description: "lyflow diff --json 的原样输出：加了哪些节点、删了哪些、参数改了什么。",
      inputSchema: { a: z.string(), b: z.string() },
    },
    async (args) => {
      if (!config.cli) return bad(CLI_MISSING);
      const result = await runCli(config, ["diff", args.a, args.b, "--json"], {
        timeoutMs: Math.min(DEFAULT_CLI_TIMEOUT_MS, 120000),
      });
      const last = result.lines.length > 0 ? result.lines[result.lines.length - 1] : undefined;
      if (!last) {
        return bad(`lyflow diff 没有输出（退出码 ${result.code}）`, { stderr: result.stderr });
      }
      return ok({ exitCode: result.code, diff: last.value });
    },
  );
}

const CLI_MISSING =
  "没有配置 LYFLOW_CLI。eval / perturb / diff_graphs 起的是本地 lyflow 可执行文件，" +
  "把它的路径放进 MCP 服务的环境变量 LYFLOW_CLI 再试。";

function nonCloudSummary(info: OutputInfo): Record<string, unknown> {
  const value = info.value;
  if (info.type === "Tensor") {
    return {
      kind: "tensor",
      shape: value?.shape ?? null,
      count: value?.count ?? info.elementCount ?? null,
      min: value?.min ?? null,
      max: value?.max ?? null,
      mean: value?.mean ?? null,
      note: "张量数据不进 IPC，只有形状与统计量（见 execution-event schema）",
    };
  }
  if (info.type === "Indices") {
    return {
      kind: "indices",
      count: info.elementCount ?? null,
      note: "这一版 HTTP 契约只有点云的二进制端点，下标取不到逐个值",
    };
  }
  return { kind: "value", value: value ?? null };
}
