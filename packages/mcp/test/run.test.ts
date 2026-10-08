import assert from "node:assert/strict";
import test from "node:test";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { coreSummary, parseDiagnostics, summarizeOutputs, summarizeRun } from "../src/run.js";
import type { ExecutionEvent } from "../src/types.js";
import { assessment, projectValue, valueAt } from "../src/summary.js";
import { ArtifactStore } from "../src/artifacts.js";
import { freezeSplits } from "../src/evaluations.js";
import { assessCandidate, generateCandidates } from "../src/experiments.js";

test("复杂记录分页保留全量分布，artifact 在原文件改变后仍可读取",async()=>{
  const value={kind:"Record",type:"glue.StationMeasure",data:{count:4,counts:{ok:3,noBead:1},unit:"mm",width:[1,null,3,5]}};
  const result=projectValue(value,{fields:["width"],offset:1,limit:2});
  assert.deepEqual((result.value as {width:unknown[]}).width,[null,3]);
  assert.equal(result.truncated,true);
  assert.deepEqual(((result.arrays as Record<string,{statistics:unknown}>)?.["width"]?.statistics),{count:4,valid:3,missing:1,min:1,max:5,mean:3,p50:3,p95:4.8,std:2});
  assert.equal(assessment(value)["measurementStatus"],"incomplete");
  assert.equal(assessment({type:"glue.StationMeasure",data:{}})["measurementStatus"],"unknown");
  assert.equal(assessment({type:"glue.Breaks",data:{count:0,pathOk:false}})["detectionStatus"],"invalid");
  const bundle={kind:"Bundle",fields:[{name:"stations",value}]};
  assert.equal(valueAt(bundle,"stations.width.2"),3);
  assert.deepEqual(assessment(bundle),{stations:assessment(value)});
  assert.deepEqual(projectValue({points:[[10,20],[30,40],[50,60]]},{offset:1,limit:1}).value,{points:[[30,40]]});
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),"lyflow-artifact-")),file=path.join(dir,"rows.jsonl"),store=new ArtifactStore(dir);
  fs.writeFileSync(file,JSON.stringify({id:"a",width:[1,null,3,5]})+"\n");
  const ref=store.register(file,"jsonl");fs.writeFileSync(file,"{}");
  const read=await store.read(ref.artifactId,{offset:0,limit:1});assert.equal((read["rows"] as {id:string}[])[0]?.id,"a");
  assert.equal(read["total"],1);await assert.rejects(store.read("../rows"),/artifactId/);
  assert.equal(path.dirname(path.resolve(dir)),path.resolve(os.tmpdir()));fs.rmSync(dir,{recursive:true});
});

test("工件分组划分与样本顺序无关，多帧不会泄漏到其他集合",()=>{
  const samples=Array.from({length:12},(_,i)=>({id:String(i),tags:{workpiece:`p${i%6}`}}));
  const spec={groupTag:"workpiece",seed:"fixed",train:0.6,validation:0.2,holdout:0.2};
  const first=freezeSplits(samples,spec),second=freezeSplits([...structuredClone(samples)].reverse(),spec);
  assert.deepEqual(first,second);assert.deepEqual(new Set(Object.values(first)),new Set(["train","validation","holdout"]));
  for(let i=0;i<6;i++) assert.equal((samples[i]?.tags as Record<string,unknown>)["split"],(samples[i+6]?.tags as Record<string,unknown>)["split"]);
  assert.throws(()=>freezeSplits([{id:"x",tags:{}}],spec),/分组标签/);
  assert.throws(()=>freezeSplits([{id:"x"},{id:"x"}],undefined),/唯一/);
});

test("候选缺失或违反质量约束时不参与排名，搜索空间保留基线且受组合上限约束",()=>{
  const objectives=[{metric:"run.durationMs",direction:"minimize" as const,stat:"p95" as const,weight:1}];
  const state={status:"complete",summaries:[{paramSet:0,metric:"run.durationMs",groups:{train:{p95:2}}},{paramSet:0,metric:"quality.executionOk",groups:{train:{mean:1}}}],
    qualitySummaries:[{paramSet:0,groups:{train:{metrics:{measurementMissing:{max:1}}}}}]};
  assert.equal(assessCandidate(state,0,"train",objectives,[]).qualified,false);
  assert.equal(assessCandidate({...state,qualitySummaries:[]},0,"train",objectives,[]).qualified,true);
  const wrong={...state,qualitySummaries:[{paramSet:0,groups:{train:{metrics:{defectFn:{max:1}}}}}]};
  assert.equal(assessCandidate(wrong,0,"train",objectives,[]).qualified,false);
  assert.equal(assessCandidate(wrong,0,"train",objectives,[{metric:"quality.defectFn",max:1,stat:"max"}]).qualified,true);
  for(const metric of ["measurementWithinTolerance","breakWithinTolerance"]){
    const outsideTolerance={...state,qualitySummaries:[{paramSet:0,groups:{train:{metrics:{[metric]:{min:0.5,mean:0.75}}}}}]};
    const rejected=assessCandidate(outsideTolerance,0,"train",objectives,[]);
    assert.equal(rejected.qualified,false);
    assert.equal(rejected.violations[0]?.["reason"],"default_annotation_tolerance_gate");
    assert.equal(assessCandidate(outsideTolerance,0,"train",objectives,[{metric:`quality.${metric}`,max:1,stat:"mean"}]).qualified,false);
    assert.equal(assessCandidate(outsideTolerance,0,"train",objectives,[{metric:`quality.${metric}`,min:0.7,stat:"mean"}]).qualified,true);
    assert.equal(assessCandidate(outsideTolerance,0,"train",objectives,[{metric:`quality.${metric}`,min:0.8,stat:"mean"}]).qualified,false);
  }
  assert.equal(assessCandidate({...state,status:"timeout"},0,"train",objectives,[]).score,null);
  assert.equal(assessCandidate(state,1,"train",objectives,[]).qualified,false);
  const candidates=generateCandidates({a:[1,2],b:[3,4,5]},{a:1,b:3},"grid");assert.equal(candidates.length,7);assert.deepEqual(candidates[0],{});
  assert.throws(()=>generateCandidates({a:[1,2],b:[3,4,5]},{},"grid",5),/超过/);
});

const events: ExecutionEvent[] = [
  { kind: "run_started", runId: "r1", seq: 0, nodeCount: 2 },
  { kind: "node_state", runId: "r1", seq: 1, nodeId: "gen", state: "running" },
  {
    kind: "node_state",
    runId: "r1",
    seq: 2,
    nodeId: "gen",
    state: "done",
    durationMs: 12,
    stats: { elementCount: 24000, outputsAvailable: true },
  },
  {
    kind: "node_state",
    runId: "r1",
    seq: 3,
    nodeId: "lazy",
    state: "skipped",
    stats: { outputsAvailable: false, reason: "not_demanded" },
  },
  {
    kind: "node_state",
    runId: "r1",
    seq: 4,
    nodeId: "fit",
    state: "error",
    errors: [{ phase: "execute", code: "not_enough_points", message: "点太少" }],
  },
  { kind: "run_finished", runId: "r1", seq: 5, status: "error", durationMs: 40 },
];

test("run 摘要每个节点只留最后一次状态", () => {
  const summary = summarizeRun(events);
  assert.equal(summary.status, "error");
  assert.equal(summary.durationMs, 40);
  assert.equal(summary.nodes.length, 3);

  const gen = summary.nodes.find((n) => n.id === "gen");
  assert.deepEqual(gen, {
    id: "gen",
    state: "done",
    outputsAvailable: true,
    durationMs: 12,
    elementCount: 24000,
    errors: [],
  });

  const lazy = summary.nodes.find((n) => n.id === "lazy");
  assert.equal(lazy?.state, "skipped");
  assert.equal(lazy?.outputsAvailable, false);

  const fit = summary.nodes.find((n) => n.id === "fit");
  assert.equal(fit?.errors.length, 1);
  assert.equal(fit?.errors[0]?.code, "not_enough_points");
});

test("图级输出只留名字、端口与值，不留字节", () => {
  const outputs = summarizeOutputs({
    gap: {
      node: "n_gap",
      port: "gap",
      type: "Measurement",
      elementCount: 1,
      byteSize: 0,
      value: { kind: "Measurement", value: 5.7031, unit: "mm" },
    },
    thinned: { node: "voxel", port: "cloud", type: "PointCloud", elementCount: 4321, byteSize: 69136 },
    missing: { node: "n", port: "p", type: "Measurement", elementCount: 0, byteSize: 0, missing: true },
  });
  assert.deepEqual(outputs["gap"], {
    node: "n_gap",
    port: "gap",
    type: "Measurement",
    elementCount: 1,
    value: { kind: "Measurement", value: 5.7031, unit: "mm" },
  });
  assert.equal(outputs["thinned"]?.value, undefined);
  assert.equal(outputs["missing"]?.missing, true);
});

test("core 的 run summary 原样透出，不从 node_state 重建", () => {
  assert.equal(coreSummary(events), null, "老 core 没有 summary 字段");

  const withSummary: ExecutionEvent[] = [
    ...events.slice(0, -1),
    {
      kind: "run_finished",
      runId: "r1",
      seq: 5,
      status: "ok",
      durationMs: 40,
      summary: {
        runId: "r1",
        status: "degraded",
        durationMs: 40,
        nodes: { fit: { state: "error", code: "not_enough_points" } },
        outputs: {
          gap: { state: "value", node: "n_gap", port: "gap", type: "Measurement", elementCount: 1 },
          flush: { state: "inactive", node: "lazy", port: "cloud", reason: "not_demanded" },
          bundle: { state: "failed", node: "n_b", port: "b", from: "fit", code: "not_enough_points" },
        },
        decisions: { n_fb: { choice: "b", reason: "a 失败", port: "choice", type: "FallbackChoice" } },
        contractViolations: [],
      },
    },
  ];
  const s = coreSummary(withSummary);
  // run_finished 说 ok，summary 说 degraded —— 两件事，都要留着
  assert.equal(summarizeRun(withSummary).status, "ok");
  assert.equal(s?.status, "degraded");
  assert.equal(s?.outputs["flush"]?.state, "inactive");
  assert.equal(s?.outputs["flush"]?.reason, "not_demanded");
  assert.equal(s?.outputs["bundle"]?.state, "failed");
  assert.equal(s?.outputs["bundle"]?.from, "fit");
  assert.equal(s?.decisions["n_fb"]?.["choice"], "b");
  assert.deepEqual(s?.contractViolations, []);
});

test("校验失败的 400 正文能还原成诊断数组", () => {
  const diagnostics = parseDiagnostics(
    '[{"nodeId":"n1","severity":"error","code":"unknown_op","message":"没有这个算子"}]',
  );
  assert.equal(diagnostics?.length, 1);
  assert.equal(diagnostics?.[0]?.code, "unknown_op");

  const wrapped = parseDiagnostics('{"error":"[{\\"nodeId\\":\\"n1\\",\\"code\\":\\"cycle\\"}]"}');
  assert.equal(wrapped?.[0]?.code, "cycle");

  assert.equal(parseDiagnostics("校验失败，没有执行"), null);
});
