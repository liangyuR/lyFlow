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
  newDoc,
  placeAtScreen,
  pressCtrl,
  pressKey,
  pressF5,
  runAndWait,
  saveGraphTo,
} from "./page.mjs";

const SHOW = "test.param_showcase";
const PANEL_WIDTH_KEY = "lyflow.paramPanel.width";

// ------------------------------------------------------------ 页面侧的小工具

const docOf = (cdp) => cdp.eval(`return window.__lyflow.stores.graph.getState().doc;`);
const snap = (cdp) => cdp.eval(`return window.__lyflow.snapshot();`);
const paramsOf = async (cdp, id) => (await docOf(cdp)).nodes.find((n) => n.id === id)?.params ?? {};

async function panelState(cdp) {
  return cdp.eval(`return window.__lyflow.stores.ui.getState().paramPanel;`);
}

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

/** 虚拟化列表里把某一项滚到挂上为止：从顶上开始一屏一屏往下翻，找到就滚进视口中间。
 *  返回它在屏幕上的矩形；翻到底都没有返回 null。 */
async function reveal(cdp, selector) {
  return cdp.eval(`
    const frame = () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const list = document.querySelector('[data-testid="pp-list"]');
    if (!list) return null;
    const find = () => document.querySelector(${lit(selector)});
    let el = find();
    if (!el) {
      list.scrollTop = 0;
      await frame();
      for (let i = 0; i < 400 && !(el = find()); i += 1) {
        if (list.scrollTop + list.clientHeight >= list.scrollHeight - 1) break;
        list.scrollTop += Math.max(80, list.clientHeight * 0.7);
        await frame();
      }
    }
    if (!el) return null;
    const lr = list.getBoundingClientRect();
    const r0 = el.getBoundingClientRect();
    if (r0.top < lr.top + 40 || r0.bottom > lr.bottom - 40) {
      list.scrollTop += r0.top - lr.top - lr.height / 3;
      await frame();
    }
    const r = find()?.getBoundingClientRect();
    return r ? { x: r.left, y: r.top, w: r.width, h: r.height } : null;
  `);
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
  report.ok("test.param_showcase 在 e2e 构建里注册了（LYFLOW_TEST_OPS=1）", hasOp);

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
  report.eq("起始：面板关着、Inspector 在", [d.panel, d.inspector], [false, true]);

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
  await openPanel(cdp);
  d = await dom();
  report.ok("重新打开：还是拖过的宽度（记住宽度）", Math.abs(d.right.w - width1) <= 2, `${d.right.w} vs ${width1}`);

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
  report.eq("store 里 maximized = true", (await panelState(cdp)).maximized, true);
  await cdp.eval(`document.querySelector('[data-testid="pp-maximize"]').click(); return true;`);
  await sleep(250);
  d = await dom();
  report.ok("还原：画布回来", d.canvas.w > 300, `${d.canvas.w}px`);
  report.ok("还原：面板回到记住的宽度", Math.abs(d.right.w - width1) <= 2, `${d.right.w} vs ${width1}`);

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
  report.ok("s5 那一节找得到", Boolean(s5));
  await cdp.eval(`document.querySelector('[data-testid="pp-node-title-${ids.s5}"]').click(); return true;`);
  await sleep(700);
  const centered = await cdp.eval(`
    const c = document.querySelector('.app__canvas').getBoundingClientRect();
    const n = document.querySelector('[data-testid="node-${ids.s5}"]').getBoundingClientRect();
    return { selected: [...window.__lyflow.stores.ui.getState().selectedNodes],
             dx: Math.round(n.left + n.width / 2 - (c.left + c.width / 2)),
             dy: Math.round(n.top + n.height / 2 - (c.top + c.height / 2)) };
  `);
  report.eq("面板里点标题 → 画布选中 s5", centered.selected, [ids.s5]);
  report.ok("…并把它居中（中心偏差 ≤ 12 px）", Math.abs(centered.dx) <= 12 && Math.abs(centered.dy) <= 12,
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
  report.eq("打开 Write File（path 行的 visibleWhen）", await clickIn(cdp, rowSel(`${ids.show}.writeFile`), 'input[type="checkbox"]'), "ok");
  await sleep(120);

  const seen = new Set();
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
    seen.add(c.type);
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
  report.eq("transform：切到 4×4 矩阵视图", view, "matrix");
  await typeIn(cdp, rowSel(`${ids.show}.pose`), "0.2", { index: 11 });
  pose = (await paramsOf(cdp, ids.show)).pose;
  report.eq("transform：矩阵视图里改 m[11]（tz）", pose?.[11], 0.2);
  seen.add("vec4f"); // roi 也是 vec4f，验收 14 另拖一次

  // curve：真鼠标拖第 2 个控制点；列表里改第 1 个点的 y；加一个点；换插值方式
  const before = (await paramsOf(cdp, ids.show)).response;
  report.eq("curve：没改过时 doc 里没有这个键（稀疏存储）", before, undefined);
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
  const shape = curve && Array.isArray(curve.points) && curve.points.every((p, i, a) =>
    p.length === 2 && p[0] >= 0 && p[0] <= 1 && (i === 0 || p[0] > a[i - 1][0]));
  report.ok("curve：值的形状合规（x 在 0–1 严格递增）", shape, JSON.stringify(curve));
  // falloff（advanced 里的第二个 curve）：列表里改一个数
  await typeIn(cdp, rowSel(`${ids.show}.falloff`), "0.8", { index: 1 });
  report.eq("curve（advanced 组里的 falloff）：列表改值", (await paramsOf(cdp, ids.show)).falloff?.points?.[0], [0, 0.8]);
  seen.add("curve");
  report.eq("14 种类型都改到了", [...seen].sort(), ["bool", "color", "curve", "enum", "flags", "float", "int", "path",
    "string", "text", "transform", "vec2f", "vec3f", "vec4f"]);

  // core 收下的就是这些值：跑一遍，echo 里原样回来
  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("改完 14 种之后运行成功（core 接受每一种值）", run.status, "ok");
  const echo = await cdp.eval(`
    const n = window.__lyflow.stores.execution.getState().nodes.get(${lit(ids.show)});
    return n?.stats?.outputs?.find((o) => o.port === 'echo')?.value?.data ?? null;
  `);
  const now = await paramsOf(cdp, ids.show);
  report.ok("echo 回显的 pose 与 doc 一致", approxEq(echo?.params?.pose, now.pose), JSON.stringify(echo?.params?.pose));
  report.ok("echo 回显的 response 与 doc 一致", JSON.stringify(echo?.params?.response) ===
    JSON.stringify({ interp: now.response.interp, points: now.response.points }), JSON.stringify(echo?.params?.response));

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
  report.eq("重开后 transform 不变", reopened.pose, now.pose);
  report.eq("重开后 curve 不变", reopened.response, now.response);
  report.eq("重开后 falloff 不变", reopened.falloff, now.falloff);
  const shownPose = await (async () => {
    await reveal(cdp, rowSel(`${ids.show}.pose`));
    return cdp.eval(`return document.querySelector('[data-testid="transform-summary-pose"]')?.textContent ?? null;`);
  })();
  report.eq("重开后面板里的 transform 摘要不变", shownPose, "T[0.5, 0, 0.2] R[0, 0, 90]°");
  const pts = await (async () => {
    await reveal(cdp, rowSel(`${ids.show}.response`));
    return cdp.eval(`return document.querySelector('[data-testid="curve-response"]')?.getAttribute('data-points');`);
  })();
  report.eq("重开后面板里的曲线还是 4 个点", pts, "4");
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
  report.eq("点书签把 show.gain 纳入配方", await clickIn(cdp, rowSel(`${ids.show}.gain`), `[data-testid="pp-bookmark-${ids.show}.gain"]`), "ok");
  const gp = (await docOf(cdp)).params?.gain;
  report.ok("书签：doc 里出现了图参数 gain，绑着 show.gain", gp?.binds?.[0] === `${ids.show}.gain`, JSON.stringify(gp));
  const bookmark = await cdp.eval(`return document.querySelector('[data-testid="pp-bookmark-${ids.show}.gain"]')?.getAttribute('data-on');`);
  report.eq("书签变成实心（已纳入）", bookmark, "1");
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
  for (const [chip, pred] of [["modified", (r) => r.modified], ["recipe", (r) => r.recipe], ["diag", (r) => r.diag]]) {
    await setChip(chip);
    report.eq(`chip「${chip}」的结果 = 期望`, await listedKeys(cdp), want(pred));
  }
  await setChip("all");

  for (const q of ["gain", "体素", "demo-tag", "参数全类型 iter", "LEAF"]) {
    await setQuery(q);
    const terms = q.toLowerCase().split(/\s+/).filter(Boolean);
    const expect = want((r) => terms.every((t) => r.hay.includes(t)));
    report.eq(`搜索「${q}」的结果 = 期望（${expect.length} 行）`, await listedKeys(cdp), expect);
    const c = await chipCount();
    report.eq(`搜索「${q}」时「全部」计数跟着变`, c.all, expect.length);
  }
  await setQuery("");

  // 按类型：选 vec3f，再叠「已改动」
  await cdp.eval(`
    const el = document.querySelector('[data-testid="pp-type"]');
    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(el, 'vec3f');
    el.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);
  await sleep(150);
  report.eq("按类型 vec3f 的结果 = 期望", await listedKeys(cdp), want((r) => r.type === "vec3f"));
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
  report.eq("全类型示例图（examples/param-showcase.lyflow.json）跑通", first.status, "ok");
  const planKeys = async () => {
    await cdp.eval(`await window.__lyflow.plan(); return true;`);
    return cdp.eval(`return Object.fromEntries([...window.__lyflow.stores.cache.getState().plan].map(([id, n]) => [id, n.cacheKey]));`);
  };
  const keys0 = await planKeys();
  await openPanel(cdp);

  const defA = `[data-testid="pp-def-n_pre_a"]`;
  report.ok("子图实例下面有「子图定义」一栏", Boolean(await reveal(cdp, defA)));
  const defText = await cdp.eval(`return document.querySelector(${lit(defA)})?.textContent ?? null;`);
  report.ok("标着「子图定义 · 2 个实例共享」", (defText ?? "").includes("子图定义 · 2 个实例共享"), defText);
  await clickIn(cdp, defA);
  const badge = await (async () => {
    await reveal(cdp, `[data-testid="pp-shared-n_pre_a/s_voxel"]`);
    return cdp.eval(`return document.querySelector('[data-testid="pp-shared-n_pre_a/s_voxel"]')?.textContent ?? null;`);
  })();
  report.eq("定义里的节点标题旁有共享标记", badge, "子图定义 · 2 个实例共享");
  const shared = await cdp.eval(`return document.querySelector('[data-testid="prow-n_pre_a/s_voxel.leafSize"]')?.getAttribute('data-shared') ?? null;`);
  report.eq("定义里的行也标着共享（data-shared = 2）", shared, "2");

  report.eq("在实例 A 下面改定义里的 leafSize（x → 0.03）",
    await typeIn(cdp, rowSel("n_pre_a/s_voxel.leafSize"), "0.03"), "ok");
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
  report.ok("库算子放进了画布", Boolean(lib?.id), JSON.stringify(lib));
  const libHead = await reveal(cdp, `[data-testid="pp-node-${lib.id}"]`);
  const libInfo = await cdp.eval(`
    return {
      badge: document.querySelector('[data-testid="pp-library-${lib.id}"]')?.textContent ?? null,
      def: !!document.querySelector('[data-testid="pp-def-${lib.id}"]'),
      inner: document.querySelectorAll('[data-node^="${lib.id}/"]').length,
    };
  `);
  report.ok("面板里有库算子那一节", Boolean(libHead));
  report.eq("库算子：标「库算子 · 内部只读」、没有定义可展开、没有内部行", libInfo, { badge: "库算子 · 内部只读", def: false, inner: 0 });
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
  report.eq("先跑一遍（2D 视图要一片底图）", run.status, "ok");
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
  report.eq("撤销记录是一次拖动", (await snap(cdp)).undoLabel, "拖动 ROI");
  await undoByKey(cdp);
  report.eq("一次 Ctrl+Z 还原（显式值删掉 = 回到默认值）", (await paramsOf(cdp, ids.show)).roi, before);
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
  report.ok(`图里 ${total} 个可见参数（≥ 1000）`, total >= 1000, String(total));

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
  report.eq("「全部」计数 = 可见参数数", virt.chipAll, total);
  report.ok("列表是虚拟化的：只挂了一小部分", virt.mounted > 0 && virt.mounted < 120 && virt.total > 500, JSON.stringify(virt));

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
  report.ok("滚轮真的把列表滚下去了", scrollAfter > scrollBefore + 2000, `${scrollBefore} → ${scrollAfter}`);
  report.ok(`滚动时的帧率 ${fps} fps ≥ 50`, fps != null && fps >= 50, `${fps} fps`);

  // 输入一个数 → 画布状态（graph store 的 doc）更新并画完一帧
  const target = await cdp.eval(`
    const row = document.querySelector('[data-testid="pp-list"] .prow[data-type="float"][data-enabled="1"]');
    return row ? { testid: row.getAttribute('data-testid'), node: row.getAttribute('data-node'), param: row.getAttribute('data-param') } : null;
  `);
  report.ok("找到一个挂着的 float 行", Boolean(target), JSON.stringify(target));
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
  report.eq("输入的值进了 doc", v, 0.777);
  report.ok(`输入一个数到画布状态更新 ${inputMs} ms < 100 ms`, inputMs < 100, `${inputMs} ms`);
  await resetPanel(cdp);
  await newDoc(cdp);
}

// 顺序有意义：suiteSubgraphShared 里存库、重扫库目录（refresh_library 会 drop 掉全部运行）；那之后的第一次运行
// 全部命中缓存却取不到点云（「core 没有该结果」，见 docs/param-recipe-p2-acceptance.md「P2 之外发现的问题」），
// 要 2D 视图底图的 ROI 一组排在它前面。
export const paramsP2Suites = [
  suiteLayout,
  suiteAllTypes,
  suiteConditions,
  suiteSearchFilter,
  suiteRoi,
  suiteSubgraphShared,
  suitePerf,
];
