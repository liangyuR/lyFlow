import { sleep } from "./cdp.mjs";
import {
  buildGraph,
  canvasBox,
  centerOf,
  clickAt,
  clickUntilPicked,
  dragMouse,
  lit,
  mustOk,
  newDoc,
  normalizeZoom,
  placeAtScreen,
  pressEscape,
  pressF5,
  runAndWait,
  select,
} from "./page.mjs";

const CHAIN_NODES = [
  { key: "gen", op: "gen.synthetic", params: { pointCount: 5000, seed: 21 } },
  { key: "pass", op: "filter.passthrough", params: { min: -100, max: 100 } },
  { key: "pick", op: "segment.extract_indices" },
  { key: "fit", op: "fit.line_2d", params: { distThresh: 0.2 } },
  { key: "rr", op: "util.reroute" },
];

const CHAIN_EDGES = [
  { from: ["gen", "cloud"], to: ["pass", "cloud"] },
  { from: ["gen", "cloud"], to: ["pick", "cloud"] },
  { from: ["pass", "indices"], to: ["pick", "indices"] },
  { from: ["pick", "selected"], to: ["fit", "cloud"] },
  { from: ["fit", "line"], to: ["rr", "in"] },
];

const peekWindows = (cdp) => cdp.eval(`return window.__lyflow.snapshot().peek;`);

const resetPeek = (cdp) =>
  cdp.eval(`window.__lyflow.stores.peek.getState().closeAll(); return true;`);

function edgeOf(cdp, fromNode, fromPort, toNode, toPort) {
  return cdp.eval(`
    const doc = window.__lyflow.stores.graph.getState().doc;
    const e = doc.edges.find((x) =>
      x.from.node === ${lit(fromNode)} && x.from.port === ${lit(fromPort)} &&
      x.to.node === ${lit(toNode)} && x.to.port === ${lit(toPort)});
    return e ? e.id : null;
  `);
}

function portStat(cdp, nodeId, port) {
  return cdp.eval(`
    const e = window.__lyflow.stores.execution.getState();
    const n = e.nodes.get(${lit(nodeId)});
    const o = n ? (n.stats?.outputs ?? []).find((x) => x.port === ${lit(port)}) : null;
    return o ? { type: o.type, elementCount: o.elementCount, value: o.value ?? null } : null;
  `);
}

async function edgePoint(cdp, edgeId) {
  const selector = `.react-flow__edge[data-id="${edgeId}"]`;
  return cdp.eval(`
    const group = document.querySelector(${lit(selector)});
    if (!group) return null;
    const path = group.querySelector('.react-flow__edge-interaction') || group.querySelector('path');
    if (!path || typeof path.getTotalLength !== 'function') return null;
    const len = path.getTotalLength();
    const ctm = path.getScreenCTM();
    if (!ctm || !(len > 0)) return null;
    const cuts = [0.5, 0.45, 0.55, 0.4, 0.6, 0.35, 0.65, 0.3, 0.7, 0.25, 0.75, 0.2, 0.8];
    for (const t of cuts) {
      const p = path.getPointAtLength(len * t);
      const s = new DOMPoint(p.x, p.y).matrixTransform(ctm);
      const x = Math.round(s.x);
      const y = Math.round(s.y);
      const hit = document.elementFromPoint(x, y);
      const owner = hit && hit.closest ? hit.closest('.react-flow__edge') : null;
      if (owner && owner.getAttribute('data-id') === ${lit(edgeId)}) return { x, y, t };
    }
    return null;
  `);
}

async function doubleClickAt(cdp, point) {
  const common = { x: point.x, y: point.y, button: "left", clickCount: 2, buttons: 1 };
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: point.x, y: point.y, buttons: 0 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", ...common });
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...common });
  await sleep(320);
}

async function openByDoubleClick(cdp, report, edgeId, label) {
  void report;
  const point = await edgePoint(cdp, edgeId);
  mustOk(Boolean(point), `${label}：在这条边上找到了真能点中的落点`,
    `edge=${edgeId} point=${JSON.stringify(point)}`);
  const before = await peekWindows(cdp);
  await doubleClickAt(cdp, point);
  const after = await peekWindows(cdp);
  const win = after.find((w) => !before.some((b) => b.id === w.id)) ?? null;
  return { point, before, after, win };
}

async function park(cdp) {
  const box = await canvasBox(cdp);
  await cdp.eval(`
    const s = window.__lyflow.stores.peek;
    const ids = s.getState().windows.map((w) => w.id);
    ids.forEach((id, i) => s.getState().move(id, {
      x: ${box.x + 8} + i * 28,
      y: ${box.y + box.h - 56},
    }));
    return ids.length;
  `);
  await sleep(150);
}

function readPeek(cdp, peekId) {
  const root = `[data-testid="edge-peek"][data-peek-id="${peekId}"]`;
  return cdp.eval(`
    const el = document.querySelector(${lit(root)});
    if (!el) return null;
    const q = (sel) => el.querySelector(sel);
    const text = (sel) => { const n = q(sel); return n ? n.textContent : null; };
    const rows = {};
    for (const tr of el.querySelectorAll('[data-testid="peek-value-table"] tr')) {
      const td = tr.querySelector('td');
      rows[tr.getAttribute('data-field')] = td ? td.textContent : '';
    }
    const head = q('[data-testid="peek-indices"] .peek-indices__head');
    const tensor = q('[data-testid="peek-tensor"]');
    const tcanvas = q('[data-testid="peek-tensor-canvas"]');
    const base = q('[data-testid="peek-base"]');
    const tag = q('[data-testid="peek-locked-tag"]');
    const png = q('[data-testid="peek-export-png"]');
    return {
      view: el.getAttribute('data-view'),
      type: el.getAttribute('data-type'),
      locked: el.getAttribute('data-locked'),
      edgeId: el.getAttribute('data-edge-id'),
      title: text('[data-testid="peek-title"]'),
      lockedTag: tag ? tag.textContent : null,
      lockedRun: tag ? tag.getAttribute('data-run') : null,
      hasOpenMain: !!q('[data-testid="peek-open-main"]'),
      pngDisabled: png ? png.disabled : null,
      status: text('[data-testid="peek-status"]'),
      cloudStatus: text('[data-testid="peek-cloud-status"]'),
      cloudCanvas: !!q('[data-testid="peek-cloud-canvas"] canvas'),
      countText: text('.peek-cloud__count'),
      base: base ? base.getAttribute('data-node') : null,
      baseText: base ? base.textContent : null,
      views: [...el.querySelectorAll('.peek__view')].map((b) => b.getAttribute('data-testid')),
      rows,
      indicesTotal: head ? Number(head.getAttribute('data-total')) : null,
      indicesSource: head ? head.getAttribute('data-source-cloud') : null,
      tensorMsg: text('[data-testid="peek-tensor-msg"]'),
      tensor: tensor
        ? {
            rank: Number(tensor.getAttribute('data-rank')),
            layout: tensor.getAttribute('data-layout'),
            sliceCount: Number(tensor.getAttribute('data-slice-count')),
          }
        : null,
      tensorCanvas: tcanvas
        ? { w: Number(tcanvas.getAttribute('data-w')), h: Number(tcanvas.getAttribute('data-h')) }
        : null,
    };
  `);
}

async function waitPeek(cdp, peekId, ready, tries = 70) {
  let dom = null;
  for (let i = 0; i < tries; i += 1) {
    dom = await readPeek(cdp, peekId);
    if (dom && ready(dom)) return dom;
    await sleep(120);
  }
  return dom;
}

function countsOf(text) {
  if (typeof text !== "string" || text.length === 0) return null;
  const parts = text
    .replace(/\s/g, "")
    .split("/")
    .map((s) => Number(s.replace(/[^\d]/g, "")));
  if (parts.length === 0 || !Number.isFinite(parts[0])) return null;
  return { shown: parts[0], total: Number.isFinite(parts[1]) ? parts[1] : parts[0] };
}

function clickIn(cdp, peekId, selector) {
  const root = `[data-testid="edge-peek"][data-peek-id="${peekId}"]`;
  return cdp.eval(`
    const el = document.querySelector(${lit(root)});
    const btn = el ? el.querySelector(${lit(selector)}) : null;
    if (!btn) return false;
    btn.click();
    return true;
  `);
}

async function inspectorOutput(cdp, nodeId, port) {
  await select(cdp, nodeId);
  await sleep(220);
  return cdp.eval(`
    const row = document.querySelector('[data-testid="output-${port}"]');
    if (!row) return null;
    const v = row.querySelector('.insp-out__value');
    return v ? v.textContent : null;
  `);
}

async function spread(cdp, ids) {
  await normalizeZoom(cdp, 0.6);
  const box = await canvasBox(cdp);
  const at = (f) => Math.round(box.w * f);
  await placeAtScreen(cdp, {
    [ids.gen]: { x: 12, y: 44 },
    [ids.pass]: { x: at(0.22), y: 200 },
    [ids.pick]: { x: at(0.43), y: 44 },
    [ids.fit]: { x: at(0.63), y: 44 },
    [ids.rr]: { x: at(0.83), y: 44 },
  });
  return box;
}

async function prepare(cdp, genParams = {}) {
  await newDoc(cdp);
  await resetPeek(cdp);
  const nodes = CHAIN_NODES.map((n) =>
    n.key === "gen" ? { ...n, params: { ...n.params, ...genParams } } : n,
  );
  const ids = await buildGraph(cdp, nodes, CHAIN_EDGES);
  await spread(cdp, ids);
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "查看器要看的这张图跑通了", JSON.stringify(run.nodes));
  return { ids, run };
}

/** 点云 / 2D 几何 / Indices 三组看的是同一张图：只 prepare 一次（省两次运行），
 *  组与组之间 resetPeek。每组各自兜住异常，一组中断不连累后面两组。 */
async function suiteSharedGraphPeeks(cdp, report) {
  report.section("Edge Peek：点云 / 2D 几何 / Indices 三组共用的图");
  const fixture = await prepare(cdp);
  for (const suite of [suiteCloudPeek, suiteShapeAndValuePeek, suiteIndicesPeek]) {
    await resetPeek(cdp);
    try {
      await suite(cdp, report, fixture);
    } catch (e) {
      report.fail(`分组 ${suite.name} 中断`, e.stack ?? String(e));
    }
  }
}

async function suiteCloudPeek(cdp, report, fixture) {
  report.section("双击点云边：浮窗弹出、默认 3D、点数与该端口一致；再双击不重复开");

  const { ids } = fixture;
  const edge = await edgeOf(cdp, ids.gen, "cloud", ids.pass, "cloud");
  mustOk(typeof edge === "string", "找得到那条点云边", String(edge));

  const opened = await openByDoubleClick(cdp, report, edge, "点云边");
  report.eq("双击开出了一个新窗", opened.after.length, opened.before.length + 1);
  if (!opened.win) {
    report.fail("双击没有开出窗", JSON.stringify(opened.after));
    return;
  }

  // 测量（与主预览共用 hooks/useMeasure）：窗口自己的开关，单击点中一个点；主预览的测量不跟着开。
  // 放在 park 之前：挪到角落之后窗口的画布大半在画面外，点不准
  await waitPeek(cdp, opened.win.id, (d) => d.cloudCanvas && countsOf(d.countText));
  const winSel = `[data-testid="edge-peek"][data-peek-id="${opened.win.id}"]`;
  await clickIn(cdp, opened.win.id, '[data-testid="peek-measure"]');
  const peekPicks = () => cdp.eval(`
    return Number(document.querySelector(${lit(winSel + ' [data-testid="peek-cloud"]')})?.getAttribute('data-measure') ?? -1);
  `);
  const picked = await clickUntilPicked(cdp, await centerOf(cdp, `${winSel} [data-testid="peek-cloud-canvas"]`),
    async () => (await peekPicks()) === 1);
  const mainMeasuring = await cdp.eval(`return document.querySelector('.viewer').getAttribute('data-measuring');`);
  report.ok("查看器里开「测量」：单击点中一个点，主预览的测量没跟着开",
    Boolean(picked) && mainMeasuring === "0", JSON.stringify({ picked, mainMeasuring }));
  await clickIn(cdp, opened.win.id, '[data-testid="peek-measure"]');
  report.eq("再点一次关掉、点位清掉", await peekPicks(), 0);

  // ⤢：转过视角之后回到全貌。相机带阻尼，等读数停下来再记
  const canvasSel = `${winSel} [data-testid="peek-cloud-canvas"]`;
  const steadyCamera = async () => {
    let last = null;
    for (let i = 0; i < 40; i += 1) {
      const now = await cdp.eval(`return document.querySelector(${lit(canvasSel)})?.dataset.cameraPos ?? null;`);
      if (now !== null && now === last) return now.split(",").map(Number);
      last = now;
      await sleep(150);
    }
    return last ? last.split(",").map(Number) : null;
  };
  const cam0 = await steadyCamera();
  const mid = await centerOf(cdp, canvasSel);
  await dragMouse(cdp, mid, { x: mid.x + 70, y: mid.y + 25 });
  const cam1 = await steadyCamera();
  await clickAt(cdp, await centerOf(cdp, `${winSel} [data-testid="peek-fit"]`));
  const cam2 = await steadyCamera();
  const near = (a, b) => a && b && a.every((v, i) => Math.abs(v - b[i]) <= 1e-2 * Math.max(1, Math.abs(b[i])));
  report.ok("查看器里真鼠标转过视角，点 ⤢ 回到全貌", cam0 && cam1 && !near(cam1, cam0) && near(cam2, cam0),
    JSON.stringify({ cam0, cam1, cam2 }));

  // 转过视角之后重跑（清掉缓存，窗里换成新的一片云、还在同一个地方）：视角留着，不拉回全貌
  await dragMouse(cdp, mid, { x: mid.x - 60, y: mid.y + 30 });
  const cam3 = await steadyCamera();
  const shownRun = () => cdp.eval(`return document.querySelector(${lit(winSel + ' [data-testid="peek-cloud"]')})?.dataset.run ?? null;`);
  const runBefore = await shownRun();
  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  const rerun = await runAndWait(cdp, () => pressF5(cdp));
  let runAfter = runBefore;
  for (let i = 0; i < 40 && runAfter === runBefore; i += 1) {
    await sleep(150);
    runAfter = await shownRun();
  }
  await waitPeek(cdp, opened.win.id, (d) => d.cloudCanvas && countsOf(d.countText));
  const cam4 = await steadyCamera();
  report.ok("查看器里转过视角之后重跑：窗里换成了新的一次，视角留着（不拉回全貌）",
    rerun.status === "ok" && runAfter !== runBefore && cam3 && !near(cam3, cam2) && near(cam4, cam3),
    JSON.stringify({ runBefore, runAfter, cam2, cam3, cam4 }));

  // 点大小：+ 放大（降采样后只剩几千点时看得清）
  const sizeOf = () => cdp.eval(`return Number(document.querySelector(${lit(winSel + ' [data-testid="peek-cloud"]')})?.dataset.pointSize);`);
  const size0 = await sizeOf();
  await clickAt(cdp, await centerOf(cdp, `${winSel} [data-testid="peek-point-bigger"]`));
  const size1 = await sizeOf();
  report.ok("点 + 点变大", size1 > size0, `${size0} → ${size1}`);
  await park(cdp);

  report.eq("窗口挂在被双击的那条边上", opened.win.edgeId, edge);
  report.eq("源端口就是边的起点", [opened.win.node, opened.win.port], [ids.gen, "cloud"]);
  report.eq("运行时类型判成 PointCloud", opened.win.type, "PointCloud");
  report.eq("默认视图是 3D 点云", opened.win.view, "cloud3d");

  const dom = await waitPeek(cdp, opened.win.id, (d) => d.cloudCanvas && countsOf(d.countText));
  report.ok("3D 画布真的建起来了", dom?.cloudCanvas === true, JSON.stringify(dom));
  report.ok("窗里没有「未运行」之类的占位", dom?.status === null, String(dom?.status));

  const stat = await portStat(cdp, ids.gen, "cloud");
  const counts = countsOf(dom?.countText);
  report.eq("窗里的总点数 = 该端口报的点数", counts?.total ?? null, stat?.elementCount ?? null);

  report.eq("点云窗上「导出 PNG」可用", dom?.pngDisabled ?? null, false);

  // 「在主 3D 视图打开」按钮在不在、点不点得动，由下面主视图钉没钉住来判
  await clickIn(cdp, opened.win.id, '[data-testid="peek-open-main"]');
  await sleep(400);
  const pinState = await cdp.eval(`
    const v = document.querySelector('.viewer');
    return {
      pinnedNode: window.__lyflow.stores.ui.getState().pinnedNode,
      viewerPinned: v ? v.getAttribute('data-pinned') : null,
    };
  `);
  report.eq("主 3D 视图钉在了这条边的源节点上", pinState.pinnedNode, ids.gen);
  report.eq("主 3D 视图上标出了「已钉住」", pinState.viewerPinned, "1");
  await cdp.eval(`window.__lyflow.stores.ui.getState().setPinnedNode(null); return true;`);

  const again = await openByDoubleClick(cdp, report, edge, "同一条点云边再双击");
  report.eq("同一条边再双击不重复开窗，还是原来那一个窗",
    (again?.after ?? []).map((w) => w.id), [opened.win.id]);

  await pressEscape(cdp);
  await sleep(250);
  const afterEscape = await peekWindows(cdp);
  report.eq("没有运行在跑时 Esc 关掉最前面的浮窗", afterEscape.length, 0);

  // 新开的窗口沿用主预览的着色（以前固定是强度，主预览里选好的要一个窗口一个窗口重选）
  const prefs = await cdp.eval(`
    const s = window.__lyflow.stores.ui.getState();
    const before = { prefs: s.viewerPrefs, raw: localStorage.getItem('lyflow.viewer.display') };
    s.setViewerPrefs({ shading: 'height' });
    return before;
  `);
  try {
    const reopened = await openByDoubleClick(cdp, report, edge, "主预览切到高度后再开");
    const shading = reopened?.win
      ? (await cdp.eval(`
          await new Promise((r) => setTimeout(r, 300));
          return document.querySelector(${lit(`[data-testid="edge-peek"][data-peek-id="${reopened.win.id}"] [data-testid="peek-cloud"]`)})?.dataset.shading ?? null;
        `))
      : null;
    report.eq("主预览切到高度着色后，新开的窗口也是高度", shading, "height");
  } finally {
    await cdp.eval(`
      const before = ${lit(prefs)};
      window.__lyflow.stores.ui.setState({ viewerPrefs: before.prefs });
      if (before.raw === null) localStorage.removeItem('lyflow.viewer.display');
      else localStorage.setItem('lyflow.viewer.display', before.raw);
      window.__lyflow.stores.peek.getState().closeAll();
      return true;
    `);
  }
}

async function suiteShapeAndValuePeek(cdp, report, fixture) {
  report.section("双击 2D 几何边：默认正交视图 + 底图标签；文本视图与 Inspector 同一口径");

  const { ids } = fixture;
  const edge = await edgeOf(cdp, ids.fit, "line", ids.rr, "in");
  mustOk(typeof edge === "string", "找得到那条 Line2D 边", String(edge));

  const opened = await openByDoubleClick(cdp, report, edge, "Line2D 边");
  if (!opened?.win) {
    report.fail("Line2D 边没开出窗", JSON.stringify(opened?.after));
    return;
  }
  await park(cdp);

  report.eq("运行时类型判成 Line2D", opened.win.type, "Line2D");
  report.eq("默认视图是正交 2D（几何要有剖面当参照）", opened.win.view, "cloud2d");

  const dom = await waitPeek(cdp, opened.win.id, (d) => Boolean(d.base) && countsOf(d.countText));
  report.eq("底图借的是上游最近的那片云", dom?.base ?? null, ids.pick);
  report.ok("底图标签写明是谁的云", /^底图：/.test(dom?.baseText ?? ""),
    `baseText=${dom?.baseText} status=${dom?.status} cloudStatus=${dom?.cloudStatus}`);
  report.ok("底图真的画出了点", (countsOf(dom?.countText)?.total ?? 0) > 0,
    `countText=${dom?.countText}`);

  // 视图切换按钮在不在，由下一条「切到了文本视图」来判
  await clickIn(cdp, opened.win.id, '[data-testid="peek-view-value"]');
  const valueDom = await waitPeek(
    cdp,
    opened.win.id,
    (d) => d.view === "value" && Object.keys(d.rows).length > 0,
  );
  report.eq("切到了文本视图", valueDom?.view ?? null, "value");
  report.eq("正交视图下「导出 PNG」可用", dom?.pngDisabled ?? null, false);
  report.eq("文本视图没有画面，「导出 PNG」置灰", valueDom?.pngDisabled ?? null, true);

  const rows = valueDom?.rows ?? {};
  report.ok("键值表里有 Line2D 的字段", "point" in rows && "dir" in rows, JSON.stringify(rows));
  const fromTable =
    rows.start && rows.end
      ? `${rows.start} → ${rows.end}`
      : `过 ${rows.point} 方向 ${rows.dir}`;
  const inspector = await inspectorOutput(cdp, ids.fit, "line");
  report.eq("键值表里的数字与 Inspector 的显示一字不差", fromTable, inspector);
}

async function suiteIndicesPeek(cdp, report, fixture) {
  report.section("双击 Indices 边：条数与该端口的 elementCount 一致");

  const { ids } = fixture;
  const edge = await edgeOf(cdp, ids.pass, "indices", ids.pick, "indices");
  mustOk(typeof edge === "string", "找得到那条 Indices 边", String(edge));

  const opened = await openByDoubleClick(cdp, report, edge, "Indices 边");
  if (!opened?.win) {
    report.fail("Indices 边没开出窗", JSON.stringify(opened?.after));
    return;
  }
  await park(cdp);

  report.eq("运行时类型判成 Indices", opened.win.type, "Indices");
  report.eq("默认视图是下标列表", opened.win.view, "indices");

  const stat = await portStat(cdp, ids.pass, "indices");
  const dom = await waitPeek(cdp, opened.win.id, (d) => (d.indicesTotal ?? 0) > 0);
  report.eq("摘要里的 total = 该端口的 elementCount", dom?.indicesTotal ?? null,
    stat?.elementCount ?? null);
  report.ok("摘要带上了来源云的 id", (dom?.indicesSource ?? "") !== "",
    `sourceCloud=${dom?.indicesSource} status=${dom?.status}`);
}

async function suiteTensorPeek(cdp, report) {
  const present = await cdp.eval(`
    return window.__lyflow.stores.manifest.getState().operatorsById.has('gap.profile_tensor');
  `);
  if (present !== true) {
    report.section("张量视图（本次构建没有 gap 包，未验）");
    return;
  }

  report.section("双击张量边：rank / 布局 / 切片尺寸与形状对得上");

  await newDoc(cdp);
  await resetPeek(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "a", op: "gen.synthetic", params: { pointCount: 1280, seed: 5 } },
      { key: "b", op: "gen.synthetic", params: { pointCount: 1280, seed: 6 }, row: 1 },
      { key: "t", op: "gap.profile_tensor" },
      { key: "rr", op: "util.reroute" },
    ],
    [
      { from: ["a", "cloud"], to: ["t", "primary"] },
      { from: ["b", "cloud"], to: ["t", "secondary"] },
      { from: ["t", "tensor"], to: ["rr", "in"] },
    ],
  );
  await normalizeZoom(cdp, 0.6);
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.a]: { x: 12, y: 44 },
    [ids.b]: { x: 12, y: 190 },
    [ids.t]: { x: Math.round(box.w * 0.38), y: 110 },
    [ids.rr]: { x: Math.round(box.w * 0.72), y: 110 },
  });

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("张量这张图跑通了", run.status, "ok");
  if (run.status !== "ok") report.fail("失败详情", JSON.stringify(run.nodes));

  const edge = await edgeOf(cdp, ids.t, "tensor", ids.rr, "in");
  mustOk(typeof edge === "string", "找得到那条张量边", String(edge));

  const opened = await openByDoubleClick(cdp, report, edge, "张量边");
  if (!opened?.win) {
    report.fail("张量边没开出窗", JSON.stringify(opened?.after));
    return;
  }
  await park(cdp);

  report.eq("运行时类型判成 Tensor", opened.win.type, "Tensor");
  report.eq("默认视图是张量图像", opened.win.view, "tensor");

  const stat = await portStat(cdp, ids.t, "tensor");
  const shape = stat?.value?.shape ?? null;
  report.eq("事件里带回了 [2, 6, 1280] 的形状", shape, [2, 6, 1280]);

  const dom = await waitPeek(cdp, opened.win.id, (d) => (d.tensorCanvas?.w ?? 0) > 0);
  report.eq("rank 与形状的维数一致", dom?.tensor?.rank ?? null, shape?.length ?? null);
  report.ok(
    "布局推断出了一个能出图的结果",
    typeof dom?.tensor?.layout === "string" &&
      dom.tensor.layout !== "none" &&
      dom.tensor.layout.length > 0,
    `layout=${dom?.tensor?.layout} msg=${dom?.tensorMsg}`,
  );
  const w = dom?.tensorCanvas?.w ?? 0;
  const h = dom?.tensorCanvas?.h ?? 0;
  report.ok("画布宽高取自形状里的两维", Array.isArray(shape) && shape.includes(w) && shape.includes(h),
    `w=${w} h=${h} shape=${JSON.stringify(shape)}`);
  report.eq("这一片的元素数 = 画布宽 × 高", dom?.tensor?.sliceCount ?? null, w * h);
}

/** 图像视图（docs/image-plan.md 阶段 1）：图像来自 core 的测试算子 test.make_image（LYFLOW_TEST_OPS=1，
 *  harness 起 app 时设）—— 阶段 1 还没有 OpenCV 包。像素值公式与 core/tests/image_test_op.h 的 testImageValue 一致。 */
function readImagePeek(cdp, peekId) {
  const root = `[data-testid="edge-peek"][data-peek-id="${peekId}"]`;
  return cdp.eval(`
    const el = document.querySelector(${lit(root)});
    const v = el ? el.querySelector('[data-testid="peek-image"]') : null;
    if (!v) return null;
    const n = (k) => Number(v.getAttribute(k));
    const readout = el.querySelector('[data-testid="peek-image-readout"]');
    const msg = el.querySelector('[data-testid="peek-image-msg"]');
    const stage = el.querySelector('.peek-image__stage');
    const r = stage ? stage.getBoundingClientRect() : null;
    return {
      fullW: n('data-full-w'), fullH: n('data-full-h'), channels: n('data-channels'),
      depth: v.getAttribute('data-depth'), level: v.getAttribute('data-level'),
      w: n('data-w'), h: n('data-h'),
      readout: readout ? readout.textContent : null,
      msg: msg ? msg.textContent : null,
      stage: r ? { x: r.left, y: r.top, w: r.width, h: r.height } : null,
    };
  `);
}

async function suiteImagePeek(cdp, report) {
  const present = await cdp.eval(`
    return window.__lyflow.stores.manifest.getState().operatorsById.has('test.make_image');
  `);
  mustOk(present === true, "manifest 里有 test.make_image（harness 设了 LYFLOW_TEST_OPS=1）", String(present));

  report.section("双击图像边：先看适配窗口的缩小级别，切到原图后悬停读到的像素值与公式一致");

  await newDoc(cdp);
  await resetPeek(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "img", op: "test.make_image", params: { width: 3000, height: 1000, channels: 3 } },
      { key: "rr", op: "util.reroute" },
    ],
    [{ from: ["img", "image"], to: ["rr", "in"] }],
  );
  await normalizeZoom(cdp, 0.8);
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.img]: { x: 40, y: 120 },
    [ids.rr]: { x: Math.round(box.w * 0.6), y: 120 },
  });
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "合成图像这张图跑通了", JSON.stringify(run.nodes));

  const edge = await edgeOf(cdp, ids.img, "image", ids.rr, "in");
  const opened = await openByDoubleClick(cdp, report, edge, "图像边");
  mustOk(Boolean(opened?.win), "图像边开出了窗", JSON.stringify(opened?.after));
  // 不 park：要在窗里悬停，窗得整个在画布里
  await cdp.eval(`
    window.__lyflow.stores.peek.getState().move(${lit(opened.win.id)}, { x: ${box.x + 30}, y: ${box.y + 30} });
    return true;
  `);
  await sleep(150);
  report.eq("运行时类型 Image、默认视图 image", [opened.win.type, opened.win.view], ["Image", "image"]);

  let dom = null;
  for (let i = 0; i < 80; i += 1) {
    dom = await readImagePeek(cdp, opened.win.id);
    if (dom && dom.w > 0) break;
    await sleep(120);
  }
  // 3000×1000：长边 ≤ 2048 的最小级别是 1（块均值 2×2），1500×500
  report.eq("适配级别 = 1、这一级 1500×500、原图 3000×1000×3 u8",
    dom && [dom.level, dom.w, dom.h, dom.fullW, dom.fullH, dom.channels, dom.depth],
    ["1", 1500, 500, 3000, 1000, 3, "u8"]);

  const switched = await cdp.eval(`
    const el = document.querySelector('[data-testid="edge-peek"][data-peek-id=${lit(opened.win.id)}]');
    const sel = el.querySelector('[data-testid="peek-image-level"]');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, '0');
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);
  mustOk(switched === true, "切到原图");
  for (let i = 0; i < 80; i += 1) {
    dom = await readImagePeek(cdp, opened.win.id);
    if (dom && dom.level === "0" && dom.w === 3000) break;
    await sleep(120);
  }
  mustOk(dom?.level === "0" && dom.w === 3000, "原图取到了（分段取齐）", JSON.stringify(dom));

  const at = { x: Math.round(dom.stage.x + dom.stage.w / 2), y: Math.round(dom.stage.y + dom.stage.h / 2) };
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: at.x, y: at.y, buttons: 0 });
  await sleep(200);
  dom = await readImagePeek(cdp, opened.win.id);
  const m = /^\((\d+), (\d+)\) = \[(\d+), (\d+), (\d+)\]$/.exec(dom?.readout ?? "");
  mustOk(Boolean(m), "悬停出了读数", String(dom?.readout));
  const [x, y, r, g, b] = m.slice(1).map(Number);
  const f = (c) => (x * 3 + y * 5 + c * 60) % 256;
  report.eq(`(${x}, ${y}) 的 RGB 与合成公式一致`, [r, g, b], [f(0), f(1), f(2)]);

  // std-image（OpenCV）在真 app 里：灰度 → Otsu → 区域统计；掩膜那条边的视图是单通道 u8
  await resetPeek(cdp);
  await newDoc(cdp);
  const chain = await buildGraph(
    cdp,
    [
      { key: "img", op: "test.make_image", params: { width: 64, height: 48, channels: 3 } },
      { key: "gray", op: "image.to_gray" },
      { key: "bin", op: "image.threshold" },
      { key: "stats", op: "image.region_stats" },
    ],
    [
      { from: ["img", "image"], to: ["gray", "image"] },
      { from: ["gray", "image"], to: ["bin", "image"] },
      { from: ["gray", "image"], to: ["stats", "image"] },
      { from: ["bin", "mask"], to: ["stats", "mask"] },
    ],
  );
  await normalizeZoom(cdp, 0.7);
  await placeAtScreen(cdp, {
    [chain.img]: { x: 20, y: 60 },
    [chain.gray]: { x: Math.round(box.w * 0.27), y: 60 },
    [chain.bin]: { x: Math.round(box.w * 0.5), y: 60 },
    [chain.stats]: { x: Math.round(box.w * 0.74), y: 160 },
  });
  const ran = await runAndWait(cdp, () => pressF5(cdp));
  const area = (await portStat(cdp, chain.stats, "area"))?.value?.value;
  report.ok("OpenCV 链路在 app 里跑通：Otsu 分出的前景面积在 (0, 64×48) 之间",
    ran.status === "ok" && area > 0 && area < 64 * 48, JSON.stringify({ status: ran.status, area }));
  const maskEdge = await edgeOf(cdp, chain.bin, "mask", chain.stats, "mask");
  const maskWin = await openByDoubleClick(cdp, report, maskEdge, "掩膜边");
  let maskDom = null;
  for (let i = 0; i < 60 && maskWin?.win; i += 1) {
    maskDom = await readImagePeek(cdp, maskWin.win.id);
    if (maskDom && maskDom.w > 0) break;
    await sleep(120);
  }
  report.eq("掩膜边：单通道 u8、原图尺寸", maskDom && [maskDom.channels, maskDom.depth, maskDom.w, maskDom.h],
    [1, "u8", 64, 48]);
}

async function suiteLockPeek(cdp, report) {
  report.section("锁定快照：改参数重跑后，锁定窗的数值不变，未锁定窗跟着变");

  const { ids } = await prepare(cdp, { pointCount: 5000 });
  const lockedEdge = await edgeOf(cdp, ids.gen, "cloud", ids.pass, "cloud");
  const freeEdge = await edgeOf(cdp, ids.pick, "selected", ids.fit, "cloud");
  mustOk(Boolean(lockedEdge && freeEdge), "两条点云边都找得到", `${lockedEdge} / ${freeEdge}`);

  const a = await openByDoubleClick(cdp, report, lockedEdge, "要锁定的点云边");
  if (!a?.win) {
    report.fail("要锁定的那条边没开出窗", JSON.stringify(a?.after));
    return;
  }
  await park(cdp);
  const b = await openByDoubleClick(cdp, report, freeEdge, "不锁定的点云边");
  if (!b?.win) {
    report.fail("不锁定的那条边没开出窗", JSON.stringify(b?.after));
    return;
  }
  await park(cdp);
  report.eq("两个窗同时开着", b.after.length, 2);

  const lockedBefore = await waitPeek(cdp, a.win.id, (d) => countsOf(d.countText));
  const freeBefore = await waitPeek(cdp, b.win.id, (d) => countsOf(d.countText));
  report.eq("锁定前两个窗都显示 5000 点",
    [countsOf(lockedBefore?.countText)?.total ?? null, countsOf(freeBefore?.countText)?.total ?? null],
    [5000, 5000]);

  const runBefore = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  // 锁定按钮在不在，由下一条「窗上标出了已锁定」来判
  await clickIn(cdp, a.win.id, '[data-testid="peek-lock"]');
  await sleep(250);
  const afterClick = await readPeek(cdp, a.win.id);
  report.eq("窗上标出了「已锁定」", afterClick?.locked ?? null, "1");
  report.ok("标题栏出现了「已锁定 · runId 末段」的标记",
    /^已锁定/.test(afterClick?.lockedTag ?? ""), String(afterClick?.lockedTag));
  report.eq("标记上带的是锁定那一刻的 runId", afterClick?.lockedRun ?? null, runBefore);
  const flags = await peekWindows(cdp);
  report.eq("store 里也只有这一个窗是锁定的",
    flags.filter((w) => w.locked).map((w) => w.id), [a.win.id]);

  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.gen)}, 'pointCount', 9000);
    return true;
  `);
  // 重跑通过、换了新 runId，由「未锁定窗跟着变成 9000」与「锁定标记仍指向旧的那一次」合起来判
  await runAndWait(cdp, () => pressF5(cdp));

  const freeAfter = await waitPeek(
    cdp,
    b.win.id,
    (d) => (countsOf(d.countText)?.total ?? 0) === 9000,
  );
  report.eq("未锁定窗跟着新一次运行变成 9000",
    countsOf(freeAfter?.countText)?.total ?? null, 9000);

  const lockedAfter = await readPeek(cdp, a.win.id);
  report.eq("锁定窗仍然停在锁定时那一刻的 5000",
    countsOf(lockedAfter?.countText)?.total ?? null, 5000);
  report.eq("锁定窗还挂着锁", lockedAfter?.locked ?? null, "1");
  report.eq("锁定标记仍指向旧的那一次运行", lockedAfter?.lockedRun ?? null, runBefore);
  report.ok("锁定窗没有退回「未运行」之类的占位", lockedAfter?.status === null,
    String(lockedAfter?.status));

  // 再点一次解锁
  await clickIn(cdp, a.win.id, '[data-testid="peek-lock"]');
  const thawed = await waitPeek(
    cdp,
    a.win.id,
    (d) => d.locked === "0" && (countsOf(d.countText)?.total ?? 0) === 9000,
  );
  report.eq("解锁后跟回最新一次运行的 9000",
    [thawed?.locked ?? null, countsOf(thawed?.countText)?.total ?? null], ["0", 9000]);
}

async function suiteLifecyclePeek(cdp, report) {
  report.section("生命周期：边被删 → 窗自动关；进子图 → 顶层的窗消失");

  const { ids } = await prepare(cdp);
  const doomed = await edgeOf(cdp, ids.gen, "cloud", ids.pass, "cloud");
  const kept = await edgeOf(cdp, ids.fit, "line", ids.rr, "in");
  if (!doomed || !kept) {
    report.fail("拿不到这两条边", `${doomed} / ${kept}`);
    return;
  }

  const a = await openByDoubleClick(cdp, report, doomed, "要删掉的那条边");
  if (!a?.win) {
    report.fail("要删掉的那条边没开出窗", JSON.stringify(a?.after));
    return;
  }
  await park(cdp);
  const b = await openByDoubleClick(cdp, report, kept, "留着的那条边");
  if (!b?.win) {
    report.fail("留着的那条边没开出窗", JSON.stringify(b?.after));
    return;
  }
  await park(cdp);
  report.eq("两个窗同时开着", b.after.length, 2);

  await cdp.eval(`
    window.__lyflow.stores.graph.getState().disconnect([${lit(doomed)}]);
    return true;
  `);
  await sleep(350);
  const left = await peekWindows(cdp);
  report.eq("删掉边之后只剩另一条边的窗", left.map((w) => w.edgeId), [kept]);

  const composed = await cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().setSelection([${lit(ids.gen)}], []);
    return b.stores.graph.getState().composeSubgraph([${lit(ids.gen)}]);
  `);
  mustOk(Boolean(composed?.nodeId), "把孤立的生成节点合成了一个子图", JSON.stringify(composed));
  await sleep(250);
  const stillTop = await peekWindows(cdp);
  report.eq("合成子图不碰到不相干的那条边，它的窗还在", stillTop.length, 1);

  await cdp.eval(`
    window.__lyflow.stores.ui.getState().enterSubgraph({ nodeId: ${lit(composed.nodeId)},
                                                         subgraphId: ${lit(composed.subgraphId)} });
    return true;
  `);
  await sleep(350);
  const inside = await peekWindows(cdp);
  report.eq("进了子图，顶层的窗消失了", inside.length, 0);

  await cdp.eval(`window.__lyflow.stores.ui.getState().exitTo(0); return true;`);
  await sleep(300);
  const back = await peekWindows(cdp);
  report.eq("退回顶层也不会把关掉的窗变回来", back.length, 0);
}

async function suiteEdgeMenu(cdp, report) {
  report.section("边的右键菜单：四项都在（插入算子在最前）；「在此插入 Reroute」接过了原来的双击行为");

  const { ids } = await prepare(cdp);
  await resetPeek(cdp);
  const edge = await edgeOf(cdp, ids.gen, "cloud", ids.pass, "cloud");
  const point = await edgePoint(cdp, edge);
  mustOk(Boolean(point), "在边上找到了真能点中的落点", `edge=${edge} point=${JSON.stringify(point)}`);

  const items = await cdp.eval(`
    const el = document.elementFromPoint(${point.x}, ${point.y});
    if (!el) return 'no-element';
    el.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true, clientX: ${point.x}, clientY: ${point.y},
    }));
    await new Promise((done) => setTimeout(done, 200));
    const menu = document.querySelector('[data-testid="edge-context-menu"]');
    if (!menu) return 'no-menu';
    return [...menu.querySelectorAll('button')].map((b) => b.getAttribute('data-testid'));
  `);
  report.eq("菜单里正好是插入算子 / 查看内容 / 插入 Reroute / 删除连线", items,
    ["edge-ctx-insert", "edge-ctx-peek", "edge-ctx-reroute", "edge-ctx-delete"]);
  if (!Array.isArray(items)) return;

  // 插 Reroute 本身（节点多一个、边一拆二）由 m3 的 1.3 分组验，这里只看它不顺手开浮窗
  await cdp.eval(`
    const btn = document.querySelector('[data-testid="edge-ctx-reroute"]');
    if (!btn) return false;
    btn.click();
    return true;
  `);
  await sleep(300);

  const windows = await peekWindows(cdp);
  report.eq("插 Reroute 这条路不会顺手开一个浮窗", windows.length, 0);

  const peeked = await cdp.eval(`
    const doc = window.__lyflow.stores.graph.getState().doc;
    const e = doc.edges.find((x) => x.from.node === ${lit(ids.fit)} && x.from.port === 'line');
    if (!e) return 'no-edge';
    const el = document.querySelector('.react-flow__edge[data-id="' + e.id + '"] .react-flow__edge-interaction');
    if (!el) return 'no-path';
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true,
      clientX: Math.round(r.left + r.width / 2), clientY: Math.round(r.top + r.height / 2),
    }));
    await new Promise((done) => setTimeout(done, 200));
    const btn = document.querySelector('[data-testid="edge-ctx-peek"]');
    if (!btn) return 'no-menu';
    btn.click();
    await new Promise((done) => setTimeout(done, 300));
    return window.__lyflow.stores.peek.getState().windows.length;
  `);
  report.eq("菜单里的「查看内容」也能开窗", peeked, 1);
}

/** 真的双击一条边、读浮窗 DOM 这几样，单节点运行的验收 11b 也要（docs/node-run-plan.md R7）。 */
export { countsOf, openByDoubleClick, park, peekWindows, resetPeek, waitPeek };

export const peekSuites = [
  suiteSharedGraphPeeks,
  suiteTensorPeek,
  suiteImagePeek,
  suiteLockPeek,
  suiteLifecyclePeek,
  suiteEdgeMenu,
];
