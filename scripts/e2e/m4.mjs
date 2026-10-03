// M4 验收：子图（§1）、live preview（§2）、大图性能（§4）。CLI（§3）由 cargo test
// 覆盖，理由见 ../../docs/m4-acceptance.md。与 M2/M3 的分组共用一个 app 实例。

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { ROOT } from "./harness.mjs";
import {
  buildGraph,
  centerOf,
  clickAt,
  dragMouse,
  lit,
  mustOk,
  newDoc,
  pressCtrl,
  pressEscape,
  pressF5,
  pressKey,
  replan,
  restoreClipboard,
  setViewport,
  runAndWait,
  select,
  selectAndReadViewer,
  stubClipboard,
} from "./page.mjs";

/** 一条五节点直链：生成 → 裁剪 → 体素 → 去噪 → 直通。合成的对象是中间三个。 */
const CHAIN_NODES = [
  { key: "gen", op: "gen.synthetic", params: { pointCount: 60000, seed: 91 } },
  { key: "crop", op: "filter.crop_box" },
  { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
  { key: "sor", op: "filter.statistical_outlier", params: { meanK: 20 } },
  { key: "tail", op: "filter.passthrough", params: { min: -100, max: 100 } },
];
const CHAIN_EDGES = [
  { from: ["gen", "cloud"], to: ["crop", "cloud"] },
  { from: ["crop", "cloud"], to: ["voxel", "cloud"] },
  { from: ["voxel", "cloud"], to: ["sor", "cloud"] },
  { from: ["sor", "cloud"], to: ["tail", "cloud"] },
];

const snapshot = (cdp) => cdp.eval(`return window.__lyflow.snapshot();`);

/** 走 store 合成子图。右键菜单那条路在 §1.4 的分组里单独验。 */
async function compose(cdp, ids) {
  return cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().setSelection(${lit(ids)}, []);
    return b.stores.graph.getState().composeSubgraph(${lit(ids)});
  `);
}

/** 双击节点**身体**进子图。标题那一片双击是改名（P1 #25），别打在那儿。 */
async function enterByDoubleClick(cdp, nodeId) {
  return cdp.eval(`
    const el = document.querySelector('[data-testid="node-${nodeId}"]');
    if (!el) return 'no-node';
    const r = el.getBoundingClientRect();
    const x = r.left + r.width / 2;
    const y = r.top + r.height * 0.75;
    for (const type of ['mousedown', 'mouseup', 'click', 'dblclick']) {
      el.dispatchEvent(new MouseEvent(type, {
        bubbles: true, cancelable: true, view: window,
        detail: type === 'dblclick' ? 2 : 1, clientX: x, clientY: y,
      }));
    }
    await new Promise((done) => setTimeout(done, 250));
    return window.__lyflow.stores.ui.getState().path.length;
  `);
}

// -------------------------------------------------------- §1.1 合成与展开

async function composeChecks(cdp, report) {
  report.section("§1 子图：合成之后结果一模一样，事件里是路径式 id");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);

  // 右键「选中上游 / 选中下游」：沿连线把整条链选上（合成、静音、整理之前先这样圈出来）
  const selectVia = (nodeId, item) => cdp.eval(`
    const u = window.__lyflow.stores.ui.getState();
    u.setSelection([], []);
    const el = document.querySelector('[data-testid="node-${nodeId}"]');
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true, clientX: r.left + r.width / 2, clientY: r.top + r.height / 2,
    }));
    await new Promise((d) => setTimeout(d, 150));
    const btn = document.querySelector('[data-testid="${item}"]');
    if (!btn) return 'no-menu-item';
    btn.click();
    await new Promise((d) => setTimeout(d, 150));
    return [...window.__lyflow.stores.ui.getState().selectedNodes].sort();
  `);
  report.eq("右键 voxel →「选中上游」：gen、crop 与它自己", await selectVia(ids.voxel, "ctx-select-upstream"),
    [ids.gen, ids.crop, ids.voxel].sort());
  report.eq("右键 sor →「选中下游」：它自己与 tail", await selectVia(ids.sor, "ctx-select-downstream"),
    [ids.sor, ids.tail].sort());
  // 右键「对准选中的节点」（与 F 同一档）：菜单里有这一项、标着 F；点了菜单收起，选中的那段（上一步选的 sor 与 tail）
  // 落到画布中间
  const fit = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${ids.sor}"]');
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: r.left + 10, clientY: r.top + 10 }));
    await new Promise((d) => setTimeout(d, 150));
    const btn = document.querySelector('[data-testid="ctx-fit-selection"]');
    if (!btn) return null;
    const key = btn.querySelector('kbd')?.textContent ?? '';
    btn.click();
    await new Promise((d) => setTimeout(d, 600));
    const pane = document.querySelector('.react-flow__pane').getBoundingClientRect();
    const a = document.querySelector('[data-testid="node-${ids.sor}"]').getBoundingClientRect();
    const b = document.querySelector('[data-testid="node-${ids.tail}"]').getBoundingClientRect();
    const mid = (Math.min(a.left, b.left) + Math.max(a.right, b.right)) / 2;
    return { key, menuClosed: !document.querySelector('[data-testid="ctx-fit-selection"]'),
             dx: Math.round(mid - (pane.left + pane.width / 2)) };
  `);
  report.ok("右键「对准选中的节点」（标着 F）：点了菜单收起、视图对准它",
    fit?.key === "F" && fit.menuClosed && Math.abs(fit.dx) < 60, JSON.stringify(fit));
  await cdp.eval(`window.__lyflow.stores.ui.getState().setSelection([], []); return true;`);

  const flat = await runAndWait(cdp, () => pressF5(cdp));
  const flatTail = flat.nodes[ids.tail]?.elementCount;
  const flatVoxel = flat.nodes[ids.voxel]?.elementCount;
  report.ok("末端有点数", flatTail > 0, `tail=${flatTail} flat.status=${flat.status}`);

  const composed = await compose(cdp, [ids.crop, ids.voxel, ids.sor]);
  mustOk(Boolean(composed?.nodeId), "composeSubgraph 返回了新节点", JSON.stringify(composed));

  const after = await snapshot(cdp);
  report.ok(
    "顶层剩下 gen / 子图 / tail 三个节点",
    after.level.nodes.length === 3 &&
      after.level.nodes.includes(ids.gen) &&
      after.level.nodes.includes(ids.tail) &&
      after.level.nodes.includes(composed.nodeId),
    JSON.stringify(after.level.nodes),
  );
  const def = after.subgraphs[composed.subgraphId];
  report.eq("子图收了三个节点", def.nodes.length, 3);
  report.eq("跨边界的入边变成了一个输入端口", def.inputs.length, 1);
  report.eq("跨边界的出边变成了一个输出端口", def.outputs.length, 1);
  report.eq("子图节点的 op 是 sub: 引用", after.doc.nodes.find((n) => n.id === composed.nodeId).op,
    `sub:${composed.subgraphId}`);

  // 展开后的计划：事件 id 是路径，子图这个节点本身不在计划里（F1/F2）
  const plan = await replan(cdp);
  const planIds = Object.keys(plan.plan);
  report.ok(
    "计划里是展开后的路径 id",
    planIds.includes(`${composed.nodeId}/${ids.voxel}`),
    JSON.stringify(planIds),
  );
  report.ok("子图节点本身不在计划里", !planIds.includes(composed.nodeId), JSON.stringify(planIds));

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("末端点数与合成前完全一致", run.nodes[ids.tail]?.elementCount, flatTail);
  report.eq(
    "子图内部的体素点数也一致",
    run.nodes[`${composed.nodeId}/${ids.voxel}`]?.elementCount,
    flatVoxel,
  );
  report.ok(
    "子图内部全部命中缓存（合成不改变 cacheKey 以外的东西）",
    ["skipped", "done"].includes(run.nodes[`${composed.nodeId}/${ids.sor}`]?.state),
    JSON.stringify(run.nodes[`${composed.nodeId}/${ids.sor}`]),
  );

  // 画布上的子图节点要显示聚合状态（F2）
  const dom = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${composed.nodeId}"]');
    return el ? {
      state: el.getAttribute('data-node-state'),
      subgraph: el.getAttribute('data-subgraph'),
      badge: !!document.querySelector('[data-testid="node-subbadge-${composed.nodeId}"]'),
      children: document.querySelector('[data-testid="node-children-${composed.nodeId}"]')?.textContent ?? null,
    } : null;
  `);
  report.ok("子图节点带子图角标", dom?.badge === true, JSON.stringify(dom));
  report.eq("data-subgraph 指向定义", dom?.subgraph, composed.subgraphId);
  report.ok(
    "状态聚合成一个（全 done/skipped → done/skipped）",
    ["done", "skipped"].includes(dom?.state ?? ""),
    JSON.stringify(dom),
  );
  report.eq("角标显示内部进度 3/3", dom?.children, "3/3");

  return { ids, composed, flatTail };
}

// ------------------------------------------------------ §1.4 进入 / 退出

async function suiteNavigate(cdp, report, fixture) {
  report.section("§1 子图导航：双击进入、面包屑、Esc 退出");

  const { ids, composed } = fixture;
  const depth = await enterByDoubleClick(cdp, composed.nodeId);
  report.eq("双击子图节点进去了", depth, 1);

  const inside = await snapshot(cdp);
  const rendered = await cdp.eval(`
    return [...document.querySelectorAll('[data-testid^="node-n_"]')]
      .map((el) => el.getAttribute('data-testid').slice(5));
  `);
  report.ok(
    "当前层级与画布上渲染的都是子图的三个内部节点",
    inside.level.nodes.length === 3 && rendered.length === 3 && rendered.includes(ids.voxel),
    `level=${JSON.stringify(inside.level.nodes)} 画布=${JSON.stringify(rendered)}`,
  );
  report.eq("事件前缀是路径", inside.pathPrefix, `${composed.nodeId}/`);

  const crumb = await cdp.eval(`
    const el = document.querySelector('[data-testid="breadcrumb"]');
    return el ? { depth: el.getAttribute('data-depth'), text: el.textContent } : null;
  `);
  report.eq("面包屑显示深度 1", crumb?.depth, "1");
  report.ok("面包屑里有子图名", (crumb?.text ?? "").includes("子图"), crumb?.text);

  // 进了子图之后，内部节点的状态来自路径事件
  const innerState = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${ids.voxel}"]');
    return el ? el.getAttribute('data-node-state') : null;
  `);
  report.ok(
    "内部节点显示自己的执行状态",
    ["done", "skipped"].includes(innerState ?? ""),
    String(innerState),
  );

  // 3D 视图：选中内部节点看得到输出（路径 id 直接查结果仓）
  const view = await selectAndReadViewer(cdp, ids.voxel);
  report.ok("子图里选中体素能看到点云", view.count > 0, JSON.stringify(view));

  await pressEscape(cdp);
  await sleep(200);
  const out = await snapshot(cdp);
  report.eq("Esc 退回顶层", out.path.length, 0);

  // 面包屑那条路也要能用
  await enterByDoubleClick(cdp, composed.nodeId);
  await clickAt(cdp, await centerOf(cdp, '[data-testid="breadcrumb-root"]'));
  await sleep(250);
  const viaCrumb = await snapshot(cdp);
  report.eq("点面包屑的「顶层」也能退出，选中刚出来的子图节点", { depth: viaCrumb.path.length, selected: viaCrumb.selected },
    { depth: 0, selected: [composed.nodeId] });
}

// ------------------------------------------------------- §1.4 提升参数

async function suitePromote(cdp, report, fixture) {
  report.section("§1 提升参数：内参变只读，改外参只重算受影响的内部节点");

  const { ids, composed } = fixture;
  await enterByDoubleClick(cdp, composed.nodeId);
  await select(cdp, ids.voxel);
  await sleep(150);

  // 走真实的参数右键菜单
  const promoted = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-leafSize"]');
    if (!row) return 'no-row';
    const r = row.getBoundingClientRect();
    row.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true, clientX: r.left + 10, clientY: r.top + 10,
    }));
    await new Promise((done) => setTimeout(done, 120));
    const btn = document.querySelector('[data-testid="param-menu-promote"]');
    if (!btn) return 'no-promote';
    btn.click();
    await new Promise((done) => setTimeout(done, 150));
    return window.__lyflow.snapshot().subgraphs[${lit(composed.subgraphId)}].params;
  `);
  report.ok(
    "右键菜单里提升成功",
    Array.isArray(promoted) && promoted.length === 1 && promoted[0].name === "leafSize",
    JSON.stringify(promoted),
  );
  report.eq(
    "绑定指到内部节点的那个参数",
    promoted[0]?.binds,
    [{ node: ids.voxel, param: "leafSize" }],
  );

  const readOnly = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-leafSize"]');
    const input = row.querySelector('input');
    return { promotedAs: row.getAttribute('data-promoted'), disabled: input?.disabled ?? null };
  `);
  report.eq("内参标注了提升来源", readOnly.promotedAs, "leafSize");
  report.eq("内参变成只读", readOnly.disabled, true);

  await pressEscape(cdp);
  await sleep(200);

  // 外层表单上出现了这个参数
  await select(cdp, composed.nodeId);
  await sleep(200);
  const outerForm = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-leafSize"]');
    return { exists: !!row, disabled: row?.querySelector('input')?.disabled ?? null };
  `);
  report.ok("外层节点的表单上有这个参数", outerForm.exists, JSON.stringify(outerForm));
  report.eq("外层可编辑", outerForm.disabled, false);

  // 跑一次垫底，然后只改这个提升参数
  await runAndWait(cdp, () => pressF5(cdp));
  await cdp.eval(`
    window.__lyflow.stores.graph.getState()
      .setParam(${lit(composed.nodeId)}, 'leafSize', [0.05, 0.05, 0.05]);
    return true;
  `);
  const plan = await replan(cdp);
  const cachedOf = (id) => plan.plan[id]?.cached;
  report.eq("源头仍然命中缓存", cachedOf(ids.gen), true);
  report.eq("子图里的裁剪也命中缓存", cachedOf(`${composed.nodeId}/${ids.crop}`), true);
  report.eq("被改的体素要重算", cachedOf(`${composed.nodeId}/${ids.voxel}`), false);
  report.eq("它的下游也要重算", cachedOf(`${composed.nodeId}/${ids.sor}`), false);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("改完还能跑通", run.status, "ok");
  report.eq("源头是 skipped", run.nodes[ids.gen]?.state, "skipped");
  report.eq("体素是 done", run.nodes[`${composed.nodeId}/${ids.voxel}`]?.state, "done");
}

// --------------------------------------------------------- §1 嵌套与递归

async function suiteNested(cdp, report) {
  report.section("§1 嵌套两层：cacheKey 稳定，重跑全 skipped");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES.slice(0, 4), CHAIN_EDGES.slice(0, 3));

  const inner = await compose(cdp, [ids.voxel, ids.sor]);
  const outer = await compose(cdp, [ids.crop, inner.nodeId]);
  mustOk(Boolean(inner?.nodeId && outer?.nodeId), "两层都合成出来了",
    JSON.stringify({ inner, outer }));

  const plan = await replan(cdp);
  const nested = `${outer.nodeId}/${inner.nodeId}/${ids.voxel}`;
  report.ok("计划里出现两层路径", Object.keys(plan.plan).includes(nested),
    JSON.stringify(Object.keys(plan.plan)));

  const first = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("第一次跑通", first.status, "ok");

  // 日志页（日志只留最近一次运行的，所以接在真算了一遍的这次后面）：两层子图里的 sor 写的那条日志，节点写名字（带它在哪两层），按内容筛得出来，点它打开到那一层
  const sorPath = `${outer.nodeId}/${inner.nodeId}/${ids.sor}`;
  await cdp.eval(`window.__lyflow.stores.ui.getState().toggleDrawer('log'); return true;`);
  await sleep(200);
  await cdp.eval(`
    const input = document.querySelector('[data-testid="drawer-log-filter"]');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
    setter.call(input, '离群点');
    input.dispatchEvent(new Event('input', { bubbles: true }));
    return true;
  `);
  await sleep(200);
  const logRow = await cdp.eval(`
    const rows = [...document.querySelectorAll('[data-testid="drawer-logs"] .drawer__log')];
    const hit = rows.find((r) => r.querySelector('.drawer__log-node')?.title === ${lit(sorPath)});
    return { rows: rows.length, name: hit?.querySelector('.drawer__log-node')?.textContent ?? null,
             text: hit?.querySelector('.drawer__log-text')?.textContent ?? null };
  `);
  report.ok("日志按内容筛出 sor 那条，节点写的是带层级的名字",
    logRow.rows >= 1 && (logRow.name ?? "").split(" › ").length === 3 && /离群点/.test(logRow.text ?? ""), JSON.stringify(logRow));
  await cdp.eval(`
    const rows = [...document.querySelectorAll('[data-testid="drawer-logs"] .drawer__log-node')];
    rows.find((b) => b.title === ${lit(sorPath)})?.click();
    return true;
  `);
  await sleep(300);
  report.eq("点日志里的节点名：打开到它所在的那一层、选中它", await cdp.eval(`
    const s = window.__lyflow.stores.ui.getState();
    return { path: s.path.map((p) => p.nodeId), selected: [...s.selectedNodes] };
  `), { path: [outer.nodeId, inner.nodeId], selected: [ids.sor] });
  // 在日志里点一下、选上那一行字，再按 Ctrl+C / Ctrl+X：归浏览器（复制的是那段文字），
  // 选中的节点不进剪贴板、也不被剪掉。测试自己的 copy / cut 监听取消默认动作，不动用户的剪贴板
  await stubClipboard(cdp);
  await cdp.eval(`
    [...document.querySelectorAll('[data-testid="drawer-logs"] .drawer__log')]
      .find((r) => r.querySelector('.drawer__log-node')?.title === ${lit(sorPath)})
      ?.querySelector('.drawer__log-text')?.setAttribute('data-ly-probe', '');
    return true;
  `);
  const logText = await centerOf(cdp, "[data-ly-probe]");
  mustOk(logText != null, "日志里那一行的文字", logText);
  await clickAt(cdp, logText);
  const levelCount = () => cdp.eval(`return window.__lyflow.snapshot().level.nodes.length;`);
  const textCopyBefore = {
    nodes: await levelCount(),
    app: await cdp.eval(`return JSON.stringify(window.__lyflow.stores.ui.getState().clipboard);`),
  };
  await cdp.eval(`
    const t = document.querySelector('[data-ly-probe]');
    const range = document.createRange();
    range.selectNodeContents(t);
    getSelection().removeAllRanges();
    getSelection().addRange(range);
    window.__lyCopyEvents = [];
    window.__lyOnCopy = (e) => { window.__lyCopyEvents.push(e.type + ':' + getSelection().toString()); e.preventDefault(); };
    document.addEventListener('copy', window.__lyOnCopy);
    document.addEventListener('cut', window.__lyOnCopy);
    return true;
  `);
  await pressCtrl(cdp, "c");
  await sleep(150);
  await pressCtrl(cdp, "x");
  await sleep(150);
  const textCopy = await cdp.eval(`
    document.removeEventListener('copy', window.__lyOnCopy);
    document.removeEventListener('cut', window.__lyOnCopy);
    getSelection().removeAllRanges();
    document.querySelector('[data-ly-probe]')?.removeAttribute('data-ly-probe');
    return { events: window.__lyCopyEvents, written: window.__lyClip,
             app: JSON.stringify(window.__lyflow.stores.ui.getState().clipboard) };
  `);
  report.ok("日志里选着一段文字时 Ctrl+C / Ctrl+X 归浏览器：复制的是那段文字，节点没进剪贴板、也没被剪掉",
    textCopy.events.length === 2 && /^copy:.*离群点/.test(textCopy.events[0]) && textCopy.written === "" &&
      textCopy.app === textCopyBefore.app && (await levelCount()) === textCopyBefore.nodes,
    JSON.stringify({ ...textCopy, before: textCopyBefore }));
  // 再选上那段文字、然后在画布上点那个节点：点节点（拖也一样）不清掉旧的文字选区（点空白处才清），
  // 这时 Ctrl+C 要的是节点 —— 所以不能只看「页面上有没有选区」
  await cdp.eval(`
    const t = [...document.querySelectorAll('[data-testid="drawer-logs"] .drawer__log')]
      .find((r) => r.querySelector('.drawer__log-node')?.title === ${lit(sorPath)})?.querySelector('.drawer__log-text');
    const range = document.createRange();
    range.selectNodeContents(t);
    getSelection().removeAllRanges();
    getSelection().addRange(range);
    return true;
  `);
  const sorOnCanvas = await centerOf(cdp, `[data-testid="node-${ids.sor}"]`);
  mustOk(sorOnCanvas != null, "画布上的 sor 节点", sorOnCanvas);
  await clickAt(cdp, sorOnCanvas);
  await pressCtrl(cdp, "c");
  await sleep(150);
  const afterNodeClick = await cdp.eval(`
    const stale = getSelection().toString();
    getSelection().removeAllRanges();
    let ops = null;
    try { ops = JSON.parse(window.__lyClip).nodes.map((n) => n.op); } catch {}
    return { stale: /离群点/.test(stale), ops };
  `);
  await restoreClipboard(cdp);
  report.ok("文字还选着、但刚在画布上点了节点：Ctrl+C 复制的是节点", afterNodeClick.stale &&
    JSON.stringify(afterNodeClick.ops) === JSON.stringify(["filter.statistical_outlier"]), JSON.stringify(afterNodeClick));
  await cdp.eval(`const u = window.__lyflow.stores.ui.getState(); u.exitTo(0); u.toggleDrawer(); return true;`);

  const keys1 = JSON.stringify((await snapshot(cdp)).cache.ranWith);

  const second = await runAndWait(cdp, () => pressF5(cdp));
  const states = Object.values(second.nodes).map((n) => n.state);
  report.ok("重跑全部 skipped", states.every((s) => s === "skipped"), JSON.stringify(second.nodes));
  const keys2 = JSON.stringify((await snapshot(cdp)).cache.ranWith);
  report.eq("两次编译出来的 cacheKey 一模一样", keys1, keys2);

  // 进两层再出来
  await enterByDoubleClick(cdp, outer.nodeId);
  await enterByDoubleClick(cdp, inner.nodeId);
  const deep = await snapshot(cdp);
  report.eq("能进到第二层", deep.path.length, 2);
  report.eq("第二层的事件前缀是两段", deep.pathPrefix, `${outer.nodeId}/${inner.nodeId}/`);
  // 子图里复制粘贴：复制的是这一层的节点（以前取的是顶层的 doc.nodes，在这里一个都拿不到）
  const levelOps = () => cdp.eval(`return window.__lyflow.snapshot().level.nodes.map((id) =>
    window.__lyflow.stores.graph.getState().doc.subgraphs[${lit(inner.subgraphId)}].nodes.find((n) => n.id === id)?.op ?? null);`);
  const opsBefore = await levelOps();
  await stubClipboard(cdp);
  await cdp.eval(`
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    window.__lyflow.stores.ui.getState().setSelection([${lit(ids.voxel)}], []);
    return true;
  `);
  await pressCtrl(cdp, "c");
  await sleep(150);
  await pressCtrl(cdp, "v");
  await sleep(400);
  await restoreClipboard(cdp);
  const opsAfter = await levelOps();
  const pasted = opsAfter.length === opsBefore.length + 1;
  report.ok("在第二层里复制粘贴：多出来的是这一层的那个体素节点", pasted &&
    opsAfter.filter((o) => o === "filter.voxel_grid").length === opsBefore.filter((o) => o === "filter.voxel_grid").length + 1,
    JSON.stringify({ opsBefore, opsAfter }));
  // 没粘上就别撤：撤掉的会是外面那次合成子图，后面几条全跟着错
  if (pasted) {
    await pressCtrl(cdp, "z");
    await sleep(200);
  }
  // 这一层的 Ctrl+A / Ctrl+M / Ctrl+E：以前在顶层找节点 —— 全选选上的是这一层没有的 id，
  // 静音与折叠永远是「打开」，再按一次也取消不了
  await pressCtrl(cdp, "a");
  await sleep(150);
  const selectedAll = await cdp.eval(`return [[...window.__lyflow.stores.ui.getState().selectedNodes].sort(),
    [...window.__lyflow.snapshot().level.nodes].sort()];`);
  report.eq("在第二层里 Ctrl+A：选上的是这一层的全部节点", selectedAll[0], selectedAll[1]);
  const innerFlags = () => cdp.eval(`return window.__lyflow.stores.graph.getState().doc.subgraphs[${lit(inner.subgraphId)}].nodes
    .map((n) => [n.bypass === true, n.ui?.collapsed === true]);`);
  // 选区自己设（不靠上面那次 Ctrl+A），这一条只钉「第一个选中节点的状态从哪一层找」
  await cdp.eval(`window.__lyflow.stores.ui.getState().setSelection(window.__lyflow.snapshot().level.nodes, []); return true;`);
  const flagsBefore = await innerFlags();
  await pressCtrl(cdp, "m");
  await sleep(150);
  await pressCtrl(cdp, "e");
  await sleep(150);
  const flagsOn = await innerFlags();
  await pressCtrl(cdp, "m");
  await sleep(150);
  await pressCtrl(cdp, "e");
  await sleep(150);
  report.ok("在第二层里 Ctrl+M / Ctrl+E 按两次：先全静音、全折叠，再全部还原",
    flagsOn.every(([b, c]) => b && c) && JSON.stringify(await innerFlags()) === JSON.stringify(flagsBefore),
    JSON.stringify({ flagsBefore, flagsOn }));
  await pressEscape(cdp);
  await sleep(150);
  await pressEscape(cdp);
  await sleep(300);
  // 出来之后选中刚出来的那个子图节点、焦点在它上面（以前什么都没选中，刚才在看哪一个得自己再找）
  report.eq("连按两次 Esc 回到顶层，选中的是刚出来的外层子图节点、焦点在它上面", await cdp.eval(`
    const s = window.__lyflow.stores.ui.getState();
    return { depth: s.path.length, selected: [...s.selectedNodes],
             focused: document.activeElement?.closest('.react-flow__node')?.getAttribute('data-id') ?? null };
  `), { depth: 0, selected: [outer.nodeId], focused: outer.nodeId });

  // 查找节点（Ctrl+F）：在顶层按名字找，直接跳进两层子图里的那个节点
  await pressCtrl(cdp, "f");
  await sleep(150);
  report.ok("Ctrl+F 打开查找节点", await cdp.eval(`return !!document.querySelector('[data-testid="node-finder"]');`));
  const voxelLabel = await cdp.eval(`return window.__lyflow.stores.manifest.getState().operatorsById.get('filter.voxel_grid').label;`);
  await cdp.send("Input.insertText", { text: voxelLabel });
  await sleep(150);
  const found = await cdp.eval(`
    const row = document.querySelector('[data-testid="node-finder-row"]');
    return row ? { id: row.dataset.id, where: row.querySelector('.finder__where')?.textContent ?? null } : null;
  `);
  report.ok("按名字找到两层子图里的那个节点，行上写着它在哪两层里",
    found?.id === nested && (found?.where ?? "").includes(" › "), JSON.stringify(found));
  await pressKey(cdp, "Enter", 13);
  await sleep(300);
  report.eq("回车：打开到那一层、选中它，弹层关掉", await cdp.eval(`
    const s = window.__lyflow.stores.ui.getState();
    return { path: s.path.map((p) => p.nodeId), selected: [...s.selectedNodes], open: s.finderOpen };
  `), { path: [outer.nodeId, inner.nodeId], selected: [ids.voxel], open: false });
  await sleep(150);
  report.eq("检查器写着它的节点 id（路径 id，--to / --set 认的那个）",
    await cdp.eval(`return document.querySelector('[data-testid="inspector-node-id"]')?.textContent ?? null;`), `#${nested}`);

  // 静音的节点：状态栏「静音 N」连子图里面的一起数，点它列出来（行上标「静音」），Alt+Enter 选上这一层的、说一声别的层
  // 还有几个；查询里 op: 筛算子。以前忘了取消的静音只看得到节点上的斜纹，大图里常常不在视野里
  await pressCtrl(cdp, "m");
  await sleep(150);
  await cdp.eval(`window.__lyflow.stores.ui.getState().exitTo(0); return true;`);
  await select(cdp, ids.gen);
  await sleep(100);
  await pressCtrl(cdp, "m");
  await sleep(200);
  const chip = await cdp.eval(`
    const b = document.querySelector('[data-testid="statusbar-muted"]');
    return b ? { text: b.textContent, title: b.title } : null;
  `);
  report.ok("在第二层静音体素、顶层静音 gen：状态栏「静音 2」，悬停写这一层 1 个、别的层 1 个",
    chip?.text === "静音 2" && /这一层 1 个，别的层 1 个/.test(chip?.title ?? ""), JSON.stringify(chip));
  await clickAt(cdp, await centerOf(cdp, '[data-testid="statusbar-muted"]'));
  await sleep(200);
  report.eq("点它：查找节点打开、查询是 is:muted，列出这两个（行上标「静音」）", await cdp.eval(`
    return { query: document.querySelector('[data-testid="node-finder-input"]')?.value ?? null,
             rows: [...document.querySelectorAll('[data-testid="node-finder-row"]')]
               .map((r) => ({ id: r.dataset.id, muted: !!r.querySelector('.finder__muted') })) };
  `), { query: "is:muted ", rows: [{ id: ids.gen, muted: true }, { id: nested, muted: true }] });
  await cdp.eval(`window.__lyflow.stores.ui.getState().setViewerMaximized(true); return true;`);
  await pressKey(cdp, "Enter", 13, ["alt"]);
  await sleep(200);
  report.eq("预览最大化着按 Alt+Enter：先还原画布，选上这一层的那个（gen），说一声另一个在别的层；弹层关掉", await cdp.eval(`
    const s = window.__lyflow.stores.ui.getState();
    return { selected: [...s.selectedNodes], open: s.finderOpen, toast: s.toast?.text ?? null, maximized: s.viewerMaximized };
  `), { selected: [ids.gen], open: false, toast: "选中了这一层的 1 个节点（另有 1 个在别的层，没选）", maximized: false });
  await pressCtrl(cdp, "f");
  await sleep(150);
  await cdp.send("Input.insertText", { text: "op:statistical" });
  await sleep(150);
  report.eq("查询里 op: 按算子筛：op:statistical 只剩两层子图里的 sor",
    await cdp.eval(`return [...document.querySelectorAll('[data-testid="node-finder-row"]')].map((r) => r.dataset.id);`), [sorPath]);
  await pressEscape(cdp);
  await sleep(100);
}

async function suiteRecursion(cdp, report) {
  report.section("§1 递归引用被拒");

  // 手改过的文件才可能长这样：界面上没有能造出递归的操作
  const diags = await cdp.eval(`
    const b = window.__lyflow;
    const doc = {
      schemaVersion: 1, id: '01RECURSE0000000000000000',
      nodes: [{ id: 'n_a', op: 'sub:loop', params: {}, ui: { position: { x: 0, y: 0 } } }],
      edges: [],
      subgraphs: {
        loop: {
          name: '自引用',
          nodes: [{ id: 'inner', op: 'sub:loop', params: {} }],
          edges: [], inputs: [], outputs: [], params: [],
        },
      },
    };
    b.stores.graph.getState().loadDoc(doc, null);
    return await b.transport.validateGraph(doc, null);
  `);
  const hit = (diags ?? []).find((d) => d.code === "recursive_subgraph");
  report.ok("validate 报 recursive_subgraph", Boolean(hit), JSON.stringify(diags));
  report.ok(
    "诊断挂在展开时出问题的那个节点上",
    String(hit?.nodeId ?? "").startsWith("n_a"),
    String(hit?.nodeId),
  );

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("跑它只会失败，不会崩", run.status, "error");
}

// ------------------------------------------------------------ §1 库算子

async function suiteLibrary(cdp, report) {
  report.section("§1 库算子：保存到库 → 面板里出现 → 新图拖出即用");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);
  const composed = await compose(cdp, [ids.voxel, ids.sor]);
  const libId = `e2e_clean_${Date.now().toString(36)}`;

  // 走真实的右键菜单 + 对话框
  const saved = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${composed.nodeId}"]');
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true,
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2,
    }));
    await new Promise((d) => setTimeout(d, 150));
    const open = document.querySelector('[data-testid="ctx-save-library"]');
    if (!open) return 'no-menu-item';
    open.click();
    await new Promise((d) => setTimeout(d, 150));
    const idInput = document.querySelector('[data-testid="library-id"]');
    if (!idInput) return 'no-dialog';
    const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
    setter.call(idInput, ${lit(libId)});
    idInput.dispatchEvent(new Event('input', { bubbles: true }));
    await new Promise((d) => setTimeout(d, 80));
    document.querySelector('[data-testid="library-save"]').click();
    await new Promise((d) => setTimeout(d, 1500));
    return 'ok';
  `);
  report.eq("右键 → 保存到库 → 对话框走通", saved, "ok");

  const status = await cdp.eval(`return await window.__lyflow.transport.getLibraryStatus();`);

  const libFile = status.dirs?.[0] ? path.join(status.dirs[0], `${libId}.lyflow-op.json`) : null;
  report.ok("库文件写到了 app data 下的 library/", libFile != null && fs.existsSync(libFile),
    `${libFile} status=${JSON.stringify(status)}`);

  const inManifest = await cdp.eval(`
    const ops = window.__lyflow.stores.manifest.getState().bundle.operators;
    const op = ops.find((o) => o.id === ${lit("lib." + libId)});
    return op ? { id: op.id, category: op.category, inputs: op.inputs.length, outputs: op.outputs.length } : null;
  `);
  report.ok("manifest 里出现了 lib.<id>", Boolean(inManifest), JSON.stringify(inManifest));
  report.ok("分类挂在 Library/ 下", (inManifest?.category ?? "").startsWith("Library/"),
    inManifest?.category);

  const inPalette = await cdp.eval(`
    const rows = [...document.querySelectorAll('[data-op-id]')].map((el) => el.getAttribute('data-op-id'));
    return rows.includes(${lit("lib." + libId)});
  `);
  report.ok("节点面板里也有它（manifest 驱动，前端零改动）", inPalette === true);

  // 新建一张图，从库里拖一个出来跑
  await newDoc(cdp);
  const fresh = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 40000, seed: 77 } },
      { key: "lib", op: `lib.${libId}` },
    ],
    [{ from: ["gen", "cloud"], to: ["lib", "cloud"] }],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("库算子在新图里跑通", run.status, "ok");
  const innerDone = Object.keys(run.nodes).filter((id) => id.startsWith(`${fresh.lib}/`));
  report.ok(
    "库算子展开成两个内部节点，都有结果",
    innerDone.length === 2 && innerDone.every((id) => ["done", "skipped"].includes(run.nodes[id].state)),
    `内部=${JSON.stringify(innerDone)} ${JSON.stringify(run.nodes)}`,
  );

  // 展开为内联子图（docs/library-inline-plan.md）：定义由 core 给，换成 sub: 之后跑出来的结果与库算子相同
  const counts = () => cdp.eval(`
    const nodes = window.__lyflow.stores.execution.getState().nodes;
    const out = {};
    for (const [id, n] of nodes) {
      if (id.startsWith(${lit(fresh.lib + "/")})) out[id] = (n.stats?.outputs ?? []).map((o) => o.elementCount);
    }
    return out;
  `);
  const libCounts = await counts();
  const menuHit = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${fresh.lib}"]');
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true,
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 }));
    await new Promise((d) => setTimeout(d, 150));
    const item = document.querySelector('[data-testid="ctx-inline-library"]');
    if (!item) return false;
    item.click();
    return true;
  `);
  mustOk(menuHit, "库算子的右键菜单里有「展开为内联子图」");
  await cdp.waitFor(
    `window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(fresh.lib)})?.op.startsWith('sub:')`,
    { timeoutMs: 5000, what: "库算子节点换成 sub:" },
  ).catch(() => {});
  const inlined = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const n = g.doc.nodes.find((x) => x.id === ${lit(fresh.lib)});
    const sg = n.op.startsWith('sub:') ? g.doc.subgraphs?.[n.op.slice(4)] : null;
    return { op: n.op, inner: sg ? sg.nodes.length : 0, undo: g.past[g.past.length - 1]?.label ?? null };
  `);
  report.ok("右键「展开为内联子图」：节点变成 sub:，定义（两个内部节点）拷进了图，是一条撤销「展开库算子」",
    inlined.op.startsWith("sub:") && inlined.inner === 2 && inlined.undo === "展开库算子", JSON.stringify(inlined));
  const rerun = await runAndWait(cdp, () => pressF5(cdp));
  const subCounts = await counts();
  report.ok("展开之后再跑：内部节点的输出点数与库算子时相同",
    rerun.status === "ok" && Object.keys(libCounts).length === 2 && JSON.stringify(subCounts) === JSON.stringify(libCounts),
    JSON.stringify({ libCounts, subCounts }));
  await pressCtrl(cdp, "z");
  await sleep(200);
  report.eq("Ctrl+Z 之后 toast 说撤掉的是哪一步", await cdp.eval(`return window.__lyflow.stores.ui.getState().toast?.text ?? null;`),
    "已撤销：展开库算子");
  report.eq("一次撤销回到库算子",
    await cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(fresh.lib)}).op;`),
    `lib.${libId}`);

  // 库目录设置（docs/library-dirs.md）：工具栏「库 ▾」里粘贴一个目录、添加 → 当场重扫、manifest 里出现那个目录里的
  // 库算子；CLI 读同一份设置也认得它；× 去掉就没了。设置写在真的 app data 里，收尾一定还原
  if (libFile && fs.existsSync(libFile)) {
    const extraId = `e2e_extra_${Date.now().toString(36)}`;
    const extraDir = path.join(os.tmpdir(), `lyflow-e2e-libdir-${extraId}`);
    fs.mkdirSync(extraDir, { recursive: true });
    const body = JSON.parse(fs.readFileSync(libFile, "utf8"));
    body.id = extraId;
    fs.writeFileSync(path.join(extraDir, `${extraId}.lyflow-op.json`), JSON.stringify(body, null, 2), "utf8");
    const original = await cdp.eval(`return (await window.__lyflow.transport.getLibrarySettings()).extraDirs;`);
    const hasOp = (id) => cdp.eval(`return window.__lyflow.stores.manifest.getState().operatorsById.has(${lit(id)});`);
    try {
      await cdp.eval(`document.querySelector('[data-testid="library-toggle"]').click(); return true;`);
      await cdp.waitFor(`!!document.querySelector('[data-testid="library-dir-input"]')`, { what: "库目录面板打开" });
      const added = await cdp.eval(`
        const input = document.querySelector('[data-testid="library-dir-input"]');
        const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
        setter.call(input, ${lit(extraDir)});
        input.dispatchEvent(new Event('input', { bubbles: true }));
        await new Promise((d) => setTimeout(d, 80));
        document.querySelector('[data-testid="library-add"]').click();
        return true;
      `);
      mustOk(added, "在库目录面板里填了目录并点了添加");
      // 两样都等：面板先换 manifest、再去取一遍设置才刷新列表（又一次 IPC），只等 manifest 会读到还没刷新的列表
      await cdp.waitFor(`window.__lyflow.stores.manifest.getState().operatorsById.has(${lit("lib." + extraId)})
          && [...document.querySelectorAll('[data-testid="library-dir"]')].some((li) => li.getAttribute('title') === ${lit(extraDir)})`,
        { what: "新加的目录里的库算子进了 manifest、列表里也有了那个目录", timeoutMs: 15_000 }).catch(() => {});
      const listed = await cdp.eval(`
        return [...document.querySelectorAll('[data-testid="library-dir"]')].map((li) => li.getAttribute('data-kind') + ':' + li.getAttribute('title'));
      `);
      const opIn = await hasOp(`lib.${extraId}`);
      report.ok("面板里「添加」：当场保存并重扫，manifest 里有了新目录里的库算子，列表里多一行可删的目录",
        opIn && listed.includes(`extra:${extraDir}`), JSON.stringify({ opIn, listed }));

      const cli = path.join(ROOT, "bridge", "target", "debug", "lyflow.exe");
      const r = spawnSync(cli, ["manifest"], { encoding: "utf8" });
      report.ok("CLI 读同一份设置：lyflow manifest 里也有它", r.status === 0 && r.stdout.includes(`lib.${extraId}`),
        `${cli} exit=${r.status} ${String(r.stderr).slice(0, 200)}`);

      await cdp.eval(`
        const li = [...document.querySelectorAll('[data-testid="library-dir"]')].find((x) => x.getAttribute('title') === ${lit(extraDir)});
        li.querySelector('[data-testid="library-remove"]').click();
        return true;
      `);
      await cdp.waitFor(`!window.__lyflow.stores.manifest.getState().operatorsById.has(${lit("lib." + extraId)})`,
        { what: "去掉目录后库算子消失", timeoutMs: 15_000 }).catch(() => {});
      report.eq("× 去掉目录：它的库算子从 manifest 里消失", await hasOp(`lib.${extraId}`), false);
    } finally {
      await cdp.eval(`
        const r = await window.__lyflow.transport.setLibraryDirs(${lit(original)});
        window.__lyflow.stores.manifest.getState().replaceBundle(r.manifest, 0);
        document.querySelector('[data-testid="library-toggle"]')?.click();
        return true;
      `);
      fs.rmSync(extraDir, { recursive: true, force: true });
    }
  }

  // 收尾：把库文件删掉，不然下一次跑会看到一堆积压的 e2e 算子
  if (libFile && fs.existsSync(libFile)) {
    fs.rmSync(libFile, { force: true });
    await cdp.eval(`
      const r = await window.__lyflow.transport.refreshLibrary();
      window.__lyflow.stores.manifest.getState().replaceBundle(r.manifest, 0);
      return r.status.count;
    `);
  }
}

// ------------------------------------------------------------ §2 preview

async function suitePreview(cdp, report) {
  report.section("§2 live preview：拖参数跟手、松手补正式运行、不污染正式缓存");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 400000, seed: 31 } },
      {
        key: "sample",
        op: "filter.random_sample",
        params: { mode: "ratio", keepRatio: 0.2, seed: 4 },
      },
    ],
    [{ from: ["gen", "cloud"], to: ["sample", "cloud"] }],
  );
  await runAndWait(cdp, () => pressF5(cdp));
  await select(cdp, ids.sample);
  await sleep(300);
  // 预热预览的缓存：源头的 40 万点先在预览命名空间里算好一次（previewMaxPoints 取编辑器自己的，缓存键才对得上）。
  // 不预热的话拖动中的第一次预览要现算源头，机器一忙就超过拖动的一步（60 ms）、每次都被下一步取消，
  // 一次也画不出来 —— 量的就成了「源头多久能算完」，不是「跟手」（2026-10-02 全量里撞上过一次）
  await runAndWait(cdp, () => cdp.eval(`
    const s = window.__lyflow.snapshot();
    await window.__lyflow.run({ targets: [${lit(ids.sample)}], preview: true, previewMaxPoints: s.preview.maxPoints });
    return true;
  `));

  // 在页面里装一个观察器，记录 3D 视图**真的换了一片云**的时刻
  await cdp.eval(`
    window.__m4 = { marks: [] };
    const viewer = document.querySelector('.viewer');
    window.__m4.observer = new MutationObserver(() => {
      window.__m4.marks.push({
        run: viewer.getAttribute('data-run'),
        view: viewer.getAttribute('data-view'),
        preview: viewer.getAttribute('data-preview'),
        at: performance.now(),
      });
    });
    window.__m4.observer.observe(viewer, { attributes: true, attributeFilter: ['data-run', 'data-view'] });
    window.__lyflow.clearTransitions();
    return true;
  `);

  // 真实鼠标拖动滑块：从中间往右拖一段
  const slider = await centerOf(cdp, '[data-testid="param-slider-keepRatio"]');
  mustOk(slider != null, "找到 keepRatio 的滑块", JSON.stringify(slider));
  // 拖得像人一样慢一点（每步约 60 ms）：dragMouse 的 12 ms 一步整段只要 100 ms，预览运行来不及画就被
  // 下一次取消，最后画出来的总是松手后补的那次正式运行 —— 量的就不是「跟手」了
  {
    const from = { x: slider.x - 40, y: slider.y };
    const common = { button: "left", buttons: 1, clickCount: 1 };
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: from.x, y: from.y, buttons: 0 });
    await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: from.x, y: from.y, ...common });
    for (let i = 1; i <= 8; i += 1) {
      await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: from.x + i * 10, y: from.y, ...common });
      await sleep(60);
    }
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: from.x + 80, y: from.y, ...common });
    await sleep(120);
  }

  // 拖动触发了预览运行：等不到就超时抛出，分组中断
  await cdp.waitFor(
    `window.__lyflow.runMarks.some((m) => m.status !== 'running')`,
    { timeoutMs: 20_000, what: "预览运行结束" },
  );

  // 事件到渲染（M4 验收的定义）：拖动中那几次**预览运行**从结束到视图画出它的云。最后一次 ok 的运行是
  // 松手后补的正式运行（全量取数，debug 构建里光 IPC 就 50–80 ms），不算「跟手」，只打出来参考。
  // 取中位数：拖动中有好几次预览，单次抖动不该决定成败
  const timing = await cdp.waitFor(
    `(() => {
       const runs = window.__lyflow.runMarks.filter((m) => m.status === 'ok');
       const marks = window.__m4.marks.filter((m) => m.view === 'cloud');
       const drawn = [];
       for (const r of runs) {
         const hit = marks.find((m) => m.run === r.runId && m.at >= r.at);
         if (hit) drawn.push({ run: r.runId, ms: Math.round(hit.at - r.at) });
       }
       const last = runs[runs.length - 1];
       // 等的是松手后那次正式运行：机器一忙，它还没跑完时最后一次画出来的是预览（2026-10-03 整跑撞上过：
       // 接着读到的源头点数是抽稀过的 20 万）
       const ex = window.__lyflow.stores.execution.getState();
       if (ex.preview || ex.runStatus === 'running' || !last || last.runId !== ex.runId) return null;
       if (!drawn.some((d) => d.run === last.runId)) return null;  // 等正式运行也画出来
       return { preview: drawn.filter((d) => d.run !== last.runId).map((d) => d.ms),
                formal: drawn.find((d) => d.run === last.runId).ms };
     })()`,
    { timeoutMs: 20_000, what: "视图画出预览与正式结果" },
  );
  const sorted = [...timing.preview].sort((a, b) => a - b);
  const latency = sorted.length > 0 ? sorted[Math.floor((sorted.length - 1) / 2)] : null;
  report.ok(`事件到渲染（拖动中的预览运行，中位数）${latency} ms < 100 ms`, latency != null && latency < 100,
    `预览 ${JSON.stringify(timing.preview)} ms，松手后的正式运行 ${timing.formal} ms`);

  const previewRun = await cdp.eval(`
    const s = window.__lyflow.snapshot();
    return { preview: s.run.preview, sourceCount: s.run.nodes[${lit(ids.gen)}]?.elementCount ?? null,
             maxPoints: s.preview.maxPoints, autoRun: s.preview.autoRun };
  `);
  report.ok(
    "松手后自动补了一次正式运行（preview 标记已经落回 false）",
    previewRun.preview === false,
    JSON.stringify(previewRun),
  );
  report.eq("正式运行的源头是全量点数", previewRun.sourceCount, 400000);
  // 拖动中每次重跑，预览都留着上一片云（只在角上写「正在计算…」）。以前每次重跑先摘掉点、盖上整块「正在计算…」，
  // 每 30 ms 一次的预览让画面一闪一闪
  report.eq("拖动中的每次重跑，预览都没有变空（留着上一片云）",
    await cdp.eval(`return window.__m4.marks.filter((m) => m.view === 'empty').length;`), 0);

  // 单独发一次 preview run，直接断言抽稀与命名空间
  const previewOnly = await runAndWait(cdp, () =>
    cdp.eval(`
      await window.__lyflow.run({
        targets: [${lit(ids.sample)}], preview: true, previewMaxPoints: 20000 });
      return true;
    `),
  );
  report.ok(
    "preview run 里源头抽到了 2 万点",
    previewOnly.nodes[ids.gen]?.elementCount === 20000,
    JSON.stringify(previewOnly.nodes),
  );

  // 正式重跑：全部命中缓存说明预览进了独立命名空间，没有污染正式的键
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("正式重跑仍然全部命中缓存",
    Object.values(full.nodes).every((n) => n.state === "skipped"), true);

  await cdp.eval(`window.__m4.observer.disconnect(); return true;`);

  // 预览钉在下游（真按 P），去拖上游的比例：钉住的那个跟着重算（以前只算改的节点，画面一动不动，像是参数没起作用）。
  // 敲数回车不触发运行：预览左上角说「画面是上一次的结果」，点它的 ▶ 算到钉住的节点；再按 P 取消钉住，预览回到选中的
  await newDoc(cdp);
  const pin = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed: 7 } },
      { key: "s1", op: "filter.random_sample", params: { mode: "ratio", keepRatio: 0.5, seed: 1 } },
      { key: "s2", op: "filter.random_sample", params: { mode: "ratio", keepRatio: 0.5, seed: 2 } },
    ],
    [{ from: ["gen", "cloud"], to: ["s1", "cloud"] }, { from: ["s1", "cloud"], to: ["s2", "cloud"] }],
  );
  await runAndWait(cdp, () => pressF5(cdp));
  const pinState = () => cdp.eval(`
    const s = window.__lyflow.snapshot();
    const v = document.querySelector('.viewer');
    return { pinned: window.__lyflow.stores.ui.getState().pinnedNode, node: v?.getAttribute('data-node') ?? null,
             s1: s.run.nodes[${lit(pin.s1)}]?.elementCount ?? null, s2: s.run.nodes[${lit(pin.s2)}]?.elementCount ?? null,
             targets: s.run.targets, preview: s.run.preview, runId: s.run.runId,
             stale: !!document.querySelector('[data-testid="viewer-stale"]'),
             toast: window.__lyflow.stores.ui.getState().toast?.text ?? null };
  `);
  await select(cdp, pin.s2);
  await sleep(200);
  await cdp.eval(`document.activeElement?.blur(); return true;`);
  await pressKey(cdp, "p", 80);
  await sleep(150);
  await select(cdp, pin.s1);
  await sleep(250);
  const p0 = await pinState();
  report.ok("选中下游 s2 按 P：钉住它，再选上游 s1 预览仍是 s2",
    p0.pinned === pin.s2 && p0.node === pin.s2 && /已钉住/.test(p0.toast ?? ""), JSON.stringify(p0));

  // 真拖 s1 的比例滑块（往左一段）：拖动中的预览与松手后的正式运行都算到 s2
  const pinSlider = await centerOf(cdp, '[data-testid="param-slider-keepRatio"]');
  mustOk(pinSlider != null, "找到 s1 的 keepRatio 滑块");
  await dragMouse(cdp, pinSlider, { x: pinSlider.x - 50, y: pinSlider.y }, { steps: 10 });
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runId !== ${lit(p0.runId)} && !s.preview && s.runStatus !== 'running' && s.runStatus !== 'idle'; })()`,
    { timeoutMs: 30_000, what: "松手后补的正式运行结束" },
  );
  await sleep(200);
  const p1 = await pinState();
  report.ok("拖上游 s1 的比例：钉住的下游 s2 跟着重算（正式运行的目标带着 s2，点数变了），预览没标「上一次的结果」",
    p1.targets.includes(pin.s2) && p1.s2 !== p0.s2 && p1.s2 != null && p1.s1 !== p0.s1 && !p1.stale && p1.node === pin.s2,
    JSON.stringify({ p0, p1 }));

  // 敲数回车：不触发运行，钉住的 s2 过时了 —— 预览左上角说一声，点它的 ▶ 一次算到 s2
  await clickAt(cdp, await centerOf(cdp, '[data-testid="param-drag-keepRatio"]'));
  await sleep(100);
  await pressCtrl(cdp, "a");
  await cdp.send("Input.insertText", { text: "0.2" });
  await pressKey(cdp, "Enter", 13);
  await cdp.eval(`document.activeElement?.blur(); return true;`);
  await replan(cdp);
  await sleep(200);
  const p2 = await pinState();
  report.ok("敲数回车改了上游：钉住的 s2 在预览左上角标「参数改过了 · 画面是上一次的结果」，点数还是旧的",
    p2.stale && p2.s2 === p1.s2 && p2.runId === p1.runId &&
      /上一次的结果/.test(await cdp.eval(`return document.querySelector('[data-testid="viewer-stale"]')?.textContent ?? '';`)),
    JSON.stringify(p2));
  const ranStale = await runAndWait(cdp, async () => clickAt(cdp, await centerOf(cdp, '[data-testid="viewer-stale-run"]')));
  await replan(cdp);
  await sleep(200);
  const p3 = await pinState();
  report.ok("点角标上的 ▶ 运行到此节点：算到 s2（目标就是它），点数变成新的，角标消失",
    ranStale.targets.length === 1 && ranStale.targets[0] === pin.s2 && p3.s2 !== p2.s2 && p3.s2 != null && !p3.stale,
    JSON.stringify({ targets: ranStale.targets, p3 }));

  // 关了自动运行再拖：松手就停在预览上 —— 画面是新值、只是抽稀过，角标说「抽稀的预览 · 还没正式运行」（不说「上一次的结果」），
  // 拖着的时候不出来；点它的 ▶ 补一次正式运行，角标走
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(false); return true;`);
  const offSlider = await centerOf(cdp, '[data-testid="param-slider-keepRatio"]');
  const pBefore = await pinState();
  const seenWhileDragging = [];
  {
    const common = { button: "left", buttons: 1, clickCount: 1 };
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: offSlider.x, y: offSlider.y, buttons: 0 });
    await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: offSlider.x, y: offSlider.y, ...common });
    for (let i = 1; i <= 6; i += 1) {
      await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: offSlider.x + i * 8, y: offSlider.y, ...common });
      await sleep(80);
      seenWhileDragging.push(await cdp.eval(`return !!document.querySelector('[data-testid="viewer-stale"]');`));
    }
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: offSlider.x + 48, y: offSlider.y, ...common });
  }
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runId !== ${lit(pBefore.runId)} && s.preview && s.runStatus !== 'running'; })()`,
    { timeoutMs: 20_000, what: "拖动的预览运行结束" },
  );
  await replan(cdp);
  await sleep(250);
  const p5 = await pinState();
  const badge5 = await cdp.eval(`return document.querySelector('[data-testid="viewer-stale"]')?.textContent ?? null;`);
  report.ok("关了自动运行拖上游：拖着的时候不出角标；松手停在预览上，角标说「抽稀的预览 · 还没正式运行」",
    seenWhileDragging.every((x) => !x) && p5.preview === true && /抽稀的预览/.test(badge5 ?? "") && !/上一次的结果/.test(badge5 ?? ""),
    JSON.stringify({ seenWhileDragging, p5, badge5 }));
  const ranFormal = await runAndWait(cdp, async () => clickAt(cdp, await centerOf(cdp, '[data-testid="viewer-stale-run"]')));
  await replan(cdp);
  await sleep(200);
  const p6 = await pinState();
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(true); return true;`);
  report.ok("点角标上的 ▶：补一次正式运行（不是预览），角标走",
    ranFormal.preview === false && !p6.stale, JSON.stringify({ preview: ranFormal.preview, p6 }));

  // 再按 P：取消钉住，预览回到选中的 s1
  await pressKey(cdp, "p", 80);
  await sleep(250);
  const p4 = await pinState();
  report.ok("再按 P：取消钉住，预览跟着选中回到 s1",
    p4.pinned === null && p4.node === pin.s1 && /取消钉住/.test(p4.toast ?? ""), JSON.stringify(p4));

  // 抽屉的「调参」页：这张图打开以来每次正式运行一行，新的在上，写着比上一次改了什么；预览运行不记。
  // 这一段跑了四次正式运行：F5、拖完补的、两次点角标上的 ▶（运行到 s2）
  const drawerBeforeRuns = await cdp.eval(`return window.__lyflow.stores.ui.getState().drawer;`);
  await cdp.eval(`if (!window.__lyflow.stores.ui.getState().drawer) window.__lyflow.stores.ui.getState().toggleDrawer('log'); return true;`);
  await sleep(150);
  await clickAt(cdp, await centerOf(cdp, '[data-testid="drawer-tab-runs"]'));
  await sleep(200);
  const runRows = await cdp.eval(`
    return [...document.querySelectorAll('[data-testid="run-record"]')].map((r) => ({
      seq: Number(r.dataset.seq), status: r.dataset.status,
      head: r.querySelector('.runs__head')?.textContent ?? '',
      diff: r.querySelector('[data-testid="run-diff"]')?.textContent ?? '' }));
  `);
  const s1Name = await cdp.eval(`
    const n = window.__lyflow.stores.graph.getState().doc.nodes.find((x) => x.id === ${lit(pin.s1)});
    return n.ui?.title ?? window.__lyflow.stores.manifest.getState().operatorsById.get(n.op).label;
  `);
  report.ok("「调参」页：四次正式运行一行一次、新的在上（预览不记）；第一次写「打开以来的第一次」，之后写 s1 的比例从多少改到多少；运行到 s2 的写着",
    JSON.stringify(runRows.map((r) => [r.seq, r.status])) === JSON.stringify([[4, "ok"], [3, "ok"], [2, "ok"], [1, "ok"]]) &&
      runRows[3].diff === "这张图打开以来的第一次" &&
      runRows.slice(0, 3).every((r) => r.diff.startsWith(`${s1Name} · `) && / → /.test(r.diff)) &&
      / → 0\.2$/.test(runRows[1].diff) && /运行到/.test(runRows[1].head) && /整张图/.test(runRows[3].head),
    JSON.stringify({ s1Name, runRows }));

  // 设为基准：点第 1 次那一行的「设为基准」—— 最上面钉一条基准，之后每一行多一行「比基准 #1：…」
  // 抽屉里放不下全部几行：先把第 1 次那一行滚进来
  const showRow1 = () => cdp.eval(`document.querySelector('[data-testid="run-record"][data-seq="1"]')?.scrollIntoView({ block: 'nearest' }); return true;`);
  await showRow1();
  await sleep(100);
  await clickAt(cdp, await centerOf(cdp, '[data-testid="run-record"][data-seq="1"] [data-testid="run-baseline"]'));
  await sleep(200);
  const based = await cdp.eval(`
    const row = (seq) => document.querySelector('[data-testid="run-record"][data-seq="' + seq + '"]');
    return { bar: document.querySelector('[data-testid="run-baseline-bar"]')?.textContent ?? null,
             mark: row(1)?.dataset.baseline ?? null,
             base4: row(4)?.querySelector('[data-testid="run-diff-base"]')?.textContent ?? null,
             base1: !!row(1)?.querySelector('[data-testid="run-diff-base"]') };
  `);
  report.ok("点第 1 次的「设为基准」：最上面钉着「基准 #1」，第 4 次那一行多一行「比基准 #1：s1 · … → …」，基准自己那一行没有",
    /^基准 #1 · /.test(based.bar ?? "") && based.mark === "1" && !based.base1 &&
      new RegExp(`^比基准 #1：${s1Name} · .+ → `).test(based.base4 ?? ""),
    JSON.stringify(based));

  // 「恢复这组参数」：点第 1 次那一行，s1 的比例回到那时的 0.5，一条撤销；Ctrl+Z 回来
  const ratioNow = () => cdp.eval(`
    // 有效值：0.5 是默认值，稀疏存储时不写进 params
    const n = window.__lyflow.stores.graph.getState().doc.nodes.find((x) => x.id === ${lit(pin.s1)});
    return n.params.keepRatio ?? window.__lyflow.stores.manifest.getState().operatorsById.get(n.op).params.find((q) => q.name === 'keepRatio').default;
  `);
  const ratioBefore = await ratioNow();
  const pastBeforeRestore = await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
  await showRow1();
  await sleep(100);
  await clickAt(cdp, await centerOf(cdp, '[data-testid="run-record"][data-seq="1"] [data-testid="run-restore"]'));
  await sleep(200);
  const restored = { ratio: await ratioNow(), steps: (await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`)) - pastBeforeRestore,
                     toast: await cdp.eval(`return window.__lyflow.stores.ui.getState().toast?.text ?? null;`) };
  await pressCtrl(cdp, "z");
  await sleep(150);
  report.ok("点第 1 次那一行的「恢复这组参数」：s1 的比例回到 0.5、一条撤销、说了改了几处；Ctrl+Z 回到恢复之前",
    restored.ratio === 0.5 && ratioBefore !== 0.5 && restored.steps === 1 && /^已恢复第 1 次运行时的参数（1 处/.test(restored.toast ?? "") &&
      (await ratioNow()) === ratioBefore,
    JSON.stringify({ ratioBefore, restored }));
  await cdp.eval(`window.__lyflow.stores.ui.setState({ drawer: ${lit(drawerBeforeRuns)} }); return true;`);

  // 拖图参数：s1 的比例纳入配方之后，在检查器上面「图参数」那一行真拖滑块 —— 拖着的时候预览它绑着的 s1，
  // 松手补一次正式运行（以前图参数那一行没有节点 id：不预览、松手也不跑）
  const gpName = await cdp.eval(`return window.__lyflow.stores.graph.getState().promoteToGraphParam(${lit(pin.s1)}, 'keepRatio');`);
  await select(cdp, pin.s1);
  await sleep(300);
  const gpBefore = await pinState();
  await cdp.eval(`
    window.__gpSeen = [];
    window.__gpStop = window.__lyflow.stores.execution.subscribe((s) => {
      if (s.preview && s.runId && !window.__gpSeen.includes(s.runId)) window.__gpSeen.push(s.runId);
    });
    return true;
  `);
  const gpSlider = await centerOf(cdp, `[data-testid="graph-param-${gpName}"] [data-testid="param-slider-keepRatio"]`);
  mustOk(gpSlider != null, "图参数那一行有滑块", JSON.stringify(gpSlider));
  {
    const common = { button: "left", buttons: 1, clickCount: 1 };
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: gpSlider.x, y: gpSlider.y, buttons: 0 });
    await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: gpSlider.x, y: gpSlider.y, ...common });
    for (let i = 1; i <= 6; i += 1) {
      await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: gpSlider.x - i * 8, y: gpSlider.y, ...common });
      await sleep(80);
    }
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: gpSlider.x - 48, y: gpSlider.y, ...common });
  }
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runId !== ${lit(gpBefore.runId)} && !s.preview && s.runStatus !== 'running' && s.runStatus !== 'idle'; })()`,
    { timeoutMs: 30_000, what: "拖图参数松手后补的正式运行结束" },
  );
  const gpAfter = await pinState();
  const gpSeen = await cdp.eval(`window.__gpStop(); return window.__gpSeen.length;`);
  report.ok("拖图参数那一行的滑块：拖着有预览运行，松手补一次正式运行、算的是它绑着的 s1（点数变了）",
    gpSeen >= 1 && gpAfter.preview === false && gpAfter.targets.includes(pin.s1) && gpAfter.s1 !== gpBefore.s1,
    JSON.stringify({ gpName, gpSeen, gpBefore, gpAfter }));
}

// ------------------------------------------------------------- §4 大图性能

async function suiteBigGraph(cdp, report) {
  report.section("§4 大图性能：300 节点 400 边");

  await newDoc(cdp);
  const built = await cdp.eval(`
    const t0 = performance.now();
    const g = () => window.__lyflow.stores.graph.getState();
    // 30 条链 × 10 个节点 = 300 个节点。每条链里穿插 4 个 util.merge，
    // 它们的第二个输入从同链更靠前的节点引一条边过来，凑到 ~400 条边。
    const lanes = [];
    for (let lane = 0; lane < 30; lane += 1) {
      const ids = [];
      const outPort = [];
      for (let step = 0; step < 10; step += 1) {
        const op = step === 0 ? 'gen.synthetic' : (step % 2 === 0 ? 'util.merge' : 'util.reroute');
        const id = g().addNode(op, { x: step * 250, y: lane * 140 });
        ids.push(id);
        outPort.push(step === 0 ? 'cloud' : (op === 'util.merge' ? 'cloud' : 'out'));
        if (step > 0) {
          g().connect(
            { node: ids[step - 1], port: outPort[step - 1] },
            { node: id, port: op === 'util.merge' ? 'a' : 'in' },
          );
        }
      }
      // merge 的第二个输入：从链头拉一条，制造扇出
      for (let step = 2; step < 10; step += 2) {
        g().connect({ node: ids[0], port: 'cloud' }, { node: ids[step], port: 'b' });
      }
      lanes.push(ids);
    }
    const doc = g().doc;
    return { ms: Math.round(performance.now() - t0), nodes: doc.nodes.length, edges: doc.edges.length, lane0: lanes[0] };
  `);
  mustOk(built.nodes === 300 && built.edges >= 380, "搭出 300 节点 / ≥ 380 边", JSON.stringify(built));

  // 「打开 < 1 s」：从 loadDoc 到画布上出现节点
  const openMs = await cdp.eval(`
    const b = window.__lyflow;
    const doc = JSON.parse(JSON.stringify(b.stores.graph.getState().doc));
    b.stores.graph.getState().newDoc();
    await new Promise((d) => setTimeout(d, 100));
    const t0 = performance.now();
    b.stores.graph.getState().loadDoc(doc, null);
    for (let i = 0; i < 200; i += 1) {
      await new Promise((d) => requestAnimationFrame(d));
      if (document.querySelectorAll('[data-testid^="node-n_"]').length > 0) break;
    }
    return Math.round(performance.now() - t0);
  `);
  report.ok(`打开 300 节点的图用了 ${openMs} ms < 1000 ms`, openMs < 1000, `${openMs} ms`);

  const virtualized = await cdp.eval(`
    const rendered = document.querySelectorAll('[data-testid^="node-n_"]').length;
    const total = window.__lyflow.stores.graph.getState().doc.nodes.length;
    return { rendered, total };
  `);
  report.ok(
    "只渲染了视野里的节点（onlyRenderVisibleElements）",
    virtualized.rendered < virtualized.total,
    JSON.stringify(virtualized),
  );

  // 键盘沿连线走（Alt+→）：从链头一路走到链尾，链尾一开始在视野外、没挂 DOM；每一步选中跟着走、画布只挪一点把它移进来，
  // 焦点落在它上面、预览跟着换；React Flow 自己的方向键挪节点不跟着响（原来的节点不动、不记撤销）
  const [head, tail] = [built.lane0[0], built.lane0[9]];
  await setViewport(cdp, { x: 60, y: 60, zoom: 1 });
  mustOk(await cdp.eval(`return !document.querySelector('.react-flow__node[data-id="${tail}"]');`), "链尾一开始不在 DOM 里");
  await clickAt(cdp, await centerOf(cdp, `[data-testid="node-${head}"] .node__head`));
  await sleep(200);
  const walk0 = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    return { past: g.past.length, pos: JSON.stringify(g.doc.nodes.find((n) => n.id === ${lit(head)}).ui.position) };
  `);
  for (let i = 0; i < 9; i += 1) {
    await pressKey(cdp, "ArrowRight", 39, ["alt"]);
    await sleep(260);
  }
  await sleep(300);
  const walked = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const el = document.querySelector('.react-flow__node[data-id="${tail}"]');
    const r = el?.getBoundingClientRect();
    const p = document.querySelector('.react-flow__pane').getBoundingClientRect();
    return {
      selected: [...window.__lyflow.stores.ui.getState().selectedNodes],
      focused: document.activeElement?.closest('.react-flow__node')?.getAttribute('data-id') ?? null,
      inPane: !!r && r.left >= p.left && r.right <= p.right && r.top >= p.top && r.bottom <= p.bottom,
      viewer: document.querySelector('.viewer')?.getAttribute('data-node') ?? null,
      pastDelta: g.past.length - ${walk0.past},
      headMoved: JSON.stringify(g.doc.nodes.find((n) => n.id === ${lit(head)}).ui.position) !== ${lit(walk0.pos)},
    };
  `);
  report.eq("Alt+→ 九下从链头走到链尾：选中、焦点、预览都跟着走，链尾移进了视野；节点没被挪、不记撤销", walked,
    { selected: [tail], focused: tail, inPane: true, viewer: tail, pastDelta: 0, headMoved: false });
  // 链尾再按一下：走不动（选中不换），React Flow 自己的方向键挪节点也不能接过去把链尾挪一格
  const tailBefore = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    return { past: g.past.length, pos: JSON.stringify(g.doc.nodes.find((n) => n.id === ${lit(tail)}).ui.position) };
  `);
  await pressKey(cdp, "ArrowRight", 39, ["alt"]);
  await sleep(250);
  const tailAfter = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    return { past: g.past.length, pos: JSON.stringify(g.doc.nodes.find((n) => n.id === ${lit(tail)}).ui.position),
             selected: [...window.__lyflow.stores.ui.getState().selectedNodes] };
  `);
  report.eq("链尾上再按 Alt+→：走不动，链尾不被挪、不记撤销", tailAfter, { ...tailBefore, selected: [tail] });

  // 拖动帧率：页面里挂一个 rAF 采样器，然后用 CDP 发**真**鼠标事件拖一个节点。
  // 合成 MouseEvent 骗不过 React Flow 的 d3-drag（它要读 event.view.document）。
  await cdp.eval(`
    // 采样循环闭包住自己这一份：上一段的循环还挂着一帧回调，读全局对象的话它会接着往新的这份里写，
    // 两个循环交替记帧间隔、一半是 0，中位数就成了几千 fps（扫过那条以前就是这么「过」的）
    const state = { frames: [], last: performance.now(), stop: false };
    window.__m4fps = state;
    const tick = () => {
      const now = performance.now();
      state.frames.push(now - state.last);
      state.last = now;
      if (!state.stop) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    return true;
  `);
  // 拖的必须是中心点真落在它身上的那个节点：DOM 里第一个节点可能在画布上沿外面（被工具栏盖着），
  // 以前就是按在工具栏上「拖」—— 节点根本没动，量的是鼠标按着扫过一片节点的 hover
  const grab = await cdp.eval(`
    for (const el of document.querySelectorAll('.react-flow__node')) {
      const r = el.getBoundingClientRect();
      const x = Math.round(r.left + r.width / 2), y = Math.round(r.top + r.height / 2);
      const hit = document.elementFromPoint(x, y);
      if (hit && hit.closest('.react-flow__node') === el && !hit.closest('.nodrag')) return { id: el.getAttribute('data-id'), x, y };
    }
    return null;
  `);
  mustOk(grab != null, "画布上有一个中心点露在外面的节点", grab);
  const posOf = (id) => cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(id)}).ui.position;`);
  const before = await posOf(grab.id);
  await dragMouse(cdp, grab, { x: grab.x + 160, y: grab.y + 90 }, { steps: 20 });
  const fps = await cdp.eval(`
    window.__m4fps.stop = true;
    const f = window.__m4fps.frames.slice(3).sort((a, b) => a - b);
    if (f.length === 0) return null;
    return Math.round(1000 / f[Math.floor(f.length / 2)]);
  `);
  const after = await posOf(grab.id);
  report.ok("拖的那个节点真的挪了", after.x !== before.x || after.y !== before.y, JSON.stringify({ before, after }));
  report.ok(`拖动时的帧率 ${fps} fps ≥ 30`, fps != null && fps >= 30, `${fps} fps`);

  // 不按键从画布左上扫到右下，hover 一路进出几十个节点：以前每进出一次几百条边各自开关 opacity
  // （每条一个合成效果节点），只剩十几帧；现在淡化由画布上一个属性统一做（styles.motion.css）
  const box = await cdp.eval(`const r = document.querySelector('.react-flow__pane').getBoundingClientRect();
    return { x: r.left, y: r.top, w: r.width, h: r.height };`);
  await cdp.eval(`
    // 采样循环闭包住自己这一份：上一段的循环还挂着一帧回调，读全局对象的话它会接着往新的这份里写，
    // 两个循环交替记帧间隔、一半是 0，中位数就成了几千 fps（扫过那条以前就是这么「过」的）
    const state = { frames: [], last: performance.now(), stop: false };
    window.__m4fps = state;
    const tick = () => {
      const now = performance.now();
      state.frames.push(now - state.last);
      state.last = now;
      if (!state.stop) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    return true;
  `);
  for (let i = 0; i <= 40; i += 1) {
    const t = i / 40;
    await cdp.send("Input.dispatchMouseEvent", {
      type: "mouseMoved",
      x: Math.round(box.x + box.w * (0.25 + 0.5 * t)),
      y: Math.round(box.y + box.h * (0.15 + 0.7 * t)),
      buttons: 0,
    });
    await sleep(12);
  }
  // 挪出画布再收尾：别把 hover 留给下一组
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: Math.round(box.x + box.w / 2), y: Math.round(box.y) - 8, buttons: 0 });
  await sleep(150);
  const hoverFps = await cdp.eval(`
    window.__m4fps.stop = true;
    const f = window.__m4fps.frames.slice(3).sort((a, b) => a - b);
    if (f.length === 0) return null;
    return Math.round(1000 / f[Math.floor(f.length / 2)]);
  `);
  report.ok(`鼠标扫过 300 节点的图时的帧率 ${hoverFps} fps ≥ 20（修前 14 上下）`, hoverFps != null && hoverFps >= 20, `${hoverFps} fps`);

  // 事件合并：一次运行下来 store 的更新次数远少于事件数
  await newDoc(cdp);
  const merged = await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    const ids = [];
    for (let i = 0; i < 40; i += 1) {
      const op = i === 0 ? 'gen.synthetic' : 'util.reroute';
      const id = b.stores.graph.getState().addNode(op, { x: i * 40, y: (i % 6) * 90 });
      ids.push(id);
      if (i > 0) {
        b.stores.graph.getState().connect(
          { node: ids[i - 1], port: i === 1 ? 'cloud' : 'out' },
          { node: id, port: 'in' },
        );
      }
    }
    void g;
    b.clearTransitions();
    let updates = 0;
    const stop = b.stores.execution.subscribe((s, p) => { if (s.nodes !== p.nodes) updates += 1; });
    const before = b.stores.execution.getState().runId;
    await b.run({});
    for (let i = 0; i < 400; i += 1) {
      const s = b.stores.execution.getState();
      if (s.runId !== before && s.runStatus !== 'running' && s.runStatus !== 'idle') break;
      await new Promise((d) => setTimeout(d, 25));
    }
    stop();
    return { updates, transitions: b.transitions.length };
  `);
  report.ok(
    `40 个节点的运行：${merged.transitions} 次状态变化合并成 ${merged.updates} 次 store 更新`,
    merged.updates < merged.transitions,
    JSON.stringify(merged),
  );
}

// ------------------------------------------------------ Ctrl+G / Ctrl+Shift+G

async function suiteShortcuts(cdp, report) {
  report.section("§1 快捷键：Ctrl+G 合成、Ctrl+Shift+G 解散，提升的参数落回内参");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES.slice(0, 4), CHAIN_EDGES.slice(0, 3));
  await cdp.eval(`
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    window.__lyflow.stores.ui.getState().setSelection([${lit(ids.voxel)}, ${lit(ids.sor)}], []);
    return true;
  `);
  await pressCtrl(cdp, "G");
  await sleep(250);
  const composed = await snapshot(cdp);
  report.eq("Ctrl+G 之后顶层剩三个节点", composed.level.nodes.length, 3);
  report.eq("多了一份子图定义", Object.keys(composed.subgraphs).length, 1);

  const subNode = composed.doc.nodes.find((n) => n.op.startsWith("sub:"));
  const subgraphId = subNode.op.slice("sub:".length);
  // 提升一个参数并改掉它，解散之后这个值必须落回内参（原 suiteDissolve 并到这里）。
  // 提升与改值各记一条撤销，下面数撤销时要跳过这两条。
  await cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().enterSubgraph({ nodeId: ${lit(subNode.id)}, subgraphId: ${lit(subgraphId)} });
    b.stores.graph.getState().promoteParam(${lit(ids.voxel)}, 'leafSize');
    b.stores.ui.getState().exitTo(0);
    b.stores.graph.getState().setParam(${lit(subNode.id)}, 'leafSize', [0.07, 0.07, 0.07]);
    return true;
  `);
  await cdp.eval(`
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    window.__lyflow.stores.ui.getState().setSelection([${lit(subNode.id)}], []);
    return true;
  `);
  await pressCtrl(cdp, "G", ["shift"]);
  await sleep(250);
  const dissolved = await snapshot(cdp);
  report.eq("Ctrl+Shift+G 之后回到四个节点", dissolved.level.nodes.length, 4);
  report.eq("子图定义也一并清掉", Object.keys(dissolved.subgraphs).length, 0);
  const leaf = dissolved.doc.nodes
    .map((n) => n.params?.leafSize)
    .find((v) => Array.isArray(v));
  report.eq("提升参数的值落回了内参", leaf, [0.07, 0.07, 0.07]);

  // 撤销栈：合成与解散各是一条
  await pressCtrl(cdp, "Z");
  await sleep(150);
  report.eq("撤销回到子图状态", (await snapshot(cdp)).level.nodes.length, 3);
  // 再撤掉改值、提升两条，第三下撤销的就是合成
  await pressCtrl(cdp, "Z");
  await sleep(150);
  await pressCtrl(cdp, "Z");
  await sleep(150);
  await pressCtrl(cdp, "Z");
  await sleep(150);
  report.eq("再撤销回到四个节点", (await snapshot(cdp)).level.nodes.length, 4);
}


// -------------------------------------------------------- M3 留下的三个尾巴

async function suiteM3Tails(cdp, report, ws) {
  report.section("M3 尾巴：法线着色可用、PNG 导出走保存对话框、core-watch 见 m4-acceptance");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  // 自带颜色、没有强度的点云（C ABI v12 起 rgb 进了载荷）：三个点，红绿蓝打包成 uint32
  const colored = path.join(ws.dir, "彩色 三点.pcd");
  fs.writeFileSync(
    colored,
    "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z rgb\nSIZE 4 4 4 4\nTYPE F F F U\nCOUNT 1 1 1 1\n" +
      "WIDTH 3\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 3\nDATA ascii\n" +
      "0 0 0 16711680\n0.1 0 0 65280\n0.2 0 0 255\n",
  );
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed: 61 } },
      { key: "nrm", op: "features.normals", params: { kSearch: 10 } },
      { key: "rgb", op: "io.load_pcd", params: { path: colored } },
    ],
    [{ from: ["gen", "cloud"], to: ["nrm", "cloud"] }],
  );
  // 跑通与否不单独断言：下面「带法线的点云也画出来了」要靠这次运行的结果
  await runAndWait(cdp, () => pressF5(cdp));

  // 源头没有法线：下拉框里那一项应当还是灰的
  await selectAndReadViewer(cdp, ids.gen);
  const beforeOpt = await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const opt = [...sel.options].find((o) => o.value === 'normal');
    return { disabled: opt.disabled, text: opt.textContent };
  `);
  report.ok("没有法线通道时「法线」是禁用的", beforeOpt.disabled === true, JSON.stringify(beforeOpt));

  const withNormals = await selectAndReadViewer(cdp, ids.nrm);
  report.ok("带法线的点云也画出来了", withNormals.count > 0, JSON.stringify(withNormals));
  const afterOpt = await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const opt = [...sel.options].find((o) => o.value === 'normal');
    return { disabled: opt.disabled, text: opt.textContent };
  `);
  report.ok("有法线通道时「法线」可选了", afterOpt.disabled === false, JSON.stringify(afterOpt));

  const shaded = await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, 'normal');
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    await new Promise((d) => setTimeout(d, 250));
    const v = document.querySelector('.viewer');
    return { shading: v.getAttribute('data-shading'), value: sel.value };
  `);
  report.eq("切到法线着色之后视图确实是 normal", shaded.shading, "normal");

  // 默认的「强度」遇到没有强度、自带颜色的点云，退到 RGB 而不是高度
  await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, 'intensity');
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);
  await selectAndReadViewer(cdp, ids.rgb);
  const rgb = await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const opt = [...sel.options].find((o) => o.value === 'rgb');
    return { shading: document.querySelector('.viewer').getAttribute('data-shading'), rgbEnabled: opt ? !opt.disabled : null };
  `);
  report.eq("自带颜色、没有强度的点云默认按 RGB 着色", rgb, { shading: "rgb", rgbEnabled: true });

  // PNG 导出的落盘那一半：对话框是原生的，CDP 驱动不了，但写文件这一步可以验
  const target = path.join(ws.dir, "导出 视图.png");
  const wrote = await cdp.eval(`
    const bytes = [137, 80, 78, 71, 13, 10, 26, 10];
    await window.__lyflow.transport.writeFileBytes(${lit(target)}, new Uint8Array(bytes));
    return true;
  `);
  const head = fs.existsSync(target) ? fs.readFileSync(target) : null;
  report.ok(
    "write_file_bytes 把字节一字不差地写到了用户指定的路径",
    wrote === true && head !== null && head.length === 8 && head[1] === 0x50,
    `${target} ${head === null ? "文件不存在" : head.toString("hex")}`,
  );

  // 路径框里粘一个资源管理器「复制文件地址」给的带引号、前后有空格的路径：存下的是去掉引号与空白的，跑得通。
  // 以前原样存下、跑的时候才报「文件不存在」。桌面壳没有文件对话框：不摆一个点了只弹提示的「浏览…」
  const copy = path.join(ws.dir, "彩色 副本.pcd");
  fs.copyFileSync(colored, copy);
  await select(cdp, ids.rgb);
  await sleep(200);
  await clickAt(cdp, await centerOf(cdp, '[data-testid="param-path"] input.ctl--str'));
  await sleep(100);
  await pressCtrl(cdp, "a");
  await cdp.send("Input.insertText", { text: `  "${copy}"  ` });
  await pressKey(cdp, "Enter", 13);
  await sleep(200);
  const pasted = await cdp.eval(`
    const n = window.__lyflow.stores.graph.getState().doc.nodes.find((x) => x.id === ${lit(ids.rgb)});
    return { param: n.params.path, shown: document.querySelector('[data-testid="param-path"] input.ctl--str')?.value ?? null,
             browse: !!document.querySelector('[data-testid="param-path"] .ctl-btn') };
  `);
  const ranPasted = await runAndWait(cdp, () => pressF5(cdp));
  await pressCtrl(cdp, "z");
  await sleep(150);
  report.ok("路径框里粘带引号、前后有空格的路径：存下的去掉了引号与空白、框里也是，跑得通；没有文件对话框时不摆「浏览…」；Ctrl+Z 回到原来的",
    pasted.param === copy && pasted.shown === copy && !pasted.browse &&
      ["done", "skipped"].includes(ranPasted.nodes[ids.rgb]?.state) &&
      (await cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.find((x) => x.id === ${lit(ids.rgb)}).params.path;`)) === colored,
    JSON.stringify({ pasted, state: ranPasted.nodes[ids.rgb]?.state, errors: ranPasted.nodes[ids.rgb]?.errors }));

  // 存着的路径带空格（老图、CLI --set 写进来的）：点进框里看一眼再点出来，不悄悄改掉它、不记撤销
  const spaced = `${colored} `;
  const pastSpaced = await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.rgb)}, 'path', ${lit(spaced)});
    return window.__lyflow.stores.graph.getState().past.length;
  `);
  await sleep(150);
  await clickAt(cdp, await centerOf(cdp, '[data-testid="param-path"] input.ctl--str'));
  await sleep(100);
  await pressKey(cdp, "Tab", 9);
  await sleep(150);
  const untouched = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    return { path: g.doc.nodes.find((x) => x.id === ${lit(ids.rgb)}).params.path, past: g.past.length };
  `);
  report.eq("存着的路径带空格：点进框里再点出来，不改它、不记撤销", untouched, { path: spaced, past: pastSpaced });
}

/** 子图内部节点出错（事件 id 是路径，ADR-0010）：顶层原来只看得到「这个子图红了」、诊断里是一串路径 id，
 *  点诊断也进不去子图（选中的是一个顶层没有的 id）。 */
async function suiteInnerError(cdp, report) {
  report.section("§1.6 子图内部出错：子图节点上写明是哪个内部节点，点它或点诊断都打开到那个节点");
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 2000 } },
      { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0, 0.01, 0.01] } },
      { key: "sor", op: "filter.statistical_outlier", params: { meanK: 10 } },
    ],
    [
      { from: ["gen", "cloud"], to: ["voxel", "cloud"] },
      { from: ["voxel", "cloud"], to: ["sor", "cloud"] },
    ],
  );
  const composed = await compose(cdp, [ids.voxel, ids.sor]);
  mustOk(Boolean(composed?.nodeId), "voxel 与 sor 合成了子图", JSON.stringify(composed));
  // 坏参数让这次运行失败；失败本身不单独断言，下面子图节点与诊断里写着的那条就是证据
  await runAndWait(cdp, () => pressF5(cdp));
  const voxelLabel = await cdp.eval(`return window.__lyflow.stores.manifest.getState().operatorsById.get('filter.voxel_grid').label;`);
  const where = () => cdp.eval(`
    const s = window.__lyflow.stores.ui.getState();
    return { path: s.path.map((p) => p.nodeId), selected: [...s.selectedNodes], param: s.focusedDiagnostic?.paramPath ?? null };
  `);

  const err = await cdp.eval(`return document.querySelector('[data-testid="node-err-${composed.nodeId}"]')?.textContent ?? null;`);
  report.ok(`子图节点上的错误写明是内部的「${voxelLabel}」`, typeof err === "string" && err.startsWith(`${voxelLabel}：`), String(err));
  await cdp.eval(`document.querySelector('[data-testid="node-err-${composed.nodeId}"]').click(); return true;`);
  await sleep(300);
  report.eq("点它：打开到子图、选中出错的那个节点、参数红框标在 leafSize 上", await where(),
    { path: [composed.nodeId], selected: [ids.voxel], param: "leafSize" });

  await cdp.eval(`window.__lyflow.stores.ui.getState().exitTo(0); window.__lyflow.stores.ui.getState().toggleDrawer('diagnostics'); return true;`);
  await sleep(300);
  const item = `diag-${composed.nodeId}/${ids.voxel}`;
  const name = await cdp.eval(`return document.querySelector('[data-testid="${item}"] .drawer__diag-node')?.textContent ?? null;`);
  report.ok("诊断里的名字带着它所在的子图（不是一串路径 id）", typeof name === "string" && name.includes(" › ") && name.endsWith(voxelLabel), String(name));
  await cdp.eval(`document.querySelector('[data-testid="${item}"]').click(); return true;`);
  await sleep(300);
  report.eq("点诊断：同样打开到那个节点", await where(), { path: [composed.nodeId], selected: [ids.voxel], param: "leafSize" });
  await cdp.eval(`window.__lyflow.stores.ui.getState().exitTo(0); window.__lyflow.stores.ui.setState({ drawer: null }); return true;`);

  await sleep(200);
  await cdp.eval(`document.querySelector('[data-testid="run-summary-error"]').click(); return true;`);
  await sleep(300);
  report.eq("点工具栏的「error 1」：定位到第一个出错的节点（在子图里也打开进去）", await where(),
    { path: [composed.nodeId], selected: [ids.voxel], param: "leafSize" });
  await cdp.eval(`const u = window.__lyflow.stores.ui.getState(); u.exitTo(0); u.setSelection([], []); return true;`);
  await sleep(200);
  await pressKey(cdp, "F8", 119);
  await sleep(300);
  report.eq("按 F8：同样跳到那个出错的节点", await where(), { path: [composed.nodeId], selected: [ids.voxel], param: "leafSize" });

  // 检查器：选中子图节点，错误一条条写着来自哪个内部节点，点它也打开到那里
  await cdp.eval(`const u = window.__lyflow.stores.ui.getState(); u.exitTo(0); u.setSelection([${lit(composed.nodeId)}], []); return true;`);
  await sleep(300);
  const src = await cdp.eval(`return document.querySelector('[data-testid="inspector-error-source"]')?.textContent ?? null;`);
  report.eq("检查器里那条错误前写着内部节点的名字", src, voxelLabel);
  await cdp.eval(`document.querySelector('[data-testid="inspector-error-source"]').click(); return true;`);
  await sleep(300);
  report.eq("点它：同样打开到那个节点", await where(), { path: [composed.nodeId], selected: [ids.voxel], param: "leafSize" });
  await cdp.eval(`window.__lyflow.stores.ui.getState().exitTo(0); return true;`);
}

/** 合成 → 进出子图 → 参数提升，三段共用合成出来的那一张图。要有名字：`pnpm e2e --only m4:suiteCompose`
 *  按函数名挑组，原来这里是个匿名箭头函数（name 是空串），挑不出来。 */
async function suiteCompose(cdp, report) {
  const fixture = await composeChecks(cdp, report);
  await suiteNavigate(cdp, report, fixture);
  await suitePromote(cdp, report, fixture);
}

export const m4Suites = [
  suiteCompose,
  suiteInnerError,
  suiteShortcuts,
  suiteNested,
  suiteRecursion,
  suiteLibrary,
  suitePreview,
  suiteBigGraph,
  suiteM3Tails,
];
