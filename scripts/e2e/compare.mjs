// 两节点输出并排对比（交互清单 #35，docs/compare-plan.md §6）。差异表每种类型的数值只在
// packages/editor/test/compare-diff.test.mjs 里钉；这里只验真界面那一半：两栏、冻结、共用相机、换 B、A 跟随。
import { sleep } from "./cdp.mjs";
import {
  buildGraph,
  centerOf,
  dragMouse,
  lit,
  mustOk,
  newDoc,
  pressCtrl,
  pressF5,
  runAndWait,
  select,
} from "./page.mjs";

// 与 peek.mjs 同一张链：gen → pass → pick → fit → rr
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

async function clickAt(cdp, p) {
  const common = { x: p.x, y: p.y, button: "left", clickCount: 1 };
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y, buttons: 0 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", buttons: 1, ...common });
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", buttons: 0, ...common });
  await sleep(150);
}

async function clickTestId(cdp, testId) {
  const p = await centerOf(cdp, `[data-testid="${testId}"]`);
  mustOk(Boolean(p), `找得到按钮 ${testId}`);
  await clickAt(cdp, p);
}

/** 视图根上的对比标记、两栏、差异表，一次读全。 */
function readCompare(cdp) {
  return cdp.eval(`
    const v = document.querySelector('.viewer');
    if (!v) return null;
    const pane = (s) => {
      const el = v.querySelector('[data-testid="compare-pane-' + s + '"]');
      return el ? {
        view: el.getAttribute('data-view'),
        bounds: el.getAttribute('data-cloud-bounds'),
        node: el.getAttribute('data-node'),
        run: el.getAttribute('data-run'),
        base: el.getAttribute('data-base'),
      } : null;
    };
    const diff = v.querySelector('[data-testid="compare-diff"]');
    const rows = {};
    if (diff) {
      for (const tr of diff.querySelectorAll('tr[data-key]')) {
        rows[tr.getAttribute('data-key')] = {
          changed: tr.getAttribute('data-changed'),
          kind: tr.getAttribute('data-kind'),
          delta: tr.querySelector('.compare-diff__delta').textContent,
        };
      }
    }
    const canvas = v.querySelector('.viewer__canvas');
    return {
      on: v.getAttribute('data-compare'),
      a: v.getAttribute('data-compare-a'),
      b: v.getAttribute('data-compare-b'),
      frozen: v.getAttribute('data-compare-frozen'),
      runB: v.getAttribute('data-compare-run-b'),
      split: v.getAttribute('data-split'),
      camera: v.getAttribute('data-camera'),
      cameraPos: canvas ? canvas.getAttribute('data-camera-pos') : null,
      canvases: v.querySelectorAll('canvas').length,
      paneA: pane('a'),
      paneB: pane('b'),
      diff: diff ? {
        changed: diff.getAttribute('data-changed-count'),
        mismatch: diff.getAttribute('data-mode-mismatch'),
        rows,
      } : null,
      runId: window.__lyflow.stores.execution.getState().runId,
    };
  `);
}

async function waitCompare(cdp, pred, what, timeoutMs = 20_000) {
  const deadline = Date.now() + timeoutMs;
  let last = null;
  while (Date.now() < deadline) {
    last = await readCompare(cdp);
    if (last && pred(last)) return last;
    await sleep(120);
  }
  mustOk(false, `等到${what}`, last);
  return last;
}

/** 选中一个节点，等预览真的切到它的点云（进入对比要拿它此刻的结果去冻结）。 */
async function selectAndWaitCloud(cdp, nodeId) {
  await select(cdp, nodeId);
  await cdp.waitFor(
    `(() => { const v = document.querySelector('.viewer');
              return v && v.getAttribute('data-node') === ${lit(nodeId)} && v.getAttribute('data-view') === 'cloud'; })()`,
    { timeoutMs: 20_000, what: `预览切到 ${nodeId} 的点云` },
  );
}

async function prepare(cdp) {
  await newDoc(cdp);
  await cdp.eval(`
    window.__lyflow.stores.compare.getState().exit();
    window.__lyflow.stores.ui.getState().setPinnedNode(null);
    window.__lyflow.stores.ui.getState().setViewerMode("3d");
    return true;
  `);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "对比要看的这张图跑通了", JSON.stringify(run.nodes));
  return ids;
}

async function suiteCompareFreeze(cdp, report) {
  report.section("对比：进入即冻结 B，改参数重跑后只有 A 变（§6 1–4）");
  const ids = await prepare(cdp);

  await selectAndWaitCloud(cdp, ids.gen);
  const button = await cdp.eval(`
    const b = document.querySelector('[data-testid="viewer-compare"]');
    const r = b.getBoundingClientRect();
    const hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
    return { disabled: b.disabled, onTop: hit === b };
  `);
  report.eq("「对比」按钮可点、没被挤出栏外", button, { disabled: false, onTop: true });
  await clickTestId(cdp, "viewer-compare");
  const first = await waitCompare(
    cdp,
    (c) => c.on === "1" && c.paneA?.view === "cloud" && c.paneB?.view === "cloud" && c.diff,
    "两栏都画出点云",
  );
  report.eq("进入对比：B 是 gen、已冻结、冻在当前这次运行",
    [first.on, first.b, first.frozen, first.runB], ["1", ids.gen, "1", first.runId]);
  report.ok("两栏的包围盒相同且非空", Boolean(first.paneA.bounds) && first.paneA.bounds === first.paneB.bounds,
    `${first.paneA.bounds} / ${first.paneB.bounds}`);
  report.eq("同一个结果比自己：0 项不同", first.diff.changed, "0");

  // 改参数重跑：A 跟着新运行，B 停在冻结的那一次（那次运行的索引此时已被后端回收，C3）
  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.gen)}, 'pointCount', 6000);
    return true;
  `);
  await runAndWait(cdp, () => pressF5(cdp));
  const after = await waitCompare(
    cdp,
    (c) => c.diff?.rows["cloud.points"]?.changed === "1" && c.paneA?.view === "cloud",
    "差异表里点数那一行标成不同",
  );
  report.eq("点数行的 Δ 写成「+1 000 (+20%)」", after.diff.rows["cloud.points"].delta, "+1 000 (+20%)");
  report.ok("A 栏换到了新一次运行，B 仍是冻结时那一次",
    after.paneA.run === after.runId && after.runB === first.runId && after.runId !== first.runId,
    `A=${after.paneA.run} B=${after.runB} now=${after.runId} old=${first.runId}`);
  report.eq("B 栏的包围盒没变", after.paneB.bounds, first.paneB.bounds);

  // 共用相机：在 A 栏里真鼠标拖一下，两栏一起转 —— 只有一块画布、一台相机
  const box = await cdp.eval(`
    const r = document.querySelector('[data-testid="compare-pane-a"]').getBoundingClientRect();
    return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
  `);
  const before = (await readCompare(cdp)).cameraPos;
  await dragMouse(cdp, box, { x: box.x + 60, y: box.y + 20 });
  await sleep(300);
  const dragged = await readCompare(cdp);
  report.ok("在 A 栏拖动，相机动了", Boolean(before) && dragged.cameraPos !== before,
    `${before} → ${dragged.cameraPos}`);
  report.eq("两栏共用一块画布", dragged.canvases, 1);

  await cdp.eval(`window.__lyflow.stores.ui.getState().setViewerMode("2d"); return true;`);
  const flat = await waitCompare(cdp, (c) => c.camera === "2d", "切到 2D");
  report.eq("2D 剖面下两栏仍是点云", [flat.paneA.view, flat.paneB.view], ["cloud", "cloud"]);
  await cdp.eval(`window.__lyflow.stores.ui.getState().setViewerMode("3d"); return true;`);
}

async function suiteCompareFollow(cdp, report) {
  report.section("对比：换 B 解冻，A 跟随选中 / 钉住（§6 5–6）");
  const ids = await prepare(cdp);
  await selectAndWaitCloud(cdp, ids.gen);
  await cdp.eval(`window.__lyflow.stores.compare.getState().toggle(); return true;`);
  await waitCompare(cdp, (c) => c.on === "1" && c.frozen === "1", "进入对比并冻结");

  // 右键「设为对比基准（B）」：已在对比就换 B
  await openNodeMenu(cdp, ids.fit);
  await clickTestId(cdp, "ctx-compare-b");
  const fit = await waitCompare(
    cdp,
    (c) => c.b === ids.fit && c.paneB?.view === "cloud" && c.diff?.rows.line,
    "B 换成 fit 并画出底图",
  );
  report.eq("换 B 同时解冻", fit.frozen, "0");
  report.eq("fit 只有 2D 几何：B 栏的底图是它上游那片云", fit.paneB.base, ids.pick);
  report.ok("gen 与 fit 没有能配上的端口：line 只在 B 侧",
    fit.diff.rows.line?.kind === "only" && Object.values(fit.diff.rows).filter((r) => r.kind === "only").length >= 1,
    JSON.stringify(fit.diff.rows));

  await select(cdp, ids.pass);
  const follow = await waitCompare(cdp, (c) => c.a === ids.pass, "A 跟着选中换到 pass");
  report.eq("点别的节点：A 换，B 不动", [follow.a, follow.b], [ids.pass, ids.fit]);

  await cdp.eval(`window.__lyflow.stores.ui.getState().setPinnedNode(${lit(ids.pass)}); return true;`);
  await select(cdp, ids.gen);
  await sleep(300);
  const pinned = await readCompare(cdp);
  report.eq("钉住后再点别的：A 不换", pinned.a, ids.pass);

  await cdp.eval(`window.__lyflow.stores.ui.getState().setPinnedNode(null); return true;`);
  await clickTestId(cdp, "viewer-compare");
  const off = await waitCompare(cdp, (c) => c.on === "0", "退出对比");
  report.ok("退出对比：两栏与差异表都收起", !off.paneA && !off.diff, JSON.stringify(off));
}

/** 真的在节点上弹出右键菜单（与 noderun.mjs 同一种派发）。 */
async function openNodeMenu(cdp, id) {
  const ok = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${id}"]');
    if (!el) return false;
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true,
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 }));
    await new Promise((r2) => setTimeout(r2, 150));
    return !!document.querySelector('[data-testid="node-context-menu"]');
  `);
  mustOk(ok, `节点 ${id} 的右键菜单弹出来了`);
}

/** 参数面板的虚拟列表里把某一行滚到挂上为止（params_p2.mjs 的 reveal 的精简版）。 */
function revealRow(cdp, selector) {
  return cdp.eval(`
    const frame = () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const list = document.querySelector('[data-testid="pp-list"]');
    if (!list) return false;
    const find = () => document.querySelector(${lit(selector)});
    list.scrollTop = 0;
    await frame();
    for (let i = 0; i < 200 && !find(); i += 1) {
      if (list.scrollTop + list.clientHeight >= list.scrollHeight - 1) break;
      list.scrollTop += Math.max(80, list.clientHeight * 0.7);
      await frame();
    }
    return !!find();
  `);
}

async function suiteCompareExits(cdp, report) {
  report.section("对比：右键进入、B 被删自动退出、快捷键开关预览、拖框互斥（§6 5、7、8）");
  const ids = await prepare(cdp);
  await selectAndWaitCloud(cdp, ids.gen);

  await openNodeMenu(cdp, ids.fit);
  await clickTestId(cdp, "ctx-compare-b");
  const entered = await waitCompare(cdp, (c) => c.on === "1" && c.b === ids.fit, "右键把 fit 设为 B");
  report.eq("没在对比时右键「设为对比基准（B）」：进入，B = fit，跟随最新不冻结",
    [entered.on, entered.b, entered.frozen, entered.a], ["1", ids.fit, "0", ids.gen]);

  await cdp.eval(`window.__lyflow.stores.graph.getState().deleteNodes([${lit(ids.fit)}]); return true;`);
  await waitCompare(cdp, (c) => c.on === "0", "B 的节点被删后退出对比");
  const toast = await cdp.eval(`return window.__lyflow.stores.ui.getState().toast?.text ?? null;`);
  report.eq("删掉 B 的节点：退出对比并提示", toast, "对比基准节点已删除，已退出对比");

  // 参数面板开着、预览收起：快捷键进入时先把预览展开，否则进入了也看不见
  await selectAndWaitCloud(cdp, ids.gen);
  await cdp.eval(`
    const ui = window.__lyflow.stores.ui.getState();
    ui.toggleParamPanel(true);
    ui.setPanelViewerOpen(false);
    document.activeElement?.blur?.();
    return true;
  `);
  await sleep(300);
  await pressCtrl(cdp, "D", ["shift"]);
  const byKey = await waitCompare(cdp, (c) => c.on === "1", "Ctrl+Shift+D 进入对比");
  const viewerOpen = await cdp.eval(`return window.__lyflow.stores.ui.getState().paramPanel.viewerOpen;`);
  report.eq("面板开着、预览收起时按 Ctrl+Shift+D：预览展开、进入对比、B 冻结在 gen",
    [viewerOpen, byKey.on, byKey.b, byKey.frozen], [true, "1", ids.gen, "1"]);
  await pressCtrl(cdp, "D", ["shift"]);
  const byKeyOff = await waitCompare(cdp, (c) => c.on === "0", "再按一次退出");
  report.eq("再按一次 Ctrl+Shift+D 退出", byKeyOff.on, "0");

  // 拖框与对比互斥（C8）：参数面板 ROI 行的「拖框」先退出对比
  await newDoc(cdp);
  const roi = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 5000, seed: 14 } },
      { key: "show", op: "test.param_showcase" },
    ],
    [{ from: ["gen", "cloud"], to: ["show", "cloud"] }],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "带 ROI 参数的图跑通了", run.status);
  await selectAndWaitCloud(cdp, roi.gen);
  await cdp.eval(`window.__lyflow.stores.compare.getState().toggle(); return true;`);
  await waitCompare(cdp, (c) => c.on === "1", "进入对比");
  const key = `${roi.show}.roi`;
  mustOk(await revealRow(cdp, `[data-testid="pp-roi-edit-${key}"]`), "参数面板里找得到 ROI 行的「拖框」");
  await cdp.eval(`document.querySelector('[data-testid="pp-roi-edit-${key}"]').click(); return true;`);
  await cdp.waitFor(`Number(document.querySelector('.viewer')?.getAttribute('data-roi-edit') || 0) > 0`,
    { what: "进入 2D 拖框", timeoutMs: 15_000 }).catch(() => {});
  const dragging = await readCompare(cdp);
  const roiEdit = await cdp.eval(`return Number(document.querySelector('.viewer').getAttribute('data-roi-edit') || 0);`);
  report.ok("对比开着时点「拖框」：先退出对比，再进 2D 拖框", dragging.on === "0" && roiEdit > 0,
    `compare=${dragging.on} roiEdit=${roiEdit}`);
  await cdp.eval(`
    const ui = window.__lyflow.stores.ui.getState();
    ui.toggleParamPanel(false);
    ui.setPanelViewerOpen(false);
    ui.setViewerMode('3d');
    return true;
  `);
}

async function suiteCompareValues(cdp, report) {
  report.section("对比：两侧都只有值 —— 两张值表格并排，差异表比 Transform（§6 9）");
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.compare.getState().exit(); return true;`);
  const ids = await buildGraph(cdp, [{ key: "pose", op: "transform.make" }], []);
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "transform.make 跑通了", run.status);
  await select(cdp, ids.pose);
  await cdp.waitFor(
    `(() => { const v = document.querySelector('.viewer');
              return v && v.getAttribute('data-node') === ${lit(ids.pose)} && v.getAttribute('data-view') === 'value'; })()`,
    { timeoutMs: 20_000, what: "预览切到 pose 的值表格" },
  );
  await cdp.eval(`window.__lyflow.stores.compare.getState().toggle(); return true;`);
  await waitCompare(cdp, (c) => c.on === "1" && c.frozen === "1", "进入对比并冻结");
  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.pose)}, 'translation', [0.1, 0, 0]);
    return true;
  `);
  await runAndWait(cdp, () => pressF5(cdp));
  const after = await waitCompare(cdp, (c) => c.diff?.rows["transform.m"]?.changed === "1", "Transform 那一行标成不同");
  const panes = await cdp.eval(`return document.querySelectorAll('.viewer [data-testid="viewer-values"]').length;`);
  report.eq("两栏都是值表格", [after.paneA.view, after.paneB.view, panes], ["value", "value", 2]);
  report.eq("Transform 只写最大绝对差", after.diff.rows["transform.m"].delta, "max|Δ| 0.1");
  await cdp.eval(`window.__lyflow.stores.compare.getState().exit(); return true;`);
}

export const compareSuites = [suiteCompareFreeze, suiteCompareFollow, suiteCompareExits, suiteCompareValues];
