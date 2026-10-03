// M8b 的分组（docs/m8-plan.md §4 验收 7–10）：从空白画布只靠拖入节点（自动连线）、插入片段、
// 2D 视图拖框、改参数建出一个模板路径测点；两个候选时不连、候选高亮；Edge Peek 看 ScanPair 的字段；
// 编辑时把 datum 框拖到错误一侧立即标红。数据是合成的一对剖面，写在临时工作区里。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { ROOT } from "./harness.mjs";
import { buildGraph, dragMouse, lit, mustOk, newDoc, pressCtrl, pressEscape, pressF5, pressKey, runAndWait, select } from "./page.mjs";

// ------------------------------------------------------------ 合成剖面（毫米）
// 与 packs/gap/tests/test_blocks.cpp 的夹具同一个形状：左板顶面 y=165，右板 y=164（高 1 mm），
// 缝两侧各一段 R1 的圆角。x 步长 0.05 mm。

export function profile(shiftMm) {
  const pts = [];
  for (let x = -20 + shiftMm; x < -3; x += 0.05) pts.push([x, 165]);
  for (let k = 0; k <= 30; k += 1) {
    const t = (Math.PI / 2) * (k / 30) + shiftMm * 0.3;
    if (t > Math.PI / 2) break;
    pts.push([-3 + Math.sin(t), 166 - Math.cos(t)]);
  }
  for (let k = 30; k >= 0; k -= 1) {
    const t = (Math.PI / 2) * (k / 30) + shiftMm * 0.3;
    if (t > Math.PI / 2) continue;
    pts.push([3 - Math.sin(t), 165 - Math.cos(t)]);
  }
  for (let x = 3 + 0.05 - shiftMm; x < 20; x += 0.05) pts.push([x, 164]);
  return pts;
}

export function writePcd(file, rows) {
  const head =
    "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n" +
    `WIDTH ${rows.length}\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS ${rows.length}\nDATA ascii\n`;
  const body = rows
    .map((r) => r.map((v) => (Number.isNaN(v) ? "nan" : v.toFixed(7))).join(" "))
    .join("\n");
  fs.writeFileSync(file, `${head}${body}\n`);
}

/** 一个测点目录（传感器帧：x=u, y=0, z=h，米）+ 模板目录（测量帧：x, y, 0）。 */
function makeScene(ws) {
  const root = path.join(ws.dir, "m8b 测点");
  const point = path.join(root, "point");
  const templates = path.join(root, "StandardGap");
  fs.mkdirSync(point, { recursive: true });
  fs.mkdirSync(templates, { recursive: true });
  const master = profile(0);
  const slave = profile(0.025);
  const sensor = (pts, nanEvery) => {
    const rows = [];
    pts.forEach(([x, y], i) => {
      if (nanEvery > 0 && i % nanEvery === 0) rows.push([NaN, 0, NaN]);
      rows.push([x / 1000, 0, y / 1000]);
    });
    return rows;
  };
  writePcd(path.join(point, "LaserProfile_L0_Master_x.pcd"), sensor(master, 97));
  writePcd(path.join(point, "LaserProfile_R1_Slave_x.pcd"), sensor(slave, 0));
  const side = (left) => master.filter(([x]) => (x < 0) === left).map(([x, y]) => [x / 1000, y / 1000, 0]);
  // 文件名就是 locate_template 槽 1 的默认值：人只填模板目录
  writePcd(path.join(templates, "left_template.pcd"), side(true));
  writePcd(path.join(templates, "right_template.pcd"), side(false));
  return { root, point, templates };
}

/** 模板坐标系里的四个角色框（毫米），与夹具的 StandardGap 配置一致；槽 1 的那四个参数（M8c L19
 *  起每个槽各有自己的四框）。按这个顺序拖：先把两个小的缝框挪到中间，大框落位时就不会盖住还没拖的占位框。 */
const ROLE_BOXES = {
  template1SeamLeftRoi: [-4.5, 164, -1.5, 167],
  template1SeamRightRoi: [1.5, 163, 4.5, 166],
  template1TargetRoi: [5, 162, 15, 166],
  template1DatumRoi: [-15, 163, -5, 167],
};

// ------------------------------------------------------------------ 页面动作

/** 从面板把一行（算子或片段）拖到画布上的一个屏幕点。HTML5 拖放走 DragEvent + 一个真的
 *  DataTransfer：面板行自己的 dragstart 把 MIME 写进去，画布自己的 dragover / drop 读出来 ——
 *  CDP 的鼠标事件不会触发 HTML5 拖放（见 README「踩过的坑」）。 */
async function dropFromPalette(cdp, rowSelector, at) {
  return cdp.eval(`
    const row = document.querySelector(${lit(rowSelector)});
    if (!row) return 'no-row';
    row.scrollIntoView({ block: 'center' });
    const dt = new DataTransfer();
    row.dispatchEvent(new DragEvent('dragstart', { bubbles: true, cancelable: true, dataTransfer: dt }));
    const target = document.elementFromPoint(${at.x}, ${at.y});
    if (!target || !target.closest('.canvas')) return 'not-canvas:' + (target ? target.className : 'null');
    const opts = { bubbles: true, cancelable: true, dataTransfer: dt, clientX: ${at.x}, clientY: ${at.y} };
    target.dispatchEvent(new DragEvent('dragenter', opts));
    const over = new DragEvent('dragover', opts);
    target.dispatchEvent(over);
    target.dispatchEvent(new DragEvent('drop', opts));
    row.dispatchEvent(new DragEvent('dragend', { bubbles: true, dataTransfer: dt }));
    return over.defaultPrevented ? 'ok' : 'rejected';
  `);
}

async function canvasRect(cdp) {
  return cdp.eval(`
    const r = document.querySelector('.canvas').getBoundingClientRect();
    return { x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width), h: Math.round(r.height) };
  `);
}

const docOf = (cdp) => cdp.eval(`return window.__lyflow.stores.graph.getState().doc;`);

const nodeByOp = (doc, op) => doc.nodes.filter((n) => n.op === op);

/** 数一下这一组里有没有人手动连过线：connect / reconnectEdge / insertOnEdge 三条路都记。 */
function countManualEdges(cdp) {
  return cdp.eval(`
    const s = window.__lyflow.stores.graph;
    window.__m8bManual = [];
    window.__m8bOrig ??= {
      connect: s.getState().connect,
      reconnectEdge: s.getState().reconnectEdge,
      insertOnEdge: s.getState().insertOnEdge,
    };
    const o = window.__m8bOrig;
    s.setState({
      connect: (a, b) => { window.__m8bManual.push('connect'); return o.connect(a, b); },
      reconnectEdge: (e, t) => { window.__m8bManual.push('reconnect'); return o.reconnectEdge(e, t); },
      insertOnEdge: (...a) => { window.__m8bManual.push('insertOnEdge'); return o.insertOnEdge(...a); },
    });
    return true;
  `);
}

function restoreManualEdges(cdp) {
  return cdp.eval(`
    const o = window.__m8bOrig;
    if (o) window.__lyflow.stores.graph.setState(o);
    return window.__m8bManual ?? [];
  `);
}

export async function setCamera(cdp, mode) {
  await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-camera"]');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, ${lit(mode)});
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);
  await cdp.waitFor(`document.querySelector('.viewer')?.getAttribute('data-camera') === ${lit(mode)}`, {
    timeoutMs: 5000,
    what: `相机切到 ${mode}`,
  });
}

/** 拖框层当前的「米 → 屏幕像素」映射，与每个框、每个把手的屏幕位置。 */
export async function roiGeometry(cdp) {
  return cdp.eval(`
    const layer = document.querySelector('[data-testid="roi-layer"]');
    if (!layer || !layer.getAttribute('data-map')) return null;
    const lr = layer.getBoundingClientRect();
    const [sx, ox, sy, oy] = layer.getAttribute('data-map').split(',').map(Number);
    const center = (el) => {
      const r = el.getBoundingClientRect();
      return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
    };
    // 框挨着框时，中心可能被邻居盖住：在框里找一个真能点中它自己的点（人也是这么点的）
    const grab = (el) => {
      const r = el.getBoundingClientRect();
      for (const fy of [0.5, 0.3, 0.7, 0.15, 0.85]) {
        for (const fx of [0.5, 0.3, 0.7, 0.15, 0.85]) {
          const x = Math.round(r.left + r.width * fx);
          const y = Math.round(r.top + r.height * fy);
          const hit = document.elementFromPoint(x, y);
          if (hit === el) return { x, y };
        }
      }
      return center(el);
    };
    const boxes = {};
    for (const el of layer.querySelectorAll('.roi-box')) {
      const name = el.getAttribute('data-testid').replace('roi-box-', '');
      const handles = {};
      for (const h of el.querySelectorAll('.roi-box__handle')) {
        const c = center(h);
        handles[h.getAttribute('data-testid').split('-').pop()] =
          { ...c, hittable: document.elementFromPoint(c.x, c.y) === h };
      }
      boxes[name] = {
        center: grab(el),
        handles,
        value: el.getAttribute('data-roi').split(',').map(Number),
        unset: el.getAttribute('data-unset') === '1',
        label: el.querySelector('.roi-box__label')?.textContent ?? '',
        color: getComputedStyle(el).borderTopColor,
      };
    }
    return { map: { sx, ox: lr.left + ox, sy, oy: lr.top + oy }, boxes };
  `);
}

/** 模板坐标（毫米）→ 屏幕像素。 */
export const screenOf = (map, xMm, yMm) => ({
  x: Math.round(map.ox + map.sx * (xMm / 1000)),
  y: Math.round(map.oy + map.sy * (yMm / 1000)),
});

/** 用真实鼠标把一个框拖成目标值：先拖框身把中心挪过去，再拖右下、左上两个角。 */
async function dragBoxTo(cdp, name, target) {
  let g = await roiGeometry(cdp);
  const [x0, y0, x1, y1] = target;
  await dragMouse(cdp, g.boxes[name].center, screenOf(g.map, (x0 + x1) / 2, (y0 + y1) / 2), { steps: 10 });
  await sleep(150);
  g = await roiGeometry(cdp);
  // 屏幕 y 向下：右下角是 (xMax, yMin)，左上角是 (xMin, yMax)
  await dragMouse(cdp, g.boxes[name].handles.se, screenOf(g.map, x1, y0), { steps: 8 });
  await sleep(150);
  g = await roiGeometry(cdp);
  await dragMouse(cdp, g.boxes[name].handles.nw, screenOf(g.map, x0, y1), { steps: 8 });
  await sleep(150);
  g = await roiGeometry(cdp);
  return g.boxes[name].value;
}

export async function waitValidated(cdp, nodeId, wantInvalid, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  let last = null;
  while (Date.now() < deadline) {
    last = await cdp.eval(`
      const el = document.querySelector('[data-testid="node-${nodeId}"]');
      const d = window.__lyflow.stores.validation.getState().byNode.get(${lit(nodeId)}) ?? [];
      return {
        invalid: el ? el.getAttribute('data-invalid') : null,
        text: el?.querySelector('.node__invalid-text')?.textContent ?? null,
        params: el?.querySelector('.node__invalid')?.getAttribute('data-params') ?? null,
        diags: d.filter((x) => x.severity === 'error').map((x) => ({ code: x.code, paramPath: x.paramPath, message: x.message })),
      };
    `);
    if (last && (last.invalid === "1") === wantInvalid) return last;
    await sleep(100);
  }
  return last;
}

async function fitCanvas(cdp) {
  await cdp.eval(`document.querySelector('.react-flow__controls-fitview')?.click(); return true;`);
  await sleep(500);
}

// ------------------------------------------------------------------ 分组

let built = null;

/** §4 验收 7：从空白画布建出模板路径测点并跑出 flush 与 gap，全程不手连一条边。 */
async function suiteBuildFromBlank(cdp, report, ws) {
  const ops = await cdp.eval(`
    const m = window.__lyflow.stores.manifest.getState();
    return {
      read: m.operatorsById.has('gap.read_scan'),
      skeleton: (m.bundle?.snippets ?? []).some((s) => s.id === 'gap.measure_skeleton'),
    };
  `);
  if (!ops.read || !ops.skeleton) {
    report.section("M8b 验收 7（本次构建没有 gap 包，未验）");
    return;
  }
  report.section("M8b 验收 7：空白画布 → 拖入节点 + 测点骨架 + 2D 拖框 + 改参数 → flush / gap");

  const scene = makeScene(ws);
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.peek.getState().closeAll(); window.__lyflow.stores.ui.getState().setPinnedNode(null); return true;`);
  await countManualEdges(cdp);
  const canvas = await canvasRect(cdp);

  // 1. 拖入读剖面：图里什么都没有，没有可连的
  const droppedRead = await dropFromPalette(cdp, '.palette [data-op-id="gap.read_scan"]',
    { x: canvas.x + 60, y: canvas.y + 80 });
  mustOk(droppedRead === "ok", "拖入 gap.read_scan", droppedRead);
  // 2. 拖入模板定位：scan 唯一候选，自动连上
  const droppedLocate = await dropFromPalette(cdp, '.palette [data-op-id="gap.locate_template"]',
    { x: canvas.x + 290, y: canvas.y + 80 });
  mustOk(droppedLocate === "ok", "拖入 gap.locate_template", droppedLocate);
  let doc = await docOf(cdp);
  const read = nodeByOp(doc, "gap.read_scan")[0]?.id;
  const locate = nodeByOp(doc, "gap.locate_template")[0]?.id;
  mustOk(Boolean(read && locate), "两个节点都落下了", JSON.stringify(doc.nodes.map((n) => n.op)));
  report.ok(
    "locate_template.scan 自动接到 read_scan.scan",
    doc.edges.length === 1 && doc.edges[0].from.node === read && doc.edges[0].to.node === locate &&
      doc.edges[0].to.port === "scan",
    JSON.stringify(doc.edges),
  );

  // 3. 插入「测点骨架」片段：8 个节点，对外的 8 个输入全都接到 locate_template 那唯一的一对输出上
  const droppedSkeleton = await dropFromPalette(cdp, '[data-testid="snippet-gap.measure_skeleton"]',
    { x: canvas.x + 520, y: canvas.y + 40 });
  mustOk(droppedSkeleton === "ok", "插入「测点骨架」", droppedSkeleton);
  doc = await docOf(cdp);
  report.eq("一共 10 个节点（计划 §3 的模板路径测点）", doc.nodes.length, 10);
  report.eq("一共 20 条边：1 条拖入时自动连的 + 11 条片段内部的 + 8 条插入时自动连的", doc.edges.length, 20);
  const intoSkeleton = doc.edges.filter((e) => e.from.node === locate);
  report.eq("骨架的 8 个对外输入都接在 locate_template 上", intoSkeleton.length, 8);
  report.ok(
    "scan 接 scan、rois 接 rois（没有接到 read_scan 的原始云上）",
    intoSkeleton.every((e) => e.from.port === e.to.port) && !doc.edges.some((e) => e.from.node === read && e.to.node !== locate),
    JSON.stringify(intoSkeleton.map((e) => `${e.from.port}→${e.to.port}`)),
  );
  const hint = await cdp.eval(`return window.__lyflow.snapshot().autoHint;`);
  report.eq("没有留下歧义（没有端口要人挑）", hint, null);

  // 4. 改基础组参数：测点目录、模板目录、两个判定的名义值
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.setParam(${lit(read)}, 'dir', ${lit(scene.point)});
    window.__lyflow.stores.graph.getState().setParam(${lit(locate)}, 'templateDir', ${lit(scene.templates)});
    return true;
  `);
  const judges = nodeByOp(doc, "gap.judge");
  for (const j of judges) {
    const nominal = j.ui?.title === "判定段差" ? 1 : 4;
    await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(j.id)}, 'nominal', ${nominal}); return true;`);
  }

  // 四个框还没画：实时校验在拼的时候就标红
  const before = await waitValidated(cdp, locate, true);
  report.eq("四个框都没填时 locate_template 立即标红", before?.invalid, "1");
  report.ok("诊断指到框参数上", (before?.diags ?? []).some((d) => d.paramPath === "template1DatumRoi"),
    JSON.stringify(before?.diags));

  // 5. 2D 视图拖四个框
  await select(cdp, locate);
  await setCamera(cdp, "2d");
  const ready = await cdp.waitFor(
    `(() => { const v = document.querySelector('.viewer');
              return v && Number(v.getAttribute('data-backdrop')) > 0 && Number(v.getAttribute('data-roi-edit')) === 4
                && document.querySelector('[data-testid="roi-layer"]')?.getAttribute('data-map'); })()`,
    { timeoutMs: 20_000, what: "模板底图与四个可拖框" },
  ).catch((e) => String(e));
  report.ok("2D 视图画出了槽 1 的模板云和四个框", Boolean(ready) && !String(ready).startsWith("Error"),
    String(ready));
  let g = await roiGeometry(cdp);
  report.ok("四个框各一种颜色、标着角色名", g !== null &&
    new Set(Object.values(g.boxes).map((b) => b.color)).size === 4 &&
    ["Datum", "Target", "Seam Left", "Seam Right"].every((r) => Object.values(g.boxes).some((b) => b.label.startsWith(r))),
    JSON.stringify(g && Object.fromEntries(Object.entries(g.boxes).map(([k, b]) => [k, [b.label, b.color]]))));
  report.ok("没填过的框标成未设置", g !== null && Object.values(g.boxes).every((b) => b.unset));

  for (const [name, target] of Object.entries(ROLE_BOXES)) {
    const got = await dragBoxTo(cdp, name, target);
    const err = Math.max(...got.map((v, i) => Math.abs(v - target[i])));
    report.ok(`${name} 拖到了 ${JSON.stringify(target)}（误差 < 0.35 mm）`, err < 0.35, JSON.stringify(got));
  }
  const params = await cdp.eval(`
    const n = window.__lyflow.stores.graph.getState().doc.nodes.find((x) => x.id === ${lit(locate)});
    return n.params;
  `);
  report.ok("拖动写回了参数（四个框都进了稀疏 params）",
    Object.keys(ROLE_BOXES).every((k) => Array.isArray(params[k]) && params[k].length === 4),
    JSON.stringify(params));
  const after = await waitValidated(cdp, locate, false);
  report.eq("框拖好之后校验干净、红框消失", after?.invalid, "0");

  // 最近拖过的那个框（Datum）写着读数：坐标、宽 × 高、框里几个点（以前要跑一遍才知道框是不是空的）
  const roiValue = (name) => cdp.eval(`
    return document.querySelector('[data-testid="roi-box-' + ${lit(name)} + '"]')?.getAttribute('data-roi').split(',').map(Number) ?? null;
  `);
  const pastNow = () => cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
  const tag = await cdp.eval(`return document.querySelector('[data-testid="roi-tag-template1DatumRoi"]')?.textContent ?? null;`);
  // 拖出来的值有吸附误差（上面验过在 0.35 mm 以内）：按框此刻的值算期望
  const v0 = await roiValue("template1DatumRoi");
  const f = (x) => String(Number(x.toPrecision(6)));
  const want = `(${f(v0[0])}, ${f(v0[1])}) → (${f(v0[2])}, ${f(v0[3])}) mm · ${f(v0[2] - v0[0])} × ${f(v0[3] - v0[1])} mm · `;
  const tagMatch = (tag ?? "").startsWith(want) ? /(\d+) 点$/.exec(tag) : null;
  report.ok("最近拖过的 Datum 框写着读数：坐标、宽 × 高（mm）、框里有点", !!tagMatch && Number(tagMatch[1]) > 0, JSON.stringify({ tag, v0 }));
  // 方向键微调：一下 0.1 mm（Shift ×10），每下一条撤销；Ctrl+Z 回去
  const pastNudge = await pastNow();
  await pressKey(cdp, "ArrowRight", 39);
  await pressKey(cdp, "ArrowUp", 38, ["shift"]);
  await sleep(150);
  const nudged = { value: await roiValue("template1DatumRoi"), steps: (await pastNow()) - pastNudge };
  await pressCtrl(cdp, "z");
  await pressCtrl(cdp, "z");
  await sleep(150);
  report.ok("焦点在框上按 → 挪 0.1 mm、Shift+↑ 挪 1 mm，各一条撤销；Ctrl+Z 两下回去",
    JSON.stringify(nudged) === JSON.stringify({ value: v0.map((x, i) => Number((x + (i % 2 === 0 ? 0.1 : 1)).toFixed(6))), steps: 2 }) &&
      JSON.stringify(await roiValue("template1DatumRoi")) === JSON.stringify(v0),
    JSON.stringify({ v0, nudged }));
  // 拖到一半按 Esc：放弃这一段，框回到原处、不记撤销
  {
    const g0 = await roiGeometry(cdp);
    const c = g0.boxes.template1DatumRoi.center;
    const common = { button: "left", buttons: 1, clickCount: 1 };
    const pastEsc = await pastNow();
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: c.x, y: c.y, buttons: 0 });
    await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: c.x, y: c.y, ...common });
    for (let i = 1; i <= 5; i += 1) {
      await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: c.x + i * 8, y: c.y, ...common });
      await sleep(30);
    }
    const midway = await roiValue("template1DatumRoi");
    await pressEscape(cdp);
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: c.x + 40, y: c.y, ...common });
    await sleep(150);
    report.ok("拖到一半按 Esc：放弃这一段，框回到原处、不记撤销",
      JSON.stringify(midway) !== JSON.stringify(v0) &&
        JSON.stringify(await roiValue("template1DatumRoi")) === JSON.stringify(v0) && (await pastNow()) === pastEsc,
      JSON.stringify({ v0, midway }));
  }

  // 6. 跑
  const run = await runAndWait(cdp, () => pressF5(cdp), 180_000);
  doc = await docOf(cdp);
  const flushNode = nodeByOp(doc, "gap.flush")[0]?.id;
  const gapNode = nodeByOp(doc, "gap.gap")[0]?.id;
  const values = await cdp.eval(`
    const e = window.__lyflow.stores.execution.getState();
    const read = (id) => (e.nodes.get(id)?.stats?.outputs ?? []).find((o) => o.port === 'value')?.value ?? null;
    return { flush: read(${lit(flushNode)}), gap: read(${lit(gapNode)}) };
  `);
  const failed = Object.entries(run.nodes).filter(([, n]) => n.state === "error")
    .map(([id, n]) => `${id}:${n.errors?.[0]?.code}:${n.errors?.[0]?.message}`);
  report.eq("整张图跑通", run.status, "ok");
  if (failed.length) report.fail("失败的节点", failed.join(" | "));
  report.ok("跑出了 flush 数值（合成剖面上是 1 mm）",
    Number.isFinite(values.flush?.value) && Math.abs(values.flush.value - 1) < 0.2, JSON.stringify(values.flush));
  report.ok("跑出了 gap 数值（合成剖面上约 √37−2 ≈ 4.08 mm）",
    Number.isFinite(values.gap?.value) && Math.abs(values.gap.value - (Math.sqrt(37) - 2)) < 0.3,
    JSON.stringify(values.gap));

  // 跑过之后再拖框：松手补一次正式运行（开着自动运行；拼图时框没画齐、节点没跑过的那几次不跑）
  {
    const before = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
    const g1 = await roiGeometry(cdp);
    const box = g1?.boxes?.template1DatumRoi;
    if (!box) {
      report.fail("跑过之后 2D 框还在", JSON.stringify(g1));
    } else {
      await dragMouse(cdp, box.center, { x: box.center.x + 6, y: box.center.y }, { steps: 6 });
      const reran = await cdp.waitFor(
        `(() => { const s = window.__lyflow.stores.execution.getState();
                  return s.runId !== ${lit(before)} && !s.preview && s.runStatus !== 'running' && s.runStatus !== 'idle'
                    ? { targets: s.targets, status: s.runStatus } : null; })()`,
        { timeoutMs: 60_000, what: "拖完框松手补的运行" },
      ).catch((e) => ({ error: String(e) }));
      report.ok("跑过之后拖框松手：补一次正式运行、算的是 locate_template", Array.isArray(reran?.targets) && reran.targets.includes(locate),
        JSON.stringify(reran));
      await pressCtrl(cdp, "z");
      await sleep(150);
    }
  }

  const manual = await restoreManualEdges(cdp);
  report.eq("全程没有手连一条边（connect / 改接 / 插到线上一次都没调）", manual, []);

  // 最终画布：样本云与变换后的框（只读）也叠上了
  await sleep(600);
  const view = await cdp.eval(`
    const v = document.querySelector('.viewer');
    return { overlay: Number(v.getAttribute('data-overlay')), view: v.getAttribute('data-view'),
             node: v.getAttribute('data-node') };
  `);
  report.ok("跑完之后样本云上叠着变换后的四个框（只读）", view.overlay >= 4 && view.view === "cloud",
    JSON.stringify(view));
  // 截图前整理一下布局（移动节点不是连线），再适配视图
  await cdp.eval(`
    [...document.querySelectorAll('.toolbar button')].find((b) => b.textContent.trim() === '整理')?.click();
    return true;
  `);
  await sleep(400);
  await fitCanvas(cdp);
  const shotPath = process.env.LYFLOW_E2E_SCREENSHOT;
  if (shotPath) {
    const shot = await cdp.send("Page.captureScreenshot", { format: "png" });
    const target = path.isAbsolute(shotPath) ? shotPath : path.join(ROOT, shotPath);
    fs.writeFileSync(target, Buffer.from(shot.data, "base64"));
    console.log(`  （最终画布截图写到了 ${target}）`);
  }
  built = { read, locate, flushNode, gapNode };
}

/** §4 验收 9：Edge Peek 在 ScanPair 边上列出三个字段，点进 merged 显示点云。 */
async function suiteBundlePeek(cdp, report) {
  if (!built) return;
  report.section("M8b 验收 9：Edge Peek 看 ScanPair 边 —— 三个字段，点进 merged 是点云");

  await select(cdp, built.read);
  await setCamera(cdp, "3d");
  const edge = await cdp.eval(`
    const doc = window.__lyflow.stores.graph.getState().doc;
    return doc.edges.find((e) => e.from.node === ${lit(built.read)} && e.to.node === ${lit(built.locate)})?.id ?? null;
  `);
  mustOk(typeof edge === "string", "找得到 read_scan → locate_template 那条 ScanPair 边", String(edge));
  // 走边的右键菜单「查看内容」（与双击同一个 openPeek），免得双击落点被节点挡住
  const opened = await cdp.eval(`
    const group = document.querySelector('.react-flow__edge[data-id="${edge}"]');
    const p = group?.querySelector('.react-flow__edge-interaction') ?? group?.querySelector('path');
    if (!p) return 'no-edge';
    const len = p.getTotalLength();
    const pt = p.getPointAtLength(len / 2);
    const s = new DOMPoint(pt.x, pt.y).matrixTransform(p.getScreenCTM());
    group.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: s.x, clientY: s.y }));
    await new Promise((r) => setTimeout(r, 150));
    const item = document.querySelector('[data-testid="edge-ctx-peek"]');
    if (!item) return 'no-menu';
    item.click();
    await new Promise((r) => setTimeout(r, 250));
    return 'ok';
  `);
  mustOk(opened === "ok", "边的右键菜单打开了查看器", opened);
  const win = await cdp.waitFor(
    `(() => { const w = window.__lyflow.snapshot().peek.find((x) => x.edgeId === ${lit(edge)}); return w ?? null; })()`,
    { timeoutMs: 5000, what: "Peek 窗" },
  ).catch(() => null);
  mustOk(win !== null, "开出了一个 Peek 窗");
  report.eq("端口类型是 Bundle<gap.ScanPair>", win.type, "Bundle<gap.ScanPair>");
  report.eq("默认视图是字段表", win.view, "fields");
  const fields = await cdp.waitFor(
    `(() => { const el = document.querySelector('[data-peek-id="${win.id}"] [data-testid="peek-fields"]');
              if (!el) return null;
              return [...el.querySelectorAll('.peek-fields__row')].map((r) => ({
                name: r.getAttribute('data-testid').replace('peek-field-', ''),
                type: r.getAttribute('data-type'), count: Number(r.getAttribute('data-count')) })); })()`,
    { timeoutMs: 5000, what: "字段表" },
  ).catch(() => null);
  report.eq("列出三个字段", (fields ?? []).map((f) => f.name), ["primary", "secondary", "merged"]);
  report.ok("三个字段都是点云、都有点", (fields ?? []).every((f) => f.type === "PointCloud" && f.count > 0),
    JSON.stringify(fields));

  await cdp.eval(`document.querySelector('[data-peek-id="${win.id}"] [data-testid="peek-field-merged"]').click(); return true;`);
  const cloud = await cdp.waitFor(
    `(() => { const el = document.querySelector('[data-peek-id="${win.id}"]');
              const count = el?.querySelector('.peek-cloud__count')?.textContent;
              return el && el.querySelector('[data-testid="peek-cloud-canvas"] canvas') && count
                ? { view: el.getAttribute('data-view'), field: el.getAttribute('data-field'),
                    type: el.getAttribute('data-type'), count } : null; })()`,
    { timeoutMs: 15_000, what: "merged 的点云" },
  ).catch(() => null);
  report.ok("点进 merged 显示点云", cloud?.view === "cloud3d" && cloud?.type === "PointCloud", JSON.stringify(cloud));
  const merged = (fields ?? []).find((f) => f.name === "merged")?.count;
  const total = cloud ? Number(cloud.count.replace(/\s/g, "").split("/").pop().replace(/[^\d]/g, "")) : null;
  report.eq("窗里的总点数 = merged 字段的点数", total, merged ?? null);
  const back = await cdp.eval(`
    const b = document.querySelector('[data-peek-id="${win.id}"] [data-testid="peek-field-back"]');
    if (!b) return null;
    b.click();
    await new Promise((r) => setTimeout(r, 200));
    return document.querySelector('[data-peek-id="${win.id}"]').getAttribute('data-view');
  `);
  report.eq("「‹ 字段」回到字段表", back, "fields");
  await cdp.eval(`window.__lyflow.stores.peek.getState().closeAll(); return true;`);
}

/** §4 验收 10：编辑时把 datum 框拖到错误一侧，节点立即标红并显示诊断。 */
async function suiteDragWrongSide(cdp, report) {
  if (!built) return;
  report.section("M8b 验收 10：2D 视图里把 datum 框拖到缝的另一侧 → 立即标红 + 诊断");

  await select(cdp, built.locate);
  await setCamera(cdp, "2d");
  await cdp.waitFor(`document.querySelector('[data-testid="roi-layer"]')?.getAttribute('data-map')`, {
    timeoutMs: 10_000, what: "拖框层",
  });
  const g = await roiGeometry(cdp);
  // 挪到 target 旁边（缝的右侧）。dragMouse 在 mouseReleased 之后还睡了 120 ms 才返回，
  // 计时从松手那一刻算，所以把这 120 ms 补回去
  await dragMouse(cdp, g.boxes.template1DatumRoi.center, screenOf(g.map, 12, 160.5), { steps: 10 });
  const released = Date.now() - 120;
  const bad = await waitValidated(cdp, built.locate, true, 3000);
  const ms = Date.now() - released;
  report.eq("松手后 locate_template 标红", bad?.invalid, "1");
  report.ok("节点上显示诊断：datum 与 target 落在缝的同一侧", /同一侧/.test(bad?.text ?? ""), JSON.stringify(bad));
  report.ok("诊断指到 template1DatumRoi",
    (bad?.diags ?? []).some((d) => d.paramPath === "template1DatumRoi" && d.code === "bad_param"),
    JSON.stringify(bad?.diags));
  report.ok("从松手到标红 < 3 s", bad?.invalid === "1" && ms < 3000, `${ms} ms`);
  const inspector = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-template1DatumRoi"]');
    return { error: row?.getAttribute('data-param-error') ?? null,
             message: row?.querySelector('.insp-param__error')?.textContent ?? null,
             list: !!document.querySelector('[data-testid="inspector-validation"]') };
  `);
  report.eq("Inspector 里 template1DatumRoi 那一行标红", inspector.error, "1");
  report.ok("错误消息贴在控件下面", /同一侧/.test(inspector.message ?? ""), JSON.stringify(inspector));
  report.ok("Inspector 顶部列出校验诊断", inspector.list);

  // 撤销那一次拖动（一整段拖动是一条撤销）
  await cdp.eval(`window.__lyflow.stores.graph.getState().undo(); return true;`);
  const fixed = await waitValidated(cdp, built.locate, false, 3000);
  report.eq("撤销一次就回到干净（整段拖动只记了一条撤销）", fixed?.invalid, "0");
  await setCamera(cdp, "3d");
}

/** §4 验收 8：唯一候选自动连上；两个候选时不连、候选端口高亮；从输出拖线时只高亮兼容的输入。 */
async function suiteAutoConnect(cdp, report) {
  const has = await cdp.eval(`return window.__lyflow.stores.manifest.getState().operatorsById.has('gap.read_scan');`);
  if (!has) {
    report.section("M8b 验收 8（本次构建没有 gap 包，未验）");
    return;
  }
  report.section("M8b 验收 8：自动连线 —— 唯一候选连上；两个候选不连、候选高亮");

  await newDoc(cdp);
  const canvas = await canvasRect(cdp);
  const at = (fx, fy) => ({ x: Math.round(canvas.x + canvas.w * fx), y: Math.round(canvas.y + canvas.h * fy) });
  await dropFromPalette(cdp, '.palette [data-op-id="gap.read_scan"]', at(0.08, 0.15));
  await dropFromPalette(cdp, '.palette [data-op-id="gap.locate_template"]', at(0.38, 0.15));
  let doc = await docOf(cdp);
  const [read1] = nodeByOp(doc, "gap.read_scan").map((n) => n.id);
  const [locate1] = nodeByOp(doc, "gap.locate_template").map((n) => n.id);
  // 唯一候选自动连上由验收 7 的「locate_template.scan 自动接到 read_scan.scan」验过，这里只是垫场

  await dropFromPalette(cdp, '.palette [data-op-id="gap.read_scan"]', at(0.08, 0.55));
  await dropFromPalette(cdp, '.palette [data-op-id="gap.locate_template"]', at(0.38, 0.55));
  doc = await docOf(cdp);
  const read2 = nodeByOp(doc, "gap.read_scan").map((n) => n.id).find((id) => id !== read1);
  const locate2 = nodeByOp(doc, "gap.locate_template").map((n) => n.id).find((id) => id !== locate1);
  report.eq("两个候选（read_scan#2.scan、locate_template#1.scan）：不连", doc.edges.length, 1);
  await sleep(150);
  // locate_template 的输入与输出都叫 scan：按侧挑元素
  const marks = await cdp.eval(`
    const mark = (node, port, side) => document.querySelector(
      '[data-testid="port-' + node + '-' + port + '"].node-port--' + side)?.getAttribute('data-auto-hint') ?? null;
    return {
      target: mark(${lit(locate2)}, 'scan', 'input'),
      read2: mark(${lit(read2)}, 'scan', 'output'),
      locate1: mark(${lit(locate1)}, 'scan', 'output'),
      read1: mark(${lit(read1)}, 'scan', 'output'),
      rois: mark(${lit(locate1)}, 'rois', 'output'),
    };
  `);
  report.eq("没连上的输入高亮成待选", marks.target, "target");
  report.eq("候选 read_scan#2.scan 高亮", marks.read2, "candidate");
  report.eq("候选 locate_template#1.scan 高亮", marks.locate1, "candidate");
  report.eq("已被 locate_template#1 取代的 read_scan#1.scan 不算候选", marks.read1, null);
  report.eq("类型不兼容的输出不亮", marks.rois, null);

  // 从输出拖线：只高亮兼容的输入（Bundle kind 也要对得上）
  await dropFromPalette(cdp, '.palette [data-op-id="gap.role_line"]', at(0.7, 0.35));
  doc = await docOf(cdp);
  const line = nodeByOp(doc, "gap.role_line")[0]?.id;
  const handle = await cdp.eval(`
    const h = document.querySelector('[data-testid="port-${read2}-scan"] .react-flow__handle');
    if (!h) return null;
    const r = h.getBoundingClientRect();
    return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
  `);
  mustOk(handle !== null, "找得到 read_scan#2 的输出端口");
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: handle.x, y: handle.y, buttons: 0 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: handle.x, y: handle.y, button: "left", buttons: 1, clickCount: 1 });
  for (let i = 1; i <= 6; i += 1) {
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: handle.x + i * 12, y: handle.y + i * 9, button: "left", buttons: 1 });
    await sleep(15);
  }
  await sleep(120);
  const verdicts = await cdp.eval(`
    const v = (node, port) => document.querySelector(
      '[data-testid="port-' + node + '-' + port + '"].node-port--input')?.getAttribute('data-port-verdict') ?? null;
    return { scan: v(${lit(line)}, 'scan'), rois: v(${lit(line)}, 'rois'), refLine: v(${lit(line)}, 'refLine'),
             locate2: v(${lit(locate2)}, 'scan') };
  `);
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: handle.x + 72, y: handle.y + 54, button: "left", buttons: 1, clickCount: 1 });
  await sleep(200);
  await pressEscape(cdp);
  report.eq("从 ScanPair 输出拖线：role_line.scan 可落", verdicts.scan, "compatible");
  report.eq("RoiSet 输入置灰", verdicts.rois, "incompatible");
  report.eq("Line2D 输入置灰", verdicts.refLine, "incompatible");
  report.eq("另一个 locate_template.scan 可落", verdicts.locate2, "compatible");
  await cdp.eval(`window.__lyflow.stores.ui.getState().closeSearch(); window.__lyflow.stores.ui.getState().clearAutoHint(); return true;`);
}

/** 算子面板：Tauri 不接管拖放（真窗口里拖得进画布的前提）；右缘分栏可拖宽并记住。
 *  分栏的上下限、松手记宽与右侧分栏是同一个 useDragSplit，P2 验收 9 已经验过，这里不重复。 */
/** m8b 验收遗留的那一条：align_template 的四个框在模板坐标系里，模板是输入端口不是文件 ——
 *  底图取自 tplLeft / tplRight 上游那一次运行的结果。没跑过要说清楚，跑过就画出来。 */
async function suiteAlignTemplateBackdrop(cdp, report, ws) {
  report.section("align_template 拖框：底图取自输入端口 tplLeft / tplRight 的上游结果");
  const scene = makeScene(ws);
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "cloud", op: "gen.synthetic", params: { pointCount: 2000, seed: 7 } },
      {
        key: "tpl",
        op: "gap.load_template",
        params: { dir: scene.templates, left: "left_template.pcd", right: "right_template.pcd" },
      },
      { key: "align", op: "gap.align_template" },
    ],
    [
      { from: ["cloud", "cloud"], to: ["align", "cloud"] },
      { from: ["tpl", "left"], to: ["align", "tplLeft"] },
      { from: ["tpl", "right"], to: ["align", "tplRight"] },
    ],
  );
  // 进拖框：与参数面板 ROI 行的「拖框」同一组动作（选中、点云场景、2D 相机）
  await cdp.eval(`
    const ui = window.__lyflow.stores.ui.getState();
    ui.setSelection([${lit(ids.align)}], []);
    ui.setViewerContentPick({ nodeId: ${lit(ids.align)}, content: 'cloud' });
    ui.setViewerMode('2d');
    return true;
  `);
  const read = () =>
    cdp.eval(`
      const v = document.querySelector('.viewer');
      return {
        roiEdit: Number(v.getAttribute('data-roi-edit')),
        backdrop: Number(v.getAttribute('data-backdrop')),
        bounds: v.getAttribute('data-backdrop-bounds'),
        status: document.querySelector('[data-testid="viewer3d-status"]')?.textContent ?? '',
      };
    `);
  await sleep(400);
  const before = await read();
  report.ok(
    "没跑过：四个框进了拖框，底图为空并说明要先运行一次",
    before.roiEdit === 4 && before.backdrop === 0 && /先运行一次/.test(before.status),
    JSON.stringify(before),
  );

  await runAndWait(cdp, () => cdp.eval(`await window.__lyflow.run({ targets: [${lit(ids.tpl)}] }); return true;`));
  await cdp.waitFor(`Number(document.querySelector('.viewer')?.getAttribute('data-backdrop')) > 0`, {
    timeoutMs: 10_000,
    what: "底图画出来",
  }).catch(() => {});
  const after = await read();
  // 左右模板在测量帧的 x = 0 两侧：底图的包围盒要跨过 0
  const [minX, , , maxX] = (after.bounds ?? "").split(",").map(Number);
  report.ok(
    "跑过上游之后：底图就是左右两片模板（包围盒跨过 x = 0）",
    after.roiEdit === 4 && after.backdrop > 0 && minX < 0 && maxX > 0,
    JSON.stringify(after),
  );
}

async function suitePalette(cdp, report) {
  report.section("算子面板：Tauri 不接管拖放；右缘分栏可拖宽");
  // 合成 DragEvent 绕过了系统那一层，验不出窗口的拖放目标被 Tauri 占着（bridge/README「踩过的坑」）
  const conf = JSON.parse(fs.readFileSync(path.join(ROOT, "bridge", "tauri.conf.json"), "utf8"));
  report.eq("tauri.conf.json 的窗口 dragDropEnabled = false", conf.app?.windows?.[0]?.dragDropEnabled, false);

  const KEY = "lyflow.palette.width";
  const dom = () =>
    cdp.eval(`
      const side = document.querySelector('.app__sidebar').getBoundingClientRect();
      const h = document.querySelector('[data-testid="left-splitter"]')?.getBoundingClientRect();
      return { side: Math.round(side.width), left: Math.round(side.left), stored: localStorage.getItem(${lit(KEY)}),
               handle: h ? { x: Math.round(h.left + h.width / 2), y: Math.round(h.top + h.height / 2) } : null };
    `);
  const d0 = await dom();
  if (!d0.handle) return report.fail("面板右缘有分栏把手", JSON.stringify(d0));
  await dragMouse(cdp, d0.handle, { x: d0.left + d0.side + 120, y: d0.handle.y }, { steps: 10 });
  const d1 = await dom();
  report.ok("真鼠标拖分栏：面板宽了约 120 px 并记进 localStorage",
    Math.abs(d1.side - d0.side - 120) <= 3 && Math.abs(Number(d1.stored) - d1.side) <= 2, `${d0.side} → ${JSON.stringify(d1)}`);
  // 还原：后面的分组按 280 的面板算坐标
  await dragMouse(cdp, d1.handle, { x: d0.left + d0.side, y: d1.handle.y }, { steps: 10 });
  await cdp.eval(`localStorage.removeItem(${lit(KEY)}); return true;`);

  // 从面板拖到一条连线上：插到它中间，加节点与插入一条撤销。以前松在线上照样按类型自动连线 —— 新节点接到上游、
  // 下游还连着原来那条线（两个点云源时干脆不连），要插进去还得自己断线重接
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [{ key: "gen", op: "gen.synthetic", params: { pointCount: 2000 } }, { key: "vox", op: "filter.voxel_grid" }],
    [{ from: ["gen", "cloud"], to: ["vox", "cloud"] }],
  );
  await sleep(250);
  const mid = await cdp.eval(`
    const p = document.querySelector('.react-flow__edge .react-flow__edge-interaction');
    if (!p) return null;
    const pt = p.getPointAtLength(p.getTotalLength() / 2).matrixTransform(p.getScreenCTM());
    return { x: Math.round(pt.x), y: Math.round(pt.y) };
  `);
  mustOk(mid != null, "画布上有 gen → vox 那条线", mid);
  const past0 = await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
  const dropped = await dropFromPalette(cdp, '.palette [data-op-id="filter.passthrough"]', mid);
  await sleep(200);
  const wired = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const pass = g.doc.nodes.find((n) => n.op === 'filter.passthrough')?.id ?? null;
    return { pass, edges: g.doc.edges.map((e) => e.from.node + '.' + e.from.port + '>' + e.to.node + '.' + e.to.port).sort(), past: g.past.length };
  `);
  const want = wired.pass ? [`${ids.gen}.cloud>${wired.pass}.cloud`, `${wired.pass}.cloud>${ids.vox}.cloud`].sort() : null;
  report.ok("从面板拖到 gen → vox 的线上：直通滤波插到中间，加节点与插入一条撤销",
    dropped === "ok" && JSON.stringify(wired.edges) === JSON.stringify(want) && wired.past - past0 === 1,
    JSON.stringify({ dropped, wired, past0 }));
}

/** 主预览的图像模式（docs/image-plan.md 阶段 3）：
 *  图像节点画自己的输出；带像素框的节点（image.crop）画输入那张图、框可拖、一次拖动一条撤销；
 *  只输出像素几何的节点（region_stats 的 bbox）画输入那张图、几何叠在上面。 */
async function suiteImageMainView(cdp, report) {
  report.section("主预览的图像模式：画哪张图、像素框拖动写回参数（可撤销）、像素几何叠在输入图上");
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "img", op: "test.make_image", params: { width: 640, height: 400, channels: 3 } },
      { key: "crop", op: "image.crop", params: { roi: [100, 50, 300, 250] } },
      { key: "gray", op: "image.to_gray" },
      { key: "bin", op: "image.threshold" },
      { key: "stats", op: "image.region_stats" },
    ],
    [
      { from: ["img", "image"], to: ["crop", "image"] },
      { from: ["img", "image"], to: ["gray", "image"] },
      { from: ["gray", "image"], to: ["bin", "image"] },
      { from: ["gray", "image"], to: ["stats", "image"] },
      { from: ["bin", "mask"], to: ["stats", "mask"] },
    ],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "图像链路跑通", JSON.stringify(run.nodes));
  // 主预览就在右栏里，不用开参数面板的预览分栏（setPanelViewerOpen）—— 那是面板的布局状态，
  // 开了不还原会让后面 params_p2 的面板少一截高度

  const pane = async (nodeId) => {
    await select(cdp, nodeId);
    for (let i = 0; i < 80; i += 1) {
      const got = await cdp.eval(`
        const v = document.querySelector('.viewer');
        const p = v && v.getAttribute('data-node') === ${lit(nodeId)}
          ? v.querySelector('[data-testid="viewer-image-pane"]') : null;
        const img = p ? p.querySelector('[data-testid="viewer-image"]') : null;
        if (!p || !img || Number(img.getAttribute('data-w')) === 0) return null;
        const box = p.querySelector('.roi-box');
        const r = box ? box.getBoundingClientRect() : null;
        return {
          view: v.getAttribute('data-view'),
          source: p.getAttribute('data-source'),
          sourceNode: p.getAttribute('data-source-node'),
          shapes: p.querySelectorAll('[data-testid="viewer-image-shapes"] > *').length,
          rois: Number(p.getAttribute('data-rois')),
          box: r ? { x: r.left, y: r.top, w: r.width, h: r.height } : null,
          roi: box ? box.getAttribute('data-roi') : null,
          corner: p.querySelector('[data-testid="viewer3d-status"]')?.textContent ?? null,
        };
      `);
      if (got) return got;
      await sleep(120);
    }
    return null;
  };

  const img = await pane(ids.img);
  report.eq("图像节点：视图是 image、画自己的输出、没有框", img && [img.view, img.source, img.rois], ["image", "output", 0]);

  // 放大之后再跑一次（新的 runId）：同一张图的新结果不重置视角、不卸掉画布
  const canvasWidth = () => cdp.eval(`
    const c = document.querySelector('[data-testid="viewer-image-canvas"]');
    return c ? Math.round(parseFloat(c.style.width)) : null;
  `);
  const stage = await cdp.eval(`
    const r = document.querySelector('[data-testid="viewer-image-pane"] .peek-image__stage').getBoundingClientRect();
    return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
  `);
  const before = await canvasWidth();
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseWheel", x: stage.x, y: stage.y, deltaX: 0, deltaY: -120 });
  await sleep(150);
  const zoomed = await canvasWidth();
  const rerun = await runAndWait(cdp, () => pressF5(cdp));
  await sleep(400);
  const after = await canvasWidth();
  report.ok("放大后再运行一次：缩放保持（同一张图的新结果不重新适配）",
    rerun.status === "ok" && zoomed > before && after === zoomed, JSON.stringify({ before, zoomed, after }));

  const crop = await pane(ids.crop);
  report.eq("image.crop：画输入那张图（上游 img）、一个像素框、框的值就是参数",
    crop && [crop.source, crop.sourceNode, crop.rois, crop.roi], ["input", ids.img, 1, "100,50,300,250"]);
  mustOk(Boolean(crop?.box), "框在画面上", JSON.stringify(crop));
  const scale = crop.box.w / 200; // 屏幕像素 / 原图像素
  const from = { x: Math.round(crop.box.x + crop.box.w / 2), y: Math.round(crop.box.y + crop.box.h / 2) };
  await dragMouse(cdp, from, { x: from.x + 60, y: from.y + 30 }, { steps: 10 });
  const moved = await cdp.eval(`
    return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.crop)}).params.roi;
  `);
  // 平移 60 / 30 个屏幕像素 = 60/scale、30/scale 个原图像素；宽高不变（取整到像素，±1）
  const dx = 60 / scale;
  const dy = 30 / scale;
  report.ok("拖框身：参数整体平移了屏幕位移 ÷ 缩放，宽高不变",
    Array.isArray(moved) && Math.abs(moved[0] - (100 + dx)) <= 1.5 && Math.abs(moved[1] - (50 + dy)) <= 1.5 &&
      moved[2] - moved[0] === 200 && moved[3] - moved[1] === 200,
    JSON.stringify({ moved, dx, dy, scale }));
  await cdp.eval(`window.__lyflow.stores.graph.getState().undo(); return true;`);
  const undone = await cdp.eval(`
    return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(ids.crop)}).params.roi;
  `);
  report.eq("一次拖动一条撤销：撤回到拖之前", undone, [100, 50, 300, 250]);

  // 节点自己出错（框落在图外）：照样画输入那张图、状态缩在角上 —— 这正是要把框拖回来的时候。
  // 先选别的节点（换了底图），免得「上一次的图」替它挡着（review 修正，PR #1）
  const setRoi = (v) => cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.crop)}, 'roi', ${lit(v)}); return true;
  `);
  await setRoi([2000, 2000, 2100, 2100]);
  const bad = await runAndWait(cdp, () => pressF5(cdp));
  await pane(ids.stats);
  const failed = await pane(ids.crop);
  report.ok("crop 出错之后选中它：仍画输入那张图、框在，角上写着出错",
    bad.nodes[ids.crop]?.state === "error" && failed?.source === "input" && failed.rois === 1 &&
      String(failed.corner).includes("出错"),
    JSON.stringify({ state: bad.nodes[ids.crop]?.state, failed }));
  await setRoi([100, 50, 300, 250]);
  mustOk((await runAndWait(cdp, () => pressF5(cdp))).status === "ok", "框改回来之后重跑成功");

  const stats = await pane(ids.stats);
  report.ok("region_stats：画输入那张图（gray），bbox（px）叠在上面",
    stats?.source === "input" && stats.sourceNode === ids.gray && stats.shapes >= 1, JSON.stringify(stats));
}

// 大图预览（docs/large-image-plan.md L3，ADR-0028）：拖参数时的预览运行在源头把大图缩小，下游在小图上算；
// 主预览按原图尺寸摆放、角上写明是预览，像素框照旧是原图坐标 —— 与正式运行时画在同一个屏幕位置
async function suiteImagePreviewScale(cdp, report) {
  report.section("大图预览（ADR-0028）：预览运行把 8.4 MP 的图缩到 1/2，主预览角标「预览 1/2」、按原图尺寸摆放，像素框与正式结果画在同一处");
  await newDoc(cdp);
  // crop 之后再接一个 blur：它的输出（2000×1000）比源头小，正式运行的适配级别是 0、预览时是缩小 1/2 的那张
  const ids = await buildGraph(
    cdp,
    [
      { key: "img", op: "test.make_image", params: { width: 4100, height: 2050, channels: 1 } },
      { key: "crop", op: "image.crop", params: { roi: [1000, 500, 3000, 1500] } },
      { key: "blur", op: "image.blur" },
    ],
    [
      { from: ["img", "image"], to: ["crop", "image"] },
      { from: ["crop", "image"], to: ["blur", "image"] },
    ],
  );
  const full = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(full.status === "ok", "大图链路跑通", JSON.stringify(full.nodes));
  await select(cdp, ids.crop);

  /** 等主预览画的是 pixelScale = want（且一个显示像素 = block 个原图像素）的那张图，读出角标、尺寸、
   *  画布的屏幕尺寸与位置；withBox 时还要框在画面上、读出框的屏幕位置。 */
  const look = async (want, { block = null, withBox = true } = {}) => {
    for (let i = 0; i < 100; i += 1) {
      const got = await cdp.eval(`
        const p = document.querySelector('[data-testid="viewer-image-pane"]');
        const img = p ? p.querySelector('[data-testid="viewer-image"]') : null;
        const box = p ? p.querySelector('.roi-box') : null;
        if (!img || img.getAttribute('data-pixel-scale') !== ${lit(String(want))} || Number(img.getAttribute('data-w')) === 0) return null;
        if (${withBox} && !box) return null;
        if (${lit(block === null ? "" : String(block))} !== '' && img.getAttribute('data-block') !== ${lit(String(block))}) return null;
        if (img.getAttribute('data-stale') === '1') return null;
        const r = box ? box.getBoundingClientRect() : null;
        const c = p.querySelector('[data-testid="viewer-image-canvas"]');
        return {
          badge: p.querySelector('[data-testid="viewer-image-preview-scale"]')?.textContent ?? null,
          fullW: Number(img.getAttribute('data-full-w')),
          block: Number(img.getAttribute('data-block')),
          canvasW: c ? Math.round(parseFloat(c.style.width)) : null,
          at: c ? c.style.transform : null,
          box: r ? [r.left, r.top, r.width, r.height].map((v) => Math.round(v)) : null,
        };
      `);
      if (got) return got;
      await sleep(120);
    }
    return null;
  };
  const drift = (a, b) => (a?.box && b?.box ? Math.max(...a.box.map((v, k) => Math.abs(v - b.box[k]))) : Infinity);
  const wheelIn = async () => {
    const at = await cdp.eval(`
      const r = document.querySelector('[data-testid="viewer-image-pane"] .peek-image__stage').getBoundingClientRect();
      return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
    `);
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseWheel", x: at.x, y: at.y, deltaX: 0, deltaY: -120 });
    await sleep(150);
  };
  const setLevel = (value) => cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-image-level"]');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, ${lit(value)});
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);
  /** 与拖参数时同一条路（lib/preview.ts）：mode = preview、只跑到 node。 */
  const previewTo = (node) => runAndWait(cdp, () =>
    cdp.eval(`void window.__lyflow.run({ targets: [${lit(node)}], preview: true }); return true;`),
  );

  const fitted = await look(1);
  mustOk(fitted !== null && fitted.badge === null, "正式结果：没有预览角标、框在画面上", JSON.stringify(fitted));
  // 先放大一格：视角要在预览 ↔ 正式之间保持（review 修正：修前取图拿新 runId 配旧尺寸，切换时重新适配、放大丢了）
  await wheelIn();
  const before = await look(1);
  mustOk(before !== null && before.canvasW > fitted.canvasW, "滚轮放大了一格", JSON.stringify({ fitted, before }));

  const pv = await previewTo(ids.crop);
  const during = await look(2);
  report.ok("预览运行：主预览画的是缩小 1/2 的图（2050 宽），角标「预览 1/2」",
    pv.status === "ok" && during?.fullW === 2050 && during.badge === "预览 1/2", JSON.stringify({ status: pv.status, during }));
  // 再跑一次正式运行（松手后补的那一次）：回到原图，视角与框还在原处
  const back = await runAndWait(cdp, () => pressF5(cdp));
  const after = await look(1);
  const d1 = drift(during, before);
  const d2 = drift(after, before);
  report.ok(`像素框与放大后的视角在预览与正式之间都保持（框最大偏差 ${d1} / ${d2} px ≤ 1，画布宽度不变 = 没有重新适配）`,
    back.status === "ok" && d1 <= 1 && d2 <= 1 && during.canvasW === before.canvasW && after.canvasW === before.canvasW,
    JSON.stringify({ before, during, after }));

  // 手选的级别按「一个显示像素是几个原图像素」记（review 第二轮）：修前「原图」= 这张图自己的第 0 级，
  // 预览时是块 2、正式时是块 1，两边认不成同一张图 —— 每次切换都闪「正在取图像」、重新适配
  await setLevel("0");
  const orig = await look(1, { block: 1 });
  report.ok("手选「原图」：取的是块 1（4100 宽）、视角不动（画布宽度与框都没变）",
    orig !== null && orig.canvasW === after.canvasW && drift(orig, after) <= 1, JSON.stringify({ after, orig }));
  const pv2 = await previewTo(ids.crop);
  const during2 = await look(2, { block: 2 });
  const back2 = await runAndWait(cdp, () => pressF5(cdp));
  const after2 = await look(1, { block: 1 });
  const d3 = drift(during2, orig);
  const d4 = drift(after2, orig);
  report.ok(`选着「原图」：预览时先显示 1/2 那一级、正式结果回到原图，框最大偏差 ${d3} / ${d4} px ≤ 1、画布宽度不变`,
    pv2.status === "ok" && back2.status === "ok" && during2?.badge === "预览 1/2" && d3 <= 1 && d4 <= 1 &&
      during2.canvasW === orig.canvasW && after2.canvasW === orig.canvasW,
    JSON.stringify({ orig, during2, after2 }));

  // 比源头小的输出：blur 的 2000×1000 正式运行适配级别 0（块 1），预览时是 1000×500（块 2）
  await setLevel("auto");
  await select(cdp, ids.blur);
  const small = await look(1, { block: 1, withBox: false });
  mustOk(small !== null && small.fullW === 2000, "选中 blur：主预览画它的输出（2000 宽、块 1）", JSON.stringify(small));
  await wheelIn();
  const zoomed = await look(1, { block: 1, withBox: false });
  mustOk(zoomed !== null && zoomed.canvasW > small.canvasW, "滚轮放大了一格", JSON.stringify({ small, zoomed }));
  const pv3 = await previewTo(ids.blur);
  const during3 = await look(2, { block: 2, withBox: false });
  const back3 = await runAndWait(cdp, () => pressF5(cdp));
  const after3 = await look(1, { block: 1, withBox: false });
  report.ok("比源头小的输出（2000×1000）：预览 1/2 ↔ 正式之间画布的尺寸与位置都不变（不重新适配）",
    pv3.status === "ok" && back3.status === "ok" && during3?.fullW === 1000 &&
      [during3.canvasW, during3.at] + "" === [zoomed.canvasW, zoomed.at] + "" &&
      [after3?.canvasW, after3?.at] + "" === [zoomed.canvasW, zoomed.at] + "",
    JSON.stringify({ zoomed, during3, after3 }));
}

export const m8bSuites = [
  suiteBuildFromBlank,
  suiteBundlePeek,
  suiteDragWrongSide,
  suiteAlignTemplateBackdrop,
  suiteImageMainView,
  suiteImagePreviewScale,
  suiteAutoConnect,
  suitePalette,
];
