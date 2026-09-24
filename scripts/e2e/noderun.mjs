// 节点运行按钮的分组（docs/node-run-plan.md §4 验收 7–12、11b，§6 修订一验收 17–20）：按钮的位置与
// 真鼠标、单击 = 智能运行（本节点 + 缺结果或过时的上游）、Shift+单击 = 强制重算、hover 提示的预告、
// 「仅此节点」的上游不齐兜底、运行中停止与抢占、hover / 关动效 / 端点对齐、计划外节点挂结果、
// 右键三项。逐条结果见 docs/node-run-acceptance.md。
//
// 点按钮一律用 CDP 的真鼠标（Input.dispatchMouseEvent）：合成的 click 骗不过 React Flow 的
// 拖动/选中判定，也骗不过 CSS 的 :hover。

import { sleep } from "./cdp.mjs";
import { alignOf, emptySpot, installMotionProbe, moveMouse, worst } from "./motion.mjs";
import { countsOf, openByDoubleClick, park, resetPeek, waitPeek } from "./peek.mjs";
import {
  buildGraph,
  canvasBox,
  centerOf,
  lit,
  newDoc,
  normalizeZoom,
  placeAtScreen,
  pressF5,
  replan,
  runAndWait,
  selectAndReadViewer,
} from "./page.mjs";

// ------------------------------------------------------------ 页面侧的小工具

const btnSel = (id) => `[data-testid="node-run-${id}"]`;

/** 真鼠标点一下（或双击）。clickCount 逐次递增，Chromium 才认得出是双击。shift = 按住 Shift 点。 */
async function click(cdp, p, { clickCount = 1, shift = false } = {}) {
  await moveMouse(cdp, p);
  for (let i = 1; i <= clickCount; i += 1) {
    const common = { x: p.x, y: p.y, button: "left", buttons: 1, clickCount: i, modifiers: shift ? 8 : 0 };
    await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", ...common });
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...common, buttons: 0 });
  }
}

/** 按钮的状态三件套。 */
const buttonOf = (cdp, id) =>
  cdp.eval(`
    const b = document.querySelector(${lit(btnSel(id))});
    if (!b) return null;
    return { state: b.getAttribute('data-run-state'), upstream: b.getAttribute('data-run-upstream'),
             upToDate: b.getAttribute('data-run-uptodate'), own: b.getAttribute('data-run-own'),
             title: b.getAttribute('title') };
  `);

/** 记下每一条 run_started / run_finished（计划里有谁、isolate 是什么）。装一次就够。 */
async function installRecorder(cdp) {
  await cdp.eval(`
    if (window.__lyNodeRun) return true;
    window.__lyNodeRun = { events: [] };
    await window.__lyflow.transport.onExecutionEvent((e) => {
      if (e.kind === 'run_started' || e.kind === 'run_finished') window.__lyNodeRun.events.push(e);
    });
    return true;
  `);
}

const startedOf = (cdp, runId) =>
  cdp.eval(`return window.__lyNodeRun.events.find((e) => e.kind === 'run_started' && e.runId === ${lit(runId)}) ?? null;`);

/** 当前运行的快照里，这些节点的状态与耗时。 */
const statesOf = (cdp, ids) =>
  cdp.eval(`
    const n = window.__lyflow.snapshot().run.nodes;
    return Object.fromEntries(${lit(ids)}.map((id) => [id, n[id] ? { state: n[id].state, durationMs: n[id].durationMs } : null]));
  `);

/** 流水账里每个节点经历过的状态。 */
const transitionsBy = (cdp) =>
  cdp.eval(`
    const out = {};
    for (const t of window.__lyflow.transitions) (out[t.nodeId] ??= []).push(t.state);
    return out;
  `);

/** a → b → c 三节点小图，摆进画布左上，按钮都在视野里。seed 每次不同，免得命中上一组的缓存。 */
async function smallChain(cdp) {
  await newDoc(cdp);
  const ids = await buildGraph(cdp, [
    { key: "a", op: "gen.synthetic", params: { pointCount: 20000, seed: (Date.now() % 9973) + 1 } },
    { key: "b", op: "filter.voxel_grid" },
    { key: "c", op: "filter.passthrough" },
  ], [
    { from: ["a", "cloud"], to: ["b", "cloud"] },
    { from: ["b", "cloud"], to: ["c", "cloud"] },
  ]);
  await normalizeZoom(cdp, 0.8);
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.a]: { x: 30, y: 40 },
    [ids.b]: { x: Math.round(box.w * 0.36), y: 40 },
    [ids.c]: { x: Math.round(box.w * 0.66), y: 40 },
  });
  await sleep(450);
  return ids;
}

/** 一个跑得够久的节点：三百万点过一道极细的体素栅格。 */
async function slowPair(cdp) {
  await newDoc(cdp);
  const ids = await buildGraph(cdp, [
    { key: "gen", op: "gen.synthetic", params: { pointCount: 3_000_000, seed: (Date.now() % 9973) + 1 } },
    { key: "slow", op: "filter.voxel_grid", params: { leafSize: [0.0006, 0.0006, 0.0006] } },
  ], [{ from: ["gen", "cloud"], to: ["slow", "cloud"] }]);
  await normalizeZoom(cdp, 0.8);
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.gen]: { x: 30, y: 40 },
    [ids.slow]: { x: Math.round(box.w * 0.45), y: 40 },
  });
  await sleep(450);
  return ids;
}

/** 等某个按钮进入某个状态（在页面里逐帧看，拿到就返回，不错过短暂的 running）。 */
const waitButton = (cdp, id, pred, timeoutMs = 20_000) =>
  cdp.eval(`
    const t0 = performance.now();
    while (performance.now() - t0 < ${timeoutMs}) {
      const b = document.querySelector(${lit(btnSel(id))});
      const s = b && { state: b.getAttribute('data-run-state'), own: b.getAttribute('data-run-own') };
      if (s && (${pred})(s)) return s;
      await new Promise((r) => requestAnimationFrame(r));
    }
    return null;
  `);

/** 这个节点给人看的名字（hover 提示里写的是它，不是 id）。 */
const labelOf = (cdp, id) =>
  cdp.eval(`return document.querySelector('[data-testid="node-${id}"] .node__title')?.textContent ?? null;`);

/** 这次运行的 run_finished（页面里另挂的记录器记下的）。 */
const finishedOf = (cdp, runId) =>
  cdp.eval(`return window.__lyNodeRun.events.find((e) => e.kind === 'run_finished' && e.runId === ${lit(runId)}) ?? null;`);

/** 当前运行里某节点的 stats（cached 等）。 */
const statsOf = (cdp, id) =>
  cdp.eval(`const n = window.__lyflow.stores.execution.getState().nodes.get(${lit(id)}); return n ? { state: n.state, cached: n.stats?.cached === true } : null;`);

/** 刚跑完立刻重编一次计划，等它落进 store（按钮的预告靠 plan 的 cached）。 */
async function settlePlan(cdp) {
  await replan(cdp);
  await sleep(150);
}

const waitRunEnd = (cdp, what) =>
  cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runStatus !== 'idle' && s.runStatus !== 'running'; })()`,
    { timeoutMs: 120_000, what },
  );

// ---------------------------------------------------- 验收 7：位置与真鼠标

async function suitePlacement(cdp, report) {
  report.section("单节点运行 验收 7：按钮在标题与徽标之后、折叠仍在；真鼠标点它不选中、不拖动、不改名");
  await installMotionProbe(cdp);
  await installRecorder(cdp);
  const ids = await smallChain(cdp);

  const layout = await cdp.eval(`
    return ${lit(Object.values(ids))}.map((id) => {
      const node = document.querySelector('[data-testid="node-' + id + '"]');
      const head = node?.querySelector('.node__head');
      const btn = head?.querySelector(${lit('[data-testid^="node-run-"]')});
      const title = head?.querySelector('.node__title');
      return { id, inHead: !!btn && btn.getAttribute('data-testid') === 'node-run-' + id,
               titleRight: title ? title.getBoundingClientRect().right : null,
               btnLeft: btn ? btn.getBoundingClientRect().left : null,
               w: btn ? Math.round(btn.getBoundingClientRect().width) : null,
               svg: btn ? Math.round(btn.querySelector('svg').getBoundingClientRect().width) : null };
    });
  `);
  report.ok("每个节点的 .node__head 里都有自己的 node-run-<id>", layout.every((r) => r.inHead), JSON.stringify(layout));
  report.ok("标题的右边界 ≤ 按钮左边界（不遮挡标题文字）",
    layout.every((r) => r.titleRight !== null && r.btnLeft !== null && r.titleRight <= r.btnLeft + 0.01), JSON.stringify(layout));
  report.ok("命中区 20 px、视觉圆 14 px（画布缩放下按比例）",
    layout.every((r) => r.w > 0 && r.svg > 0 && Math.abs(r.w / r.svg - 20 / 14) < 0.1), JSON.stringify(layout.map((r) => [r.w, r.svg])));

  // 徽标之后：静音挂上 M 徽标，按钮仍排在它后面
  const order = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.setBypass([${lit(ids.c)}], true);
    await new Promise((r) => setTimeout(r, 150));
    const head = document.querySelector('[data-testid="node-${ids.c}"] .node__head');
    const kids = [...head.children].map((c) => c.getAttribute('data-testid')?.startsWith('node-run-') ? 'run'
      : c.classList.contains('node__badge') ? 'badge' : c.classList.contains('node__title') ? 'title' : c.className);
    window.__lyflow.stores.graph.getState().undo();
    return kids;
  `);
  report.ok("顺序是 标题 → 徽标 → 按钮", order.indexOf("title") < order.indexOf("badge") && order.indexOf("badge") < order.indexOf("run"),
    JSON.stringify(order));

  const collapsed = await cdp.eval(`
    window.__lyflow.stores.graph.getState().setCollapsed([${lit(ids.b)}], true);
    await new Promise((r) => setTimeout(r, 200));
    const has = !!document.querySelector('[data-testid="node-${ids.b}"] .node__head ${btnSel(ids.b)}');
    const body = !!document.querySelector('[data-testid="node-collapsed-${ids.b}"]');
    window.__lyflow.stores.graph.getState().undo();
    await new Promise((r) => setTimeout(r, 200));
    return { has, body };
  `);
  report.ok("折叠之后按钮仍在标题栏里", collapsed.has && collapsed.body, JSON.stringify(collapsed));

  // 缺失算子的节点没有按钮（U1）
  const missing = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const before = JSON.parse(JSON.stringify(g.doc));
    const doc = JSON.parse(JSON.stringify(g.doc));
    doc.nodes.push({ id: 'n_ghost', op: 'no.such.operator', params: {}, ui: { position: { x: 60, y: 320 } } });
    g.loadDoc(doc, null);
    await new Promise((r) => setTimeout(r, 250));
    const el = document.querySelector('[data-testid="node-n_ghost"]');
    const out = { rendered: !!el, state: el?.getAttribute('data-node-state'), button: !!document.querySelector('${btnSel("n_ghost")}') };
    window.__lyflow.stores.graph.getState().loadDoc(before, null);
    await new Promise((r) => setTimeout(r, 250));
    return out;
  `);
  report.ok("缺失算子的节点没有运行按钮", missing.rendered && missing.state === "missing" && !missing.button, JSON.stringify(missing));
  await sleep(200); // loadDoc 之后等画布安定

  // 真鼠标单击源节点 a 的按钮 → 智能运行 targets=[a]；节点不被选中、不动、不改名
  await cdp.eval(`window.__lyflow.stores.ui.getState().clearSelection(); return true;`);
  const posBefore = await cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.a)}).ui.position;`);
  const at = await centerOf(cdp, btnSel(ids.a));
  const run = await runAndWait(cdp, () => click(cdp, at));
  const after = await cdp.eval(`
    const u = window.__lyflow.stores.ui.getState();
    const e = window.__lyflow.stores.execution.getState();
    return { selected: [...u.selectedNodes], renaming: !!document.querySelector('[data-testid^="node-rename-"]'),
             pos: window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.a)}).ui.position,
             targets: e.targets, isolate: e.isolate };
  `);
  report.ok("（前提）单击按钮发起了智能运行 targets=[a]（不是 isolate）", JSON.stringify(after.targets) === JSON.stringify([ids.a]) &&
    after.isolate.length === 0, JSON.stringify(after));
  report.eq("那次运行 ok", run.status, "ok");
  report.eq("单击按钮：节点没有被选中", after.selected, []);
  report.eq("单击按钮：没有进入改名", after.renaming, false);
  report.eq("单击按钮：节点位置没动", after.pos, posBefore);

  // 验收 20 的前半：本节点有编辑期校验 error 时按钮置灰（修订一 V3：唯一的置灰原因）。
  // 顺手拿这个不可点的按钮验拖动与双击 —— 可点的按钮双击会发起两次运行，第二次还是「停止」
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.b)}, 'leafSize', [0, 0.01, 0.01]); return true;`);
  const invalid = await waitButton(cdp, ids.b, "(s) => s.state === 'disabled'", 5000);
  const bInvalid = await buttonOf(cdp, ids.b);
  report.ok("（20）b 有校验错误：按钮 data-run-state=\"disabled\"，title 说明原因", invalid !== null && /校验错误/.test(bInvalid?.title ?? ""),
    JSON.stringify(bInvalid));
  const cursor = await cdp.eval(`return getComputedStyle(document.querySelector(${lit(btnSel(ids.b))})).cursor;`);
  report.eq("（20）不可用时光标 not-allowed", cursor, "not-allowed");
  report.ok("（20）上游不齐不再让按钮置灰：c 的上游 b 从没跑过，c 的按钮仍可点",
    (await buttonOf(cdp, ids.c))?.state !== "disabled", JSON.stringify(await buttonOf(cdp, ids.c)));

  const bBefore = await cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.b)}).ui.position;`);
  const from = await centerOf(cdp, btnSel(ids.b));
  const common = { button: "left", buttons: 1, clickCount: 1 };
  await moveMouse(cdp, from);
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: from.x, y: from.y, ...common });
  for (let i = 1; i <= 8; i += 1) {
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: from.x + i * 8, y: from.y + i * 3, ...common });
    await sleep(16);
  }
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: from.x + 64, y: from.y + 24, ...common, buttons: 0 });
  await sleep(200);
  const dragged = await cdp.eval(`
    return { pos: window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.b)}).ui.position,
             selected: [...window.__lyflow.stores.ui.getState().selectedNodes] };
  `);
  report.ok("按住按钮拖动：节点不跟着走、也没被选中", JSON.stringify(dragged.pos) === JSON.stringify(bBefore) &&
    !dragged.selected.includes(ids.b), JSON.stringify({ bBefore, dragged }));

  const runIdBefore = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  await click(cdp, await centerOf(cdp, btnSel(ids.b)), { clickCount: 2 });
  await sleep(300);
  const dbl = await cdp.eval(`
    return { renaming: !!document.querySelector('[data-testid^="node-rename-"]'),
             selected: [...window.__lyflow.stores.ui.getState().selectedNodes],
             runId: window.__lyflow.stores.execution.getState().runId };
  `);
  report.ok("双击按钮：没有进入改名、节点没被选中", !dbl.renaming && !dbl.selected.includes(ids.b), JSON.stringify(dbl));
  report.eq("（20）点置灰的按钮（双击）：没有发起运行", dbl.runId, runIdBefore);
  await cdp.eval(`window.__lyflow.stores.graph.getState().undo(); return true;`);
  const back = await waitButton(cdp, ids.b, "(s) => s.state !== 'disabled'", 5000);
  report.ok("（20）改回合法参数：按钮恢复可点", back !== null, JSON.stringify(back));
}

// ------------------------------------ 修订一 验收 17 / 18：智能运行、Shift = 强制

async function suiteSmart(cdp, report) {
  report.section("修订一 验收 18：全部就绪时单击 = 已缓存、不真跑；Shift+单击 = 只有本节点真跑");
  await installRecorder(cdp);
  const ids = await smallChain(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");
  await settlePlan(cdp);

  const ready = await buttonOf(cdp, ids.b);
  report.eq("全部就绪：b 的 title 是「已是最新」那条", ready?.title, "已是最新（命中缓存）—— Shift+点击强制重算");
  report.ok("全部就绪：没有要一并运行的上游、按钮可点", ready?.upstream === null && ready?.state === "done", JSON.stringify(ready));

  const bAt = await centerOf(cdp, btnSel(ids.b));
  const hit = await runAndWait(cdp, () => click(cdp, bAt));
  await sleep(150);
  const hitTrans = await transitionsBy(cdp);
  const hitStarted = await startedOf(cdp, hit.runId);
  const hitB = await statsOf(cdp, ids.b);
  report.eq("单击：运行 ok", hit.status, "ok");
  report.ok("单击：是智能运行 targets=[b]、没有 force", hitStarted && JSON.stringify(hitStarted.targets) === JSON.stringify([ids.b]) &&
    hitStarted.force.length === 0 && hitStarted.isolate.length === 0, JSON.stringify(hitStarted && { targets: hitStarted.targets, force: hitStarted.force }));
  report.ok("单击：b 显示「已缓存」（stats.cached=true），没有真执行", hitB?.state === "skipped" && hitB.cached,
    JSON.stringify(hitB));
  const skipLabel = await cdp.eval(`return document.querySelector('[data-testid="node-skip-${ids.b}"]')?.textContent ?? null;`);
  report.eq("单击：节点上的标签是「已缓存」", skipLabel, "已缓存");
  report.ok("单击：没有任何节点进入 running", !Object.values(hitTrans).some((l) => l.includes("running")), JSON.stringify(hitTrans));

  const forced = await runAndWait(cdp, () => click(cdp, bAt, { shift: true }));
  await sleep(150);
  const fTrans = await transitionsBy(cdp);
  const fStarted = await startedOf(cdp, forced.runId);
  const fA = await statsOf(cdp, ids.a);
  const fB = await statsOf(cdp, ids.b);
  const selected = await cdp.eval(`return [...window.__lyflow.stores.ui.getState().selectedNodes];`);
  report.eq("Shift+单击：运行 ok", forced.status, "ok");
  report.ok("Shift+单击：targets=[b]、force=[b]", fStarted && JSON.stringify(fStarted.force) === JSON.stringify([ids.b]) &&
    JSON.stringify(fStarted.targets) === JSON.stringify([ids.b]), JSON.stringify(fStarted && { targets: fStarted.targets, force: fStarted.force }));
  report.ok("Shift+单击：b 真执行（running → done，cached=false）", fB?.state === "done" && !fB.cached &&
    (fTrans[ids.b] ?? []).includes("running"), JSON.stringify({ fB, trans: fTrans[ids.b] }));
  report.ok("Shift+单击：a 不执行（命中缓存，没有 running）", fA?.cached === true && !(fTrans[ids.a] ?? []).includes("running"),
    JSON.stringify({ fA, trans: fTrans[ids.a] }));
  report.eq("Shift+单击：节点没有被加进选区（Shift 是多选键，按钮吞掉了它）", selected, []);
  const c = await statsOf(cdp, ids.c);
  report.eq("下游 c 两次都不进计划，仍是 done", [hitStarted?.plan.includes(ids.c), fStarted?.plan.includes(ids.c), c?.state], [false, false, "done"]);
}

async function suiteSmartUpstream(cdp, report) {
  report.section("修订一 验收 17：改上游参数后 b 仍可点，hover 提示预告「将一并运行上游」，单击后 a、b 依次运行");
  await installRecorder(cdp);
  const ids = await smallChain(cdp);
  // 另放一个与这条链无关的源 d：改 a 之后 b、c 的键都变，d 的不变 —— 挂结果（V2）要看得出区别
  const d = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const id = g.addNode('gen.synthetic', { x: 0, y: 0 });
    window.__lyflow.stores.graph.getState().setParam(id, 'pointCount', 3000);
    return id;
  `);
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, { [d]: { x: 30, y: Math.round(box.h * 0.45) } });
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");

  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.a)}, 'pointCount', 23456); return true;`);
  await settlePlan(cdp);
  const b = await buttonOf(cdp, ids.b);
  const aLabel = await labelOf(cdp, ids.a);
  report.ok("b 的按钮可点（不再因上游过时置灰）", b?.state !== "disabled", JSON.stringify(b));
  report.ok("data-run-upstream 含 a", String(b?.upstream).split(",").includes(ids.a), JSON.stringify(b));
  report.ok(`title 预告「将一并运行上游 ${aLabel}」`, typeof b?.title === "string" && b.title.includes("将一并运行上游") && b.title.includes(aLabel),
    JSON.stringify({ title: b?.title, aLabel }));

  const run = await runAndWait(cdp, async () => click(cdp, await centerOf(cdp, btnSel(ids.b))));
  await sleep(150);
  const trans = await transitionsBy(cdp);
  const started = await startedOf(cdp, run.runId);
  const finished = await finishedOf(cdp, run.runId);
  const seq = (id) => (trans[id] ?? []).join(",");
  report.eq("运行 ok", run.status, "ok");
  report.ok("a、b 都经历了 running → done", seq(ids.a).includes("running,done") && seq(ids.b).includes("running,done"), JSON.stringify(trans));
  const order = await cdp.eval(`
    const t = window.__lyflow.transitions;
    const at = (id, st) => t.findIndex((x) => x.nodeId === id && x.state === st);
    return { aDone: at(${lit(ids.a)}, 'done'), bRunning: at(${lit(ids.b)}, 'running') };
  `);
  report.ok("依次：a 的 done 早于 b 的 running", order.aDone >= 0 && order.aDone < order.bRunning, JSON.stringify(order));
  report.ok("c 不进计划", started && !started.plan.includes(ids.c), JSON.stringify(started?.plan));
  // 修订一 V2：c 的键跟着 a 变了、仓里没有当前结果 → 不挂，编辑器把它退回 idle（与 §6 验收 17 字面不同，见验收记录）
  report.ok("c 的键变了、不在 attached 里 → 退回 idle（不显示「完成」却取不到输出）",
    !finished?.attached?.includes(ids.c) && (run.nodes[ids.c]?.state ?? "idle") === "idle", JSON.stringify({ attached: finished?.attached, c: run.nodes[ids.c] }));
  report.ok("不受影响的 d 挂上了、仍是 done、按新 runId 取得到输出",
    finished?.attached?.includes(d) && run.nodes[d]?.state === "done", JSON.stringify({ attached: finished?.attached, d: run.nodes[d] }));
  const dInfo = await cdp.eval(`
    const { runId } = window.__lyflow.stores.execution.getState();
    return (await window.__lyflow.transport.getOutputInfo(runId, ${lit(d)})).map((o) => o.port);
  `);
  report.ok("getOutputInfo(新 runId, d) 有 cloud", dInfo.includes("cloud"), JSON.stringify(dInfo));
  await settlePlan(cdp);
  report.eq("跑完之后 b 回到「已是最新」", (await buttonOf(cdp, ids.b))?.title, "已是最新（命中缓存）—— Shift+点击强制重算");
}

// ------------------------------- 修订一 验收 19 的第三项：仅此节点（isolate）的兜底

async function suiteIsolateOnly(cdp, report) {
  report.section("修订一 验收 19：「仅此节点」—— 上游过时时置灰写明缺谁；绕过预判 → upstream_not_ready toast、零 running");
  await installMotionProbe(cdp);
  await installRecorder(cdp);
  const ids = await smallChain(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");

  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.a)}, 'pointCount', 23456); return true;`);
  const cache = await replan(cdp);
  report.ok("（前提）a 被标为 stale", cache.stale.includes(ids.a), JSON.stringify(cache.stale));
  const item = await openMenu(cdp, ids.b);
  report.ok("「仅此节点」置灰，data-run-reason 含 a，title 写明缺谁", item?.only?.disabled === true &&
    String(item.only.reason).split(",").includes(ids.a) && /还没有可用结果/.test(item.only.title ?? ""), JSON.stringify(item?.only));
  report.ok("同一时刻按钮本身仍可点（智能运行会把 a 一起跑）", (await buttonOf(cdp, ids.b))?.state !== "disabled",
    JSON.stringify(await buttonOf(cdp, ids.b)));
  await closeMenu(cdp);

  // 绕过预判：直接让 core 跑 isolate，由 R2 兜底
  await cdp.eval(`
    window.__lyMotion.rec ??= {};
    window.__lyMotion.rec.locate?.stop();
    window.__lyMotion.rec.locate = window.__lyMotion.record('data-flash');
    return true;
  `);
  const run = await runAndWait(cdp, () => cdp.eval(`await window.__lyflow.run({ isolate: [${lit(ids.b)}] }); return true;`));
  const toast = await cdp.eval(`
    const t = document.querySelector('[data-testid="toast"]');
    return t ? { text: t.textContent, warn: t.classList.contains('toast--warn') } : null;
  `);
  await sleep(450);
  const flashes = await cdp.eval(`const r = window.__lyMotion.rec.locate; r.stop(); return r.hits.map((h) => ({ key: h.key, value: h.value }));`);
  const finished = await finishedOf(cdp, run.runId);
  const trans = await transitionsBy(cdp);
  report.eq("运行状态 error", run.status, "error");
  report.ok("run_finished.diagnostics 指向 a、code 是 upstream_not_ready",
    finished?.error?.code === "upstream_not_ready" && finished.diagnostics?.some((d) => d.nodeId === ids.a && d.code === "upstream_not_ready"),
    JSON.stringify(finished && { error: finished.error, diagnostics: finished.diagnostics }));
  report.ok("warn 级 toast，文案是「上游 … 还没有可用结果」", toast?.warn && /还没有可用结果/.test(toast.text) && toast.text.includes(ids.a), JSON.stringify(toast));
  report.ok("没有任何节点进入 running", !Object.values(trans).some((list) => list.includes("running")), JSON.stringify(trans));
  report.ok("缺结果的上游 a 闪了一下定位光（data-flash=\"locate\"）", flashes.some((h) => h.key === `node-${ids.a}` && h.value === "locate"),
    JSON.stringify(flashes));
  const aNode = await cdp.eval(`return document.querySelector('[data-testid="node-${ids.a}"]').getAttribute('data-node-state');`);
  report.ok("a 没有被标红（它没有失败）", aNode !== "error", aNode);
  const head = await cdp.eval(`return getComputedStyle(document.querySelector('[data-testid="node-${ids.a}"] .node__head')).transform;`);
  report.ok("定位闪光不抖动（标题栏没有位移）", head === "none" || /matrix\(1, 0, 0, 1, 0, 0\)/.test(head), head);
}

// ------------------------------------------------- 验收 10：运行中停止与抢占

async function suiteStopAndPreempt(cdp, report) {
  report.section("单节点运行 验收 10 / 修订一 20：Shift+单击发起的运行中按钮 running、点它取消；全图运行里 running 的节点上点按钮是抢占，不是停止");
  await installRecorder(cdp);
  const ids = await slowPair(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");
  const slowMs = full.nodes[ids.slow]?.durationMs ?? 0;
  report.ok(`（前提）slow 节点够慢（${Math.round(slowMs)} ms ≥ 300）`, slowMs >= 300, JSON.stringify(full.nodes[ids.slow]));

  // 自己发起的那次：running + ■，再点一下 = 停止。全图刚跑过，单击只会命中缓存，所以用 Shift 强制真跑
  const before = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  await click(cdp, await centerOf(cdp, btnSel(ids.slow)), { shift: true });
  const running = await waitButton(cdp, ids.slow, "(s) => s.state === 'running' && s.own === '1'");
  report.ok("点了之后按钮 data-run-state=\"running\"、带 ■（data-run-own=1）", running !== null, JSON.stringify(running));
  const title = (await buttonOf(cdp, ids.slow))?.title;
  report.eq("运行中 title 是「停止」", title, "停止");
  const stopAt = await cdp.eval(`
    const s = window.__lyflow.stores.execution.getState();
    return { runId: s.runId, status: s.runStatus, targets: s.targets,
             stop: !!document.querySelector(${lit(btnSel(ids.slow) + " .node-run__stop")}) };
  `);
  report.ok("（前提）这时跑的是按钮发起的 targets=[slow] 那一次", stopAt.runId !== before && stopAt.status === "running" &&
    JSON.stringify(stopAt.targets) === JSON.stringify([ids.slow]) && stopAt.stop, JSON.stringify(stopAt));
  await click(cdp, await centerOf(cdp, btnSel(ids.slow)));
  await waitRunEnd(cdp, "停止后运行结束");
  const stopped = await cdp.eval(`
    const s = window.__lyflow.stores.execution.getState();
    return { runId: s.runId, status: s.runStatus };
  `);
  report.ok("再点一下：这次运行被取消（cancelled），没有发起新的", stopped.runId === stopAt.runId && stopped.status === "cancelled",
    JSON.stringify(stopped));
  report.ok("取消后按钮不再是 running", (await buttonOf(cdp, ids.slow))?.state !== "running", JSON.stringify(await buttonOf(cdp, ids.slow)));

  // 抢占：换个 seed 让全图真跑，等 slow 在全图运行里 running，点它的按钮
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.gen)}, 'seed', ${(Date.now() % 7919) + 13}); return true;`);
  await cdp.eval(`window.__lyflow.clearTransitions(); return true;`);
  await pressF5(cdp);
  const inFull = await waitButton(cdp, ids.slow, "(s) => s.state === 'running'", 60_000);
  const fullRun = await cdp.eval(`const s = window.__lyflow.stores.execution.getState(); return { runId: s.runId, targets: s.targets };`);
  report.ok("（前提）slow 在全图运行里 running，按钮也显示 running 但没有 ■", inFull !== null && inFull.own === null &&
    fullRun.targets.length === 0, JSON.stringify({ inFull, fullRun }));
  await click(cdp, await centerOf(cdp, btnSel(ids.slow)));
  const preempt = await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runId !== ${lit(fullRun.runId)} && s.targets.length === 1 ? { runId: s.runId, targets: s.targets } : null; })()`,
    { timeoutMs: 30_000, what: "抢占后的智能运行开始" },
  );
  report.eq("点击发起了新的智能运行（targets=[slow]）", preempt.targets, [ids.slow]);
  await waitRunEnd(cdp, "抢占后的运行结束");
  const end = await cdp.eval(`
    const s = window.__lyflow.stores.execution.getState();
    return { runId: s.runId, status: s.runStatus, marks: window.__lyflow.runMarks.map((m) => ({ runId: m.runId, status: m.status })) };
  `);
  report.ok("被抢占的全图运行以 cancelled 收场", end.marks.some((m) => m.runId === fullRun.runId && m.status === "cancelled"),
    JSON.stringify(end.marks));
  report.ok("抢占它的那次跑完了（ok），而不是被当成「停止」", end.runId === preempt.runId && end.status === "ok", JSON.stringify(end));
}

// ------------------------------------ 验收 11：hover、关动效、端点对齐

async function suiteLook(cdp, report) {
  report.section("单节点运行 验收 11：hover 实心 accent、关动效时进度环不转但仍显示、端点对齐 ≤ 1 px");
  await installMotionProbe(cdp);
  const ids = await smallChain(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");

  const accent = await cdp.eval(`
    const probe = document.createElement('span');
    probe.style.color = 'var(--lyflow-accent)';
    document.querySelector('.app').appendChild(probe);
    const c = getComputedStyle(probe).color;
    probe.remove();
    return c;
  `);
  const ringOf = (id) => cdp.eval(`
    const b = document.querySelector(${lit(btnSel(id))});
    const ring = getComputedStyle(b.querySelector('.node-run__ring'));
    const play = getComputedStyle(b.querySelector('.node-run__play'));
    const svg = new DOMMatrixReadOnly(getComputedStyle(b.querySelector('svg')).transform === 'none' ? undefined : getComputedStyle(b.querySelector('svg')).transform);
    return { fill: ring.fill, stroke: ring.stroke, play: play.opacity, scale: +svg.a.toFixed(3) };
  `);
  const idle = await ringOf(ids.b);
  await moveMouse(cdp, await centerOf(cdp, btnSel(ids.b)));
  await sleep(300);
  const hovered = await ringOf(ids.b);
  const rowsHover = await alignOf(cdp);
  report.ok("（对照）没 hover 时是空心圈", idle.fill !== accent && idle.play === "0", JSON.stringify(idle));
  report.eq("hover 按钮：圆的计算后 fill 是 accent（实心）", hovered.fill, accent);
  report.ok("hover 按钮：白色 ▶ 显出来、SVG 放大（按钮不含端口，A6 允许）", hovered.play === "1" && hovered.scale > 1, JSON.stringify(hovered));
  report.ok(`hover 按钮期间：${rowsHover.length} 条边端点与锚点最大偏差 ${worst(rowsHover)} px ≤ 1`,
    rowsHover.length === 2 && worst(rowsHover) <= 1, JSON.stringify(rowsHover));
  await moveMouse(cdp, await emptySpot(cdp));
  await sleep(150);

  // 进度环：换到慢图，动效开着时转圈，关了停住但还画着
  const slow = await slowPair(cdp);
  const warm = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）慢图全图运行 ok", warm.status, "ok");
  const sampleArc = async () => {
    // 强制重算：慢图刚全跑过，不 force 的话 slow 命中缓存、根本不进 running
    await cdp.eval(`window.__lyflow.run({ targets: [${lit(slow.slow)}], force: [${lit(slow.slow)}] }); return true;`);
    const shot = await cdp.eval(`
      const t0 = performance.now();
      while (performance.now() - t0 < 20000) {
        const b = document.querySelector(${lit(btnSel(slow.slow))});
        const arc = b?.querySelector('.node-run__arc');
        if (b?.getAttribute('data-run-state') === 'running' && arc) {
          const cs = getComputedStyle(arc);
          const r = arc.getBoundingClientRect();
          return { animation: cs.animationName, dash: cs.strokeDasharray, visible: cs.visibility !== 'hidden' && cs.display !== 'none' && r.width > 0,
                   stroke: cs.stroke, transform: cs.transform, progress: b.getAttribute('data-run-progress') };
        }
        await new Promise((r) => requestAnimationFrame(r));
      }
      return null;
    `);
    const rows = await alignOf(cdp);
    await cdp.eval(`
      const { runId } = window.__lyflow.stores.execution.getState();
      if (runId) await window.__lyflow.transport.cancelRun(runId);
      return true;
    `);
    await waitRunEnd(cdp, "取消采样用的运行");
    return { shot, rows };
  };
  const on = await sampleArc();
  report.ok("（对照）动效开着时进度环在转（animation-name）", on.shot !== null && (on.shot.progress !== null || on.shot.animation === "lyflow-node-run-spin"),
    JSON.stringify(on.shot));
  report.ok(`运行中（进度环在画）：端点与锚点最大偏差 ${worst(on.rows)} px ≤ 1`, on.rows.length === 1 && worst(on.rows) <= 1, JSON.stringify(on.rows));

  await cdp.send("Emulation.setEmulatedMedia", { features: [{ name: "prefers-reduced-motion", value: "reduce" }] });
  try {
    await sleep(150);
    const off = await sampleArc();
    report.ok("关动效：进度环仍显示（有弧、有描边）", off.shot !== null && off.shot.visible && off.shot.dash !== "none", JSON.stringify(off.shot));
    report.eq("关动效：进度环不转（animation-name 为 none）", off.shot?.animation, "none");
  } finally {
    await cdp.send("Emulation.setEmulatedMedia", { features: [] });
    await sleep(150);
  }
}

// ------------------------------------ 验收 11b：计划外节点挂结果（R7）

async function suiteAttached(cdp, report) {
  report.section("单节点运行 验收 11b：单击中间节点后，下游仍是 done、Edge Peek 与 3D 视图照样取得到数据；从没跑过的下游保持 idle");
  await installRecorder(cdp);
  await newDoc(cdp);
  await resetPeek(cdp);
  // a → b → c → d：只运行 b，c、d 都在计划外。d 的入边 c→d 的源是 c —— 挂结果挂的就是它
  const ids = await buildGraph(cdp, [
    { key: "a", op: "gen.synthetic", params: { pointCount: 20000, seed: (Date.now() % 9973) + 5 } },
    { key: "b", op: "filter.voxel_grid" },
    { key: "c", op: "filter.passthrough" },
    { key: "d", op: "util.reroute" },
  ], [
    { from: ["a", "cloud"], to: ["b", "cloud"] },
    { from: ["b", "cloud"], to: ["c", "cloud"] },
    { from: ["c", "cloud"], to: ["d", "in"] },
  ]);
  await normalizeZoom(cdp, 0.7);
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.a]: { x: 20, y: 40 },
    [ids.b]: { x: Math.round(box.w * 0.26), y: 40 },
    [ids.c]: { x: Math.round(box.w * 0.52), y: 40 },
    [ids.d]: { x: Math.round(box.w * 0.78), y: 40 },
  });
  await sleep(400);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");
  const cCount = full.nodes[ids.c]?.elementCount ?? null;

  // 全图跑完之后才加的下游 e：它从没跑过
  const e = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const id = g.addNode('filter.passthrough', { x: 0, y: 0 });
    window.__lyflow.stores.graph.getState().connect({ node: ${lit(ids.c)}, port: 'cloud' }, { node: id, port: 'cloud' });
    return id;
  `);
  await placeAtScreen(cdp, { [e]: { x: Math.round(box.w * 0.52), y: Math.round(box.h * 0.45) } });
  await sleep(300);

  const run = await runAndWait(cdp, async () => click(cdp, await centerOf(cdp, btnSel(ids.b))));
  await sleep(200);
  const finished = await cdp.eval(`return window.__lyNodeRun.events.find((x) => x.kind === 'run_finished' && x.runId === ${lit(run.runId)}) ?? null;`);
  const trans = await transitionsBy(cdp);
  report.eq("（前提）只运行 b 的那次 ok", run.status, "ok");
  report.ok("run_finished.attached 含计划外的 c、d，不含从没跑过的 e",
    finished && [ids.c, ids.d].every((id) => finished.attached?.includes(id)) && !finished.attached.includes(e),
    JSON.stringify(finished?.attached));
  report.ok("c、d 没有执行（没有任何状态迁移，更没有 running）", !trans[ids.c] && !trans[ids.d], JSON.stringify(trans));
  report.eq("下游 c、d 仍是 done", [run.nodes[ids.c]?.state, run.nodes[ids.d]?.state], ["done", "done"]);
  report.eq("从没跑过的 e 保持 idle", run.nodes[e]?.state ?? "idle", "idle");

  // 按新 runId 真的取得到：先问 transport，再走真实的 3D 视图与 Edge Peek
  const info = await cdp.eval(`
    const { runId } = window.__lyflow.stores.execution.getState();
    return (await window.__lyflow.transport.getOutputInfo(runId, ${lit(ids.c)})).map((o) => [o.port, o.elementCount]);
  `);
  report.ok("getOutputInfo(新 runId, c) 有 cloud 输出", info.some(([p, n]) => p === "cloud" && n === cCount), JSON.stringify({ info, cCount }));

  const viewer = await selectAndReadViewer(cdp, ids.c);
  report.ok(`选中 c：3D 视图画出了点云（${viewer.count}/${viewer.total}）`, viewer.hasCanvas && viewer.count > 0 && viewer.total === cCount,
    JSON.stringify(viewer));
  await cdp.eval(`window.__lyflow.stores.ui.getState().clearSelection(); return true;`);

  const edge = await cdp.eval(`
    const doc = window.__lyflow.stores.graph.getState().doc;
    return doc.edges.find((x) => x.from.node === ${lit(ids.c)} && x.to.node === ${lit(ids.d)})?.id ?? null;
  `);
  const opened = await openByDoubleClick(cdp, report, edge, "d 的入边（源是 c）");
  report.ok("双击 d 的入边开出了 Edge Peek", Boolean(opened?.win), JSON.stringify(opened?.after));
  if (opened?.win) {
    await park(cdp);
    const dom = await waitPeek(cdp, opened.win.id, (x) => x.cloudCanvas && countsOf(x.countText));
    const counts = countsOf(dom?.countText);
    report.ok("Edge Peek 里是 c 的点云，总点数与 c 报的一致", dom?.cloudCanvas === true && counts?.total === cCount,
      JSON.stringify({ dom: dom && { status: dom.status, countText: dom.countText, cloudStatus: dom.cloudStatus }, cCount }));
    report.ok("Edge Peek 没有「未运行 / 取不到」之类的占位", dom?.status === null, String(dom?.status));
  }
  await resetPeek(cdp);

  // 结果仓里没有当前 cacheKey 的结果就不挂：改 c 的参数（c、d 的键都变了）再只运行 b，
  // c、d 退回 idle —— 不能显示「完成」却取不到输出
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.c)}, 'max', 50); return true;`);
  const again = await runAndWait(cdp, async () => click(cdp, await centerOf(cdp, btnSel(ids.b))));
  const fin2 = await cdp.eval(`return window.__lyNodeRun.events.find((x) => x.kind === 'run_finished' && x.runId === ${lit(again.runId)}) ?? null;`);
  report.eq("（前提）这次只运行 b 也 ok", again.status, "ok");
  report.ok("c、d 的键变了、仓里没有 → 不在 attached 里", fin2 && !fin2.attached.includes(ids.c) && !fin2.attached.includes(ids.d),
    JSON.stringify(fin2?.attached));
  report.eq("编辑器把 c、d 退回 idle", [again.nodes[ids.c]?.state ?? "idle", again.nodes[ids.d]?.state ?? "idle"], ["idle", "idle"]);
  // 智能运行（修订一 V3）：a、b 的键都没变，两个都命中缓存 —— 显示「已缓存」，不是 idle
  report.eq("a、b 命中缓存（skipped/cached），不受 c 的改动影响", [again.nodes[ids.a]?.state, again.nodes[ids.b]?.state, again.nodes[ids.b]?.cached],
    ["skipped", "skipped", true]);
}

// ------------------------------------------------------ 验收 12：右键菜单

async function openMenu(cdp, id) {
  return cdp.eval(`
    const el = document.querySelector('[data-testid="node-${id}"]');
    if (!el) return null;
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true,
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 }));
    await new Promise((r2) => setTimeout(r2, 150));
    const read = (tid) => {
      const item = document.querySelector('[data-testid="' + tid + '"]');
      return item ? { disabled: item.disabled, reason: item.getAttribute('data-run-reason'),
                      text: item.textContent.trim(), title: item.getAttribute('title') } : null;
    };
    const menu = document.querySelector('[data-testid="node-context-menu"]');
    const order = menu ? [...menu.querySelectorAll('button')].map((b) => b.getAttribute('data-testid')) : [];
    return { runTo: read('run-to-node'), force: read('ctx-force-node'), only: read('ctx-run-node-only'), order };
  `);
}

const closeMenu = async (cdp) => {
  await cdp.eval(`document.querySelector('.react-flow__pane')?.click(); return true;`);
  await sleep(100);
};

async function suiteMenu(cdp, report) {
  report.section("修订一 验收 19：右键三项 —— 运行到此 = 单击、强制重算此节点 = Shift+单击、仅此节点（用现有上游）");
  await installRecorder(cdp);
  const ids = await smallChain(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");
  await sleep(200);

  const items = await openMenu(cdp, ids.b);
  const at = (tid) => items.order.indexOf(tid);
  report.ok("三项都在、文案对", items.runTo?.text.startsWith("运行到此节点") && items.force?.text === "强制重算此节点" &&
    items.only?.text === "仅此节点（用现有上游）", JSON.stringify(items));
  report.ok("三项放在一起、依次排列", at("run-to-node") >= 0 && at("ctx-force-node") === at("run-to-node") + 1 &&
    at("ctx-run-node-only") === at("run-to-node") + 2, JSON.stringify(items.order));
  report.ok("全部就绪时三项都可点", !items.runTo.disabled && !items.force.disabled && !items.only.disabled, JSON.stringify(items));

  // 运行到此 = 单击：targets=[b]，b 命中缓存
  const runTo = await runAndWait(cdp, async () => click(cdp, await centerOf(cdp, '[data-testid="run-to-node"]')));
  const s1 = await startedOf(cdp, runTo.runId);
  const b1 = await statsOf(cdp, ids.b);
  report.ok("「运行到此」：targets=[b]、不带 force、b 命中缓存（与单击一致）", s1 && JSON.stringify(s1.targets) === JSON.stringify([ids.b]) &&
    s1.force.length === 0 && s1.isolate.length === 0 && b1?.cached === true, JSON.stringify({ s1: s1 && { targets: s1.targets, force: s1.force }, b1 }));

  // 强制重算此节点 = Shift+单击：targets=[b]、force=[b]，b 真跑、a 命中缓存
  await openMenu(cdp, ids.b);
  const forced = await runAndWait(cdp, async () => click(cdp, await centerOf(cdp, '[data-testid="ctx-force-node"]')));
  const s2 = await startedOf(cdp, forced.runId);
  const a2 = await statsOf(cdp, ids.a);
  const b2 = await statsOf(cdp, ids.b);
  report.ok("「强制重算此节点」：targets=[b]、force=[b]，b 真跑、a 命中缓存（与 Shift+单击一致）",
    s2 && JSON.stringify(s2.force) === JSON.stringify([ids.b]) && b2?.state === "done" && !b2.cached && a2?.cached === true,
    JSON.stringify({ s2: s2 && { targets: s2.targets, force: s2.force }, a2, b2 }));

  // 仅此节点：isolate=[b]，其余节点状态与耗时不变
  await sleep(200);
  const before = await statesOf(cdp, [ids.a, ids.c]);
  await openMenu(cdp, ids.b);
  const only = await runAndWait(cdp, async () => click(cdp, await centerOf(cdp, '[data-testid="ctx-run-node-only"]')));
  await sleep(200);
  const s3 = await startedOf(cdp, only.runId);
  const trans = await transitionsBy(cdp);
  report.ok("「仅此节点」：isolate=[b]、运行 ok", only.status === "ok" && s3 && JSON.stringify(s3.isolate) === JSON.stringify([ids.b]),
    JSON.stringify(s3?.isolate));
  report.eq("「仅此节点」：只有 b 的显示变了", Object.keys(trans).sort(), [ids.b]);
  report.eq("「仅此节点」：a、c 的状态与耗时不变", await statesOf(cdp, [ids.a, ids.c]), before);
  report.eq("菜单点完就收起", await cdp.eval(`return !!document.querySelector('[data-testid="node-context-menu"]');`), false);
}

export const nodeRunSuites = [
  suitePlacement,
  suiteSmart,
  suiteSmartUpstream,
  suiteIsolateOnly,
  suiteStopAndPreempt,
  suiteLook,
  suiteAttached,
  suiteMenu,
];
