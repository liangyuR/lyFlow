// 参数面板（docs/param-recipe-plan.md P2）的分组：验收 9–15。逐条结果见 docs/param-recipe-p2-acceptance.md。
//
//  9 面板开关（工具栏按钮 / Ctrl+Shift+P）、拖宽、最大化与还原、宽度记忆；打开时 Inspector 不显示；
//    画布选中 ↔ 面板定位双向联动。
// 10 全类型示例（test.param_showcase）：14 种类型每种的控件都能改值，doc 里值与类型都对；
//    transform 与 curve 改 → 存盘 → 重开，值不变，core 收到的也是这一份（echo）。
// 11 advanced 默认折叠；visibleWhen / enabledWhen 随另一个参数的改动即时生效。
// 12 搜索与每个过滤 chip 的结果：期望值在 Node 这边对着 doc + manifest 独立算。
// 13 子图定义行有「N 个实例共享」标记，改它两个实例的有效值都变；库算子内部只读（不展开）。
// 14 ROI 缩略图 →「拖框」进 2D 拖框视图，真鼠标拖框，参数变，一次撤销还原。
// 15 性能：脚本生成 1000 参数的图，打开面板 < 300 ms，滚动 ≥ 50 fps，输入一个数到画布状态更新 < 100 ms。
//
// test.param_showcase 只在 LYFLOW_TEST_OPS=1 时注册进 core（harness.mjs 起 app 时设）。
// 面板的行是虚拟化的：要操作的行先 reveal（滚到它挂上为止），再走真实 DOM 事件；指针手势用 CDP 的真鼠标。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { ROOT } from "./harness.mjs";
import {
  buildGraph,
  dragMouse,
  lit,
  mustOk,
  newDoc,
  placeAtScreen,
  pressCtrl,
  pressKey,
  pressF5,
  runAndWait,
  saveGraphTo,
  revealInList as reveal,
} from "./page.mjs";

const SHOW = "test.param_showcase";
const PANEL_WIDTH_KEY = "lyflow.paramPanel.width";

// ------------------------------------------------------------ 页面侧的小工具

const docOf = (cdp) => cdp.eval(`return window.__lyflow.stores.graph.getState().doc;`);
const snap = (cdp) => cdp.eval(`return window.__lyflow.snapshot();`);
const paramsOf = async (cdp, id) => (await docOf(cdp)).nodes.find((n) => n.id === id)?.params ?? {};

async function openPanel(cdp) {
  await cdp.eval(`window.__lyflow.stores.ui.getState().toggleParamPanel(true); return true;`);
  await cdp.waitFor(`!!document.querySelector('[data-testid="pp-list"]')`, { what: "参数面板挂上" });
  await sleep(150);
}

/** 收尾：面板关掉、视图还原，别影响后面的分组。 */
async function resetPanel(cdp) {
  await cdp.eval(`
    const ui = window.__lyflow.stores.ui.getState();
    ui.setParamPanelMaximized(false);
    ui.toggleParamPanel(false);
    ui.setPanelViewerOpen(false);
    ui.setViewerMode('3d');
    ui.setPath([]);
    return true;
  `);
  await sleep(120);
}

/** 在一行里的某个输入框里「打字」再失焦：原生 value setter + input 事件（React 才看得见），失焦才提交。 */
async function typeIn(cdp, rowSel, text, { index = 0, tag = "input" } = {}) {
  if (!(await reveal(cdp, rowSel))) return "no-row";
  return cdp.eval(`
    const row = document.querySelector(${lit(rowSel)});
    const el = row?.querySelectorAll(${lit(tag)})[${index}];
    if (!el) return 'no-input';
    el.focus();
    const proto = el instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, ${lit(text)});
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.blur();
    await new Promise((d) => setTimeout(d, 80));
    return 'ok';
  `);
}

/** 真按键：聚焦 selector 指的输入框、全选、打字、按 Esc。返回 Esc 之后输入框里显示的字。 */
async function typeThenEscape(cdp, selector, text) {
  const found = await cdp.eval(`
    const el = document.querySelector(${lit(selector)});
    if (!el) return false;
    el.scrollIntoView({ block: 'center' });
    el.focus();
    return true;
  `);
  if (!found) return null;
  await pressKey(cdp, "a", 65, ["ctrl"]);
  await cdp.send("Input.insertText", { text });
  await pressKey(cdp, "Escape", 27);
  await sleep(150);
  return cdp.eval(`return document.querySelector(${lit(selector)})?.value ?? null;`);
}

/** 行里的下拉框选一项（原生 setter + change 事件）。 */
async function chooseIn(cdp, rowSel, value, selector = "select") {
  if (!(await reveal(cdp, rowSel))) return "no-row";
  return cdp.eval(`
    const el = document.querySelector(${lit(rowSel)})?.querySelector(${lit(selector)});
    if (!el) return 'no-select';
    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(el, ${lit(value)});
    el.dispatchEvent(new Event('change', { bubbles: true }));
    await new Promise((d) => setTimeout(d, 80));
    return 'ok';
  `);
}

/** 点一个元素（先 reveal 它所在的行）。 */
async function clickIn(cdp, rowSel, selector = null) {
  if (!(await reveal(cdp, rowSel))) return "no-row";
  return cdp.eval(`
    const row = document.querySelector(${lit(rowSel)});
    const el = ${selector ? `row?.querySelector(${lit(selector)})` : "row"};
    if (!el) return 'no-target';
    el.click();
    await new Promise((d) => setTimeout(d, 80));
    return 'ok';
  `);
}

/** 真鼠标点一下屏幕上的一个点。 */
async function mouseClick(cdp, p) {
  const common = { x: p.x, y: p.y, button: "left", clickCount: 1 };
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y, buttons: 0 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", buttons: 1, ...common });
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", buttons: 0, ...common });
  await sleep(150);
}

async function blurActive(cdp) {
  await cdp.eval(`if (document.activeElement instanceof HTMLElement) document.activeElement.blur(); return true;`);
}

async function undoByKey(cdp) {
  await blurActive(cdp);
  await pressCtrl(cdp, "Z");
  await sleep(200);
}

const pressCtrlShiftP = (cdp) =>
  pressKey(cdp, "P", 80, ["ctrl", "shift"]).then(() => sleep(200));

const rowSel = (key) => `[data-testid="prow-${key}"]`;

/** 一个 showcase 节点接在 gen 后面，跑一遍（ROI 的底图、echo 都要这次运行）。 */
async function showcaseGraph(cdp, { seed = 11, extra = [] } = {}) {
  await newDoc(cdp);
  return buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 8000, seed } },
      { key: "show", op: SHOW },
      ...extra,
    ],
    [{ from: ["gen", "cloud"], to: ["show", "cloud"] }],
  );
}

// ------------------------------------------------------------ 验收 9

async function suiteLayout(cdp, report) {
  report.section("P2 验收 9：面板开关、拖宽、最大化、宽度记忆；打开时 Inspector 不显示；画布 ↔ 面板联动");
  await resetPanel(cdp);
  const hasOp = await cdp.eval(`return !!window.__lyflow.stores.manifest.getState().operatorsById.get(${lit(SHOW)});`);
  mustOk(hasOp, "test.param_showcase 没注册：起 app 时要 LYFLOW_TEST_OPS=1");

  // 六个节点，面板的列表足够长，才验得出「画布选中 → 面板滚过去」
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 4000 } },
      { key: "s1", op: SHOW },
      { key: "s2", op: SHOW },
      { key: "s3", op: SHOW, row: 1 },
      { key: "s4", op: SHOW, row: 1 },
      { key: "s5", op: SHOW, row: 1 },
    ],
    [{ from: ["gen", "cloud"], to: ["s1", "cloud"] }],
  );
  await cdp.eval(`localStorage.removeItem(${lit(PANEL_WIDTH_KEY)}); return true;`);

  const dom = () =>
    cdp.eval(`
      const r = (sel) => { const el = document.querySelector(sel); if (!el) return null;
        const b = el.getBoundingClientRect(); return { w: Math.round(b.width), h: Math.round(b.height), x: Math.round(b.left) }; };
      return {
        panel: !!document.querySelector('[data-testid="param-panel"]'),
        inspector: !!document.querySelector('.app__inspector'),
        pressed: document.querySelector('[data-testid="param-panel-toggle"]')?.getAttribute('aria-pressed'),
        right: r('[data-testid="right-pane"]'),
        canvas: r('.app__canvas'),
        sidebar: r('.app__sidebar'),
        body: r('.app__body'),
      };
    `);

  let d = await dom();

  await blurActive(cdp);
  await pressCtrlShiftP(cdp);
  d = await dom();
  report.eq("Ctrl+Shift+P 打开面板，Inspector 不显示", [d.panel, d.inspector, d.pressed], [true, false, "true"]);
  await pressCtrlShiftP(cdp);
  d = await dom();
  report.eq("再按一次 Ctrl+Shift+P 关上，Inspector 回来", [d.panel, d.inspector], [false, true]);
  await cdp.eval(`document.querySelector('[data-testid="param-panel-toggle"]').click(); return true;`);
  await sleep(200);
  d = await dom();
  report.eq("工具栏「参数」按钮打开", [d.panel, d.inspector, d.pressed], [true, false, "true"]);
  const width0 = d.right.w;
  report.ok("默认宽度比 Inspector 宽（≥ 560）", width0 >= 560, `${width0}px`);

  // 拖宽：真鼠标拖分栏把手 120 px。窗口不够宽时（上限 = 工作区 − 算子面板 − 最小画布）往窄里拖
  const handle = await cdp.eval(`
    const b = document.querySelector('[data-testid="right-splitter"]').getBoundingClientRect();
    return { x: Math.round(b.left + b.width / 2), y: Math.round(b.top + b.height / 2) };
  `);
  // 拖动中的状态类挂在编辑器根上、不碰宿主的 body（docs/multi-instance-research.md 的防呆）：
  // 按下把手那一刻就挂上，原地松开就撤，宽度不变，不影响下面的拖宽
  const press = { x: handle.x, y: handle.y, button: "left", clickCount: 1 };
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: handle.x, y: handle.y, buttons: 0 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", buttons: 1, ...press });
  const resizing = () => cdp.eval(`
    return { root: document.querySelector('[data-lyflow-editor]').classList.contains('lyflow-is-resizing'),
             body: document.body.className.includes('resizing') };
  `);
  const during = await resizing();
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", buttons: 0, ...press });
  await sleep(80);
  const released = await resizing();
  report.eq("拖分栏的状态类挂在编辑器根上、不碰 body，松手就撤",
    [during.root, during.body, released.root], [true, false, false]);

  const limit = d.body.w - 280 - 320;
  const delta = width0 + 120 <= limit ? 120 : -120;
  await dragMouse(cdp, handle, { x: handle.x - delta, y: handle.y }, { steps: 10 });
  d = await dom();
  const width1 = d.right.w;
  report.ok(`真鼠标拖分栏：面板宽度变了约 ${delta} px`, Math.abs(width1 - width0 - delta) <= 6, `${width0} → ${width1}（上限 ${limit}）`);
  const stored = Number(await cdp.eval(`return localStorage.getItem(${lit(PANEL_WIDTH_KEY)});`));
  report.ok("宽度记进了 localStorage", Math.abs(stored - width1) <= 2, `stored=${stored} width=${width1}`);

  await cdp.eval(`window.__lyflow.stores.ui.getState().toggleParamPanel(false); return true;`);
  await sleep(150);
  d = await dom();
  report.ok("关上面板：右侧回到 Inspector 的宽度", d.right.w < Math.min(width0, width1) - 100, `${d.right.w}px`);

  // 预览与检查器之间、底部抽屉的上沿都能上下拖，松手按比例记住；双击把手回到默认、忘掉记住的。
  // 以前预览固定占右栏 44%、抽屉固定 220 px，日志一多只看得到十来行
  const rows = () => cdp.eval(`
    const h = (sel) => Math.round(document.querySelector(sel)?.getBoundingClientRect().height ?? -1);
    const mid = (sel) => { const b = document.querySelector(sel)?.getBoundingClientRect();
      return b ? { x: Math.round(b.left + b.width / 2), y: Math.round(b.top + b.height / 2) } : null; };
    return { viewer: h('.app__viewer'), drawer: h('[data-testid="drawer"]'),
             viewerHandle: mid('[data-testid="viewer-splitter"]'), drawerHandle: mid('[data-testid="drawer-splitter"]'),
             stored: [localStorage.getItem('lyflow.viewer.fraction'), localStorage.getItem('lyflow.drawer.fraction')] };
  `);
  const toggleLog = () => cdp.eval(`window.__lyflow.stores.ui.getState().toggleDrawer('log'); return true;`);
  const doubleClick = async (p) => {
    for (const clickCount of [1, 2]) {
      await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: p.x, y: p.y, button: "left", buttons: 1, clickCount });
      await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: p.x, y: p.y, button: "left", buttons: 0, clickCount });
    }
    await sleep(150);
  };
  // 先双击两个把手回到默认：上一次跑到一半留下的比例不算
  const r0 = await rows();
  mustOk(r0.viewerHandle != null, "预览下沿有可拖的把手", r0);
  await doubleClick(r0.viewerHandle);
  await toggleLog();
  await sleep(200);
  const r1 = await rows();
  mustOk(r1.drawerHandle != null, "抽屉打开后上沿有可拖的把手", r1);
  await doubleClick(r1.drawerHandle);
  await toggleLog();
  await sleep(200);
  const v0 = await rows();
  await dragMouse(cdp, v0.viewerHandle, { x: v0.viewerHandle.x, y: v0.viewerHandle.y + 80 }, { steps: 10 });
  const v1 = await rows();
  await toggleLog();
  await sleep(200);
  const d0 = await rows();
  await dragMouse(cdp, d0.drawerHandle, { x: d0.drawerHandle.x, y: d0.drawerHandle.y - 60 }, { steps: 10 });
  const d1 = await rows();
  report.ok("预览往下拖高 80、抽屉往上拖高 60，松手都记住了",
    Math.abs(v1.viewer - v0.viewer - 80) <= 6 && Math.abs(d1.drawer - d0.drawer - 60) <= 6 && d1.stored.every((s) => s !== null),
    JSON.stringify({ v0: v0.viewer, v1: v1.viewer, d0: d0.drawer, d1: d1.drawer, stored: d1.stored }));
  // 双击两个把手：回到默认、记住的删掉（后面的分组照默认的样子跑，对比时预览更高的那一档也还在）
  await doubleClick(d1.drawerHandle);
  const d2 = await rows();
  await toggleLog();
  await sleep(200);
  const v2 = await rows();
  await doubleClick(v2.viewerHandle);
  const v3 = await rows();
  report.ok("双击把手：抽屉与预览回到默认的高度，记住的比例删掉",
    Math.abs(d2.drawer - d0.drawer) <= 2 && Math.abs(v3.viewer - v0.viewer) <= 2 && v3.stored.every((s) => s === null),
    JSON.stringify({ d0: d0.drawer, d2: d2.drawer, v0: v0.viewer, v3: v3.viewer, stored: v3.stored }));
  await openPanel(cdp);
  d = await dom();
  report.ok("重新打开：还是拖过的宽度（记住宽度）", Math.abs(d.right.w - width1) <= 2, `${d.right.w} vs ${width1}`);

  // 窗口窄了（1920 屏半屏贴靠是 960，桌面窗口最小 900）：两侧面板让位，画布留够 320、面板不出窗口；
  // 拉回来还是记住的宽度。修前两侧都不缩：参数面板开着时画布挤成 0，面板右边一截跑到窗口外点不着
  const vh = await cdp.eval(`return window.innerHeight;`);
  await cdp.send("Emulation.setDeviceMetricsOverride", { width: 900, height: vh, deviceScaleFactor: 0, mobile: false });
  await sleep(250);
  const narrow = await dom();
  const vw = await cdp.eval(`return window.innerWidth;`);
  await cdp.send("Emulation.clearDeviceMetricsOverride");
  await sleep(250);
  d = await dom();
  report.ok("窗口窄到 900：画布留够 320、参数面板整个在窗口里；拉回来还是记住的宽度",
    narrow.canvas.w >= 318 && narrow.right.x + narrow.right.w <= vw + 1 && Math.abs(d.right.w - width1) <= 2,
    `窄：画布 ${narrow.canvas.w}px，面板 ${narrow.right.x}..${narrow.right.x + narrow.right.w} / 窗口 ${vw}；拉回来面板 ${d.right.w} vs ${width1}`);

  // 最大化 → 画布收起；还原 → 画布回来，宽度不变
  await cdp.eval(`document.querySelector('[data-testid="pp-maximize"]').click(); return true;`);
  await sleep(250);
  d = await dom();
  report.ok(
    "最大化：画布与算子面板收起，面板占满",
    d.canvas.w <= 1 && (d.sidebar === null || d.sidebar.w === 0),
    JSON.stringify(d),
  );
  report.ok("最大化：面板宽度 ≈ 整个工作区", Math.abs(d.right.w - d.body.w) <= 4, `${d.right.w} vs ${d.body.w}`);
  await cdp.eval(`document.querySelector('[data-testid="pp-maximize"]').click(); return true;`);
  await sleep(250);
  d = await dom();
  report.ok("还原：画布回来、面板回到记住的宽度", d.canvas.w > 300 && Math.abs(d.right.w - width1) <= 2,
    `画布 ${d.canvas.w}px，面板 ${d.right.w} vs ${width1}`);

  // 画布选中 → 面板滚过去并高亮。先把列表滚到底，再在画布上真鼠标点 s1
  await cdp.eval(`const l = document.querySelector('[data-testid="pp-list"]'); l.scrollTop = l.scrollHeight; return true;`);
  await sleep(200);
  await cdp.eval(`window.__lyflow.stores.ui.getState().clearSelection(); return true;`);
  // 画布被面板挤得很窄：先把 s1 摆进画布左上角再点（placeAtScreen 的坐标相对画布容器，见 page.mjs）
  await placeAtScreen(cdp, { [ids.s1]: { x: 30, y: 80 } });
  const nodeBox = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${ids.s1}"] .node__head') ?? document.querySelector('[data-testid="node-${ids.s1}"]');
    const b = el.getBoundingClientRect();
    const x = Math.round(b.left + Math.min(30, b.width / 3)), y = Math.round(b.top + b.height / 2);
    const top = document.elementFromPoint(x, y);
    return { x, y, hit: top?.closest('[data-testid^="node-"]')?.getAttribute('data-testid') ?? (top ? top.tagName + '.' + top.className : null) };
  `);
  await mouseClick(cdp, nodeBox);
  await sleep(350);
  const located = await cdp.eval(`
    const head = document.querySelector('[data-testid="pp-node-${ids.s1}"]');
    const list = document.querySelector('[data-testid="pp-list"]').getBoundingClientRect();
    const selected = [...window.__lyflow.stores.ui.getState().selectedNodes];
    if (!head) return { mounted: false, selected };
    const b = head.getBoundingClientRect();
    return { mounted: true, focused: head.getAttribute('data-focused'), inView: b.top >= list.top - 1 && b.bottom <= list.bottom + 1,
             selected };
  `);
  report.eq("画布上点 s1 → 选中它", { hit: nodeBox.hit, selected: located.selected }, { hit: `node-${ids.s1}`, selected: [ids.s1] });
  report.eq("面板滚到 s1 那一节、在视口里、高亮", [located.mounted, located.inView, located.focused], [true, true, "1"]);

  // 面板里点 s5 的标题 → 画布选中并居中 s5
  const s5 = await reveal(cdp, `[data-testid="pp-node-title-${ids.s5}"]`);
  mustOk(Boolean(s5), "面板里 s5 那一节找得到");
  await cdp.eval(`document.querySelector('[data-testid="pp-node-title-${ids.s5}"]').click(); return true;`);
  await sleep(700);
  const centered = await cdp.eval(`
    const c = document.querySelector('.app__canvas').getBoundingClientRect();
    const n = document.querySelector('[data-testid="node-${ids.s5}"]').getBoundingClientRect();
    return { selected: [...window.__lyflow.stores.ui.getState().selectedNodes],
             dx: Math.round(n.left + n.width / 2 - (c.left + c.width / 2)),
             dy: Math.round(n.top + n.height / 2 - (c.top + c.height / 2)) };
  `);
  report.ok("面板里点标题 → 画布选中 s5 并把它居中（中心偏差 ≤ 12 px）",
    JSON.stringify(centered.selected) === JSON.stringify([ids.s5]) && Math.abs(centered.dx) <= 12 && Math.abs(centered.dy) <= 12,
    JSON.stringify(centered));
  await resetPanel(cdp);
}

// ------------------------------------------------------------ 验收 10

/** 每种类型一条：怎么在面板里改、改完 doc 里该是什么。 */
function typeCases(show) {
  const r = (p) => rowSel(`${show}.${p}`);
  return [
    { type: "bool", param: "enabled", act: (cdp) => clickIn(cdp, r("enabled"), 'input[type="checkbox"]'), want: false },
    { type: "int", param: "iterations", act: (cdp) => typeIn(cdp, r("iterations"), "42"), want: 42 },
    { type: "float", param: "gain", act: (cdp) => typeIn(cdp, r("gain"), "1.5"), want: 1.5 },
    { type: "vec2f", param: "offset", act: (cdp) => typeIn(cdp, r("offset"), "0.25", { index: 1 }), want: [0, 0.25] },
    { type: "vec3f", param: "scale", act: (cdp) => typeIn(cdp, r("scale"), "2"), want: [2, 1, 1] },
    { type: "vec4f", param: "weights", act: (cdp) => typeIn(cdp, r("weights"), "0.5", { index: 3 }), want: [0.25, 0.25, 0.25, 0.5] },
    { type: "enum", param: "mode", act: (cdp) => chooseIn(cdp, r("mode"), "precise"), want: "precise" },
    {
      type: "flags",
      param: "features",
      act: (cdp) => cdp.eval(`
        const row = document.querySelector(${lit(r("features"))});
        const chip = [...(row?.querySelectorAll('.ctl-chip') ?? [])].find((b) => b.textContent === '颜色');
        if (!chip) return 'no-chip';
        chip.click();
        return 'ok';
      `),
      reveal: true,
      want: 7,
    },
    { type: "string", param: "tag", act: (cdp) => typeIn(cdp, r("tag"), "abc"), want: "abc" },
    { type: "text", param: "note", act: (cdp) => typeIn(cdp, r("note"), "第一行\n第二行", { tag: "textarea" }), want: "第一行\n第二行" },
    { type: "path", param: "exportPath", act: (cdp) => typeIn(cdp, r("exportPath"), "D:/lyflow-e2e/out.pcd"), want: "D:/lyflow-e2e/out.pcd" },
    {
      type: "color",
      param: "tint",
      act: async (cdp) => {
        if (!(await reveal(cdp, r("tint")))) return "no-row";
        return cdp.eval(`
          const el = document.querySelector(${lit(r("tint"))}).querySelector('input[type="color"]');
          Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, '#ff8000');
          el.dispatchEvent(new Event('input', { bubbles: true }));
          // 真的取色器关上时发 change：这一段选色就记成一条撤销
          el.dispatchEvent(new Event('change', { bubbles: true }));
          return 'ok';
        `);
      },
      want: [1, 128 / 255, 0],
    },
    {
      type: "transform",
      param: "pose",
      act: (cdp) => typeIn(cdp, r("pose"), "0.5"),
      want: [1, 0, 0, 0.5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    },
  ];
}

const approxEq = (a, b) =>
  Array.isArray(a) && Array.isArray(b)
    ? a.length === b.length && a.every((v, i) => approxEq(v, b[i]))
    : typeof a === "number" && typeof b === "number"
      ? Math.abs(a - b) < 1e-9
      : JSON.stringify(a) === JSON.stringify(b);

async function suiteAllTypes(cdp, report, ws) {
  report.section("P2 验收 10：14 种类型的控件都能改值，doc 里值与类型都对；transform / curve 存盘重开不变");
  await resetPanel(cdp);
  const ids = await showcaseGraph(cdp);
  await openPanel(cdp);
  // 高级组默认收起：展开它（flags、curve 之一在里面）；Write File 打开后 Export Path 才露出来
  await clickIn(cdp, `[data-testid="pp-group-n:${ids.show}|g:高级|a"]`);
  const wrote = await clickIn(cdp, rowSel(`${ids.show}.writeFile`), 'input[type="checkbox"]');
  mustOk(wrote === "ok", "打开 Write File（path 行的 visibleWhen）", wrote);
  await sleep(120);

  for (const c of typeCases(ids.show)) {
    if (c.reveal) await reveal(cdp, rowSel(`${ids.show}.${c.param}`));
    const how = await c.act(cdp);
    await sleep(120);
    const v = (await paramsOf(cdp, ids.show))[c.param];
    const typeOk =
      c.type === "int" || c.type === "flags"
        ? Number.isInteger(v)
        : c.type === "float"
          ? typeof v === "number"
          : c.type === "bool"
            ? typeof v === "boolean"
            : ["string", "text", "path", "enum"].includes(c.type)
              ? typeof v === "string"
              : Array.isArray(v) && v.every((x) => typeof x === "number");
    report.ok(`${c.type}：${c.param} 在面板里改了，doc 里是 ${JSON.stringify(c.want)}（类型对）`,
      how === "ok" && approxEq(v, c.want) && typeOk, `操作 ${how}，doc 里 ${JSON.stringify(v)}`);
  }

  // 打字之后按 Esc：撤回、不提交。以前两种都把打进去的提交了 —— Esc 先 setText 再 blur，
  // 同一个事件里 onBlur 拿到的还是这一帧打进去的 text
  for (const [param, kind, text] of [["iterations", "num", "77"], ["tag", "str", "zzz"]]) {
    await reveal(cdp, rowSel(`${ids.show}.${param}`));
    const before = { value: (await paramsOf(cdp, ids.show))[param], past: await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`) };
    const shown = await typeThenEscape(cdp, `${rowSel(`${ids.show}.${param}`)} input.ctl--${kind}`, text);
    const after = { value: (await paramsOf(cdp, ids.show))[param], past: await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`) };
    report.ok(`${param}：打了 ${text} 再按 Esc，值没变、没记撤销，框里回到原值`,
      approxEq(after.value, before.value) && after.past === before.past && shown === String(before.value),
      JSON.stringify({ before, after, shown }));
  }

  // 聚焦的数字框上滚滚轮：值不变（浏览器本来会一格一格改它 —— 点过一下框、再滚面板，参数就悄悄变了）
  {
    const numSel = `${rowSel(`${ids.show}.iterations`)} input.ctl--num`;
    await reveal(cdp, rowSel(`${ids.show}.iterations`));
    const before = (await paramsOf(cdp, ids.show)).iterations;
    const at = await cdp.eval(`
      const el = document.querySelector(${lit(numSel)});
      el.focus();
      const b = el.getBoundingClientRect();
      return { x: Math.round(b.left + b.width / 2), y: Math.round(b.top + b.height / 2) };
    `);
    for (let i = 0; i < 3; i += 1) {
      await cdp.send("Input.dispatchMouseEvent", { type: "mouseWheel", x: at.x, y: at.y, deltaX: 0, deltaY: 120 });
      await sleep(60);
    }
    await cdp.eval(`document.activeElement?.blur(); return true;`);
    await sleep(120);
    report.eq("聚焦的数字框上滚三下滚轮：值不变", (await paramsOf(cdp, ids.show)).iterations, before);
  }

  // 数字框里按 ↑：按这个参数的步长走一格（没声明 step 时是范围的 1/200），回车提交。浏览器自己的步进在没声明
  // step 的浮点框上是 ±1：weights 的 A 是 0.25，按一下成了 1.25（失焦再被夹成 1）
  {
    const wSel = `${rowSel(`${ids.show}.weights`)} input.ctl--num`;
    await reveal(cdp, rowSel(`${ids.show}.weights`));
    const before = (await paramsOf(cdp, ids.show)).weights[0];
    await cdp.eval(`document.querySelector(${lit(wSel)}).focus(); return true;`);
    await pressKey(cdp, "ArrowUp", 38);
    await sleep(80);
    const shown = await cdp.eval(`return document.querySelector(${lit(wSel)}).value;`);
    await pressKey(cdp, "Enter", 13);
    await sleep(150);
    const after = (await paramsOf(cdp, ids.show)).weights[0];
    report.ok("数字框里按 ↑ 走一格（weights 没声明 step：范围 0–1 的 1/200），回车提交",
      approxEq(Number(shown), before + 0.005) && approxEq(after, before + 0.005), JSON.stringify({ before, shown, after }));
  }

  // 下拉框里选了一项（焦点还在它上面）就按 Ctrl+Z：撤得掉。以前下拉框算输入框，Ctrl+Z 交给浏览器、什么也不发生
  {
    const modeSel = `${rowSel(`${ids.show}.mode`)} select`;
    await reveal(cdp, rowSel(`${ids.show}.mode`));
    const pastNow = () => cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
    const before = { mode: (await paramsOf(cdp, ids.show)).mode, past: await pastNow() };
    await cdp.eval(`document.querySelector(${lit(modeSel)}).focus(); return true;`);
    await pressKey(cdp, "ArrowDown", 40);
    await sleep(150);
    const chosen = { mode: (await paramsOf(cdp, ids.show)).mode, past: await pastNow() };
    await pressCtrl(cdp, "z");
    await sleep(200);
    const undone = { mode: (await paramsOf(cdp, ids.show)).mode, past: await pastNow(),
      shown: await cdp.eval(`return document.querySelector(${lit(modeSel)})?.value ?? null;`) };
    report.ok("下拉框里按 ↓ 换了一项，焦点还在上面就按 Ctrl+Z：撤掉、框里也回去",
      chosen.mode !== before.mode && chosen.past === before.past + 1 &&
        undone.mode === before.mode && undone.past === before.past && undone.shown === before.mode,
      JSON.stringify({ before, chosen, undone }));
  }

  // 取色器拖着选（一路发 input，关上时一个 change）：整段一条撤销，值是最后那个。以前每一下 input 都是一条
  {
    await reveal(cdp, rowSel(`${ids.show}.tint`));
    const pastNow = () => cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
    const p0 = await pastNow();
    await cdp.eval(`
      const el = document.querySelector(${lit(rowSel(`${ids.show}.tint`))}).querySelector('input[type="color"]');
      const set = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
      for (const c of ['#102030', '#203040', '#304050', '#405060', '#506070']) {
        set.call(el, c);
        el.dispatchEvent(new Event('input', { bubbles: true }));
        await new Promise((r) => setTimeout(r, 30));
      }
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return true;
    `);
    await sleep(100);
    const tint = (await paramsOf(cdp, ids.show)).tint;
    report.ok("取色器拖着选（五次 input 再关上）：只记一条撤销，值是最后那个",
      (await pastNow()) - p0 === 1 && approxEq(tint, [0x50 / 255, 0x60 / 255, 0x70 / 255]), JSON.stringify({ added: (await pastNow()) - p0, tint }));
  }

  // color 的 alpha（overlay 带 alpha 通道）
  await typeIn(cdp, rowSel(`${ids.show}.overlay`), "0.25", { tag: "input.ctl--num" });
  report.ok("color（带 alpha）：A 改成 0.25，其余三个分量不动",
    approxEq((await paramsOf(cdp, ids.show)).overlay, [1, 0.5, 0.1, 0.25]), JSON.stringify((await paramsOf(cdp, ids.show)).overlay));

  // transform：旋转 Z 改成 90°，再切到矩阵视图改 tz
  await typeIn(cdp, rowSel(`${ids.show}.pose`), "90", { index: 5 });
  let pose = (await paramsOf(cdp, ids.show)).pose;
  report.ok("transform：R z = 90° → 左上 3×3 是绕 Z 的 90°，平移还是 0.5",
    Array.isArray(pose) && pose.length === 16 && approxEq([pose[0], pose[1], pose[4], pose[5], pose[3]], [0, -1, 1, 0, 0.5]),
    JSON.stringify(pose));
  const summary = await cdp.eval(`return document.querySelector('[data-testid="transform-summary-pose"]')?.textContent ?? null;`);
  report.eq("transform：行上显示「T[x, y, z] R[rx, ry, rz]°」", summary, "T[0.5, 0, 0] R[0, 0, 90]°");
  await clickIn(cdp, rowSel(`${ids.show}.pose`), '[data-testid="transform-view-pose"]');
  const view = await cdp.eval(`return document.querySelector('[data-testid="transform-pose"]')?.getAttribute('data-view');`);
  mustOk(view === "matrix", "transform：切到 4×4 矩阵视图", view);
  await typeIn(cdp, rowSel(`${ids.show}.pose`), "0.2", { index: 11 });
  pose = (await paramsOf(cdp, ids.show)).pose;
  report.eq("transform：矩阵视图里改 m[11]（tz）", pose?.[11], 0.2);

  // curve：真鼠标拖第 2 个控制点；列表里改第 1 个点的 y；加一个点；换插值方式
  const ptSel = `[data-testid="curve-pt-response-1"]`;
  await reveal(cdp, rowSel(`${ids.show}.response`));
  const pt = await cdp.eval(`
    const b = document.querySelector(${lit(ptSel)}).getBoundingClientRect();
    return { x: Math.round(b.left + b.width / 2), y: Math.round(b.top + b.height / 2) };
  `);
  const undoBefore = (await snap(cdp)).undoLabel;
  await dragMouse(cdp, pt, { x: pt.x + 30, y: pt.y - 25 }, { steps: 8 });
  let curve = (await paramsOf(cdp, ids.show)).response;
  report.ok("curve：真鼠标拖第 2 个点 → x、y 都变大", curve && curve.points[1][0] > 0.5 && curve.points[1][1] > 0.35,
    JSON.stringify(curve));
  const afterDrag = await snap(cdp);
  report.eq("curve：一次拖动一条撤销记录", afterDrag.undoLabel === "拖动曲线控制点" && afterDrag.undoLabel !== undoBefore, true);
  await typeIn(cdp, rowSel(`${ids.show}.response`), "0.1", { index: 1 });
  curve = (await paramsOf(cdp, ids.show)).response;
  report.eq("curve：列表里改第 1 个点的 y", curve?.points?.[0], [0, 0.1]);
  await clickIn(cdp, rowSel(`${ids.show}.response`), '[data-testid="curve-add-response"]');
  curve = (await paramsOf(cdp, ids.show)).response;
  report.eq("curve：「+ 控制点」加一个", curve?.points?.length, 4);
  await chooseIn(cdp, rowSel(`${ids.show}.response`), "linear", '[data-testid="curve-interp-response"]');
  curve = (await paramsOf(cdp, ids.show)).response;
  report.eq("curve：换成线性插值", curve?.interp, "linear");
  // falloff（advanced 里的第二个 curve）：列表里改一个数
  await typeIn(cdp, rowSel(`${ids.show}.falloff`), "0.8", { index: 1 });
  report.eq("curve（advanced 组里的 falloff）：列表改值", (await paramsOf(cdp, ids.show)).falloff?.points?.[0], [0, 0.8]);

  // core 收下的就是这些值：跑一遍，echo 里原样回来
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "改完 14 种之后运行成功（core 接受每一种值）", run.status);
  const echo = await cdp.eval(`
    const n = window.__lyflow.stores.execution.getState().nodes.get(${lit(ids.show)});
    return n?.stats?.outputs?.find((o) => o.port === 'echo')?.value?.data ?? null;
  `);
  const now = await paramsOf(cdp, ids.show);
  report.ok("echo 回显的 pose、response 与 doc 一致", approxEq(echo?.params?.pose, now.pose) && JSON.stringify(echo?.params?.response) ===
    JSON.stringify({ interp: now.response.interp, points: now.response.points }),
    JSON.stringify({ pose: echo?.params?.pose, response: echo?.params?.response }));

  // 往返：存盘 → 读回来重开 → 值逐字不变
  const file = path.join(ws.dir, "P2 全类型往返.lyflow.json");
  await saveGraphTo(cdp, file);
  const disk = JSON.parse(fs.readFileSync(file, "utf8")).nodes.find((n) => n.id === ids.show).params;
  report.ok("磁盘上 transform 是 16 个数、curve 是对象", disk.pose?.length === 16 && typeof disk.response === "object",
    JSON.stringify({ pose: disk.pose, response: disk.response }));
  await cdp.eval(`
    const b = window.__lyflow;
    const loaded = await b.transport.loadGraph(${lit(file)});
    b.stores.graph.getState().newDoc();
    b.stores.graph.getState().loadDoc(loaded.doc, ${lit(file)});
    return true;
  `);
  await sleep(300);
  const reopened = await paramsOf(cdp, ids.show);
  report.eq("重开后 transform（pose）、curve（response、falloff）不变",
    { pose: reopened.pose, response: reopened.response, falloff: reopened.falloff },
    { pose: now.pose, response: now.response, falloff: now.falloff });
  const shownPose = await (async () => {
    await reveal(cdp, rowSel(`${ids.show}.pose`));
    return cdp.eval(`return document.querySelector('[data-testid="transform-summary-pose"]')?.textContent ?? null;`);
  })();
  const pts = await (async () => {
    await reveal(cdp, rowSel(`${ids.show}.response`));
    return cdp.eval(`return document.querySelector('[data-testid="curve-response"]')?.getAttribute('data-points');`);
  })();
  report.eq("重开后面板里的 transform 摘要不变、曲线还是 4 个点", { shownPose, pts }, { shownPose: "T[0.5, 0, 0.2] R[0, 0, 90]°", pts: "4" });
  await resetPanel(cdp);
}

// ------------------------------------------------------------ 验收 11

async function suiteConditions(cdp, report) {
  report.section("P2 验收 11：advanced 默认折叠；visibleWhen / enabledWhen 随另一个参数即时生效");
  await resetPanel(cdp);
  const ids = await showcaseGraph(cdp, { seed: 12 });
  await openPanel(cdp);
  const adv = `[data-testid="pp-group-n:${ids.show}|g:高级|a"]`;
  await reveal(cdp, adv);
  const state = () =>
    cdp.eval(`
      const q = (s) => document.querySelector(s);
      return {
        advOpen: q(${lit(adv)})?.getAttribute('data-open') ?? null,
        seed: !!q('[data-testid="prow-${ids.show}.seed"]'),
      };
    `);
  let s = await state();
  report.eq("advanced 组（高级）默认收起，里面的行不挂", [s.advOpen, s.seed], ["0", false]);
  await clickIn(cdp, adv);
  await sleep(120);
  s = await state();
  report.eq("点开标题：展开，行出来", [s.advOpen, s.seed], ["1", true]);

  const has = (param) => reveal(cdp, rowSel(`${ids.show}.${param}`)).then(Boolean);
  const enabled = async (param) => {
    await reveal(cdp, rowSel(`${ids.show}.${param}`));
    return cdp.eval(`
      const row = document.querySelector(${lit(rowSel(`${ids.show}.${param}`))});
      return row ? { attr: row.getAttribute('data-enabled'), disabled: row.querySelector('input, select')?.disabled ?? null } : null;
    `);
  };
  report.eq("visibleWhen：Mode = 快速时 Custom Factor 不在", await has("customFactor"), false);
  await chooseIn(cdp, rowSel(`${ids.show}.mode`), "custom");
  report.eq("Mode 改成自定义 → Custom Factor 立刻出现", await has("customFactor"), true);
  await chooseIn(cdp, rowSel(`${ids.show}.mode`), "fast");
  report.eq("改回快速 → 又藏起来", await has("customFactor"), false);

  report.eq("enabledWhen（eq）：Enabled 开着，Tolerance 可编辑", await enabled("tolerance"), { attr: "1", disabled: false });
  await clickIn(cdp, rowSel(`${ids.show}.enabled`), 'input[type="checkbox"]');
  report.eq("关掉 Enabled → Tolerance 立刻置灰", await enabled("tolerance"), { attr: "0", disabled: true });
  report.eq("enabledWhen（ne）：Mode = 快速时 Precision 置灰", await enabled("precision"), { attr: "0", disabled: true });
  await chooseIn(cdp, rowSel(`${ids.show}.mode`), "precise");
  report.eq("Mode 改成精确 → Precision 可编辑", await enabled("precision"), { attr: "1", disabled: false });
  await resetPanel(cdp);
}

// ------------------------------------------------------------ 验收 12

/** 期望值：照 lib/paramPanel.ts 写明的判据，在 Node 这边对着 doc + manifest 独立算一遍（只有顶层、没有子图）。 */
function expectedRows(doc, ops, validation) {
  const eq = (a, b) => JSON.stringify(canon(a)) === JSON.stringify(canon(b));
  const canon = (v) =>
    Array.isArray(v) ? v.map(canon)
      : v && typeof v === "object" ? Object.fromEntries(Object.keys(v).sort().map((k) => [k, canon(v[k])])) : v;
  const holds = (c, eff) => {
    if (!c) return true;
    const a = eff[c.param];
    if (c.eq !== undefined) return eq(a, c.eq);
    if (c.ne !== undefined) return !eq(a, c.ne);
    if (c.in !== undefined) return c.in.some((v) => eq(a, v));
    return true;
  };
  const bound = {};
  for (const [name, gp] of Object.entries(doc.params ?? {})) for (const b of gp.binds) bound[b] = name;
  const text = (v) => (typeof v === "string" ? v : JSON.stringify(v));
  const rows = [];
  for (const [name, gp] of Object.entries(doc.params ?? {})) {
    const [node, param] = [gp.binds[0].slice(0, gp.binds[0].lastIndexOf(".")), gp.binds[0].slice(gp.binds[0].lastIndexOf(".") + 1)];
    const decl = ops[doc.nodes.find((n) => n.id === node)?.op]?.params.find((p) => p.name === param);
    rows.push({
      key: `gp:${name}`, type: gp.type, modified: decl ? !eq(gp.default, decl.default) : false, recipe: true,
      diag: validation.graph.some((d) => d.paramPath === name),
      hay: [name, gp.label ?? "", gp.binds.join(" "), text(gp.default), "图参数"].join(" ").toLowerCase(),
    });
  }
  for (const node of doc.nodes) {
    const op = ops[node.op];
    const eff = {};
    for (const p of op.params) eff[p.name] = p.default;
    for (const [k, v] of Object.entries(node.params ?? {})) eff[k] = v;
    for (const p of op.params) {
      const g = bound[`${node.id}.${p.name}`];
      if (g) eff[p.name] = doc.params[g].default;
    }
    const title = node.ui?.title || op.label || node.id;
    for (const p of op.params) {
      if (!holds(p.visibleWhen, eff)) continue;
      const v = eff[p.name];
      rows.push({
        key: `${node.id}.${p.name}`, type: p.type, modified: !eq(v, p.default), recipe: Boolean(bound[`${node.id}.${p.name}`]),
        diag: (validation.byNode[node.id] ?? []).some((d) => d.paramPath === p.name),
        hay: [p.name, p.label ?? "", title, node.id, text(v)].join(" ").toLowerCase(),
      });
    }
  }
  return rows;
}

/** 把虚拟化列表从头滚到尾，收齐所有挂过的行的键（gp:名字 / 节点.参数）。
 *  expand：一路上把收起的组（advanced 默认收起）点开。 */
async function listedKeys(cdp, { expand = false } = {}) {
  return cdp.eval(`
    const frame = () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const list = document.querySelector('[data-testid="pp-list"]');
    const keys = new Set();
    list.scrollTop = 0;
    await frame();
    for (let i = 0; i < 400; i += 1) {
      if (${expand}) {
        const closed = list.querySelectorAll('[data-testid^="pp-group-"][data-open="0"]');
        for (const b of closed) b.click();
        if (closed.length > 0) await frame();
      }
      for (const el of list.querySelectorAll('.prow')) {
        const gp = el.getAttribute('data-graph-param');
        const id = el.getAttribute('data-testid');
        keys.add(id.startsWith('pp-gp-') ? 'gp:' + gp : id.slice('prow-'.length));
      }
      if (list.scrollTop + list.clientHeight >= list.scrollHeight - 1) break;
      list.scrollTop += Math.max(80, list.clientHeight * 0.7);
      await frame();
    }
    list.scrollTop = 0;
    return [...keys].sort();
  `);
}

async function suiteSearchFilter(cdp, report) {
  report.section("P2 验收 12：搜索与每个过滤 chip 的结果（期望值对着 doc 独立算）");
  await resetPanel(cdp);
  const ids = await showcaseGraph(cdp, {
    seed: 13,
    extra: [{ key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.03, 0.03, 0.03] }, row: 1 }],
  });
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.connect({ node: ${lit(ids.gen)}, port: 'cloud' }, { node: ${lit(ids.voxel)}, port: 'cloud' });
    const s = window.__lyflow.stores.graph.getState();
    s.setParam(${lit(ids.show)}, 'tag', 'demo-tag');
    s.setParam(${lit(ids.show)}, 'mode', 'custom');
    s.setParam(${lit(ids.show)}, 'iterations', 5000);   // 越过 max 1000：一条诊断
    s.renameNode(${lit(ids.voxel)}, '体素滤波');
    return true;
  `);
  await openPanel(cdp);
  // 纳入配方：点 show.gain 行右端的空心书签
  const marked = await clickIn(cdp, rowSel(`${ids.show}.gain`), `[data-testid="pp-bookmark-${ids.show}.gain"]`);
  mustOk(marked === "ok", "点书签把 show.gain 纳入配方", marked);
  const gp = (await docOf(cdp)).params?.gain;
  const bookmark = await cdp.eval(`return document.querySelector('[data-testid="pp-bookmark-${ids.show}.gain"]')?.getAttribute('data-on');`);
  report.ok("书签：doc 里出现了图参数 gain，绑着 show.gain；书签变成实心（已纳入）", gp?.binds?.[0] === `${ids.show}.gain` && bookmark === "1",
    JSON.stringify({ gp, bookmark }));
  await cdp.eval(`await window.__lyflow.validate(); return true;`);
  await sleep(250);

  const { doc, ops, validation } = await cdp.eval(`
    const b = window.__lyflow;
    const doc = b.stores.graph.getState().doc;
    const m = b.stores.manifest.getState().operatorsById;
    const ops = {};
    for (const n of doc.nodes) ops[n.op] = m.get(n.op);
    const v = b.stores.validation.getState();
    return { doc, ops, validation: { byNode: Object.fromEntries(v.byNode), graph: [...v.graphLevel] } };
  `);
  const rows = expectedRows(doc, ops, validation);
  const want = (pred) => rows.filter(pred).map((r) => r.key).sort();
  report.ok("有诊断的那一行真的有（iterations 越界）", want((r) => r.diag).includes(`${ids.show}.iterations`),
    JSON.stringify(validation.byNode[ids.show]));

  const chipCount = () =>
    cdp.eval(`return Object.fromEntries(['all','modified','recipe','diag'].map((c) =>
      [c, Number(document.querySelector('[data-testid="pp-chip-' + c + '"]').getAttribute('data-count'))]));`);
  const counts = await chipCount();
  report.eq("chip 计数：全部 / 已改动 / 配方 / 诊断", counts, {
    all: rows.length,
    modified: rows.filter((r) => r.modified).length,
    recipe: rows.filter((r) => r.recipe).length,
    diag: rows.filter((r) => r.diag).length,
  });

  const setChip = (c) => cdp.eval(`document.querySelector('[data-testid="pp-chip-${c}"]').click(); return true;`).then(() => sleep(150));
  const setQuery = (q) =>
    cdp.eval(`
      const el = document.querySelector('[data-testid="pp-search"]');
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, ${lit(q)});
      el.dispatchEvent(new Event('input', { bubbles: true }));
      return true;
    `).then(() => sleep(150));

  // 「全部」：过滤没生效时 advanced 组是收起的，一路滚一路点开，再逐行收齐
  report.eq("「全部」列出的行 = 期望（advanced 展开后）", await listedKeys(cdp, { expand: true }), want(() => true));
  // 「已改动」「配方」两个 chip 的列表不再逐个比：计数在上面比过，列表判据与「诊断」同一条路，下面还有「类型叠已改动」
  await setChip("diag");
  report.eq("chip「diag」的结果 = 期望", await listedKeys(cdp), want((r) => r.diag));
  await setChip("all");

  // 两个搜索词：多词 AND（跨节点标题与参数名）、大小写不敏感
  const searchCounts = { got: {}, want: {} };
  for (const q of ["参数全类型 iter", "LEAF"]) {
    await setQuery(q);
    const terms = q.toLowerCase().split(/\s+/).filter(Boolean);
    const expect = want((r) => terms.every((t) => r.hay.includes(t)));
    report.eq(`搜索「${q}」的结果 = 期望（${expect.length} 行）`, await listedKeys(cdp), expect);
    searchCounts.got[q] = (await chipCount()).all;
    searchCounts.want[q] = expect.length;
  }
  report.eq("搜索时「全部」计数跟着变", searchCounts.got, searchCounts.want);
  await setQuery("");

  // 按类型：选 vec3f，再叠「已改动」
  await cdp.eval(`
    const el = document.querySelector('[data-testid="pp-type"]');
    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(el, 'vec3f');
    el.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);
  await sleep(150);
  await setChip("modified");
  report.eq("类型 vec3f + 已改动 = 期望", await listedKeys(cdp), want((r) => r.type === "vec3f" && r.modified));
  const typeOpt = await cdp.eval(`
    return [...document.querySelectorAll('[data-testid="pp-type"] option')].find((o) => o.value === 'vec3f')?.textContent ?? null;
  `);
  report.eq("「类型 ▾」下拉里的计数（叠着当前 chip）", typeOpt, `vec3f (${want((r) => r.type === "vec3f" && r.modified).length})`);

  // 诊断贴在对应的行下
  await setChip("diag");
  await cdp.eval(`
    const el = document.querySelector('[data-testid="pp-type"]');
    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(el, '');
    el.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);
  await sleep(150);
  await reveal(cdp, rowSel(`${ids.show}.iterations`));
  const diagText = await cdp.eval(`return document.querySelector(${lit(rowSel(`${ids.show}.iterations`))})?.querySelector('.pp-diag')?.textContent ?? null;`);
  report.ok("诊断贴在 iterations 这一行下（P2.8）", (diagText ?? "").includes("1000"), diagText);

  // 图参数的诊断（nodeId 为空）贴在「图参数」分组那一行下：把 gain 的 default 设成越过它自己的 max
  await cdp.eval(`window.__lyflow.stores.graph.getState().setGraphParamDefault('gain', 50); await window.__lyflow.validate(); return true;`);
  await sleep(250);
  await reveal(cdp, `[data-testid="pp-gp-gain"]`);
  const gpDiag = await cdp.eval(`
    const row = document.querySelector('[data-testid="pp-gp-gain"]');
    return { diag: row?.getAttribute('data-diag'), text: row?.querySelector('.pp-diag')?.textContent ?? null };
  `);
  report.ok("图参数自己的诊断贴在「图参数」分组那一行下", Number(gpDiag.diag) >= 1 && (gpDiag.text ?? "").includes("10"),
    JSON.stringify(gpDiag));
  // 规格（⚙）里改名字再按 Esc：撤回，图参数不改名（以前改了，一条撤销）
  await cdp.eval(`document.querySelector('[data-testid="pp-spec-gain"]')?.click(); return true;`);
  await sleep(150);
  const specShown = await typeThenEscape(cdp, '[data-testid="pp-spec-gain-name"]', "renamedGain");
  report.ok("图参数规格里改名字再按 Esc：不改名，框里回到 gain",
    specShown === "gain" && (await cdp.eval(`return Object.keys(window.__lyflow.stores.graph.getState().doc.params ?? {});`)).includes("gain"),
    JSON.stringify(specShown));
  await cdp.eval(`document.querySelector('[data-testid="pp-spec-gain"]')?.click(); return true;`);
  await setChip("all");
  await resetPanel(cdp);
}

// ------------------------------------------------------------ 验收 13

async function suiteSubgraphShared(cdp, report, ws) {
  report.section("P2 验收 13：子图定义行标「N 个实例共享」，改它两个实例都变；库算子内部只读");
  await resetPanel(cdp);
  const example = JSON.parse(fs.readFileSync(path.join(ROOT, "examples", "param-showcase.lyflow.json"), "utf8"));
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.newDoc();
    window.__lyflow.stores.graph.getState().loadDoc(${lit(example)}, null);
    return true;
  `);
  await sleep(300);
  const first = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(first.status === "ok", "全类型示例图（examples/param-showcase.lyflow.json）跑通", first.status);
  const planKeys = async () => {
    await cdp.eval(`await window.__lyflow.plan(); return true;`);
    return cdp.eval(`return Object.fromEntries([...window.__lyflow.stores.cache.getState().plan].map(([id, n]) => [id, n.cacheKey]));`);
  };
  const keys0 = await planKeys();
  await openPanel(cdp);

  const defA = `[data-testid="pp-def-n_pre_a"]`;
  mustOk(Boolean(await reveal(cdp, defA)), "子图实例下面有「子图定义」一栏");
  const defText = await cdp.eval(`return document.querySelector(${lit(defA)})?.textContent ?? null;`);
  report.ok("标着「子图定义 · 2 个实例共享」", (defText ?? "").includes("子图定义 · 2 个实例共享"), defText);
  await clickIn(cdp, defA);
  const badge = await (async () => {
    await reveal(cdp, `[data-testid="pp-shared-n_pre_a/s_voxel"]`);
    return cdp.eval(`return document.querySelector('[data-testid="pp-shared-n_pre_a/s_voxel"]')?.textContent ?? null;`);
  })();
  const shared = await cdp.eval(`return document.querySelector('[data-testid="prow-n_pre_a/s_voxel.leafSize"]')?.getAttribute('data-shared') ?? null;`);
  report.eq("定义里的节点标题旁有共享标记、定义里的行也标着共享（data-shared = 2）", { badge, shared },
    { badge: "子图定义 · 2 个实例共享", shared: "2" });

  const typedDef = await typeIn(cdp, rowSel("n_pre_a/s_voxel.leafSize"), "0.03");
  mustOk(typedDef === "ok", "在实例 A 下面改定义里的 leafSize（x → 0.03）", typedDef);
  const def = (await docOf(cdp)).subgraphs.sg_pre.nodes[0].params;
  report.eq("doc 里改的是子图定义本身", def.leafSize, [0.03, 0.01, 0.01]);
  await clickIn(cdp, `[data-testid="pp-def-n_pre_b"]`);
  await reveal(cdp, rowSel("n_pre_b/s_voxel.leafSize"));
  const shownB = await cdp.eval(`return [...document.querySelector('[data-testid="prow-n_pre_b/s_voxel.leafSize"]').querySelectorAll('input')].map((i) => i.value);`);
  report.eq("实例 B 下面同一行也显示 0.03（有效值跟着变）", shownB, ["0.03", "0.01", "0.01"]);
  const keys1 = await planKeys();
  report.eq("两个实例里的 voxel 的 cacheKey 都变了（core 看到的有效值都变）",
    [keys1["n_pre_a/s_voxel"] !== keys0["n_pre_a/s_voxel"], keys1["n_pre_b/s_voxel"] !== keys0["n_pre_b/s_voxel"]], [true, true]);
  const run = await runAndWait(cdp, () => pressF5(cdp));
  // 结果仓按内容寻址：同一个 leafSize 以前跑过的话是 skipped（命中缓存），也算「按新值出的结果」
  const a = run.nodes["n_pre_a/s_voxel"];
  const b = run.nodes["n_pre_b/s_voxel"];
  const a0 = first.nodes["n_pre_a/s_voxel"];
  report.ok("两个实例都按新的 leafSize 出了结果（点数相同、且与改之前不同）",
    ["done", "skipped"].includes(a?.state) && ["done", "skipped"].includes(b?.state) &&
      a.elementCount === b.elementCount && a.elementCount !== a0?.elementCount,
    JSON.stringify({ a, b, before: a0 }));

  // 库算子：把子图存成库、放进画布 —— 面板里标「内部只读」，没有「子图定义」一栏可展开
  const libId = `e2e_p2_${Date.now().toString(36)}`;
  const lib = await cdp.eval(`
    const b = window.__lyflow;
    await b.transport.saveAsLibrary(b.stores.graph.getState().doc, 'sg_pre', { id: ${lit(libId)}, category: 'E2E' });
    const r = await b.transport.refreshLibrary();
    b.stores.manifest.getState().replaceBundle(r.manifest, 0);
    const id = b.stores.graph.getState().addNode(${lit("lib." + libId)}, { x: 300, y: 420 });
    return { id, dirs: r.status.dirs };
  `);
  const libHead = await reveal(cdp, `[data-testid="pp-node-${lib.id}"]`);
  const libInfo = await cdp.eval(`
    return {
      badge: document.querySelector('[data-testid="pp-library-${lib.id}"]')?.textContent ?? null,
      def: !!document.querySelector('[data-testid="pp-def-${lib.id}"]'),
      inner: document.querySelectorAll('[data-node^="${lib.id}/"]').length,
    };
  `);
  mustOk(Boolean(libHead), "面板里有库算子那一节", lib);
  report.eq("库算子放进了画布：标「库算子 · 内部只读」、没有定义可展开、没有内部行", { placed: Boolean(lib?.id), ...libInfo },
    { placed: true, badge: "库算子 · 内部只读", def: false, inner: 0 });
  const libFile = lib.dirs?.[0] ? path.join(lib.dirs[0], `${libId}.lyflow-op.json`) : null;
  if (libFile && fs.existsSync(libFile)) {
    fs.rmSync(libFile, { force: true });
    await cdp.eval(`
      const r = await window.__lyflow.transport.refreshLibrary();
      window.__lyflow.stores.manifest.getState().replaceBundle(r.manifest, 0);
      return true;
    `);
  }
  void ws;
  await resetPanel(cdp);
}

// ------------------------------------------------------------ 验收 14

async function suiteRoi(cdp, report) {
  report.section("P2 验收 14：ROI 缩略图 →「拖框」进 2D 拖框视图，拖动后参数变，一次撤销还原");
  await resetPanel(cdp);
  const ids = await showcaseGraph(cdp, { seed: 14 });
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "先跑一遍（2D 视图要一片底图）", run.status);
  await openPanel(cdp);
  const key = `${ids.show}.roi`;
  await reveal(cdp, rowSel(key));
  const thumb = await cdp.eval(`
    const t = document.querySelector('[data-testid="pp-roi-thumb-${key}"]');
    return t ? { boxes: t.querySelectorAll('.prow__thumb-box').length, current: t.querySelectorAll('.prow__thumb-box.is-current').length } : null;
  `);
  report.eq("ROI 行有缩略图，当前这个框高亮", thumb, { boxes: 1, current: 1 });
  const viewerBefore = await cdp.eval(`return document.querySelector('.app__viewer')?.classList.contains('is-collapsed');`);
  report.eq("面板开着时 3D 视图默认收成标题栏", viewerBefore, true);

  await cdp.eval(`document.querySelector('[data-testid="pp-roi-edit-${key}"]').click(); return true;`);
  await cdp.waitFor(`document.querySelector('[data-testid="roi-box-roi"]') !== null`, { what: "2D 拖框视图里出现 ROI 框", timeoutMs: 15_000 });
  await sleep(600);
  const entered = await cdp.eval(`
    const v = document.querySelector('.viewer');
    return { collapsed: document.querySelector('.app__viewer').classList.contains('is-collapsed'),
             camera: v.getAttribute('data-camera'), node: v.getAttribute('data-node'),
             editing: Number(v.getAttribute('data-roi-edit')),
             selected: [...window.__lyflow.stores.ui.getState().selectedNodes] };
  `);
  report.eq("点「拖框」：视图展开、相机切 2D、选中这个节点、进入拖框", entered,
    { collapsed: false, camera: "2d", node: ids.show, editing: 1, selected: [ids.show] });
  // 视图要先把这个节点的云取回来（首次取要经 IPC 拿几千个点），缩略图才换成底图的范围
  const viewerCloud = await cdp
    .waitFor(`document.querySelector('.viewer')?.getAttribute('data-view') === 'cloud' &&
              document.querySelector('.viewer')?.getAttribute('data-node') === ${lit(ids.show)}`,
      { what: "3D 视图显示出这个节点的云", timeoutMs: 20_000 })
    .then(() => true, () => false);
  const thumbFromCloud = await cdp
    .waitFor(`document.querySelector('[data-testid="pp-roi-thumb-${key}"]')?.getAttribute('data-from-cloud') === '1'`,
      { what: "缩略图换成底图范围", timeoutMs: 5_000 })
    .then(() => "1", async () =>
      cdp.eval(`return document.querySelector('[data-testid="pp-roi-thumb-${key}"]')?.getAttribute('data-from-cloud') + ' / view=' +
        document.querySelector('.viewer')?.getAttribute('data-view') + ' node=' + document.querySelector('.viewer')?.getAttribute('data-node') +
        ' status=' + document.querySelector('[data-testid="viewer3d-status"]')?.textContent +
        ' state=' + window.__lyflow.stores.execution.getState().nodes.get(${lit(ids.show)})?.state;`));
  report.eq("视图取过云之后缩略图按底图范围画", { viewerCloud, thumbFromCloud }, { viewerCloud: true, thumbFromCloud: "1" });

  const before = (await paramsOf(cdp, ids.show)).roi;
  const box = await cdp.eval(`
    const b = document.querySelector('[data-testid="roi-box-roi"]').getBoundingClientRect();
    return { x: Math.round(b.left + b.width / 2), y: Math.round(b.top + b.height / 2), w: b.width };
  `);
  await dragMouse(cdp, { x: box.x, y: box.y }, { x: box.x + 30, y: box.y }, { steps: 10 });
  const after = (await paramsOf(cdp, ids.show)).roi;
  report.ok("真鼠标拖框身向右：xMin 与 xMax 都变大、宽度不变",
    Array.isArray(after) && after[0] > (before?.[0] ?? -0.3) && after[2] > (before?.[2] ?? 0.3) &&
      Math.abs((after[2] - after[0]) - 0.6) < 1e-6, `${JSON.stringify(before)} → ${JSON.stringify(after)}`);
  const shown = await cdp.eval(`
    const row = document.querySelector(${lit(rowSel(key))});
    return row ? [...row.querySelectorAll('input')].map((i) => Number(i.value)) : null;
  `);
  report.ok("面板那一行的数跟着变", Array.isArray(shown) && Math.abs(shown[0] - after[0]) < 1e-9, JSON.stringify(shown));
  const dragLabel = (await snap(cdp)).undoLabel;
  await undoByKey(cdp);
  report.eq("撤销记录是一次拖动（「拖动 ROI」），一次 Ctrl+Z 还原（显式值删掉 = 回到默认值）",
    { undoLabel: dragLabel, roi: (await paramsOf(cdp, ids.show)).roi }, { undoLabel: "拖动 ROI", roi: before });
  await resetPanel(cdp);
}

// ------------------------------------------------------------ 验收 15

/** 脚本生成一张 1000 参数的图：50 个 test.param_showcase，每个 20 个可见参数（22 个里两个被 visibleWhen 藏着），
 *  值各不相同。写成文件再按「打开文件」的路径读进来。 */
function bigGraph(dir) {
  const nodes = [];
  for (let i = 0; i < 50; i += 1) {
    nodes.push({
      id: `n_big_${i}`,
      op: SHOW,
      opVersion: "1.0.0",
      params: { iterations: 10 + i, gain: 0.5 + (i % 7) * 0.1, tag: `node-${i}`, scale: [1, 1 + i / 100, 1] },
      ui: { position: { x: (i % 10) * 260, y: Math.floor(i / 10) * 180 }, title: `示例 ${i}` },
    });
  }
  const doc = { schemaVersion: 1, id: "01JPARAMPERF00000000000000", name: "P2 性能 · 1000 参数", nodes, edges: [], groups: [], subgraphs: {}, x: {} };
  const file = path.join(dir, "P2 性能 1000 参数.lyflow.json");
  fs.writeFileSync(file, JSON.stringify(doc, null, 2));
  return file;
}

async function suitePerf(cdp, report, ws) {
  report.section("P2 验收 15：1000 参数的图 —— 打开面板 < 300 ms、滚动 ≥ 50 fps、输入到画布状态 < 100 ms");
  await resetPanel(cdp);
  const file = bigGraph(ws.dir);
  await cdp.eval(`
    const b = window.__lyflow;
    const loaded = await b.transport.loadGraph(${lit(file)});
    b.stores.graph.getState().newDoc();
    b.stores.graph.getState().loadDoc(loaded.doc, ${lit(file)});
    b.stores.ui.getState().clearSelection();
    return true;
  `);
  await sleep(800);
  const total = await cdp.eval(`
    const ops = window.__lyflow.stores.manifest.getState().operatorsById;
    let n = 0;
    for (const node of window.__lyflow.stores.graph.getState().doc.nodes) {
      const op = ops.get(node.op);
      n += op.params.filter((p) => !p.visibleWhen).length;
    }
    return n;
  `);

  // 打开：从 toggle 到第一批行画出来（两帧：提交 + 绘制）
  const openMs = await cdp.eval(`
    const frame = () => new Promise((r) => requestAnimationFrame(r));
    const t0 = performance.now();
    window.__lyflow.stores.ui.getState().toggleParamPanel(true);
    for (let i = 0; i < 300; i += 1) {
      await frame();
      if (document.querySelectorAll('[data-testid="pp-list"] .prow').length > 0) break;
    }
    await frame();
    return Math.round(performance.now() - t0);
  `);
  report.ok(`打开面板 ${openMs} ms < 300 ms`, openMs < 300, `${openMs} ms`);
  const virt = await cdp.eval(`
    const l = document.querySelector('[data-testid="pp-list"]');
    return { total: Number(l.getAttribute('data-total')), mounted: Number(l.getAttribute('data-mounted')),
             chipAll: Number(document.querySelector('[data-testid="pp-chip-all"]').getAttribute('data-count')) };
  `);
  report.ok(`图里 ${total} 个可见参数（≥ 1000），「全部」计数 = 可见参数数，列表是虚拟化的：只挂了一小部分`,
    total >= 1000 && virt.chipAll === total && virt.mounted > 0 && virt.mounted < 120 && virt.total > 500,
    JSON.stringify({ total, ...virt }));

  // 滚动帧率：rAF 采样 + 真鼠标滚轮一路往下滚
  await cdp.eval(`
    window.__ppfps = { frames: [], last: performance.now(), stop: false };
    const tick = () => {
      const now = performance.now();
      window.__ppfps.frames.push(now - window.__ppfps.last);
      window.__ppfps.last = now;
      if (!window.__ppfps.stop) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    return true;
  `);
  const at = await cdp.eval(`
    const b = document.querySelector('[data-testid="pp-list"]').getBoundingClientRect();
    return { x: Math.round(b.left + b.width / 2), y: Math.round(b.top + b.height / 2) };
  `);
  const scrollBefore = await cdp.eval(`return document.querySelector('[data-testid="pp-list"]').scrollTop;`);
  for (let i = 0; i < 60; i += 1) {
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseWheel", x: at.x, y: at.y, deltaX: 0, deltaY: 160 });
    await sleep(16);
  }
  await sleep(200);
  const fps = await cdp.eval(`
    window.__ppfps.stop = true;
    const f = window.__ppfps.frames.slice(3).sort((a, b) => a - b);
    if (f.length === 0) return null;
    return Math.round(1000 / f[Math.floor(f.length / 2)]);
  `);
  const scrollAfter = await cdp.eval(`return document.querySelector('[data-testid="pp-list"]').scrollTop;`);
  report.ok(`滚轮真的把列表滚下去了，滚动时的帧率 ${fps} fps ≥ 50`, scrollAfter > scrollBefore + 2000 && fps != null && fps >= 50,
    `滚动 ${scrollBefore} → ${scrollAfter}，${fps} fps`);

  // 输入一个数 → 画布状态（graph store 的 doc）更新并画完一帧
  const target = await cdp.eval(`
    const row = document.querySelector('[data-testid="pp-list"] .prow[data-type="float"][data-enabled="1"]');
    return row ? { testid: row.getAttribute('data-testid'), node: row.getAttribute('data-node'), param: row.getAttribute('data-param') } : null;
  `);
  mustOk(Boolean(target), "找到一个挂着的 float 行", target);
  const inputMs = await cdp.eval(`
    const row = document.querySelector('[data-testid="${target?.testid}"]');
    const input = row.querySelector('input');
    const store = window.__lyflow.stores.graph;
    const before = store.getState().doc;
    input.focus();
    const t0 = performance.now();
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, '0.777');
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.blur();
    for (let i = 0; i < 60 && store.getState().doc === before; i += 1) await new Promise((r) => setTimeout(r, 1));
    await new Promise((r) => requestAnimationFrame(() => r()));
    return Math.round(performance.now() - t0);
  `);
  const v = (await paramsOf(cdp, target.node))[target.param];
  report.ok(`输入的值进了 doc，输入一个数到画布状态更新 ${inputMs} ms < 100 ms`, v === 0.777 && inputMs < 100,
    `doc 里 ${JSON.stringify(v)}，${inputMs} ms`);
  await resetPanel(cdp);
  await newDoc(cdp);
}

export const paramsP2Suites = [
  suiteLayout,
  suiteAllTypes,
  suiteConditions,
  suiteSearchFilter,
  suiteSubgraphShared,
  suiteRoi,
  suitePerf,
];
