// M8b 的分组（docs/m8-plan.md §4 验收 7–10）：从空白画布只靠拖入节点（自动连线）、插入片段、
// 2D 视图拖框、改参数建出一个模板路径测点；两个候选时不连、候选高亮；Edge Peek 看 ScanPair 的字段；
// 编辑时把 datum 框拖到错误一侧立即标红。数据是合成的一对剖面，写在临时工作区里。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { ROOT } from "./harness.mjs";
import { dragMouse, lit, newDoc, pressEscape, pressF5, runAndWait, select } from "./page.mjs";

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
  report.eq("拖入 gap.read_scan", await dropFromPalette(cdp, '.palette [data-op-id="gap.read_scan"]',
    { x: canvas.x + 60, y: canvas.y + 80 }), "ok");
  // 2. 拖入模板定位：scan 唯一候选，自动连上
  report.eq("拖入 gap.locate_template", await dropFromPalette(cdp, '.palette [data-op-id="gap.locate_template"]',
    { x: canvas.x + 290, y: canvas.y + 80 }), "ok");
  let doc = await docOf(cdp);
  const read = nodeByOp(doc, "gap.read_scan")[0]?.id;
  const locate = nodeByOp(doc, "gap.locate_template")[0]?.id;
  report.ok("两个节点都落下了", Boolean(read && locate), JSON.stringify(doc.nodes.map((n) => n.op)));
  report.ok(
    "locate_template.scan 自动接到 read_scan.scan",
    doc.edges.length === 1 && doc.edges[0].from.node === read && doc.edges[0].to.node === locate &&
      doc.edges[0].to.port === "scan",
    JSON.stringify(doc.edges),
  );

  // 3. 插入「测点骨架」片段：8 个节点，对外的 8 个输入全都接到 locate_template 那唯一的一对输出上
  report.eq("插入「测点骨架」", await dropFromPalette(cdp, '[data-testid="snippet-gap.measure_skeleton"]',
    { x: canvas.x + 520, y: canvas.y + 40 }), "ok");
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
  report.ok("找得到 read_scan → locate_template 那条 ScanPair 边", typeof edge === "string", String(edge));
  if (!edge) return;
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
  report.eq("边的右键菜单打开了查看器", opened, "ok");
  const win = await cdp.waitFor(
    `(() => { const w = window.__lyflow.snapshot().peek.find((x) => x.edgeId === ${lit(edge)}); return w ?? null; })()`,
    { timeoutMs: 5000, what: "Peek 窗" },
  ).catch(() => null);
  report.ok("开出了一个 Peek 窗", win !== null);
  if (!win) return;
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
  const t0 = Date.now();
  // 挪到 target 旁边（缝的右侧）
  await dragMouse(cdp, g.boxes.template1DatumRoi.center, screenOf(g.map, 12, 160.5), { steps: 10 });
  const bad = await waitValidated(cdp, built.locate, true, 3000);
  const ms = Date.now() - t0;
  report.eq("松手后 locate_template 标红", bad?.invalid, "1");
  report.ok("节点上显示诊断：datum 与 target 落在缝的同一侧", /同一侧/.test(bad?.text ?? ""), JSON.stringify(bad));
  report.ok("诊断指到 template1DatumRoi",
    (bad?.diags ?? []).some((d) => d.paramPath === "template1DatumRoi" && d.code === "bad_param"),
    JSON.stringify(bad?.diags));
  report.ok(`从松手到标红在 3 秒内（${ms} ms，含拖动本身）`, ms < 3000 + 2000);
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
  report.ok("唯一候选：locate_template.scan 自动接到 read_scan.scan",
    doc.edges.length === 1 && doc.edges[0].from.node === read1 && doc.edges[0].to.node === locate1,
    JSON.stringify(doc.edges));

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
  report.ok("找得到 read_scan#2 的输出端口", handle !== null);
  if (handle) {
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
  }
  await cdp.eval(`window.__lyflow.stores.ui.getState().closeSearch(); window.__lyflow.stores.ui.getState().clearAutoHint(); return true;`);
}

export const m8bSuites = [suiteBuildFromBlank, suiteBundlePeek, suiteDragWrongSide, suiteAutoConnect];
