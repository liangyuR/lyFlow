// 页面侧的公共动作：搭图、按键、真实鼠标拖拽、等一次运行结束。
// M2 与 M3 两份验收脚本共用，避免各写一份之后互相漂移。

import { sleep } from "./cdp.mjs";

/** JS 字符串字面量。路径里有反斜杠和中文，手工拼会出事。 */
export const lit = (v) => JSON.stringify(v);

/** 前提与动作结果（「图跑通了」「菜单点到了」「返回 'ok'」）：不成立就中断这一组 —— run.mjs 报「分组 X 中断」
 *  并带上 what 与实际值 —— 但不单独算一条断言。真正要验的行为照旧用 report.ok / report.eq。 */
export function mustOk(cond, what, detail = "") {
  if (!cond) throw new Error(`前提不成立：${what}${detail === "" ? "" : ` —— ${typeof detail === "string" ? detail : JSON.stringify(detail)}`}`);
}

export async function newDoc(cdp) {
  await cdp.eval(`
    const b = window.__lyflow;
    // 上一组万一留了个搜索面板没关，它的全屏 backdrop 会把整个画布盖住
    b.stores.ui.getState().closeSearch();
    b.stores.ui.getState().endConnection();
    b.stores.graph.getState().newDoc();
    b.stores.ui.getState().clearSelection();
    b.stores.execution.getState().reset();
    b.stores.cache.getState().reset();
    b.clearTransitions();
    return true;
  `);
}

/** 按声明搭一张图。走 store 的语义化动作而不是直接塞 doc（见 ./README.md）。 */
export async function buildGraph(cdp, nodes, edges) {
  return cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const ids = {};
    const specs = ${lit(nodes)};
    for (let i = 0; i < specs.length; i++) {
      const s = specs[i];
      const id = g.addNode(s.op, { x: 40 + i * 210, y: 80 + (s.row ?? 0) * 150 });
      if (!id) throw new Error('addNode 失败: ' + s.op);
      ids[s.key] = id;
      for (const [k, v] of Object.entries(s.params ?? {})) {
        window.__lyflow.stores.graph.getState().setParam(id, k, v);
      }
    }
    for (const e of ${lit(edges)}) {
      const verdict = window.__lyflow.stores.graph.getState().connect(
        { node: ids[e.from[0]], port: e.from[1] },
        { node: ids[e.to[0]], port: e.to[1] },
      );
      if (!verdict.ok) throw new Error('connect 失败 ' + JSON.stringify(e) + ': ' + verdict.reason);
    }
    return ids;
  `);
}

export async function saveGraphTo(cdp, filePath) {
  return cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    await b.transport.saveGraph(${lit(filePath)}, g.doc);
    b.stores.graph.getState().markSaved(${lit(filePath)});
    return b.stores.graph.getState().filePath;
  `);
}

const MODIFIER = { alt: 1, ctrl: 2, meta: 4, shift: 8 };

/** 真实按键，走完整的快捷键链路（CDP 的 Input 域 = 用户真的按下去）。 */
export async function pressKey(cdp, key, windowsVirtualKeyCode, mods = []) {
  const modifiers = mods.reduce((m, name) => m | (MODIFIER[name] ?? 0), 0);
  const base = {
    key,
    code: key,
    windowsVirtualKeyCode,
    nativeVirtualKeyCode: windowsVirtualKeyCode,
    modifiers,
  };
  await cdp.send("Input.dispatchKeyEvent", { type: "rawKeyDown", ...base });
  await cdp.send("Input.dispatchKeyEvent", { type: "keyUp", ...base });
}

/** Ctrl+<字母>。字母的 virtual key code 就是它的大写 ASCII。 */
export const pressCtrl = (cdp, letter, extra = []) =>
  pressKey(cdp, letter.toLowerCase(), letter.toUpperCase().charCodeAt(0), ["ctrl", ...extra]);

export const pressF5 = (cdp) => pressKey(cdp, "F5", 116);
export const pressShiftF5 = (cdp) => pressKey(cdp, "F5", 116, ["shift"]);
export const pressEscape = (cdp) => pressKey(cdp, "Escape", 27);
export const pressQuestion = (cdp) => pressKey(cdp, "?", 191, ["shift"]);

/** 真实鼠标拖拽。连线吸附、拖节点到线上这类手感项只有真事件才验得到。 */
export async function dragMouse(cdp, from, to, { steps = 12, button = "left" } = {}) {
  const common = { button, buttons: 1, clickCount: 1 };
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: from.x, y: from.y, buttons: 0 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: from.x, y: from.y, ...common });
  // 先挪 2 px 把 React Flow 的 nodeDragThreshold 吃掉。不这么做的话第一步的位移
  // 会被整段吞掉 —— 位移越大丢得越多，落点就永远差那么一截。
  await cdp.send("Input.dispatchMouseEvent", {
    type: "mouseMoved", x: from.x + 2, y: from.y, ...common,
  });
  await sleep(20);
  for (let i = 1; i <= steps; i++) {
    const t = i / steps;
    await cdp.send("Input.dispatchMouseEvent", {
      type: "mouseMoved",
      x: Math.round(from.x + (to.x - from.x) * t),
      y: Math.round(from.y + (to.y - from.y) * t),
      ...common,
    });
    await sleep(12);
  }
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: to.x, y: to.y, ...common });
  await sleep(120);
}

/** 把画布缩放压到 maxScale 以下。fitView 会随节点数把缩放放大到 2 倍以上，
 *  那时两个节点并排就塞不进画布，拖拽类断言的落点会跑到右侧面板上去。 */
export async function normalizeZoom(cdp, maxScale = 0.8) {
  for (let i = 0; i < 10; i += 1) {
    const scale = await cdp.eval(`
      const vp = document.querySelector('.react-flow__viewport');
      return new DOMMatrixReadOnly(getComputedStyle(vp).transform).a;
    `);
    if (scale <= maxScale) break;
    await cdp.eval(`
      const btn = document.querySelector('.react-flow__controls-zoomout');
      if (btn) btn.click();
      return true;
    `);
    await sleep(120);
  }
  await sleep(150);
}

/** 画布本身的位置与大小。摆位前先问一句，免得把节点放到右侧面板底下。 */
export async function canvasBox(cdp) {
  return cdp.eval(`
    const r = document.querySelector('.canvas').getBoundingClientRect();
    return { x: Math.round(r.left), y: Math.round(r.top),
             w: Math.round(r.width), h: Math.round(r.height) };
  `);
}

/** 按**屏幕**坐标摆节点。画布的缩放平移会累积，直接用画布坐标摆位很容易把节点
 *  推到状态栏底下 —— 拖拽类断言必须先把几何定死。 */
export async function placeAtScreen(cdp, placement) {
  await cdp.eval(`
    // viewport 的 transform 是相对**画布容器**的，不是相对窗口的 ——
    // 再加一次 canvas.left 会把节点整体推出画布，而 DOM 查询照样找得到它。
    const vp = document.querySelector('.react-flow__viewport');
    const m = new DOMMatrixReadOnly(getComputedStyle(vp).transform);
    const moves = Object.entries(${lit(placement)}).map(([id, p]) => ({
      id,
      position: { x: (p.x - m.e) / m.a, y: (p.y - m.f) / m.d },
    }));
    window.__lyflow.stores.graph.getState().applyLayout(moves);
    return true;
  `);
  await sleep(300);
}

/** 当前画布缩放。连线吸附的容差是**画布坐标**，屏幕上的像素数要按它换算。 */
export async function viewportScale(cdp) {
  return cdp.eval(`
    const vp = document.querySelector('.react-flow__viewport');
    return new DOMMatrixReadOnly(getComputedStyle(vp).transform).a;
  `);
}

/** 元素中心的屏幕坐标。拿不到元素返回 null。 */
export async function centerOf(cdp, selector) {
  return cdp.eval(`
    const el = document.querySelector(${lit(selector)});
    if (!el) return null;
    const r = el.getBoundingClientRect();
    return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
  `);
}

/** 触发一次运行并等它结束。先记下当前 runId，否则会读到上一次的快照（见 ./README.md）。 */
export async function runAndWait(cdp, fire, timeoutMs = 120_000) {
  const before = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  await cdp.eval(`window.__lyflow.clearTransitions(); return true;`);
  await fire();
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runId !== ${lit(before)} && s.runStatus !== 'idle' && s.runStatus !== 'running'; })()`,
    { timeoutMs, what: "新一次运行结束" },
  );
  return cdp.eval(`return window.__lyflow.snapshot().run;`);
}

export async function select(cdp, nodeId) {
  await cdp.eval(`
    window.__lyflow.stores.ui.getState().setSelection([${lit(nodeId)}], []);
    return true;
  `);
}

/** 立刻重编一次计划并等结果落进 store。不等 debounce，验收脚本不想为 150ms 睡一觉。 */
export async function replan(cdp) {
  await cdp.eval(`await window.__lyflow.plan(); return true;`);
  return cdp.eval(`return window.__lyflow.snapshot().cache;`);
}

/** 选中一个节点，等 3D 视图真的切过去，返回它显示的点数与状态（见 ./README.md）。 */
export async function selectAndReadViewer(cdp, nodeId, timeoutMs = 30_000) {
  await select(cdp, nodeId);
  const deadline = Date.now() + timeoutMs;
  let info = null;
  while (Date.now() < deadline) {
    info = await cdp.eval(`
      const v = document.querySelector('.viewer');
      if (!v || v.getAttribute('data-node') !== ${lit(nodeId)}) return null;
      if (v.getAttribute('data-view') === 'loading') return null;
      const count = v.querySelector('.viewer__count');
      const status = v.querySelector('[data-testid="viewer3d-status"]');
      const base = v.querySelector('[data-testid="viewer-base"]');
      return {
        view: v.getAttribute('data-view'),
        text: count ? count.textContent : null,
        status: status ? status.textContent : null,
        base: v.getAttribute('data-base'),
        baseText: base ? base.textContent : null,
        hasCanvas: !!v.querySelector('.viewer__canvas canvas'),
      };
    `);
    if (info) break;
    await sleep(120);
  }
  if (!info) return { count: 0, total: 0, hasCanvas: false, status: "等视图切换超时" };
  const nth = (i) =>
    info.text ? Number(info.text.replace(/\s/g, "").split("/")[i].replace(/[^\d]/g, "")) : 0;
  return {
    count: nth(0),
    total: info.text && info.text.includes("/") ? nth(1) : nth(0),
    hasCanvas: info.hasCanvas,
    status: info.status,
    view: info.view,
    base: info.base,
    baseText: info.baseText,
  };
}

/** 视图上两个只读的包围盒：底图云的与叠画几何的。各是六个数 [minXYZ, maxXYZ]，读不到是 null。 */
export async function viewerBounds(cdp) {
  const read = await cdp.eval(`
    const v = document.querySelector('.viewer');
    if (!v) return null;
    return { cloud: v.getAttribute('data-cloud-bounds'),
             overlay: v.getAttribute('data-overlay-bounds') };
  `);
  const parse = (s) => (s ? s.split(",").map(Number) : null);
  return { cloud: parse(read?.cloud), overlay: parse(read?.overlay) };
}
