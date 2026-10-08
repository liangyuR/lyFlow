import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { EvaluationStore, readRows, fileIdentity, type Snapshot } from "./evaluations.js";
import { object, projectValue } from "./summary.js";
import { patchArgv } from "./argv.js";
import { runCli } from "./cli.js";
import { readGraphDoc } from "./graph.js";
import { digest } from "./inspect.js";

const decisionTruth=z.object({output:z.string().min(1),ok:z.boolean()}).passthrough();
const measurementTruth=z.object({path:z.string().min(1),value:z.number(),unit:z.string().min(1),unitPath:z.string().min(1),tolerance:z.number().nonnegative().optional()}).passthrough();
const pixelPoint=z.tuple([z.number(),z.number()]);
export const sampleSchema=z.object({
  id:z.string().min(1).describe("数据集中唯一的帧 id"),
  set:z.record(z.unknown()).nullable().optional().describe("未绑定节点参数覆盖，如 n_load.path"),
  graphParams:z.record(z.unknown()).optional().describe("逐帧顶层参数，如模板、stations、标定文件"),
  tags:z.record(z.union([z.string(),z.number(),z.boolean()])).nullable().optional().describe("workpiece/batch 等分组标签，多帧同一工件不得跨集合"),
  scene:z.never().optional(),
  truth:z.object({pose:decisionTruth.optional(),verdict:decisionTruth.optional(),measurements:z.array(measurementTruth).optional(),
    breaks:z.object({output:z.string().min(1),coordinate:z.enum(["path_s_px","image_px"]).optional(),lineOutput:z.string().min(1).optional(),
      minIou:z.number().min(0).max(1).optional(),endpointTolerance:z.number().nonnegative().optional(),maxProjectionDistance:z.number().nonnegative().optional(),
      intervals:z.array(z.object({sStart:z.number().optional(),sEnd:z.number().optional(),start:pixelPoint.optional(),end:pixelPoint.optional()}).passthrough())
        .describe("path_s_px 给 sStart<sEnd；image_px 给 start/end 并指定 lineOutput；空数组表示确认无断口")}).passthrough().optional()
  }).strict().optional().describe("缺省表示未标注；路径/单位/区间语义由 CLI 验证和评分")
}).passthrough();

export const splitSchema = z.object({ groupTag:z.string().min(1),seed:z.string().default("lyflow-v1"),
  train:z.number().min(0).max(1).default(0.6),validation:z.number().min(0).max(1).default(0.2),holdout:z.number().min(0).max(1).default(0.2) });
const objectiveSchema = z.object({ metric:z.string(),direction:z.enum(["minimize","maximize"]),stat:z.enum(["mean","p50","p95","min","max"]).default("mean"),weight:z.number().positive().default(1) });
const constraintSchema = z.object({ metric:z.string(),min:z.number().optional(),max:z.number().optional(),stat:z.enum(["mean","p50","p95","min","max"]).default("mean") })
  .refine((c) => (c.min !== undefined || c.max !== undefined) && (c.min === undefined || c.max === undefined || c.min <= c.max),"约束需要 min/max，且 min ≤ max");
export type Objective = z.infer<typeof objectiveSchema>;
export type Constraint = z.infer<typeof constraintSchema>;
const ok = (v:Record<string,unknown>) => ({ content:[{type:"text" as const,text:JSON.stringify(v)}],structuredContent:v });
const fail = (e:unknown) => ({ content:[{type:"text" as const,text:JSON.stringify({error:String(e instanceof Error ? e.message : e)})}],isError:true });

export function parameterRoles(snapshot: Snapshot): Map<string,{role:string;value:unknown;bound?:string}> {
  const out = new Map<string,{role:string;value:unknown;bound?:string}>();
  for (const row of snapshot.effectiveParams) {
    const op = snapshot.manifest.operators.find((o) => o.id === row["op"]);
    const spec = object(op?.params?.find((p) => object(p)["name"] === row["param"]));
    const role = String(spec["tuningRole"] ?? (spec["type"] === "path" ? "input" : "unspecified"));
    const bound = typeof row["graphParam"] === "string" ? row["graphParam"] : undefined;
    out.set(`${row["node"]}.${row["param"]}`,{role,value:row["value"],...(bound ? {bound} : {})});
    if (bound) {
      const previous = out.get(bound);
      const roles = [previous?.role,role];
      out.set(bound,{role:roles.includes("acceptance") ? "acceptance" : roles.includes("input") ? "input" : roles.includes("geometry") ? "geometry" : previous && previous.role !== role ? "unspecified" : role,value:row["value"]});
    }
  }
  return out;
}
export function generateCandidates(space:Record<string,unknown[]>, baseline:Record<string,unknown>, mode:"one_factor"|"grid", maxCandidates=10000):Record<string,unknown>[] {
  if (!Object.keys(space).length || Object.values(space).some((a) => !a.length)) throw new Error("space 需要非空参数值列表");
  let candidates: Record<string,unknown>[] = [{}];
  for (const [key,values] of Object.entries(space)) {
    if (mode === "grid") {
      if (candidates.length * values.length > maxCandidates) throw new Error(`候选数超过 ${maxCandidates}`);
      candidates = candidates.flatMap((c) => values.map((v) => ({...c,[key]:v})));
    } else candidates.push(...values.filter((v) => digest(v) !== digest(baseline[key])).map((v) => ({[key]:v})));
    if (candidates.length > maxCandidates) throw new Error(`候选数超过 ${maxCandidates}`);
  }
  return [...new Map([{},...candidates].map((c) => [digest(c),c])).values()];
}
interface MetricIndex { summaries:unknown;quality:unknown;byMetric:Map<string,Record<string,unknown>>;byQuality:Map<number,Record<string,unknown>> }
const metricIndexes=new WeakMap<Record<string,unknown>,MetricIndex>();
function metricValue(state:Record<string,unknown>, paramSet:number, group:string, metric:string, stat:string):number|null {
  let index=metricIndexes.get(state);
  if(!index || index.summaries!==state["summaries"] || index.quality!==state["qualitySummaries"]) {
    index={summaries:state["summaries"],quality:state["qualitySummaries"],
      byMetric:new Map(((state["summaries"] as Record<string,unknown>[]|undefined) ?? []).map((s)=>[`${s["paramSet"]}:${s["metric"]}`,s])),
      byQuality:new Map(((state["qualitySummaries"] as Record<string,unknown>[]|undefined) ?? []).map((s)=>[Number(s["paramSet"]),s]))};
    metricIndexes.set(state,index);
  }
  const summary = index.byMetric.get(`${paramSet}:${metric}`);
  let value = object(object(summary?.["groups"])[group])[stat];
  if (value === undefined && metric.startsWith("quality.")) {
    const q = index.byQuality.get(paramSet);
    value = object(object(object(object(q?.["groups"])[group])["metrics"])[metric.slice(8)])[stat];
  }
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}
export function assessCandidate(state:Record<string,unknown>, candidate:number, group:string, objectives:Objective[], constraints:Constraint[]):{qualified:boolean;score:number|null;objectives:Record<string,unknown>[];violations:Record<string,unknown>[]} {
  const violated: Record<string,unknown>[] = [];
  if (state["status"] !== "complete") violated.push({reason:"evaluation_incomplete",status:state["status"]});
  const guard:Constraint[] = [{metric:"quality.executionOk",min:1,stat:"mean"},...constraints];
  for (const c of guard) {
    const value = metricValue(state,candidate,group,c.metric,c.stat);
    if (value === null || c.min !== undefined && value < c.min || c.max !== undefined && value > c.max) violated.push({...c,value});
  }
  for (const metric of ["poseMissing","measurementMissing","productMissing","breakOutputMissing"]) {
    const value = metricValue(state,candidate,group,`quality.${metric}`,"max");
    if (value !== null && value > 0) violated.push({metric:`quality.${metric}`,max:0,value,reason:"annotation_output_missing"});
  }
  for(const metric of ["poseFail","defectFn","defectFp","productFalseAccept","productFalseReject"]){
    const explicit=constraints.some((c)=>c.metric===`quality.${metric}`&&c.max!==undefined
      || metric==="defectFn"&&(c.metric==="quality.defectRecall"&&c.min!==undefined || c.metric==="quality.defectMissRate"&&c.max!==undefined)
      || metric==="defectFp"&&c.metric==="quality.defectPrecision"&&c.min!==undefined);
    const value=metricValue(state,candidate,group,`quality.${metric}`,"max");
    if(!explicit&&value!==null&&value>0)violated.push({metric:`quality.${metric}`,max:0,value,reason:"default_annotation_quality_gate"});
  }
  for(const metric of ["measurementWithinTolerance","breakWithinTolerance"]){
    const explicit=constraints.some((c)=>c.metric===`quality.${metric}`&&c.min!==undefined);
    const value=metricValue(state,candidate,group,`quality.${metric}`,"min");
    if(!explicit&&value!==null&&value<1)violated.push({metric:`quality.${metric}`,min:1,value,reason:"default_annotation_tolerance_gate"});
  }
  let score = 0;
  const values = objectives.map((o) => {
    const value = metricValue(state,candidate,group,o.metric,o.stat);
    if (value === null) violated.push({metric:o.metric,reason:"objective_missing"});
    else score += value * o.weight * (o.direction === "maximize" ? 1 : -1);
    return {...o,value};
  });
  return {qualified:violated.length === 0,score:violated.length ? null : score,objectives:values,violations:violated};
}
function lockedSpecs(snapshot:Snapshot, index:number):string {
  const candidate = snapshot.paramSets[index]; if (!candidate) throw new Error("paramSet 不存在");
  return digest([...parameterRoles(snapshot)].filter(([,r]) => r.role === "acceptance").map(([k,r]) => [k,
    r.bound ? snapshot.args.graphParams?.[r.bound] ?? candidate[r.bound] ?? r.value : candidate[k] ?? r.value]));
}
function comparisonMetric(row:Record<string,unknown>, metric:string):number|null {
  const value=object(row["metrics"])[metric] ?? (metric.startsWith("quality.") ? object(object(row["quality"])["metrics"])[metric.slice(8)] : undefined);
  return typeof value==="number" && Number.isFinite(value) ? value : null;
}
/** Match CLI Grouping::name_of, using the CLI's normalized tags and holdout decision. */
function comparisonGroup(snapshot:Snapshot, row:Record<string,unknown>):string {
  const side=snapshot.args.holdout ? row["holdout"]===true ? "holdout" : "train" : null;
  const tag=snapshot.args.groupBy ? String(object(row["tags"])[snapshot.args.groupBy] ?? "(none)") : null;
  return side!==null && tag!==null ? `${side}/${tag}` : side ?? tag ?? "all";
}
export async function compareEvaluations(store:EvaluationStore, a:string,b:string, ai:number,bi:number, objectives:Objective[], constraints:Constraint[], group?:string):Promise<Record<string,unknown>> {
  const before = store.load(a), after = store.load(b);
  const checks = ["datasetHash","manifestHash","cachePolicy"] as const;
  for (const key of checks) if (before.snapshot[key] !== after.snapshot[key]) throw new Error(`评估不可比较: ${key} 不同`);
  const grouping=(s:Snapshot)=>({holdout:s.args.holdout || null,groupBy:s.args.groupBy || null});
  if(digest(grouping(before.snapshot))!==digest(grouping(after.snapshot)))throw new Error("评估不可比较：统计分组规则不同");
  const cliBefore=object(before.snapshot.build["cliExecutable"])["sha256"],cliAfter=object(after.snapshot.build["cliExecutable"])["sha256"];
  if(typeof cliBefore!=="string"||cliBefore!==cliAfter)throw new Error("评估不可比较：CLI 执行或评分实现不同");
  if (before.snapshot.build["buildFingerprint"] !== after.snapshot.build["buildFingerprint"] || digest(before.snapshot.splitAssignment) !== digest(after.snapshot.splitAssignment)) throw new Error("评估不可比较：构建或样本分组不同");
  if (lockedSpecs(before.snapshot,ai) !== lockedSpecs(after.snapshot,bi)) throw new Error("验收规格不同；不能作为检测参数优化结果比较");
  if (before.state.integrity?.length || after.state.integrity?.length) throw new Error("评估期间输入或构建改变，结果不可比较");
  // Input identities must agree even if the paths are identical.
  const deps = (s:Snapshot) => digest(s.dependencies.map((f) => [f.path,f.sha256]).sort(([a],[b]) => String(a).localeCompare(String(b))));
  if (deps(before.snapshot) !== deps(after.snapshot)) throw new Error("输入/模板/标定/库文件哈希不同");
  const selectedGroup = group ?? (after.snapshot.partition !== "all" ? after.snapshot.partition : after.snapshot.split ? "validation" : "all");
  const baseline = assessCandidate(before.state as unknown as Record<string,unknown>,ai,selectedGroup,objectives,constraints);
  const candidate = assessCandidate(after.state as unknown as Record<string,unknown>,bi,selectedGroup,objectives,constraints);
  const map = new Map<string,Record<string,unknown>>();
  await readRows(path.join(store.dir(a),"rows.jsonl"),(r) => { if (r["paramSet"] === ai && comparisonGroup(before.snapshot,r)===selectedGroup) map.set(String(r["sample"]),{metrics:r["metrics"],status:r["status"],errors:r["errors"],quality:projectValue(r["quality"],{limit:8}).value}); });
  const pairs: Record<string,unknown>[] = []; let improved=0,degraded=0,unchanged=0,unpaired=0;
  await readRows(path.join(store.dir(b),"rows.jsonl"),(r) => {
    if (r["paramSet"] !== bi || comparisonGroup(after.snapshot,r)!==selectedGroup) return;
    const old = map.get(String(r["sample"])); if (!old) { unpaired++; return; } map.delete(String(r["sample"]));
    const delta = objectives.map((o) => {
      const x = comparisonMetric(old,o.metric), y = comparisonMetric(r,o.metric);
      return {metric:o.metric,before:x,after:y,delta:x !== null && y !== null ? y-x : null,
        gain:x !== null && y !== null ? (y-x)*o.weight*(o.direction === "maximize" ? 1 : -1) : null};
    });
    const missing = delta.some((d) => d.gain === null), gain = delta.reduce((n,d) => n+(d.gain ?? 0),0);
    const status = missing ? "missing" : gain > 1e-12 ? "improved" : gain < -1e-12 ? "degraded" : "unchanged";
    if (status === "improved") improved++; else if (status === "degraded" || status === "missing") degraded++; else unchanged++;
    pairs.push({sample:r["sample"],status,delta,before:projectValue({status:old["status"],quality:old["quality"],errors:old["errors"]},{limit:8}).value,
      after:projectValue({status:r["status"],quality:r["quality"],errors:r["errors"]},{limit:8}).value});
  });
  unpaired += map.size;
  return {baselineId:a,candidateId:b,baselineParamSet:ai,candidateParamSet:bi,group:selectedGroup,baseline,candidate,improved,degraded,unchanged,unpaired,
    comparable:unpaired===0 && before.state.status === "complete" && after.state.status === "complete",examples:pairs.filter((p) => p["status"] !== "unchanged").slice(0,10),artifact:store.artifacts.put(pairs)};
}

export function registerExperimentTools(server:McpServer,store:EvaluationStore):void {
  const background = new Map<string,{promise:Promise<unknown>;abort:AbortController}>();
  const previousClose = server.server.onclose;
  server.server.onclose = () => {
    previousClose?.();
    for (const job of background.values()) job.abort.abort();
    for (const job of store.active.values()) job.abort();
  };
  const launch = (id:string,grant?:{additionalRuns?:number|undefined;additionalMs?:number|undefined}) => {
    if (background.has(id) || store.active.has(id)) throw new Error("任务已运行");
    const {state}=store.load(id);state.status="prepared";
    fs.writeFileSync(path.join(store.dir(id),"state.json"),JSON.stringify(state));
    const abort=new AbortController();
    const run = store.execute(id,abort.signal,undefined,grant).catch((e:unknown) => {
      const {state} = store.load(id); state.status="failed"; state.stderr=String(e); fs.writeFileSync(path.join(store.dir(id),"state.json"),JSON.stringify(state));
    }).finally(() => background.delete(id)); background.set(id,{promise:run,abort});
  };
  server.registerTool("get_evaluation",{title:"读取评估记录",description:"查询冻结身份、指标、缺失原因与分页样本；每行 replay 可直接交给 run_graph 重现失败。",
    inputSchema:{evaluationId:z.string(),offset:z.number().int().nonnegative().optional(),limit:z.number().int().min(0).max(100).optional(),failuresOnly:z.boolean().optional(),sample:z.string().optional(),paramSet:z.number().int().nonnegative().optional()},annotations:{readOnlyHint:true}},
    async ({evaluationId,...options}) => {try{return ok(await store.describe(evaluationId,options));}catch(e){return fail(e);}});
  server.registerTool("compare_evaluations",{title:"比较基线与候选",description:"同构建、同输入、同分组、同缓存策略、同验收规格才比较。给约束、指标方向和尺度权重，返回改善/退化样本及证据。默认用 validation。",
    inputSchema:{baselineId:z.string(),candidateId:z.string(),baselineParamSet:z.number().int().nonnegative().default(0),candidateParamSet:z.number().int().nonnegative().default(0),objectives:z.array(objectiveSchema).min(1),constraints:z.array(constraintSchema).default([]),group:z.string().optional()},annotations:{readOnlyHint:true}},
    async (a) => {try{return ok(await compareEvaluations(store,a.baselineId,a.candidateId,a.baselineParamSet,a.candidateParamSet,a.objectives,a.constraints,a.group));}catch(e){return fail(e);}});
  server.registerTool("export_candidate",{title:"导出评估过的候选",description:"导出候选图或顶层参数配方与 provenance sidecar，保留 evaluationId、输入与构建哈希。新文件写入，不覆盖已有文件。recipe 不能包含节点覆盖。",
    inputSchema:{evaluationId:z.string(),paramSet:z.number().int().nonnegative().default(0),format:z.enum(["graph","recipe"]).default("graph"),out:z.string().optional()},annotations:{readOnlyHint:false,destructiveHint:false}},
    async (a,extra) => {try{
      const {snapshot,state}=store.load(a.evaluationId); if(state.status!=="complete" || state.integrity?.length) throw new Error("只导出完整且身份一致的评估；先完成验证集评估");
      const changed=await store.verify(snapshot); if(changed.length) throw new Error(`冻结输入已改变: ${changed.join(",")}`);
      const candidate=snapshot.paramSets[a.paramSet]; if(!candidate) throw new Error("paramSet 不存在");
      const dir=path.join(store.dir(a.evaluationId),"exports");fs.mkdirSync(dir,{recursive:true});
      const out=path.resolve(a.out ?? path.join(dir,`${crypto.randomUUID()}.${a.format === "graph" ? "lyflow.json" : "lyflow-recipe.json"}`));
      if(fs.existsSync(out) || fs.existsSync(`${out}.provenance.json`)) throw new Error("导出路径已存在，请选新文件名");
      const values={...Object.fromEntries(Object.entries(candidate).filter(([k])=>!k.includes("."))),...snapshot.args.graphParams};
      if(a.format==="graph") {
        // Even an empty candidate has a valid graph; no-action patch is unnecessary.
        if(!Object.keys(values).length && !Object.keys(candidate).some((k)=>k.includes("."))) fs.copyFileSync(snapshot.graphPath,out);
        else {const argv=patchArgv({graphPath:snapshot.graphPath,baseDir:snapshot.baseDir,graphParams:values,
          set:Object.entries(candidate).filter(([k])=>k.includes(".")).map(([k,v])=>`${k}=${JSON.stringify(v)}`),dryRun:false,out});
          const r=await runCli(store.config,argv,{signal:extra.signal});if(r.code!==0)throw new Error(r.stderr);}
      } else {
        if(snapshot.sourceOverrides.set.length || Object.keys(candidate).some((k)=>k.includes("."))) throw new Error("节点覆盖不能表达为配方；改用 format:graph");
        const graph=readGraphDoc(snapshot.graphPath),defs=object(graph["params"]);
        const recipeValues={...Object.fromEntries(Object.entries(defs).map(([k,v])=>[k,object(v)["default"]])),...values};
        const r=await runCli(store.config,["recipes",path.join(store.dir(a.evaluationId),"source.lyflow.json"),"--json"],{signal:extra.signal});
        const info=r.lines.find((l)=>l.value["kind"]==="recipe_dir")?.value;
        if(r.code!==0 || !info?.["specDigest"])throw new Error("CLI 未返回配方 specDigest");
        fs.writeFileSync(out,JSON.stringify({schemaVersion:1,name:path.basename(out).replace(/\.lyflow-recipe\.json$/, ""),graph:{id:graph.id,specDigest:info["specDigest"]},values:recipeValues,note:`evaluationId=${a.evaluationId}; paramSet=${a.paramSet}`,updatedAt:new Date().toISOString()}),{flag:"wx"});
      }
      const provenance={evaluationId:a.evaluationId,paramSet:a.paramSet,graphHash:snapshot.graphHash,datasetHash:snapshot.datasetHash,build:snapshot.build,split:snapshot.split,partition:snapshot.partition,
        cachePolicy:snapshot.cachePolicy,baseDir:snapshot.baseDir,dependencies:snapshot.dependencies,export:fileIdentity(out),evaluationArtifact:state.rowsArtifact};
      fs.writeFileSync(`${out}.provenance.json`,JSON.stringify(provenance),{flag:"wx"});
      return ok({out,provenancePath:`${out}.provenance.json`,baseDir:snapshot.baseDir,targetGraph:a.format==="recipe"?path.join(store.dir(a.evaluationId),"source.lyflow.json"):out,artifact:store.artifacts.put(provenance)});
    }catch(e){return fail(e);}});
  server.registerTool("start_tuning",{title:"启动有预算的调参",description:"按工件分组冻结 train/validation/holdout，在 train 上做单因素敏感度或网格搜索。默认只动 detection；acceptance/input 始终锁定。预算到期保留部分记录。",
    inputSchema:{graphPath:z.string(),baseDir:z.string().optional(),samplesPath:z.string().optional(),samples:z.array(sampleSchema).optional(),recipe:z.string().optional(),graphParams:z.record(z.unknown()).optional(),set:z.array(z.string()).optional(),
      split:splitSchema,space:z.record(z.array(z.unknown()).min(1)),mode:z.enum(["one_factor","grid"]).default("one_factor"),allowedRoles:z.array(z.enum(["detection","geometry"])).default(["detection"]),lockedParams:z.array(z.string()).default([]),
      objectives:z.array(objectiveSchema).min(1),constraints:z.array(constraintSchema).default([]),maxRuns:z.number().int().min(1).max(100000).default(1000),timeoutMs:z.number().int().min(100).max(3600000).default(600000),jobs:z.number().int().min(1).max(16).default(1)},annotations:{readOnlyHint:false,destructiveHint:false}},
    async(a,extra)=>{try{
      if(!a.samples && !a.samplesPath)throw new Error("调参需要带工件标签的样本集");
      const raw=readGraphDoc(a.graphPath),baseline:Record<string,unknown>={};
      for(const key of Object.keys(a.space)){const dot=key.lastIndexOf(".");baseline[key]=dot<0?object(object(raw["params"])[key])["default"]:raw.nodes.find((n)=>n.id===key.slice(0,dot))?.params?.[key.slice(dot+1)];}
      // Core defaults and recipe values are used for the actual baseline; {} is always candidate 0.
      const params=generateCandidates(a.space,baseline,a.mode);
      const metric=[...new Set(["quality.executionOk",...a.objectives.map((o)=>o.metric),...a.constraints.map((c)=>c.metric)])];
      const id=await store.prepare({...a,metric,params,partition:"train",cachePolicy:"cold",summary:true},extra.signal,true);
      const {snapshot}=store.load(id),roles=parameterRoles(snapshot);
      for(const key of Object.keys(a.space)){
        const entry=roles.get(key);if(!entry)throw new Error(`未知参数 ${key}`);
        if(entry.bound)throw new Error(`param_conflict: ${key} 绑定到 ${entry.bound}；space 改用 ${entry.bound}`);
        if(a.lockedParams.includes(key)||!["detection","geometry"].includes(entry.role)||!a.allowedRoles.includes(entry.role as "detection"|"geometry"))throw new Error(`参数 ${key} 的角色 ${entry.role} 已锁定`);
        const dataset=JSON.parse(fs.readFileSync(path.join(store.dir(id),"dataset.json"),"utf8")) as Record<string,unknown>[];
        if(dataset.some((s)=>Object.hasOwn(object(s["set"]),key)||Object.hasOwn(object(s["graphParams"]),key)))throw new Error(`样本覆盖了搜索参数 ${key}，会使候选无效`);
      }
      fs.writeFileSync(path.join(store.dir(id),"tuning.json"),JSON.stringify({space:a.space,mode:a.mode,objectives:a.objectives,constraints:a.constraints,allowedRoles:a.allowedRoles,lockedParams:a.lockedParams}));
      launch(id);return ok({jobId:id,evaluationId:id,status:"running",candidateCount:params.length,sampleCount:snapshot.sampleCount,plannedRuns:params.length*snapshot.sampleCount,maxRuns:a.maxRuns,timeoutMs:a.timeoutMs,partition:"train"});
    }catch(e){return fail(e);}});
  server.registerTool("get_job",{title:"查询调参任务",description:"返回状态、预算消耗、约束合格的训练集候选与单因素敏感度；验证集与留出集仍需单独 eval。",
    inputSchema:{jobId:z.string()},annotations:{readOnlyHint:true}},async({jobId})=>{try{
      const result=await store.describe(jobId,{limit:0}),{snapshot,state}=store.load(jobId),file=path.join(store.dir(jobId),"tuning.json");
      delete result["summaries"];delete result["qualitySummaries"];
      if(fs.existsSync(file)){
        const spec=JSON.parse(fs.readFileSync(file,"utf8")) as {objectives:Objective[];constraints:Constraint[];space:Record<string,unknown[]>;mode:string};
        const candidates=snapshot.paramSets.map((params,i)=>({paramSet:i,params,...assessCandidate(state as unknown as Record<string,unknown>,i,"train",spec.objectives,spec.constraints)}));
        const ranked=candidates.filter((c)=>c.qualified).sort((a,b)=>Number(b.score)-Number(a.score));
        const base=rolesBaseline(snapshot),sensitivity:Record<string,unknown>[]=[];
        if(spec.mode==="one_factor")for(const key of Object.keys(spec.space)){
          const points=candidates.filter((c)=>c.paramSet===0||Object.keys(c.params).length===1&&Object.hasOwn(c.params,key)).map((c)=>({x:c.paramSet===0?base.get(key)?.value:c.params[key],score:c.score})).filter((p):p is {x:number;score:number}=>typeof p.x==="number"&&typeof p.score==="number");
          const mx=points.reduce((n,p)=>n+p.x,0)/points.length,my=points.reduce((n,p)=>n+p.score,0)/points.length,den=points.reduce((n,p)=>n+(p.x-mx)**2,0);
          sensitivity.push({param:key,n:points.length,slope:den>0?points.reduce((n,p)=>n+(p.x-mx)*(p.score-my),0)/den:null,points});
        }
        const best=ranked[0];
        return ok({...result,candidates:candidates.slice(0,100),candidateCount:candidates.length,ranking:ranked.slice(0,10),sensitivity,
          validationArgs:best?{graphPath:snapshot.graphPath,baseDir:snapshot.baseDir,samplesPath:path.join(store.dir(jobId),"dataset.jsonl"),params:[best.params],metric:snapshot.args.metric,graphParams:snapshot.args.graphParams,split:snapshot.split,partition:"validation",cachePolicy:"cold",summary:true}:null,
          note:"排名仅来自 train；validation 确认候选，holdout 最后验收一次。敏感度针对声明的评分尺度。"});
      }
      return ok(result);
    }catch(e){return fail(e);}});
  server.registerTool("cancel_job",{title:"取消后台任务",description:"终止 CLI 子进程，已经落盘的行保留；查询状态确认收尾。",inputSchema:{jobId:z.string()},annotations:{readOnlyHint:false,destructiveHint:false}},async({jobId})=>{try{
    const {state}=store.load(jobId),pending=background.get(jobId);pending?.abort.abort();
    return ok({jobId,cancellationRequested:store.cancel(jobId)||!!pending,status:state.status});
  }catch(e){return fail(e);}});
  server.registerTool("resume_job",{title:"续跑冻结任务",description:"核对冻结输入与构建，跳过已完成行。预算耗尽时显式追加 additionalRuns / additionalMs；使用原来的候选和分组。",
    inputSchema:{jobId:z.string(),additionalRuns:z.number().int().positive().max(100000).optional(),additionalMs:z.number().int().positive().max(3600000).optional()},annotations:{readOnlyHint:false,destructiveHint:false}},async({jobId,...grant})=>{try{
      const {snapshot,state}=store.load(jobId);if(state.status==="complete")throw new Error("任务已完成");
      const changed=await store.verify(snapshot);if(changed.length)throw new Error(`冻结内容已变: ${changed.join(",")}`);
      if(state.runBudget+(grant.additionalRuns??0)<=state.startedRuns||state.timeBudgetMs+(grant.additionalMs??0)<=state.elapsedMs)throw new Error("预算已用尽，需追加相应预算");
      launch(jobId,grant);return ok({jobId,status:"running",resumed:true});
    }catch(e){return fail(e);}});
}
function rolesBaseline(snapshot:Snapshot):ReturnType<typeof parameterRoles>{return parameterRoles(snapshot);}
