import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import readline from "node:readline";
import type { Config } from "./config.js";
import { evalArgv, paramsArgv, patchArgv, samplesArgv, type EvalInput } from "./argv.js";
import { runCli, type JsonLine } from "./cli.js";
import { readGraphDoc } from "./graph.js";
import { digest } from "./inspect.js";
import { object, projectValue } from "./summary.js";
import { ArtifactStore, type ArtifactRef } from "./artifacts.js";
import type { ManifestBundle } from "./types.js";

export interface SplitSpec {
  groupTag: string;
  seed: string;
  train: number;
  validation: number;
  holdout: number;
}
export interface EvaluationInput extends EvalInput {
  samples?: Record<string, unknown>[] | undefined;
  split?: SplitSpec | undefined;
  partition?: "all" | "train" | "validation" | "holdout" | undefined;
  timeoutMs?: number | undefined;
  cachePolicy?: "cold" | "warm" | undefined;
}
interface FileIdentity { path: string; bytes: number; sha256: string }
export interface Snapshot {
  id: string;
  createdAt: string;
  sourceGraph: string;
  sourceOverrides: { set: string[]; graphParams: Record<string,unknown>; recipe: string | null };
  graphPath: string;
  baseDir: string;
  graphHash: string;
  datasetHash: string;
  split: SplitSpec | null;
  splitAssignment: Record<string, string>;
  partition: string;
  build: Record<string, unknown>;
  manifest: ManifestBundle;
  manifestHash: string;
  dependencies: FileIdentity[];
  effectiveParams: Record<string, unknown>[];
  args: EvaluationInput;
  sampleCount: number;
  paramSets: Record<string, unknown>[];
  snapshotFiles: FileIdentity[];
  cachePolicy: "cold" | "warm";
}
export interface EvaluationState {
  evaluationId: string;
  status: "prepared" | "running" | "complete" | "cancelled" | "timeout" | "interrupted" | "failed" | "budget_exhausted";
  snapshotHash: string;
  rowCount: number;
  attempts: number;
  startedRuns: number;
  elapsedMs: number;
  runBudget: number;
  timeBudgetMs: number;
  startedAt?: string;
  finishedAt?: string;
  exitCode?: number;
  argv?: string[];
  summaries?: Record<string, unknown>[];
  qualitySummaries?: Record<string, unknown>[];
  stderr?: string;
  integrity?: string[];
  rowsArtifact?: ArtifactRef;
  snapshotArtifact?: ArtifactRef;
  summariesArtifact?: ArtifactRef;
}

export function fileIdentity(file: string): FileIdentity {
  const resolved = fs.realpathSync(file);
  const hash = crypto.createHash("sha256");
  const fd = fs.openSync(resolved, "r"), buf = Buffer.alloc(128 * 1024);
  try { for (let n; (n = fs.readSync(fd, buf)) > 0;) hash.update(buf.subarray(0, n)); }
  finally { fs.closeSync(fd); }
  return { path: resolved, bytes: fs.statSync(resolved).size, sha256: hash.digest("hex") };
}
function save(file: string, value: unknown): void {
  const temp = `${file}.${crypto.randomUUID()}.tmp`;
  fs.writeFileSync(temp, JSON.stringify(value)); fs.renameSync(temp, file);
}
function cliIdentity(executable: string): FileIdentity {
  const locations=[path.resolve(executable),...(path.basename(executable)===executable ? (process.env["PATH"] ?? "").split(path.delimiter).map((d)=>path.join(d,executable)) : [])];
  const suffixes=process.platform==="win32"&&!path.extname(executable) ? ["",...(process.env["PATHEXT"] ?? ".EXE").split(";")] : [""];
  for(const candidate of locations)for(const suffix of suffixes) {
    const file=candidate+suffix;if(fs.existsSync(file)&&fs.statSync(file).isFile())return fileIdentity(file);
  }
  throw new Error("不能定位 CLI 可执行文件，LYFLOW_CLI 请给完整路径");
}
function checkedId(id: string): string {
  if (!/^[0-9a-f]{8}-[0-9a-f-]{27}$/.test(id)) throw new Error("无效的 evaluationId");
  return id;
}
async function cli(config: Config, argv: string[], signal?: AbortSignal): Promise<Record<string, unknown>[]> {
  const result = await runCli(config, argv, { signal });
  if (result.code !== 0) throw new Error(`lyflow ${argv[0]} 退出 ${result.code}: ${result.stderr}`);
  return result.lines.map((l) => l.value);
}

/** A group stays in one partition. Hash order is stable and independent of sample order. */
export function freezeSplits(samples: Record<string, unknown>[], spec?: SplitSpec): Record<string, string> {
  const ids = new Set<string>(), assignment: Record<string, string> = {};
  for (const s of samples) {
    if (typeof s["id"] !== "string" || !s["id"] || ids.has(s["id"])) throw new Error("样本 id 必须非空且唯一");
    ids.add(s["id"]);
  }
  if (!spec) return assignment;
  const { train, validation, holdout } = spec;
  if (![train, validation, holdout].every((v) => Number.isFinite(v) && v >= 0) || Math.abs(train + validation + holdout - 1) > 1e-9)
    throw new Error("split 的 train + validation + holdout 必须等于 1");
  const groups = [...new Set(samples.map((s) => {
    const group = object(s["tags"])[spec.groupTag];
    if (!["string", "number", "boolean"].includes(typeof group)) throw new Error(`样本 ${s["id"]} 缺分组标签 ${spec.groupTag}`);
    return String(group);
  }))].sort((a, b) => digest([spec.seed, a]).localeCompare(digest([spec.seed, b])));
  const fractions=[train,validation,holdout], counts=fractions.map((f)=>Math.floor(groups.length*f));
  if(groups.length < fractions.filter((f)=>f>0).length) throw new Error("工件组数不足，无法划分所有非零 partition");
  while(counts.reduce((a,b)=>a+b,0)<groups.length){const i=fractions.map((f,i)=>({i,remainder:groups.length*f-counts[i]!})).sort((a,b)=>b.remainder-a.remainder||a.i-b.i)[0]!.i;counts[i]=counts[i]!+1;}
  fractions.forEach((f,i)=>{if(f>0&&counts[i]===0){const donor=counts.map((n,i)=>({n,i})).sort((a,b)=>b.n-a.n)[0]!;counts[donor.i]=counts[donor.i]!-1;counts[i]=counts[i]!+1;}});
  const nTrain=counts[0]!,nValidation=counts[1]!;
  groups.forEach((g, i) => { assignment[g] = i < nTrain ? "train" : i < nTrain + nValidation ? "validation" : "holdout"; });
  for (const s of samples) s["tags"] = { ...object(s["tags"]), split: assignment[String(object(s["tags"])[spec.groupTag])] };
  return assignment;
}
function expandAxes(axes: string[] | undefined): Record<string, unknown>[] {
  let groups: Record<string, unknown>[] = [{}];
  for (const axis of axes ?? []) {
    const m = /^(.+\.[^.=]+)=(-?[\d.eE+]+):(-?[\d.eE+]+):(\d+)$/.exec(axis);
    if (!m) throw new Error(`无法冻结扫描轴 ${axis}，写成 node.param=start:end:count`);
    const a = Number(m[2]), b = Number(m[3]), n = Number(m[4]);
    if (!Number.isFinite(a) || !Number.isFinite(b) || n < 1 || n > 1000) throw new Error("轴范围或档数无效（1–1000）");
    if (groups.length * n > 10000) throw new Error("候选组合超过 10000；请缩小搜索空间");
    groups = groups.flatMap((g) => Array.from({ length: n }, (_, i) => ({ ...g, [m[1]!]: n === 1 ? a : a + (b - a) * i / (n - 1) })));
  }
  return groups;
}
export async function readRows(file: string, visit: (row: Record<string, unknown>) => void): Promise<void> {
  if (!fs.existsSync(file)) return;
  const lines = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
  for await (const line of lines) if (line.trim()) {
    const row = object(JSON.parse(line)); if (row["kind"] === "eval_row") visit(row);
  }
}
export function rowFailed(r: Record<string, unknown>): boolean {
  const q = object(object(r["quality"])["metrics"]);
  return r["status"] !== "ok" || ["poseFail", "poseMissing", "measurementMissing", "defectFn", "defectFp", "productFalseAccept", "productFalseReject", "productMissing", "breakOutputMissing"].some((k) => typeof q[k] === "number" && q[k] > 0)
    || ["measurementWithinTolerance", "breakWithinTolerance"].some((k) => typeof q[k] === "number" && q[k] < 1);
}

export class EvaluationStore {
  readonly root: string;
  readonly active = new Map<string, AbortController>();
  constructor(readonly config: Config, readonly artifacts: ArtifactStore) { this.root = path.resolve(config.workDir, "evaluations"); }
  dir(id: string): string { return path.join(this.root, checkedId(id)); }
  load(id: string): { snapshot: Snapshot; state: EvaluationState } {
    const dir = this.dir(id);
    const snapshot = JSON.parse(fs.readFileSync(path.join(dir, "snapshot.json"), "utf8")) as Snapshot;
    const state = JSON.parse(fs.readFileSync(path.join(dir, "state.json"), "utf8")) as EvaluationState;
    if (digest(snapshot) !== state.snapshotHash) throw new Error("评估快照已改变；拒绝续跑/比较/导出");
    if (state.status === "running" && !this.active.has(id)) { state.status = "interrupted"; save(path.join(dir, "state.json"), state); }
    return { snapshot, state };
  }
  async prepare(input: EvaluationInput, signal?: AbortSignal, varyBaseline = false): Promise<string> {
    if (!this.config.cli) throw new Error("评估需要 LYFLOW_CLI");
    if (input.samples && [input.samplesPath, input.samplesGlob, input.samplesDir].some(Boolean)) throw new Error("samples 与样本文件选择只能给一个");
    const id = crypto.randomUUID(), dir = this.dir(id); fs.mkdirSync(dir, { recursive: true });
    const sourceGraph = path.resolve(input.graphPath), baseDir = path.resolve(input.baseDir ?? path.dirname(sourceGraph));
    const graphPath = path.join(dir, "graph.lyflow.json");
    fs.copyFileSync(sourceGraph, graphPath);
    fs.copyFileSync(sourceGraph,path.join(dir,"source.lyflow.json"));
    let recipe: string | undefined;
    if (input.recipe) { recipe = path.join(dir, "recipe.lyflow-recipe.json"); fs.copyFileSync(input.recipe, recipe); }
    if (recipe || input.set?.length || Object.keys(input.graphParams ?? {}).length) {
      await cli(this.config, patchArgv({ graphPath, baseDir, set: input.set, graphParams: input.graphParams, recipe, dryRun: false }), signal);
    }
    const graph = readGraphDoc(graphPath);
    const info = (await cli(this.config, ["info"], signal)).find((v) => v["kind"] === "core_info");
    if (!info || !info["buildFingerprint"]) throw new Error("CLI 缺构建身份，请使用带 info 的版本");
    const build={...info,cliExecutable:cliIdentity(this.config.cli)};
    const manifest = (await cli(this.config, ["manifest"], signal))[0] as unknown as ManifestBundle;
    if (!Array.isArray(manifest?.operators)) throw new Error("CLI manifest 不合法");
    const effective = await cli(this.config, paramsArgv({ graphPath, baseDir }), signal);
    const samples = input.samples ? structuredClone(input.samples) : input.samplesPath
      ? fs.readFileSync(input.samplesPath, "utf8").split(/\r?\n/).filter((s) => s.trim()).map((s) => object(JSON.parse(s)))
      : input.samplesGlob || input.samplesDir ? await cli(this.config, ["samples", ...samplesArgv(input)], signal) : [{ id: "-", set: {} }];
    if (!samples.length) throw new Error("样本集为空");
    const splitAssignment = freezeSplits(samples, input.split);
    const datasetHash = digest(samples);
    const selected = samples.filter((s) => !input.partition || input.partition === "all" || object(s["tags"])["split"] === input.partition);
    if (!selected.length) throw new Error(`划分 ${input.partition} 没有样本；检查工件组数和比例`);
    const samplesPath = path.join(dir, "samples.jsonl");
    fs.writeFileSync(samplesPath, selected.map((s) => JSON.stringify(s)).join("\n") + "\n");
    fs.writeFileSync(path.join(dir, "dataset.json"), JSON.stringify(samples));
    fs.writeFileSync(path.join(dir,"dataset.jsonl"),samples.map((s) => JSON.stringify(s)).join("\n")+"\n");
    const axes = expandAxes(input.param), explicit = input.params?.length ? input.params : [{}];
    const paramSets = explicit.flatMap((p) => axes.map((a) => ({ ...p, ...a })));
    const paramsPath = path.join(dir, "paramsets.json"); fs.writeFileSync(paramsPath, JSON.stringify(paramSets));
    const pathKeys = new Set(effective.filter((row) => {
      const op = manifest.operators.find((o) => o.id === row["op"]);
      return object(op?.params?.find((p) => object(p)["name"] === row["param"]))["type"] === "path";
    }).flatMap((r) => [`${r["node"]}.${r["param"]}`, ...(r["graphParam"] ? [String(r["graphParam"])] : [])]));
    const files = new Set<string>();
    const add = (key: string, value: unknown) => { if (pathKeys.has(key) && typeof value === "string" && value) files.add(path.resolve(baseDir, value)); };
    effective.forEach((r) => add(`${r["node"]}.${r["param"]}`, r["value"]));
    samples.forEach((s) => { Object.entries(object(s["set"])).forEach(([k,v]) => add(k,v)); Object.entries(object(s["graphParams"])).forEach(([k,v]) => add(k,v)); });
    paramSets.forEach((p) => Object.entries(p).forEach(([k,v]) => add(k,v)));
    // Library graph definitions can change independently of the compiled core.
    const walk = (root: string) => { if (!fs.existsSync(root)) return; for (const e of fs.readdirSync(root, { withFileTypes: true })) {
      const f = path.join(root,e.name); if (e.isDirectory() && !e.isSymbolicLink()) walk(f); else if (e.isFile() && (e.name.endsWith(".lyflow-op.json") || e.name.endsWith(".lyflow.json"))) files.add(f);
    } };
    if (Array.isArray(info["libraryDirs"])) info["libraryDirs"].forEach((d) => walk(String(d)));
    const dependencies = [...files].map(fileIdentity);
    const cachePolicy = input.cachePolicy ?? (input.noCache === false ? "warm" : "cold");
    const args: EvaluationInput = { ...input, graphPath, baseDir, samplesPath, params: paramSets,
      param: undefined, recipe: undefined, set: undefined, samples: undefined, samplesGlob: undefined, samplesDir: undefined,
      bind: undefined, bindPair: undefined, pattern: undefined, sampleSubdir: undefined, sortBy: undefined, splitHalf: undefined,
      noCache: cachePolicy === "cold", groupBy: input.groupBy ?? (input.split ? "split" : undefined) };
    if (varyBaseline) args.graphParams = Object.fromEntries(Object.entries(input.graphParams ?? {}).filter(([k]) => !paramSets.some((p) => Object.hasOwn(p,k))));
    const frozenFiles = [graphPath, path.join(dir,"source.lyflow.json"), samplesPath, paramsPath, path.join(dir,"dataset.json"), path.join(dir,"dataset.jsonl"), ...(recipe ? [recipe] : [])];
    const snapshot: Snapshot = { id, createdAt: new Date().toISOString(), sourceGraph,
      sourceOverrides:{set:input.set ?? [],graphParams:input.graphParams ?? {},recipe:input.recipe ?? null},graphPath, baseDir,
      graphHash: digest(graph), datasetHash, split: input.split ?? null, splitAssignment, partition: input.partition ?? "all",
      build, manifest, manifestHash: digest(manifest), dependencies, effectiveParams: effective, args,
      sampleCount: selected.length, paramSets, snapshotFiles: frozenFiles.map(fileIdentity), cachePolicy };
    save(path.join(dir,"snapshot.json"), snapshot);
    save(path.join(dir,"state.json"), { evaluationId:id,status:"prepared",snapshotHash:digest(snapshot),rowCount:0,attempts:0,
      startedRuns:0,elapsedMs:0,runBudget:input.maxRuns ?? selected.length*paramSets.length,timeBudgetMs:input.timeoutMs ?? 600000,
      snapshotArtifact:this.artifacts.put(snapshot) } satisfies EvaluationState);
    return id;
  }
  async verify(snapshot: Snapshot): Promise<string[]> {
    const changed: string[] = [];
    for (const f of [...snapshot.snapshotFiles, ...snapshot.dependencies]) {
      try { if (fileIdentity(f.path).sha256 !== f.sha256) changed.push(f.path); } catch { changed.push(f.path); }
    }
    try {
      const frozenCli=object(snapshot.build["cliExecutable"])["sha256"];
      if(!this.config.cli || typeof frozenCli!=="string" || cliIdentity(this.config.cli).sha256!==frozenCli)changed.push("cli.executable");
      const info = (await cli(this.config,["info"])).find((v) => v["kind"] === "core_info");
      if (info?.["buildFingerprint"] !== snapshot.build["buildFingerprint"]) changed.push("core.buildFingerprint");
      const manifest = (await cli(this.config,["manifest"]))[0];
      if (digest(manifest) !== snapshot.manifestHash) changed.push("core.manifest");
    } catch (e) { changed.push(`build: ${String(e)}`); }
    return changed;
  }
  async execute(id: string, signal?: AbortSignal, onLine?: (line: JsonLine) => void, grant: { additionalRuns?: number | undefined; additionalMs?: number | undefined } = {}): Promise<EvaluationState> {
    if (this.active.has(id)) throw new Error("评估已经在运行");
    const { snapshot, state } = this.load(id), dir = this.dir(id);
    if (state.status === "complete") throw new Error("评估已完成；新运行请创建新的 evaluationId");
    state.runBudget += grant.additionalRuns ?? 0; state.timeBudgetMs += grant.additionalMs ?? 0;
    const maxRuns = state.runBudget - state.startedRuns, timeoutMs = state.timeBudgetMs - state.elapsedMs;
    if (maxRuns <= 0 || timeoutMs <= 0) throw new Error("运行次数或时长预算已用尽；resume_job 可显式追加 additionalRuns / additionalMs");
    const integrity = await this.verify(snapshot); if (integrity.length) throw new Error(`冻结输入/构建已改变: ${integrity.join(", ")}`);
    const abort = new AbortController(); this.active.set(id,abort);
    const cancel = () => abort.abort(); signal?.addEventListener("abort",cancel,{ once:true }); if (signal?.aborted) cancel();
    state.status = "running"; state.attempts += 1; state.startedAt = new Date().toISOString();
    save(path.join(dir,"state.json"),state);
    const rowsPath = path.join(dir,"rows.jsonl"), previous = path.join(dir,`resume-${state.attempts}.jsonl`);
    const seen = new Set<string>();
    if (fs.existsSync(rowsPath)) {
      fs.writeFileSync(previous, "");
      await readRows(rowsPath,(r) => { if (r["status"] !== "cancelled") {
        const key = `${r["paramSet"]}:${r["sample"]}`;
        if (!seen.has(key)) { seen.add(key); fs.appendFileSync(previous,`${JSON.stringify(r)}\n`); }
      } });
      fs.copyFileSync(previous, rowsPath);
    }
    const attemptPath = path.join(dir,`attempt-${state.attempts}.jsonl`);
    const argv = evalArgv({ ...snapshot.args, progressJson:true, maxRuns, resumeRows: seen.size ? previous : undefined }, path.join(dir,"paramsets.json"));
    state.argv = argv; state.rowCount = seen.size;
    const started = Date.now();
    try {
      const result = await runCli(this.config,argv,{ signal:abort.signal, timeoutMs, outputPath:attemptPath, retainRows:false,
        onLine: (line) => {
          if (line.value["kind"] === "eval_started") { state.startedRuns += 1; save(path.join(dir,"state.json"),state); }
          if (line.value["kind"] === "eval_row") {
            const key = `${line.value["paramSet"]}:${line.value["sample"]}`;
            if (seen.has(key)) throw new Error("CLI 重复交出同一候选/样本组合");
            seen.add(key);
            fs.appendFileSync(rowsPath,`${line.text}\n`);
            state.rowCount += 1; save(path.join(dir,"state.json"),state);
          }
          onLine?.(line);
        } });
      state.rowCount = seen.size;
      if (!fs.existsSync(rowsPath)) fs.writeFileSync(rowsPath,"");
      state.status = result.cancelled ? "cancelled" : result.timedOut ? "timeout"
        : result.lines.some((l) => l.value["kind"] === "eval_budget") ? "budget_exhausted"
        : result.code === 4 || result.spawnError || state.rowCount < snapshot.sampleCount * snapshot.paramSets.length ? "failed" : "complete";
      state.exitCode = result.code; state.stderr = result.stderr;
      state.summaries = result.lines.filter((l) => l.value["kind"] === "eval_summary").map((l) => l.value);
      state.qualitySummaries = result.lines.filter((l) => l.value["kind"] === "quality_summary").map((l) => l.value);
      state.summariesArtifact=this.artifacts.put({summaries:state.summaries,qualitySummaries:state.qualitySummaries});
      state.integrity = await this.verify(snapshot); if (state.integrity.length) state.status = "failed";
      state.rowsArtifact = this.artifacts.register(rowsPath,"jsonl");
    } catch (e) { state.status = "failed"; state.stderr = String(e); }
    finally { state.elapsedMs += Date.now() - started; state.finishedAt = new Date().toISOString(); this.active.delete(id); signal?.removeEventListener("abort",cancel); save(path.join(dir,"state.json"),state); }
    return state;
  }
  cancel(id: string): boolean { const abort = this.active.get(id); abort?.abort(); return !!abort; }
  async describe(id: string, options: { offset?: number | undefined; limit?: number | undefined; failuresOnly?: boolean | undefined; sample?: string | undefined; paramSet?: number | undefined } = {}): Promise<Record<string,unknown>> {
    const {snapshot,state} = this.load(id); const rows: Record<string,unknown>[] = []; let total = 0;
    const offset = options.offset ?? 0, limit = options.limit ?? 10;
    const dataset = limit>0 ? JSON.parse(fs.readFileSync(path.join(this.dir(id),"dataset.json"),"utf8")) as Record<string,unknown>[] : [];
    await readRows(path.join(this.dir(id),"rows.jsonl"),(r) => {
      if (options.failuresOnly && !rowFailed(r) || options.sample !== undefined && r["sample"] !== options.sample || options.paramSet !== undefined && r["paramSet"] !== options.paramSet) return;
      if (total >= offset && rows.length < limit) {
        const sample = dataset.find((s) => s["id"] === r["sample"]);
        const candidate = snapshot.paramSets[Number(r["paramSet"])] ?? {};
        const set = Object.fromEntries(Object.entries(candidate).filter(([k]) => k.includes(".")));
        const graphParams = Object.fromEntries(Object.entries(candidate).filter(([k]) => !k.includes(".")));
        rows.push({ ...projectValue(r,{limit:8}).value as Record<string,unknown>, replay: { graphPath:snapshot.graphPath,baseDir:snapshot.baseDir,
          set:{...set,...object(sample?.["set"])}, graphParams:{...graphParams,...snapshot.args.graphParams,...object(sample?.["graphParams"])},mode:"full",detail:"compact" } });
      }
      total += 1;
    });
    const elapsedMs=state.elapsedMs+(state.status==="running"&&state.startedAt ? Math.max(0,Date.now()-Date.parse(state.startedAt)) : 0);
    const summaries=(state.summaries ?? []).filter((s)=>options.paramSet===undefined || s["paramSet"]===options.paramSet);
    const qualitySummaries=(state.qualitySummaries ?? []).filter((s)=>options.paramSet===undefined || s["paramSet"]===options.paramSet);
    const boundGroups=(s:Record<string,unknown>)=>{
      const groups=Object.entries(object(s["groups"]));
      return {...s,groups:Object.fromEntries(groups.slice(0,20)),groupCount:groups.length,groupsTruncated:groups.length>20};
    };
    const assignments=Object.entries(snapshot.splitAssignment);
    return { ...state,summaries:summaries.slice(0,100).map(boundGroups),qualitySummaries:qualitySummaries.slice(0,100).map(boundGroups),
      summaryCount:summaries.length,qualitySummaryCount:qualitySummaries.length,summariesTruncated:summaries.length>100 || qualitySummaries.length>100 || [...summaries,...qualitySummaries].some((s)=>Object.keys(object(s["groups"])).length>20),
      elapsedMs, remainingRuns:Math.max(0,state.runBudget-state.startedRuns),remainingMs:Math.max(0,state.timeBudgetMs-elapsedMs),stderr: state.stderr?.slice(-2000), snapshot: { createdAt:snapshot.createdAt,graphHash:snapshot.graphHash,datasetHash:snapshot.datasetHash,
      build:snapshot.build,manifestHash:snapshot.manifestHash,split:snapshot.split,splitAssignment:Object.fromEntries(assignments.slice(0,30)),splitGroupCount:assignments.length,splitAssignmentTruncated:assignments.length>30,partition:snapshot.partition,
      sampleCount:snapshot.sampleCount,candidateCount:snapshot.paramSets.length,cachePolicy:snapshot.cachePolicy },
      rows,total,offset,truncated:total > offset + rows.length, snapshotArtifact:state.snapshotArtifact ?? this.artifacts.put(snapshot) };
  }
}
