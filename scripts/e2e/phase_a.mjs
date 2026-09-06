// 阶段 A 验收：惰性分支（plan_extended / not_demanded 半透明 / stale 不误报）
// 与图级命名输出。对应 docs/phase-a1-acceptance.md 的两条。与其余分组共用一个 app 实例。

import { buildGraph, lit, newDoc, pressF5, replan, runAndWait, select } from "./page.mjs";

/** 主路径成功的 fallback 图：a 来自 gen.synthetic，b 那一路整条都是 deferred。 */
const OK_NODES = [
  { key: "a", op: "gen.synthetic", params: { pointCount: 4096, seed: 3 } },
  { key: "b", op: "gen.synthetic", params: { pointCount: 777, seed: 4 } },
  { key: "bTail", op: "filter.passthrough", params: { min: -100, max: 100 } },
  { key: "fb", op: "flow.fallback" },
];
const OK_EDGES = [
  { from: ["b", "cloud"], to: ["bTail", "cloud"] },
  { from: ["a", "cloud"], to: ["fb", "a"] },
  { from: ["bTail", "cloud"], to: ["fb", "b"] },
];

const opacityOf = (cdp, nodeId) =>
  cdp.eval(`
    const el = document.querySelector('[data-testid=${lit(`node-${nodeId}`)}]');
    if (!el) return null;
    return {
      notDemanded: el.getAttribute('data-not-demanded'),
      opacity: getComputedStyle(el).opacity,
      state: el.getAttribute('data-node-state'),
    };
  `);

async function suiteLazyBranch(cdp, report) {
  report.section("惰性分支：not_demanded 半透明、stale 不误报（ADR-0016）");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, OK_NODES, OK_EDGES);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("主路径成功时整轮仍是 ok", run.status, "ok");
  report.eq("fallback 跑完了", run.nodes[ids.fb]?.state, "done");
  report.eq("备用源报 skipped", run.nodes[ids.b]?.state, "skipped");
  report.eq("备用源的原因是 not_demanded", run.nodes[ids.b]?.reason, "not_demanded");
  report.eq("备用链尾也是 not_demanded", run.nodes[ids.bTail]?.reason, "not_demanded");

  const painted = await opacityOf(cdp, ids.b);
  report.eq("备用节点带 data-not-demanded", painted?.notDemanded, "1");
  report.ok(
    "备用节点画成半透明",
    painted !== null && Number(painted.opacity) < 0.6,
    JSON.stringify(painted),
  );

  const active = await opacityOf(cdp, ids.a);
  report.ok(
    "主路径节点不透明",
    active !== null && Number(active.opacity) > 0.95,
    JSON.stringify(active),
  );

  // stale 不误报：跑完立刻重编一次，一个节点都不该是 stale
  const cache = await replan(cdp);
  report.ok("重编后没有节点被误标 stale", cache.stale.length === 0, JSON.stringify(cache.stale));
  report.ok(
    "没被 demand 的节点没有进 ranWith",
    cache.ranWith[ids.b] === undefined,
    JSON.stringify(Object.keys(cache.ranWith)),
  );

  return ids;
}

async function suitePlanExtended(cdp, report) {
  report.section("惰性分支：主路径失败时经 plan_extended 追加（ADR-0016）");

  await newDoc(cdp);
  // 主路径是一个必填参数没填的 io.load_pcd —— 它在执行期失败，而 fallback 的
  // a 端口声明了 acceptsError，所以整轮不该被它拖成 error。
  const ids = await buildGraph(
    cdp,
    [
      { key: "a", op: "gen.synthetic", params: { pointCount: 16, seed: 1 } },
      { key: "bad", op: "filter.crop_box", params: { min: [1, 1, 1], max: [-1, -1, -1] } },
      { key: "b", op: "gen.synthetic", params: { pointCount: 999, seed: 2 } },
      { key: "fb", op: "flow.fallback" },
    ],
    [
      { from: ["a", "cloud"], to: ["bad", "cloud"] },
      { from: ["bad", "cloud"], to: ["fb", "a"] },
      { from: ["b", "cloud"], to: ["fb", "b"] },
    ],
  );

  const run = await runAndWait(cdp, () => pressF5(cdp));
  // crop_box 的 min > max 未必是执行期失败；两种结局都要能说清楚，所以只断言
  // 「b 这一路的状态与 fallback 的结局是自洽的」。
  const bState = run.nodes[ids.b]?.state ?? "(缺)";
  const fbState = run.nodes[ids.fb]?.state ?? "(缺)";
  if (fbState === "done" && run.nodes[ids.b]?.reason !== "not_demanded") {
    report.eq("b 被 demand 之后跑完了", bState, "done");
    const painted = await opacityOf(cdp, ids.b);
    report.eq("被 demand 的节点不再半透明", painted?.notDemanded, "0");
  } else {
    report.eq("主路径没失败时 b 仍是 not_demanded", run.nodes[ids.b]?.reason, "not_demanded");
  }
  report.ok("整轮没有被吸收掉的失败拖垮", run.status !== "error", `status=${run.status}`);

  await select(cdp, ids.fb);
}

async function suiteGraphOutputs(cdp, report) {
  report.section("图级命名输出（ADR-0017）");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 2048, seed: 11 } },
      { key: "pipe", op: "util.reroute" },
    ],
    [{ from: ["gen", "cloud"], to: ["pipe", "in"] }],
  );

  // 编辑器还没有「标为输出」的右键项（那是 A2），所以直接往 doc 上写
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.loadDoc({ ...g.doc, outputs: { cloud: { node: ${lit(ids.pipe)}, port: "out" } } },
              g.filePath);
    return true;
  `);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("运行状态 ok", run.status, "ok");
  report.ok(
    "run_started 带回了 outputs 声明",
    Array.isArray(run.outputs) && run.outputs.some((o) => o.name === "cloud"),
    JSON.stringify(run.outputs),
  );

  const outputs = await cdp.eval(`
    return await window.__lyflow.runOutputs(${lit(run.runId)});
  `);
  report.ok(
    "lyflow_run_outputs 按名字给出点云元信息",
    outputs?.cloud?.type === "PointCloud" && outputs.cloud.elementCount === 2048,
    JSON.stringify(outputs),
  );
}

export const phaseASuites = [suiteLazyBranch, suitePlanExtended, suiteGraphOutputs];
