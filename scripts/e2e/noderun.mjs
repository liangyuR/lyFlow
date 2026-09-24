// 「只运行此节点」的分组（docs/node-run-plan.md §4 验收 7–12）：标题栏按钮的位置与鼠标、
// 只重算一个节点、上游不齐时的预判与 core 兜底、运行中停止与抢占、hover / 关动效 / 端点对齐、
// 右键菜单。逐条结果见 docs/node-run-acceptance.md。
//
// 点按钮一律用 CDP 的真鼠标（Input.dispatchMouseEvent）：合成的 click 骗不过 React Flow 的
// 拖动/选中判定，也骗不过 CSS 的 :hover。

import { sleep } from "./cdp.mjs";
import { alignOf, emptySpot, installMotionProbe, moveMouse, worst } from "./motion.mjs";
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
} from "./page.mjs";

// ------------------------------------------------------------ 页面侧的小工具

const btnSel = (id) => `[data-testid="node-run-${id}"]`;

/** 真鼠标点一下（或双击）。clickCount 逐次递增，Chromium 才认得出是双击。 */
async function click(cdp, p, { clickCount = 1 } = {}) {
  await moveMouse(cdp, p);
  for (let i = 1; i <= clickCount; i += 1) {
    const common = { x: p.x, y: p.y, button: "left", buttons: 1, clickCount: i };
    await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", ...common });
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...common, buttons: 0 });
  }
}

/** 按钮的状态三件套。 */
const buttonOf = (cdp, id) =>
  cdp.eval(`
    const b = document.querySelector(${lit(btnSel(id))});
    if (!b) return null;
    return { state: b.getAttribute('data-run-state'), reason: b.getAttribute('data-run-reason'),
             own: b.getAttribute('data-run-own'), title: b.getAttribute('title') };
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

  // 源节点 a 永远可点：真鼠标单击 → 发起一次只跑 a 的运行；节点不被选中、不动、不改名
  await cdp.eval(`window.__lyflow.stores.ui.getState().clearSelection(); return true;`);
  const posBefore = await cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.a)}).ui.position;`);
  const at = await centerOf(cdp, btnSel(ids.a));
  const run = await runAndWait(cdp, () => click(cdp, at));
  const after = await cdp.eval(`
    const u = window.__lyflow.stores.ui.getState();
    return { selected: [...u.selectedNodes], renaming: !!document.querySelector('[data-testid^="node-rename-"]'),
             pos: window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.a)}).ui.position,
             isolate: window.__lyflow.stores.execution.getState().isolate };
  `);
  report.eq("（前提）单击按钮发起了只跑 a 的运行", after.isolate, [ids.a]);
  report.eq("那次运行 ok", run.status, "ok");
  report.eq("单击按钮：节点没有被选中", after.selected, []);
  report.eq("单击按钮：没有进入改名", after.renaming, false);
  report.eq("单击按钮：节点位置没动", after.pos, posBefore);

  // c 的上游 b 从没跑过：c 的按钮不可用。按住它拖 60 px —— 不开始拖节点（nodrag）
  const cBefore = await cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.c)}).ui.position;`);
  report.eq("（前提）c 的按钮不可用（上游 b 还没有结果）", (await buttonOf(cdp, ids.c))?.state, "disabled");
  const from = await centerOf(cdp, btnSel(ids.c));
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
    return { pos: window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.c)}).ui.position,
             selected: [...window.__lyflow.stores.ui.getState().selectedNodes] };
  `);
  report.ok("按住按钮拖动：节点不跟着走、也没被选中", JSON.stringify(dragged.pos) === JSON.stringify(cBefore) &&
    !dragged.selected.includes(ids.c), JSON.stringify({ cBefore, dragged }));

  // 双击按钮：不进改名、不选中；不可用的按钮也不发起运行
  const runIdBefore = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  await click(cdp, await centerOf(cdp, btnSel(ids.c)), { clickCount: 2 });
  await sleep(300);
  const dbl = await cdp.eval(`
    return { renaming: !!document.querySelector('[data-testid^="node-rename-"]'),
             selected: [...window.__lyflow.stores.ui.getState().selectedNodes],
             runId: window.__lyflow.stores.execution.getState().runId };
  `);
  report.ok("双击按钮：没有进入改名、节点没被选中", !dbl.renaming && !dbl.selected.includes(ids.c), JSON.stringify(dbl));
  report.eq("点不可用的按钮（双击）：没有发起运行", dbl.runId, runIdBefore);
}

// ------------------------------------------- 验收 8：全图跑过后只重算中间节点

async function suiteOnlyThis(cdp, report) {
  report.section("单节点运行 验收 8：全图跑过后点中间节点 —— 只有它 running → done，其余状态与耗时不变，下游不进计划");
  await installRecorder(cdp);
  const ids = await smallChain(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");
  await sleep(300);
  const before = await statesOf(cdp, [ids.a, ids.c]);
  report.eq("（前提）b 的按钮可点（上游 a 已有结果）", (await buttonOf(cdp, ids.b))?.state, "done");

  const bAt = await centerOf(cdp, btnSel(ids.b));
  const run = await runAndWait(cdp, () => click(cdp, bAt));
  await sleep(200);
  const trans = await transitionsBy(cdp);
  const after = await statesOf(cdp, [ids.a, ids.c]);
  const started = await startedOf(cdp, run.runId);
  report.eq("这次运行 ok", run.status, "ok");
  report.eq("只有 b 经历了状态变化", Object.keys(trans).sort(), [ids.b]);
  report.ok("b 经历了 running → done", (trans[ids.b] ?? []).join(",").includes("running,done"), JSON.stringify(trans));
  report.eq("a、c 的状态与耗时不变", after, before);
  report.ok("run_started.isolate = [b]、mode 仍是 full", started && JSON.stringify(started.isolate) === JSON.stringify([ids.b]) && started.mode === "full",
    JSON.stringify(started && { isolate: started.isolate, mode: started.mode }));
  report.ok("下游 c 不在计划里，上游 a 在（只取缓存）",
    started && !started.plan.includes(ids.c) && started.plan.includes(ids.a), JSON.stringify(started?.plan));
  report.eq("b 的按钮回到 done（绿圈）", (await buttonOf(cdp, ids.b))?.state, "done");
}

// ---------------------------------- 验收 9：上游不齐 —— 预判置灰、core 兜底报错

async function suiteNotReady(cdp, report) {
  report.section("单节点运行 验收 9：改上游参数 → 按钮 disabled 且写明缺谁；绕过预判直接跑 → upstream_not_ready toast、零 running");
  await installMotionProbe(cdp);
  await installRecorder(cdp);
  const ids = await smallChain(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");

  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.a)}, 'pointCount', 23456); return true;`);
  const cache = await replan(cdp);
  report.ok("（前提）a 被标为 stale", cache.stale.includes(ids.a), JSON.stringify(cache.stale));
  const b = await buttonOf(cdp, ids.b);
  report.eq("b 的按钮 data-run-state=\"disabled\"", b?.state, "disabled");
  report.ok("data-run-reason 含 a 的 id", String(b?.reason).split(",").includes(ids.a), JSON.stringify(b));
  report.ok("title 写明上游还没有可用结果", /还没有可用结果/.test(b?.title ?? "") && /运行到此/.test(b?.title ?? ""), b?.title);
  const cursor = await cdp.eval(`return getComputedStyle(document.querySelector(${lit(btnSel(ids.b))})).cursor;`);
  report.eq("不可用时光标 not-allowed", cursor, "not-allowed");
  report.eq("源节点 a 自己的按钮永远可点", (await buttonOf(cdp, ids.a))?.state === "disabled", false);

  // 真鼠标点不可用的按钮：什么都不发生
  const runIdBefore = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  await click(cdp, await centerOf(cdp, btnSel(ids.b)));
  await sleep(300);
  report.eq("点不可用的按钮不发起运行", await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`), runIdBefore);

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
  const finished = await cdp.eval(`return window.__lyNodeRun.events.find((e) => e.kind === 'run_finished' && e.runId === ${lit(run.runId)}) ?? null;`);
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
  report.section("单节点运行 验收 10：运行中按钮 running、点它取消；全图运行里 running 的节点上点按钮是抢占，不是停止");
  await installRecorder(cdp);
  const ids = await slowPair(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");
  const slowMs = full.nodes[ids.slow]?.durationMs ?? 0;
  report.ok(`（前提）slow 节点够慢（${Math.round(slowMs)} ms ≥ 300）`, slowMs >= 300, JSON.stringify(full.nodes[ids.slow]));

  // 自己发起的那次：running + ■，再点一下 = 停止
  const before = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  await click(cdp, await centerOf(cdp, btnSel(ids.slow)));
  const running = await waitButton(cdp, ids.slow, "(s) => s.state === 'running' && s.own === '1'");
  report.ok("点了之后按钮 data-run-state=\"running\"、带 ■（data-run-own=1）", running !== null, JSON.stringify(running));
  const title = (await buttonOf(cdp, ids.slow))?.title;
  report.eq("运行中 title 是「停止」", title, "停止");
  const stopAt = await cdp.eval(`
    const s = window.__lyflow.stores.execution.getState();
    return { runId: s.runId, status: s.runStatus, isolate: s.isolate,
             stop: !!document.querySelector(${lit(btnSel(ids.slow) + " .node-run__stop")}) };
  `);
  report.ok("（前提）这时跑的是 isolate=[slow] 的那一次", stopAt.runId !== before && stopAt.status === "running" &&
    JSON.stringify(stopAt.isolate) === JSON.stringify([ids.slow]) && stopAt.stop, JSON.stringify(stopAt));
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
  const fullRun = await cdp.eval(`const s = window.__lyflow.stores.execution.getState(); return { runId: s.runId, isolate: s.isolate };`);
  report.ok("（前提）slow 在全图运行里 running，按钮也显示 running 但没有 ■", inFull !== null && inFull.own === null &&
    fullRun.isolate.length === 0, JSON.stringify({ inFull, fullRun }));
  await click(cdp, await centerOf(cdp, btnSel(ids.slow)));
  const preempt = await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runId !== ${lit(fullRun.runId)} && s.isolate.length === 1 ? { runId: s.runId, isolate: s.isolate } : null; })()`,
    { timeoutMs: 30_000, what: "抢占后的单节点运行开始" },
  );
  report.eq("点击发起了新的单节点运行（isolate=[slow]）", preempt.isolate, [ids.slow]);
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
    await cdp.eval(`window.__lyflow.run({ isolate: [${lit(slow.slow)}] }); return true;`);
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

// ------------------------------------------------------ 验收 12：右键菜单

async function openMenu(cdp, id) {
  return cdp.eval(`
    const el = document.querySelector('[data-testid="node-${id}"]');
    if (!el) return null;
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true,
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 }));
    await new Promise((r2) => setTimeout(r2, 150));
    const item = document.querySelector('[data-testid="ctx-run-node-only"]');
    return item ? { disabled: item.disabled, reason: item.getAttribute('data-run-reason'), text: item.textContent.trim(),
                    title: item.getAttribute('title') } : null;
  `);
}

async function suiteMenu(cdp, report) {
  report.section("单节点运行 验收 12：右键「只运行此节点」—— 与按钮同一动作、同一可用性");
  await installRecorder(cdp);
  const ids = await smallChain(cdp);
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("（前提）全图运行 ok", full.status, "ok");
  await sleep(200);
  const before = await statesOf(cdp, [ids.a, ids.c]);

  const item = await openMenu(cdp, ids.b);
  report.ok("右键菜单里有「只运行此节点」且可点", item && !item.disabled && item.text === "只运行此节点", JSON.stringify(item));
  const at = await centerOf(cdp, '[data-testid="ctx-run-node-only"]');
  const run = await runAndWait(cdp, () => click(cdp, at));
  await sleep(200);
  const trans = await transitionsBy(cdp);
  const started = await startedOf(cdp, run.runId);
  report.eq("成功路径：运行 ok", run.status, "ok");
  report.ok("成功路径：与按钮一样是 isolate=[b]", started && JSON.stringify(started.isolate) === JSON.stringify([ids.b]),
    JSON.stringify(started?.isolate));
  report.eq("成功路径：只有 b 变了", Object.keys(trans).sort(), [ids.b]);
  report.eq("成功路径：a、c 的状态与耗时不变", await statesOf(cdp, [ids.a, ids.c]), before);
  report.eq("菜单点完就收起", await cdp.eval(`return !!document.querySelector('[data-testid="node-context-menu"]');`), false);

  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.a)}, 'pointCount', 34567); return true;`);
  await replan(cdp);
  const disabled = await openMenu(cdp, ids.b);
  const button = await buttonOf(cdp, ids.b);
  report.ok("不可用路径：菜单项 disabled，data-run-reason 含 a", disabled?.disabled === true && String(disabled.reason).split(",").includes(ids.a),
    JSON.stringify(disabled));
  report.ok("不可用路径：与按钮的判定一致", button?.state === "disabled" && button.reason === disabled?.reason, JSON.stringify({ button, disabled }));
  await cdp.eval(`document.querySelector('.react-flow__pane')?.click(); return true;`);
  await sleep(100);
}

export const nodeRunSuites = [
  suitePlacement,
  suiteOnlyThis,
  suiteNotReady,
  suiteStopAndPreempt,
  suiteLook,
  suiteMenu,
];
