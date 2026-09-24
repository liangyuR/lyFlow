// 动效的分组（docs/motion-plan.md §3 验收 2–9）：进场、删除残影、端点对齐、连线生长、
// 数据流动、状态闪光、hover、关动效。逐条结果见 docs/motion-acceptance.md。
//
// 动效是「时间上的」行为，断言靠两样东西：页面里的 MutationObserver 记下标记属性
// （data-entering / data-growing / data-flash）出现过几次；以及在同一个页面任务里
// 同时读 DOM 几何与 store（端点对齐、布局过渡中途）。hover 一律用 CDP 的真鼠标 ——
// 合成的 mouseenter 骗不过 CSS 的 :hover。

import { sleep } from "./cdp.mjs";
import {
  buildGraph,
  canvasBox,
  centerOf,
  lit,
  newDoc,
  normalizeZoom,
  placeAtScreen,
  pressCtrl,
  pressF5,
  runAndWait,
} from "./page.mjs";

// ------------------------------------------------------------ 页面侧的小工具

/** 装一次就够：记录器、端点对齐、DOM 位置。都挂在 window.__lyMotion 上。 */
const INSTALL = `
  if (window.__lyMotion) return true;
  const keyOf = (el) => el.getAttribute('data-testid')
    ?? el.closest('.react-flow__edge')?.getAttribute('data-id') ?? el.className?.baseVal ?? '?';
  window.__lyMotion = {
    /** 记下 attr 每一次出现（属性被设上、或带着它的元素被挂上来）。 */
    record(attr) {
      const hits = [];
      const note = (el) => {
        const v = el.getAttribute(attr);
        if (v !== null) hits.push({ key: keyOf(el), value: v, at: performance.now() });
      };
      const obs = new MutationObserver((list) => {
        for (const m of list) {
          if (m.type === 'attributes') note(m.target);
          else for (const n of m.addedNodes) {
            if (n.nodeType !== 1) continue;
            note(n);
            n.querySelectorAll('[' + attr + ']').forEach(note);
          }
        }
      });
      obs.observe(document.querySelector('.react-flow'), {
        subtree: true, childList: true, attributes: true, attributeFilter: [attr],
      });
      return { hits, stop: () => obs.disconnect() };
    },
    /** 当前层每条边：路径起止点与 React Flow 锚点（源在圆点右缘、目标在左缘，
     *  纵向取圆心）的距离，以及与圆点中心的距离。单位是画布坐标。
     *  锚点由圆心 ± 半个 offsetWidth 算，不直接用包围盒的左右缘：圆点要是被缩放了，
     *  包围盒跟着变大，拿它当基准会和 React Flow 量歪的端点「一起歪」，假绿。 */
    align() {
      const vp = document.querySelector('.react-flow__viewport');
      const k = new DOMMatrixReadOnly(getComputedStyle(vp).transform).a;
      const o = vp.getBoundingClientRect();
      const flow = (x, y) => ({ x: (x - o.left) / k, y: (y - o.top) / k });
      const dist = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);
      // 同名的输入输出端口 testid 相同（比如 voxel 的 cloud 进 cloud 出），要按侧别挑
      const handle = (ref, side) => document.querySelector(
        '[data-testid="port-' + ref.node + '-' + ref.port + '"].node-port--' + side + ' .react-flow__handle');
      const s = window.__lyflow.snapshot();
      const edges = s.doc.edges.filter((e) => s.level.edges.includes(e.id));
      return edges.map((e) => {
        const path = document.querySelector('.react-flow__edge[data-id="' + e.id + '"] .react-flow__edge-path');
        const src = handle(e.from, 'output');
        const tgt = handle(e.to, 'input');
        const hs = src?.getBoundingClientRect();
        const ht = tgt?.getBoundingClientRect();
        if (!path || !hs || !ht) return { id: e.id, missing: true };
        const n = path.getAttribute('d').match(/-?\\d+(?:\\.\\d+)?(?:e-?\\d+)?/g).map(Number);
        const start = { x: n[0], y: n[1] };
        const end = { x: n[n.length - 2], y: n[n.length - 1] };
        const srcAnchor = flow(hs.left + hs.width / 2 + (src.offsetWidth * k) / 2, hs.top + hs.height / 2);
        const tgtAnchor = flow(ht.left + ht.width / 2 - (tgt.offsetWidth * k) / 2, ht.top + ht.height / 2);
        const srcCenter = flow(hs.left + hs.width / 2, hs.top + hs.height / 2);
        const tgtCenter = flow(ht.left + ht.width / 2, ht.top + ht.height / 2);
        return {
          id: e.id,
          start: +dist(start, srcAnchor).toFixed(3),
          end: +dist(end, tgtAnchor).toFixed(3),
          startToCenter: +dist(start, srcCenter).toFixed(3),
          endToCenter: +dist(end, tgtCenter).toFixed(3),
        };
      });
    },
    /** React Flow 画出来的节点位置（包装层 translate），画布坐标。 */
    domPos(id) {
      const el = document.querySelector('.react-flow__node[data-id="' + id + '"]');
      if (!el) return null;
      const m = /translate\\(\\s*(-?[\\d.]+)px\\s*,\\s*(-?[\\d.]+)px/.exec(el.style.transform);
      return m ? { x: Number(m[1]), y: Number(m[2]) } : null;
    },
    frames: (n = 2) => new Promise((r) => {
      const step = (i) => (i <= 0 ? r() : requestAnimationFrame(() => step(i - 1)));
      step(n);
    }),
  };
  return true;
`;

const install = (cdp) => cdp.eval(INSTALL);

/** 开一个记录器，名字随便起，之后用 stopRecord 取结果。 */
const startRecord = (cdp, name, attr) =>
  cdp.eval(`
    window.__lyMotion.rec ??= {};
    window.__lyMotion.rec[${lit(name)}]?.stop();
    window.__lyMotion.rec[${lit(name)}] = window.__lyMotion.record(${lit(attr)});
    return true;
  `);
const stopRecord = (cdp, name) =>
  cdp.eval(`
    const r = window.__lyMotion.rec[${lit(name)}];
    r.stop();
    return r.hits.map((h) => ({ key: h.key, value: h.value }));
  `);

const alignOf = (cdp) => cdp.eval(`return window.__lyMotion.align();`);
const worst = (rows) =>
  rows.reduce((m, r) => (r.missing ? Infinity : Math.max(m, r.start, r.end)), 0);

/** 真鼠标挪到某处（不按键）。 */
const moveMouse = (cdp, p) =>
  cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y, buttons: 0 });

/** 画布里一块空地（左下角附近），鼠标停在那儿什么都不指着。 */
async function emptySpot(cdp) {
  const box = await canvasBox(cdp);
  return { x: box.x + 30, y: box.y + box.h - 60 };
}

const clearDoc = async (cdp) => {
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.peek?.getState().closeAll?.(); return true;`);
  // 进场窗口过期，免得上一组的标记漏到这一组
  await sleep(500);
};

// --------------------------------------------------------------- 验收 2：进场

async function suiteEnter(cdp, report) {
  report.section("动效 验收 2：进场只给编辑出来的节点 —— addNode 淡入、loadDoc 与平移不闪");
  await install(cdp);
  await clearDoc(cdp);

  await startRecord(cdp, "enter", "data-entering");
  const first = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const t0 = performance.now();
    const id = g.addNode('gen.synthetic', { x: 60, y: 60 });
    await window.__lyMotion.frames(2);
    const el = document.querySelector('[data-testid="node-' + id + '"]');
    const early = { ms: Math.round(performance.now() - t0), opacity: el ? Number(getComputedStyle(el).opacity) : null };
    await new Promise((r) => setTimeout(r, 400));
    return { id, early, late: el ? Number(getComputedStyle(el).opacity) : null,
             marker: el?.getAttribute('data-entering') ?? null };
  `);
  const hits = await stopRecord(cdp, "enter");
  report.ok(`addNode 后 ${first.early.ms} ms（≤ 50）新节点 opacity < 1`,
    first.early.ms <= 50 && first.early.opacity !== null && first.early.opacity < 1, JSON.stringify(first.early));
  report.eq("400 ms 后 opacity 回到 1", first.late, 1);
  report.ok("播过一次进场标记，播完就撤了", hits.some((h) => h.key === `node-${first.id}`) && first.marker === null,
    JSON.stringify({ hits, marker: first.marker }));

  // loadDoc 一张 10 节点的图：任何时刻都没有进场（生长）标记
  const doc = await cdp.eval(`
    const g = () => window.__lyflow.stores.graph.getState();
    g().newDoc();
    const ids = [];
    for (let i = 0; i < 10; i += 1) {
      ids.push(g().addNode(i === 0 ? 'gen.synthetic' : 'util.reroute', { x: 40 + (i % 5) * 230, y: 60 + Math.floor(i / 5) * 160 }));
      if (i > 0) g().connect({ node: ids[i - 1], port: i === 1 ? 'cloud' : 'out' }, { node: ids[i], port: 'in' });
    }
    return JSON.parse(JSON.stringify(g().doc));
  `);
  await cdp.eval(`window.__lyflow.stores.graph.getState().newDoc(); return true;`);
  await sleep(600);
  await startRecord(cdp, "load", "data-entering");
  await startRecord(cdp, "loadGrow", "data-growing");
  await cdp.eval(`window.__lyflow.stores.graph.getState().loadDoc(${lit(doc)}, null); return true;`);
  await sleep(700);
  const loaded = await cdp.eval(`return document.querySelectorAll('[data-testid^="node-n_"]').length;`);
  const loadHits = await stopRecord(cdp, "load");
  const loadGrow = await stopRecord(cdp, "loadGrow");
  report.eq("loadDoc 10 节点：画出了 10 个节点", loaded, 10);
  report.eq("loadDoc 10 节点：任何节点在任何时刻都没有进场标记", loadHits, []);
  report.eq("loadDoc 10 节点：任何连线都没有生长标记", loadGrow, []);

  // > 80 节点：编辑着搭出来（每个都标过进场），等窗口过期，再平移到另一半
  await newDoc(cdp);
  const big = await cdp.eval(`
    const g = () => window.__lyflow.stores.graph.getState();
    for (let i = 0; i < 96; i += 1) g().addNode('util.reroute', { x: (i % 24) * 260, y: Math.floor(i / 24) * 150 });
    await new Promise((r) => setTimeout(r, 900));
    const rendered = [...document.querySelectorAll('[data-testid^="node-n_"]')].map((e) => e.getAttribute('data-testid'));
    return { total: g().doc.nodes.length, rendered };
  `);
  report.ok("> 80 节点：开了虚拟化（只渲染了一部分）", big.rendered.length < big.total, `${big.rendered.length}/${big.total}`);
  await startRecord(cdp, "pan", "data-entering");
  const box = await canvasBox(cdp);
  // 中键拖动平移（PAN_BUTTONS）：往左拖两段，把右半边拉进视口
  for (let round = 0; round < 2; round += 1) {
    const from = { x: box.x + box.w - 40, y: box.y + Math.round(box.h / 2) };
    const to = { x: box.x + 40, y: from.y };
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: from.x, y: from.y, buttons: 0 });
    await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: from.x, y: from.y, button: "middle", buttons: 4, clickCount: 1 });
    for (let i = 1; i <= 10; i += 1) {
      await cdp.send("Input.dispatchMouseEvent", {
        type: "mouseMoved", x: Math.round(from.x + ((to.x - from.x) * i) / 10), y: from.y, button: "middle", buttons: 4,
      });
      await sleep(16);
    }
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: to.x, y: to.y, button: "middle", buttons: 4, clickCount: 1 });
    await sleep(250);
  }
  await sleep(400);
  const after = await cdp.eval(`return [...document.querySelectorAll('[data-testid^="node-n_"]')].map((e) => e.getAttribute('data-testid'));`);
  const panHits = await stopRecord(cdp, "pan");
  const fresh = after.filter((id) => !big.rendered.includes(id));
  report.ok("平移之后有新节点进入视口（重新挂载了）", fresh.length > 0, `新进入 ${fresh.length} 个`);
  report.eq("新进入视口的节点没有进场标记", panHits, []);
}

// --------------------------------------------------------------- 验收 3：删除

async function suiteDelete(cdp, report) {
  report.section("动效 验收 3：删除 —— doc 立即改、残影无 id/data-*、500 ms 后消失、撤销再进场");
  await install(cdp);
  await clearDoc(cdp);
  const ids = await buildGraph(cdp, [
    { key: "a", op: "gen.synthetic", params: { pointCount: 1000 } },
    { key: "b", op: "filter.voxel_grid" },
  ], [{ from: ["a", "cloud"], to: ["b", "cloud"] }]);
  await sleep(500);

  const del = await cdp.eval(`
    window.__lyflow.stores.graph.getState().deleteNodes([${lit(ids.b)}]);
    const inDoc = window.__lyflow.stores.graph.getState().doc.nodes.some((n) => n.id === ${lit(ids.b)});
    await window.__lyMotion.frames(1);
    const ghosts = [...document.querySelectorAll('.canvas__ghost')];
    const tainted = ghosts.flatMap((g) => [g, ...g.querySelectorAll('*')])
      .filter((el) => el.hasAttribute('id') || [...el.attributes].some((a) => a.name.startsWith('data-')))
      .map((el) => el.outerHTML.slice(0, 80));
    return {
      inDoc,
      node: !!document.querySelector('[data-testid="node-${ids.b}"]'),
      ghosts: ghosts.length,
      ghostHasNode: ghosts.some((g) => !!g.querySelector('.node')),
      ariaHidden: ghosts.every((g) => g.getAttribute('aria-hidden') === 'true'),
      pointer: ghosts.map((g) => getComputedStyle(g).pointerEvents),
      tainted,
    };
  `);
  report.eq("doc 里立即就没有这个节点了", del.inDoc, false);
  report.eq("下一帧 [data-testid=node-X] 已不存在", del.node, false);
  report.ok("此时有一个残影元素，里面是节点的样子", del.ghosts === 1 && del.ghostHasNode, JSON.stringify(del));
  report.eq("残影子树里没有任何 id 与 data-* 属性", del.tainted, []);
  report.ok("残影 aria-hidden、不接鼠标", del.ariaHidden && del.pointer.every((p) => p === "none"), JSON.stringify(del.pointer));
  await sleep(500);
  report.eq("500 ms 后残影消失", await cdp.eval(`return document.querySelectorAll('.canvas__ghost').length;`), 0);

  await startRecord(cdp, "undo", "data-entering");
  await cdp.eval(`window.__lyflow.stores.graph.getState().undo(); return true;`);
  await sleep(400);
  const back = await cdp.eval(`return !!document.querySelector('[data-testid="node-${ids.b}"]');`);
  const undoHits = await stopRecord(cdp, "undo");
  report.eq("撤销一次恢复节点", back, true);
  report.ok("恢复出来的节点播了进场", undoHits.some((h) => h.key === `node-${ids.b}`), JSON.stringify(undoHits));

  // 一次删超过 30 个：不出残影
  const many = await cdp.eval(`
    const g = () => window.__lyflow.stores.graph.getState();
    const list = [];
    for (let i = 0; i < 31; i += 1) list.push(g().addNode('util.reroute', { x: 40 + (i % 8) * 120, y: 300 + Math.floor(i / 8) * 70 }));
    await new Promise((r) => setTimeout(r, 500));
    g().deleteNodes(list);
    await window.__lyMotion.frames(1);
    return document.querySelectorAll('.canvas__ghost').length;
  `);
  report.eq("一次删除 31 个节点不出残影", many, 0);
}

// ----------------------------------------------------------- 验收 4：端点对齐

async function suiteAlign(cdp, report) {
  report.section("动效 验收 4：端点对齐 —— 进场后、抖动后、hover 中、布局过渡中途与结束，误差 ≤ 1 px");
  await install(cdp);
  await clearDoc(cdp);
  const ids = await buildGraph(cdp, [
    { key: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed: (Date.now() % 9973) + 1 } },
    { key: "voxel", op: "filter.voxel_grid" },
    { key: "pass", op: "filter.passthrough" },
  ], [
    { from: ["gen", "cloud"], to: ["voxel", "cloud"] },
    { from: ["voxel", "cloud"], to: ["pass", "cloud"] },
  ]);
  await normalizeZoom(cdp, 0.8);
  // 前面的分组可能把视口平移走了：按屏幕坐标摆进画布里，后面要拿真鼠标去指它们
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.gen]: { x: 30, y: 40 },
    [ids.voxel]: { x: Math.round(box.w * 0.36), y: 40 },
    [ids.pass]: { x: Math.round(box.w * 0.66), y: 40 },
  });
  await sleep(450);

  let rows = await alignOf(cdp);
  report.ok(`进场结束：${rows.length} 条边的端点与锚点最大偏差 ${worst(rows)} px ≤ 1`, rows.length === 2 && worst(rows) <= 1, JSON.stringify(rows));
  report.ok("（对照）圆点中心离端点约半个圆点宽，锚点在圆点外缘不在圆心",
    rows.every((r) => r.startToCenter > 3 && r.endToCenter > 3), JSON.stringify(rows.map((r) => [r.startToCenter, r.endToCenter])));

  // → error：坏参数跑一次，等抖动播完
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.voxel)}, 'leafSize', [0, 0.01, 0.01]); return true;`);
  const run = await runAndWait(cdp, () => pressF5(cdp));
  await sleep(450);
  rows = await alignOf(cdp);
  report.eq("（前提）voxel 真的进了 error", run.nodes[ids.voxel]?.state, "error");
  report.ok(`error 抖动结束：最大偏差 ${worst(rows)} px ≤ 1`, worst(rows) <= 1, JSON.stringify(rows));

  // hover 节点期间（真鼠标停在标题栏上）
  const head = await centerOf(cdp, `[data-testid="node-${ids.voxel}"] .node__head`);
  await moveMouse(cdp, head);
  await sleep(250);
  rows = await alignOf(cdp);
  const hovered = await cdp.eval(`return window.__lyflow.stores.ui.getState().hoverNodeId;`);
  report.eq("（前提）hover 到了 voxel 上", hovered, ids.voxel);
  report.ok(`hover 节点期间：最大偏差 ${worst(rows)} px ≤ 1`, worst(rows) <= 1, JSON.stringify(rows));
  await moveMouse(cdp, await emptySpot(cdp));
  await sleep(150);

  // 自动布局：先随手摆乱（applyLayout 直调不过渡），再按 Ctrl+L 走真实入口
  await placeAtScreen(cdp, {
    [ids.gen]: { x: 30, y: Math.round(box.h * 0.55) },
    [ids.voxel]: { x: Math.round(box.w * 0.45), y: 30 },
    [ids.pass]: { x: Math.round(box.w * 0.2), y: Math.round(box.h * 0.3) },
  });
  await cdp.eval(`window.__lyflow.stores.ui.getState().clearSelection(); return true;`);
  await pressCtrl(cdp, "L");
  const mid = await cdp.eval(`
    const canvas = document.querySelector('.canvas');
    const t0 = performance.now();
    while (canvas.getAttribute('data-layout-moving') !== '1' && performance.now() - t0 < 1000) {
      await window.__lyMotion.frames(1);
    }
    const started = canvas.getAttribute('data-layout-moving') === '1';
    await new Promise((r) => setTimeout(r, 130));
    const doc = window.__lyflow.stores.graph.getState().doc;
    const nodes = doc.nodes.map((n) => ({ id: n.id, doc: n.ui.position, dom: window.__lyMotion.domPos(n.id) }));
    return { started, still: canvas.getAttribute('data-layout-moving') === '1', nodes, rows: window.__lyMotion.align(),
             undo: window.__lyflow.snapshot().undoLabel };
  `);
  const off = (n) => Math.hypot(n.doc.x - n.dom.x, n.doc.y - n.dom.y);
  report.ok("Ctrl+L 之后进入了布局过渡", mid.started, JSON.stringify(mid));
  report.ok("取样时还在过渡中途：画面上的位置还没到 doc 的终点",
    mid.still && mid.nodes.some((n) => n.dom && off(n) > 5), JSON.stringify(mid.nodes));
  report.ok("doc 已经一步到位（一个撤销步）", String(mid.undo).includes("整理"), String(mid.undo));
  report.ok(`布局过渡中途：最大偏差 ${worst(mid.rows)} px ≤ 1`, worst(mid.rows) <= 1, JSON.stringify(mid.rows));

  await cdp.waitFor(`document.querySelector('.canvas').getAttribute('data-layout-moving') !== '1'`,
    { timeoutMs: 3000, what: "布局过渡结束" });
  await sleep(100);
  const end = await cdp.eval(`
    const doc = window.__lyflow.stores.graph.getState().doc;
    return { nodes: doc.nodes.map((n) => ({ id: n.id, doc: n.ui.position, dom: window.__lyMotion.domPos(n.id) })),
             rows: window.__lyMotion.align() };
  `);
  report.ok("过渡结束：画面位置与 doc 一致", end.nodes.every((n) => n.dom && off(n) < 0.5), JSON.stringify(end.nodes));
  report.ok(`布局过渡结束：最大偏差 ${worst(end.rows)} px ≤ 1`, worst(end.rows) <= 1, JSON.stringify(end.rows));
}

// --------------------------------------------------------------- 验收 5：生长

async function suiteGrow(cdp, report) {
  report.section("动效 验收 5：连线生长 —— connect 后带生长标记，长完路径完整、惰性边虚线复原");
  await install(cdp);
  await clearDoc(cdp);
  const ids = await buildGraph(cdp, [
    { key: "a", op: "gen.synthetic", params: { pointCount: 1000 } },
    { key: "b", op: "gen.synthetic", params: { pointCount: 500 } },
    { key: "fb", op: "flow.fallback" },
  ], []);
  await sleep(450);

  const probe = (port) => cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const from = ${lit(port === "a" ? ids.a : ids.b)};
    const verdict = g.connect({ node: from, port: 'cloud' }, { node: ${lit(ids.fb)}, port: ${lit(port)} });
    const edge = window.__lyflow.stores.graph.getState().doc.edges.find((e) => e.from.node === from);
    await window.__lyMotion.frames(2);
    const read = () => {
      const g2 = document.querySelector('.react-flow__edge[data-id="' + edge.id + '"] .ly-edge');
      const p = g2?.querySelector('.react-flow__edge-path');
      return { growing: g2?.getAttribute('data-growing') ?? null, pathLength: p?.getAttribute('pathLength') ?? null,
               dash: p ? getComputedStyle(p).strokeDasharray : null };
    };
    const early = read();
    await new Promise((r) => setTimeout(r, 450));
    return { ok: verdict.ok, id: edge.id, early, late: read() };
  `);
  const norm = (d) => String(d).replace(/px/g, "").replace(/[, ]+/g, " ").trim();

  const plain = await probe("a");
  report.ok("connect 后新边带生长标记", plain.ok && plain.early.growing === "1", JSON.stringify(plain));
  report.ok("长完：标记撤掉、没有残留的 pathLength 与 dasharray",
    plain.late.growing === null && plain.late.pathLength === null && norm(plain.late.dash) === "none", JSON.stringify(plain.late));

  const lazy = await probe("b");
  report.ok("惰性边同样带生长标记（生长期间是实线）", lazy.ok && lazy.early.growing === "1" && norm(lazy.early.dash) !== "6 4",
    JSON.stringify(lazy.early));
  report.eq("惰性边长完 dasharray 恢复为 6 4", norm(lazy.late.dash), "6 4");
  report.eq("惰性边长完没有残留 pathLength", lazy.late.pathLength, null);
}

// --------------------------------------------------------------- 验收 6：流动

/** 一条够慢的链：两百万点过三道极细的体素栅格。seed 每次不同，免得整条命中缓存。 */
function slowChain() {
  const nodes = [{ key: "gen", op: "gen.synthetic", params: { pointCount: 2_000_000, seed: (Date.now() % 9973) + 1 } }];
  const edges = [];
  for (let i = 0; i < 3; i += 1) {
    nodes.push({ key: `v${i}`, op: "filter.voxel_grid", params: { leafSize: [0.0008, 0.0008, 0.0008] } });
    edges.push({ from: [i === 0 ? "gen" : `v${i - 1}`, "cloud"], to: [`v${i}`, "cloud"] });
  }
  return { nodes, edges };
}

/** 跑一次，在第一次看到「非源头节点在 running」的那一帧拍下流动的边与期望的边。 */
async function sampleFlow(cdp) {
  const before = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  await pressF5(cdp);
  const shot = await cdp.eval(`
    const t0 = performance.now();
    while (performance.now() - t0 < 20000) {
      await window.__lyMotion.frames(1);
      const s = window.__lyflow.snapshot();
      const running = [...document.querySelectorAll('[data-node-state="running"]')]
        .map((el) => el.getAttribute('data-testid').slice(5));
      const withInput = running.filter((id) => s.doc.edges.some((e) => e.to.node === id));
      if (withInput.length === 0) continue;
      const flowing = [...document.querySelectorAll('.ly-edge[data-flowing="1"]')]
        .map((g) => g.closest('.react-flow__edge').getAttribute('data-id')).sort();
      const expected = s.doc.edges.filter((e) => running.includes(e.to.node)).map((e) => e.id).sort();
      const flow = document.querySelector('.ly-edge[data-flowing="1"] .ly-edge__flow');
      const runningNode = document.querySelector('[data-node-state="running"]');
      return { running, flowing, expected, total: s.doc.edges.length,
               flowAnimation: flow ? getComputedStyle(flow).animationName : null,
               pulse: runningNode ? getComputedStyle(runningNode).animationName : null };
    }
    return null;
  `);
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runId !== ${lit(before)} && s.runStatus !== 'idle' && s.runStatus !== 'running'; })()`,
    { timeoutMs: 120_000, what: "运行结束" },
  );
  await sleep(200);
  const leftover = await cdp.eval(`return document.querySelectorAll('[data-flowing="1"]').length;`);
  return { shot, leftover };
}

async function suiteFlow(cdp, report) {
  report.section("动效 验收 6：数据流动 —— 目标节点 running 时只有它的入边在流，跑完全图清空");
  await install(cdp);
  await clearDoc(cdp);
  const chain = slowChain();
  await buildGraph(cdp, chain.nodes, chain.edges);
  await sleep(450);
  const { shot, leftover } = await sampleFlow(cdp);
  report.ok("拍到了一个有入边的节点正在 running", shot !== null, JSON.stringify(shot));
  if (shot) {
    report.ok("它的入边 data-flowing=\"1\"", shot.expected.length > 0 && shot.expected.every((id) => shot.flowing.includes(id)),
      JSON.stringify(shot));
    report.eq("其余边没有 data-flowing", shot.flowing, shot.expected);
    report.eq("流动层在走（animation-name）", shot.flowAnimation, "lyflow-edge-flow");
  }
  report.eq("运行结束后全图没有 data-flowing=\"1\"", leftover, 0);
}

// ------------------------------------------------------------ 验收 7：状态反馈

async function suiteStateFeedback(cdp, report) {
  report.section("动效 验收 7：状态反馈 —— done 恰闪一次、error 只抖标题栏、重新挂载不闪");
  await install(cdp);
  await clearDoc(cdp);
  const ids = await buildGraph(cdp, [
    { key: "gen", op: "gen.synthetic", params: { pointCount: 200000, seed: (Date.now() % 9973) + 1 } },
    { key: "voxel", op: "filter.voxel_grid" },
  ], [{ from: ["gen", "cloud"], to: ["voxel", "cloud"] }]);
  await sleep(450);

  await startRecord(cdp, "done", "data-flash");
  const ok = await runAndWait(cdp, () => pressF5(cdp));
  await sleep(450);
  const doneHits = (await stopRecord(cdp, "done")).filter((h) => h.key === `node-${ids.voxel}`);
  report.eq("（前提）voxel 这次是真算的 done", ok.nodes[ids.voxel]?.state, "done");
  report.eq("running → done 恰有一次 done 闪光标记", doneHits.map((h) => h.value), ["done"]);

  // 实时预览不闪绿（S2）：换个 seed 让预览真的算一遍，结束时 voxel 上不该出现 data-flash="done"
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.gen)}, 'seed', ${(Date.now() % 7919) + 11}); return true;`);
  await startRecord(cdp, "preview", "data-flash");
  const preview = await runAndWait(cdp, () =>
    cdp.eval(`await window.__lyflow.run({ preview: true, previewMaxPoints: 20000 }); return true;`));
  await sleep(450);
  const previewHits = (await stopRecord(cdp, "preview")).filter((h) => h.key === `node-${ids.voxel}`);
  report.ok("（前提）这是一次预览运行，voxel 真算了（done 不是 skipped）",
    preview.preview === true && preview.nodes[ids.voxel]?.state === "done", JSON.stringify({ preview: preview.preview, voxel: preview.nodes[ids.voxel] }));
  report.eq("预览运行结束后节点没有 data-flash=\"done\"", previewHits.filter((h) => h.value === "done"), []);

  // → error：整场每一帧都记 .node 与 .node__head 的 transform
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.voxel)}, 'leafSize', [0, 0.01, 0.01]); return true;`);
  await startRecord(cdp, "error", "data-flash");
  await cdp.eval(`
    const sel = '[data-testid="node-${ids.voxel}"]';
    window.__lyShake = { stop: false, maxHead: 0, nodeTransforms: new Set(), frames: 0, errorFrames: 0 };
    const tick = () => {
      const s = window.__lyShake;
      const el = document.querySelector(sel);
      if (el) {
        s.frames += 1;
        s.nodeTransforms.add(getComputedStyle(el).transform);
        if (el.getAttribute('data-node-state') === 'error') {
          s.errorFrames += 1;
          const head = el.querySelector('.node__head');
          const m = new DOMMatrixReadOnly(getComputedStyle(head).transform === 'none' ? undefined : getComputedStyle(head).transform);
          s.maxHead = Math.max(s.maxHead, Math.abs(m.e));
        }
      }
      if (!s.stop) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    return true;
  `);
  const bad = await runAndWait(cdp, () => pressF5(cdp));
  await sleep(500);
  const shake = await cdp.eval(`
    const s = window.__lyShake; s.stop = true;
    const head = document.querySelector('[data-testid="node-${ids.voxel}"] .node__head');
    return { maxHead: s.maxHead, nodeTransforms: [...s.nodeTransforms], frames: s.frames, errorFrames: s.errorFrames,
             headAfter: getComputedStyle(head).transform };
  `);
  const errorHits = (await stopRecord(cdp, "error")).filter((h) => h.key === `node-${ids.voxel}`);
  report.eq("（前提）voxel 进了 error", bad.nodes[ids.voxel]?.state, "error");
  report.eq("→ error 有一次 error 闪光标记", errorHits.map((h) => h.value), ["error"]);
  report.ok(`抖动期间 .node__head 有非零 translateX（最大 ${shake.maxHead.toFixed(2)} px）`, shake.maxHead > 0.5, JSON.stringify(shake));
  report.eq(`.node 本身始终没有 transform（${shake.frames} 帧）`, shake.nodeTransforms, ["none"]);
  report.ok("抖完标题栏归位", shake.headAfter === "none" || /matrix\(1, 0, 0, 1, 0, 0\)/.test(shake.headAfter), shake.headAfter);

  // 重新挂载：合成子图、跑完，进出子图时那些已经 done 的节点不闪
  // 换一个没算过的 leafSize：子图里的 voxel 得是真算的 done，而不是命中缓存的 skipped
  const leaf = 0.011 + (Date.now() % 97) / 100000;
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.voxel)}, 'leafSize', [${leaf}, ${leaf}, ${leaf}]); return true;`);
  const composed = await cdp.eval(`return window.__lyflow.stores.graph.getState().composeSubgraph([${lit(ids.voxel)}]);`);
  await sleep(450);
  await runAndWait(cdp, () => pressF5(cdp));
  await sleep(450);
  await startRecord(cdp, "remount", "data-flash");
  await cdp.eval(`
    window.__lyflow.stores.ui.getState().enterSubgraph({ nodeId: ${lit(composed.nodeId)}, subgraphId: ${lit(composed.subgraphId)} });
    return true;
  `);
  await sleep(500);
  const inside = await cdp.eval(`return [...document.querySelectorAll('[data-node-state]')].map((e) => e.getAttribute('data-node-state'));`);
  await cdp.eval(`window.__lyflow.stores.ui.getState().exitTo(0); return true;`);
  await sleep(500);
  const outside = await cdp.eval(`return [...document.querySelectorAll('[data-node-state]')].map((e) => e.getAttribute('data-node-state'));`);
  const remountHits = await stopRecord(cdp, "remount");
  report.ok("（前提）子图里挂上来的节点已经是 done", inside.length > 0 && inside.every((s) => s === "done"), JSON.stringify(inside));
  report.ok("（前提）回到顶层挂上来的节点也都跑完了", outside.length === 2 && outside.every((s) => s === "done" || s === "skipped"),
    JSON.stringify(outside));
  report.eq("进出子图重新挂载的节点一次都没闪", remountHits, []);
}

// ------------------------------------------------------------------ 验收 8：hover

async function suiteHover(cdp, report) {
  report.section("动效 验收 8：hover（真鼠标）—— 节点高亮关联边、边亮两端、端口放大、拖线不淡化");
  await install(cdp);
  await clearDoc(cdp);
  const ids = await buildGraph(cdp, [
    { key: "g1", op: "gen.synthetic", params: { pointCount: 1000 } },
    { key: "voxel", op: "filter.voxel_grid" },
    { key: "pass", op: "filter.passthrough" },
    { key: "g2", op: "gen.synthetic", params: { pointCount: 800 }, row: 1 },
    { key: "crop", op: "filter.crop_box", row: 1 },
  ], [
    { from: ["g1", "cloud"], to: ["voxel", "cloud"] },
    { from: ["voxel", "cloud"], to: ["pass", "cloud"] },
    { from: ["g2", "cloud"], to: ["crop", "cloud"] },
  ]);
  await normalizeZoom(cdp, 0.7);
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.g1]: { x: 20, y: 30 },
    [ids.voxel]: { x: Math.round(box.w * 0.34), y: 30 },
    [ids.pass]: { x: Math.round(box.w * 0.66), y: 30 },
    [ids.g2]: { x: 20, y: Math.round(box.h * 0.45) },
    [ids.crop]: { x: Math.round(box.w * 0.5), y: Math.round(box.h * 0.45) },
  });
  await sleep(450);
  const edgeId = await cdp.eval(`
    const d = window.__lyflow.stores.graph.getState().doc;
    const find = (a, b) => d.edges.find((e) => e.from.node === a && e.to.node === b).id;
    return { a: find(${lit(ids.g1)}, ${lit(ids.voxel)}), b: find(${lit(ids.voxel)}, ${lit(ids.pass)}), c: find(${lit(ids.g2)}, ${lit(ids.crop)}) };
  `);
  const relations = () => cdp.eval(`
    const out = {};
    for (const g of document.querySelectorAll('.react-flow__edge')) {
      const cls = g.querySelector('.ly-edge').classList;
      out[g.getAttribute('data-id')] = cls.contains('is-related') ? 'related' : cls.contains('is-dimmed') ? 'dimmed' : '';
    }
    return out;
  `);

  await moveMouse(cdp, await centerOf(cdp, `[data-testid="node-${ids.voxel}"] .node__head`));
  await sleep(200);
  let rel = await relations();
  report.eq("鼠标到 voxel 上：它的两条边 is-related、另一条 is-dimmed",
    rel, { [edgeId.a]: "related", [edgeId.b]: "related", [edgeId.c]: "dimmed" });
  const lifted = await cdp.eval(`return getComputedStyle(document.querySelector('[data-testid="node-${ids.voxel}"]')).boxShadow;`);
  report.ok("节点 hover 加深了阴影（不位移）", /22px/.test(lifted), lifted);

  await moveMouse(cdp, await emptySpot(cdp));
  await sleep(200);
  rel = await relations();
  report.ok("移开：全部清除", Object.values(rel).every((v) => v === ""), JSON.stringify(rel));

  // 边 hover：鼠标放到 g2→crop 那条线的中点上
  const mid = await cdp.eval(`
    const p = document.querySelector('.react-flow__edge[data-id="${edgeId.c}"] .react-flow__edge-interaction');
    const pt = p.getPointAtLength(p.getTotalLength() / 2);
    const m = p.getScreenCTM();
    return { x: Math.round(pt.x * m.a + pt.y * m.c + m.e), y: Math.round(pt.x * m.b + pt.y * m.d + m.f) };
  `);
  await moveMouse(cdp, mid);
  await sleep(200);
  const ends = await cdp.eval(`
    const has = (sel, cls) => document.querySelector(sel)?.classList.contains(cls) ?? null;
    return {
      fromPort: has('[data-testid="port-${ids.g2}-cloud"]', 'node-port--edge-end'),
      toPort: has('[data-testid="port-${ids.crop}-cloud"]', 'node-port--edge-end'),
      fromNode: has('[data-testid="node-${ids.g2}"]', 'is-edge-end'),
      toNode: has('[data-testid="node-${ids.crop}"]', 'is-edge-end'),
      others: document.querySelectorAll('.node-port--edge-end').length,
      width: getComputedStyle(document.querySelector('.react-flow__edge[data-id="${edgeId.c}"] .react-flow__edge-path')).strokeWidth,
    };
  `);
  report.ok("移到边上：两端端口 node-port--edge-end、两端节点 is-edge-end",
    ends.fromPort && ends.toPort && ends.fromNode && ends.toNode && ends.others === 2, JSON.stringify(ends));
  report.ok("边本身加粗了", parseFloat(ends.width) > 2, ends.width);

  // 端口 hover：voxel 的输入圆点。放大画在 ::before 上，圆点自己的盒子不动（A6）
  const handleSel = `[data-testid="port-${ids.voxel}-cloud"].node-port--input .react-flow__handle`;
  await moveMouse(cdp, await centerOf(cdp, handleSel));
  await sleep(250);
  const readDot = () => cdp.eval(`
    const h = document.querySelector(${lit(handleSel)});
    const own = new DOMMatrixReadOnly(getComputedStyle(h).transform);
    const pseudo = getComputedStyle(h, '::before');
    const m = pseudo.transform === 'none' ? new DOMMatrixReadOnly() : new DOMMatrixReadOnly(pseudo.transform);
    return { scale: Math.hypot(m.a, m.b), ownScale: Math.hypot(own.a, own.b), shadow: pseudo.boxShadow,
             opacity: pseudo.opacity, pseudoTransform: pseudo.transform };
  `);
  const dot = await readDot();
  report.ok(`移到端口：圆点 ::before 计算后的缩放 ${dot.scale.toFixed(2)} > 1`, dot.scale > 1.05 && dot.opacity === "1", JSON.stringify(dot));
  report.ok("圆点本身没有缩放（量测拿到的盒子不变）", Math.abs(dot.ownScale - 1) < 1e-6, JSON.stringify(dot));
  report.ok("端口的光晕（::before 的 box-shadow）非 none", dot.shadow && dot.shadow !== "none", dot.shadow);

  // hover 着端口时逼 React Flow 重量一次：改个长标题把节点撑宽，ResizeObserver → updateNodeInternals。
  // 鼠标停在输入圆点上，节点往右长，它不挪；输出那条边的端点要跟到新的右缘，才说明真的重量过了
  const widthOf = () => cdp.eval(`return document.querySelector('[data-testid="node-${ids.voxel}"]').getBoundingClientRect().width;`);
  const w0 = await widthOf();
  await cdp.eval(`window.__lyflow.stores.graph.getState().renameNode(${lit(ids.voxel)}, '一个很长很长很长很长的标题，把节点撑宽'); return true;`);
  await sleep(350);
  const w1 = await widthOf();
  const still = await readDot();
  const remeasured = (await alignOf(cdp)).filter((r) => r.id === edgeId.a || r.id === edgeId.b);
  report.ok(`（前提）节点被撑宽了（${Math.round(w0)} → ${Math.round(w1)} px），鼠标仍在端口上`,
    w1 > w0 + 20 && still.scale > 1.05, JSON.stringify({ w0, w1, still }));
  report.ok(`hover 端口期间重新量测之后，它的两条边端点与锚点最大偏差 ${worst(remeasured)} px ≤ 1`,
    remeasured.length === 2 && worst(remeasured) <= 1, JSON.stringify(remeasured));
  await cdp.eval(`window.__lyflow.stores.graph.getState().undo(); return true;`);
  await sleep(200);

  // 拖连线途中经过节点：不出现 is-dimmed
  const src = await centerOf(cdp, `[data-testid="port-${ids.g2}-cloud"] .react-flow__handle`);
  const over = await centerOf(cdp, `[data-testid="node-${ids.voxel}"] .node__head`);
  const common = { button: "left", buttons: 1, clickCount: 1 };
  await moveMouse(cdp, src);
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: src.x, y: src.y, ...common });
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: src.x + 3, y: src.y, ...common });
  const seen = { dimmed: 0, pending: false };
  for (let i = 1; i <= 14; i += 1) {
    const x = Math.round(src.x + ((over.x - src.x) * i) / 14);
    const y = Math.round(src.y + ((over.y - src.y) * i) / 14);
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y, ...common });
    await sleep(16);
    const s = await cdp.eval(`return { dimmed: document.querySelectorAll('.ly-edge.is-dimmed').length,
      pending: !!window.__lyflow.stores.ui.getState().pendingFrom };`);
    seen.dimmed = Math.max(seen.dimmed, s.dimmed);
    seen.pending ||= s.pending;
  }
  await sleep(150);
  const onNode = await cdp.eval(`const u = window.__lyflow.stores.ui.getState();
    return { dimmed: document.querySelectorAll('.ly-edge.is-dimmed').length, pending: !!u.pendingFrom, hover: u.hoverNodeId };`);
  const spot = await emptySpot(cdp);
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: spot.x, y: spot.y, ...common });
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: spot.x, y: spot.y, ...common });
  await sleep(200);
  await cdp.eval(`const u = window.__lyflow.stores.ui.getState(); u.closeSearch(); u.endConnection(); return true;`);
  report.ok("（前提）确实在拖连线，而且鼠标此刻就停在 voxel 上（hoverNodeId 已经记上）",
    seen.pending && onNode.pending && onNode.hover === ids.voxel, JSON.stringify({ seen, onNode }));
  report.eq("拖连线途中经过节点，不出现 is-dimmed", Math.max(seen.dimmed, onNode.dimmed), 0);
  await moveMouse(cdp, await emptySpot(cdp));
}

// ------------------------------------------------------------ 验收 9：关动效

async function suiteReducedMotion(cdp, report) {
  report.section("动效 验收 9：prefers-reduced-motion: reduce —— 标记全不出现，流动的边静态高亮");
  await install(cdp);
  await clearDoc(cdp);
  await buildGraph(cdp, [
    { key: "a", op: "gen.synthetic", params: { pointCount: 1000 } },
    { key: "b", op: "filter.voxel_grid" },
  ], [{ from: ["a", "cloud"], to: ["b", "cloud"] }]);
  await sleep(450);
  /** 先缩远，再按 Ctrl+Shift+F 适配视图，读 60 ms 与 400 ms 时的缩放：还在变 = 视口在动画。 */
  const fitProbe = async () => {
    await normalizeZoom(cdp, 0.35);
    const scale = `new DOMMatrixReadOnly(getComputedStyle(document.querySelector('.react-flow__viewport')).transform).a`;
    const before = await cdp.eval(`return ${scale};`);
    await pressCtrl(cdp, "F", ["shift"]);
    return cdp.eval(`
      await new Promise((r) => setTimeout(r, 60));
      const early = ${scale};
      await new Promise((r) => setTimeout(r, 340));
      return { before: ${before}, early, late: ${scale} };
    `);
  };
  const animated = await fitProbe();
  report.ok("（对照）动效开着时适配视图有过渡：60 ms 时缩放还没到终值",
    animated.late !== animated.before && Math.abs(animated.early - animated.late) > 1e-3, JSON.stringify(animated));

  await cdp.send("Emulation.setEmulatedMedia", { features: [{ name: "prefers-reduced-motion", value: "reduce" }] });
  try {
    await sleep(150);
    const instant = await fitProbe();
    report.ok("关动效时适配视图一步到位：60 ms 时已经是终值（fitView 的 duration 为 0）",
      instant.late !== instant.before && Math.abs(instant.early - instant.late) < 1e-6, JSON.stringify(instant));
    const root = await cdp.eval(`const a = document.querySelector('.app'); return { motion: a.getAttribute('data-motion'), off: a.classList.contains('lyflow-motion-off') };`);
    report.ok("编辑器认出了系统设置（data-motion=off、根上 lyflow-motion-off）", root.motion === "off" && root.off, JSON.stringify(root));

    await startRecord(cdp, "rmEnter", "data-entering");
    await startRecord(cdp, "rmGrow", "data-growing");
    const added = await cdp.eval(`
      const g = () => window.__lyflow.stores.graph.getState();
      const a = g().addNode('gen.synthetic', { x: 60, y: 60 });
      const b = g().addNode('filter.voxel_grid', { x: 360, y: 60 });
      await window.__lyMotion.frames(2);
      const opacity = Number(getComputedStyle(document.querySelector('[data-testid="node-' + a + '"]')).opacity);
      g().connect({ node: a, port: 'cloud' }, { node: b, port: 'cloud' });
      await window.__lyMotion.frames(2);
      g().deleteNodes([b]);
      await window.__lyMotion.frames(1);
      const ghosts = document.querySelectorAll('.canvas__ghost').length;
      await new Promise((r) => setTimeout(r, 300));
      return { opacity, ghosts };
    `);
    const rmEnter = await stopRecord(cdp, "rmEnter");
    const rmGrow = await stopRecord(cdp, "rmGrow");
    report.eq("（2）addNode 两帧后 opacity 已经是 1", added.opacity, 1);
    report.eq("（2）没有进场标记", rmEnter, []);
    report.eq("（3）删除不出残影", added.ghosts, 0);
    report.eq("（5）connect 没有生长标记", rmGrow, []);

    await clearDoc(cdp);
    const chain = slowChain();
    await buildGraph(cdp, chain.nodes, chain.edges);
    const { shot, leftover } = await sampleFlow(cdp);
    report.ok("（6）目标节点 running 时它的入边仍有 data-flowing=\"1\"",
      shot !== null && shot.expected.length > 0 && JSON.stringify(shot.flowing) === JSON.stringify(shot.expected), JSON.stringify(shot));
    report.eq("（6）流动层计算后的 animation-name 为 none", shot?.flowAnimation, "none");
    report.eq("running 的呼吸光也停了", shot?.pulse, "none");
    report.eq("运行结束后没有残留的 data-flowing", leftover, 0);
  } finally {
    await cdp.send("Emulation.setEmulatedMedia", { features: [] });
    await sleep(150);
  }
  const back = await cdp.eval(`return document.querySelector('.app').getAttribute('data-motion');`);
  report.eq("撤掉模拟之后动效恢复", back, "on");
}

/** 端点对齐与真鼠标这几样别的分组也要（docs/node-run-plan.md 验收 11 复用对齐断言）。 */
export { install as installMotionProbe, alignOf, worst, moveMouse, emptySpot };

export const motionSuites = [
  suiteEnter,
  suiteDelete,
  suiteAlign,
  suiteGrow,
  suiteFlow,
  suiteStateFeedback,
  suiteHover,
  suiteReducedMotion,
];
