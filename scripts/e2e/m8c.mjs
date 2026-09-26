// M8c 的分组（docs/m8-plan.md §5 验收 12–14）：多模板的框分开处理。一张三模板的图，2D 视图切到
// 每个槽时恰好画出那个槽的 4 个框；拖槽 2 的框只改槽 2 的参数；「复制到其它槽」；相邻框的标签
// 互不遮挡；槽 3 的 datum 拖到 target 同侧，诊断标明「模板 3」。数据是合成的，写在临时工作区里。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { ROOT } from "./harness.mjs";
import { profile, roiGeometry, screenOf, setCamera, waitValidated, writePcd } from "./m8b.mjs";
import { buildGraph, dragMouse, lit, mustOk, newDoc, select } from "./page.mjs";

/** 三个槽的模板文件（槽 1 用默认文件名，槽 2 / 3 也是各自槽的默认值）与各自在 x 上的错位（毫米）。
 *  错开是为了让三片底图的包围盒不同 —— 切槽之后能断言底图真的换了。 */
const SLOTS = [
  { k: 1, left: "left_template.pcd", right: "right_template.pcd", shift: 0 },
  { k: 2, left: "f2_left.pcd", right: "f2_right.pcd", shift: 2 },
  { k: 3, left: "f3_left.pcd", right: "f3_right.pcd", shift: -2 },
];
const ROLES = ["Datum", "Target", "SeamLeft", "SeamRight"];

/** 每个槽的四框（毫米）：与夹具同形，跟着模板一起错位。 */
function slotBoxes(shift) {
  const at = ([x0, y0, x1, y1]) => [x0 + shift, y0, x1 + shift, y1];
  return {
    Datum: at([-15, 163, -5, 167]),
    Target: at([5, 162, 15, 166]),
    SeamLeft: at([-4.5, 164, -1.5, 167]),
    SeamRight: at([1.5, 163, 4.5, 166]),
  };
}

function makeScene(ws) {
  const root = path.join(ws.dir, "m8c 三模板");
  const point = path.join(root, "point");
  const templates = path.join(root, "StandardGap");
  fs.mkdirSync(point, { recursive: true });
  fs.mkdirSync(templates, { recursive: true });
  const master = profile(0);
  const sensor = (pts) => pts.map(([x, y]) => [x / 1000, 0, y / 1000]);
  writePcd(path.join(point, "LaserProfile_L0_Master_x.pcd"), sensor(master));
  writePcd(path.join(point, "LaserProfile_R1_Slave_x.pcd"), sensor(profile(0.025)));
  for (const s of SLOTS) {
    const side = (left) => master.filter(([x]) => (x < 0) === left).map(([x, y]) => [(x + s.shift) / 1000, y / 1000, 0]);
    writePcd(path.join(templates, s.left), side(true));
    writePcd(path.join(templates, s.right), side(false));
  }
  return { root, point, templates };
}

const paramsOf = (cdp, nodeId) => cdp.eval(`
  return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(nodeId)}).params;
`);

/** 切换条的状态：每个标签页的名字、key、是否选中；当前画了几个框、叫什么、底图包围盒。 */
function frameState(cdp) {
  return cdp.eval(`
    const v = document.querySelector('.viewer');
    const tabs = [...document.querySelectorAll('[data-testid="roi-frame-tab"]')].map((t) => ({
      label: t.textContent.trim(), key: t.getAttribute('data-frame'), active: t.getAttribute('data-active') === '1',
    }));
    const layer = document.querySelector('[data-testid="roi-layer"]');
    const boxes = layer ? [...layer.querySelectorAll('.roi-box')].map((b) => ({
      param: b.getAttribute('data-testid').replace('roi-box-', ''),
      value: b.getAttribute('data-roi').split(',').map(Number),
    })) : [];
    const groups = [...document.querySelectorAll('.insp__group--frame')].map((g) => ({
      name: g.getAttribute('data-testid').replace('inspector-frame-', ''), open: g.getAttribute('data-open') === '1',
      invalid: !!g.querySelector('summary.is-invalid'),
    }));
    return {
      tabs, boxes, groups,
      frame: v?.getAttribute('data-roi-frame') ?? null,
      edit: Number(v?.getAttribute('data-roi-edit') ?? 0),
      layerCount: Number(layer?.getAttribute('data-count') ?? 0),
      backdrop: Number(v?.getAttribute('data-backdrop') ?? 0),
      backdropBounds: v?.getAttribute('data-backdrop-bounds') ?? null,
      mapped: !!layer?.getAttribute('data-map'),
    };
  `);
}

/** 真实鼠标点一下切换条上的第 i 个标签页，等视图换到那一组、底图也读完。 */
async function clickTab(cdp, index) {
  const pt = await cdp.eval(`
    const t = document.querySelectorAll('[data-testid="roi-frame-tab"]')[${index}];
    if (!t) return null;
    const r = t.getBoundingClientRect();
    return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2), key: t.getAttribute('data-frame') };
  `);
  if (!pt) return null;
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: pt.x, y: pt.y, buttons: 0 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: pt.x, y: pt.y, button: "left", buttons: 1, clickCount: 1 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: pt.x, y: pt.y, button: "left", buttons: 1, clickCount: 1 });
  await cdp.waitFor(
    `(() => { const v = document.querySelector('.viewer');
              return v?.getAttribute('data-roi-frame') === ${lit(pt.key)} && Number(v.getAttribute('data-backdrop')) > 0
                && document.querySelector('[data-testid="roi-layer"]')?.getAttribute('data-map'); })()`,
    { timeoutMs: 15_000, what: `切到第 ${index + 1} 个槽` },
  );
  await sleep(250);
  return pt.key;
}

const expectedParams = (k) => ROLES.map((r) => `template${k}${r}Roi`).sort();

/** 两个矩形（屏幕像素）相交。 */
const intersects = (a, b) => a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;

let built = null;

/** §5 验收 12：切槽恰好 4 个框、拖槽 2 只改槽 2、复制到其它槽。 */
async function suiteSlotSwitch(cdp, report, ws) {
  const has = await cdp.eval(`return window.__lyflow.stores.manifest.getState().operatorsById.get('gap.locate_template')?.version ?? null;`);
  if (!has) {
    report.section("M8c 验收 12（本次构建没有 gap 包，未验）");
    return;
  }
  report.section("M8c 验收 12：三模板的图 —— 切槽各 4 个框、拖槽 2 只改槽 2、复制到其它槽");

  const scene = makeScene(ws);
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.peek.getState().closeAll(); window.__lyflow.stores.ui.getState().setPinnedNode(null); return true;`);
  const locateParams = { templateDir: scene.templates, template2Enabled: true, template3Enabled: true };
  for (const s of SLOTS) {
    for (const [role, box] of Object.entries(slotBoxes(s.shift))) locateParams[`template${s.k}${role}Roi`] = box;
  }
  const ids = await buildGraph(cdp, [
    { key: "read", op: "gap.read_scan", params: { dir: scene.point } },
    { key: "locate", op: "gap.locate_template", params: locateParams },
  ], [{ from: ["read", "scan"], to: ["locate", "scan"] }]);
  const locate = ids.locate;
  const clean = await waitValidated(cdp, locate, false);
  report.eq("三个槽的框都对：校验干净", clean?.invalid, "0");

  await select(cdp, locate);
  await setCamera(cdp, "2d");
  await cdp.waitFor(
    `(() => { const v = document.querySelector('.viewer');
              return v && Number(v.getAttribute('data-backdrop')) > 0 && Number(v.getAttribute('data-roi-edit')) > 0
                && document.querySelector('[data-testid="roi-layer"]')?.getAttribute('data-map'); })()`,
    { timeoutMs: 20_000, what: "模板底图与可拖框" },
  );
  let st = await frameState(cdp);
  report.eq("切换条列出三个启用的槽", st.tabs.map((t) => t.label), ["模板 1 · f1", "模板 2 · f2", "模板 3 · f3"]);
  report.ok("默认选中槽 1", st.tabs[0]?.active === true && st.frame === st.tabs[0]?.key, JSON.stringify(st.tabs));

  const bounds = new Set();
  for (let i = 0; i < SLOTS.length; i += 1) {
    const s = SLOTS[i];
    await clickTab(cdp, i);
    st = await frameState(cdp);
    // 恰好 4 个框（DOM / 拖框层 / 视图三处计数）、是它自己的四个参数、值就是这个槽的值：并成一条，
    // detail 写明哪一部分不对
    const want = slotBoxes(s.shift);
    const params = st.boxes.map((b) => b.param).sort();
    const wrong = [];
    if (!(st.boxes.length === 4 && st.layerCount === 4 && st.edit === 4)) {
      wrong.push(`框数 boxes=${st.boxes.length} layer=${st.layerCount} edit=${st.edit}`);
    }
    if (JSON.stringify(params) !== JSON.stringify(expectedParams(s.k))) wrong.push(`参数 ${JSON.stringify(params)}`);
    if (!st.boxes.every((b) => JSON.stringify(b.value) === JSON.stringify(want[ROLES.find((r) => b.param === `template${s.k}${r}Roi`)]))) {
      wrong.push(`值 ${JSON.stringify(st.boxes.map((b) => [b.param, b.value]))}`);
    }
    report.ok(`切到模板 ${s.k}：恰好 4 个框，画的是这个槽自己的四个参数和值`, wrong.length === 0, wrong.join("; "));
    report.ok(`切到模板 ${s.k}：标签页高亮在它上面`, st.tabs[i]?.active === true && st.tabs.filter((t) => t.active).length === 1);
    report.ok(`切到模板 ${s.k}：Inspector 里只有「模板槽 ${s.k}」这一节展开`,
      st.groups.filter((g) => g.open).map((g) => g.name).join() === `模板槽 ${s.k}`, JSON.stringify(st.groups));
    bounds.add(st.backdropBounds);

    if (s.k === 2) {
      const shotPath = process.env.LYFLOW_E2E_M8C_SHOT;
      if (shotPath) {
        const clip = await cdp.eval(`
          const r = document.querySelector('.viewer').getBoundingClientRect();
          return { x: r.left, y: r.top, width: r.width, height: r.height, scale: 1 };
        `);
        const shot = await cdp.send("Page.captureScreenshot", { format: "png", clip });
        const target = path.isAbsolute(shotPath) ? shotPath : path.join(ROOT, shotPath);
        fs.writeFileSync(target, Buffer.from(shot.data, "base64"));
        console.log(`  （槽 2 的 2D 视图截图写到了 ${target}）`);
      }
    }
  }
  report.eq("三个槽的底图各不相同（换槽就换了模板云）", bounds.size, 3);

  // 拖槽 2 的 datum：只改槽 2 的参数
  await clickTab(cdp, 1);
  const before = await paramsOf(cdp, locate);
  let g = await roiGeometry(cdp);
  const d2 = g.boxes.template2DatumRoi;
  const cx = (d2.value[0] + d2.value[2]) / 2;
  const cy = (d2.value[1] + d2.value[3]) / 2;
  await dragMouse(cdp, d2.center, screenOf(g.map, cx + 1.5, cy - 0.5), { steps: 10 });
  await sleep(200);
  const after = await paramsOf(cdp, locate);
  const changed = Object.keys({ ...before, ...after }).filter((k) => JSON.stringify(before[k]) !== JSON.stringify(after[k]));
  report.eq("拖槽 2 的 datum：只有 template2DatumRoi 变了", changed, ["template2DatumRoi"]);
  const moved = after.template2DatumRoi.map((v, i) => v - before.template2DatumRoi[i]);
  report.ok("拖动量对得上（x +1.5、y −0.5 mm，误差 < 0.35）",
    Math.abs(moved[0] - 1.5) < 0.35 && Math.abs(moved[2] - 1.5) < 0.35 && Math.abs(moved[1] + 0.5) < 0.35,
    JSON.stringify(moved));

  // 复制到其它槽：其余启用槽的四框与当前槽（槽 2）相同，一条撤销
  const src = Object.fromEntries(ROLES.map((r) => [r, after[`template2${r}Roi`]]));
  const copyAt = await cdp.eval(`
    const b = document.querySelector('[data-testid="roi-copy-frame"]');
    if (!b || b.disabled) return null;
    const r = b.getBoundingClientRect();
    return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
  `);
  mustOk(copyAt !== null, "切换条上有「复制到其它槽」且可点");
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: copyAt.x, y: copyAt.y, buttons: 0 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: copyAt.x, y: copyAt.y, button: "left", buttons: 1, clickCount: 1 });
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: copyAt.x, y: copyAt.y, button: "left", buttons: 1, clickCount: 1 });
  await sleep(200);
  const copied = await paramsOf(cdp, locate);
  for (const k of [1, 3]) {
    report.ok(`复制之后模板 ${k} 的四框与模板 2 相同`,
      ROLES.every((r) => JSON.stringify(copied[`template${k}${r}Roi`]) === JSON.stringify(src[r])),
      JSON.stringify(ROLES.map((r) => copied[`template${k}${r}Roi`])));
  }
  report.ok("模板 2 自己没变", ROLES.every((r) => JSON.stringify(copied[`template2${r}Roi`]) === JSON.stringify(src[r])));
  await clickTab(cdp, 2);
  st = await frameState(cdp);
  report.ok("切到模板 3 看：画的就是复制过来的四个框",
    st.boxes.length === 4 && st.boxes.every((b) => JSON.stringify(b.value) ===
      JSON.stringify(src[ROLES.find((r) => b.param === `template3${r}Roi`)])), JSON.stringify(st.boxes));
  const undone = await cdp.eval(`
    window.__lyflow.stores.graph.getState().undo();
    return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(locate)}).params;
  `);
  report.ok("撤销一次就回到复制之前（整次复制是一条撤销）",
    JSON.stringify(undone.template1DatumRoi) === JSON.stringify(after.template1DatumRoi) &&
      JSON.stringify(undone.template3SeamRightRoi) === JSON.stringify(after.template3SeamRightRoi),
    JSON.stringify([undone.template1DatumRoi, undone.template3SeamRightRoi]));

  // 关掉槽 3：切换条只剩两个
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(locate)}, 'template3Enabled', false); return true;`);
  await sleep(250);
  st = await frameState(cdp);
  report.eq("关掉槽 3 之后切换条只列两个槽", st.tabs.map((t) => t.label), ["模板 1 · f1", "模板 2 · f2"]);
  report.eq("所选的槽被关掉就退回槽 1", st.frame, st.tabs[0]?.key);
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(locate)}, 'template3Enabled', true); return true;`);
  await sleep(250);
  built = { locate };
}

/** §5 验收 13：两个框相邻时，它们的标签包围盒不相交。 */
async function suiteLabelsApart(cdp, report) {
  if (!built) return;
  report.section("M8c 验收 13：相邻两个框的角色标签互不遮挡");
  await select(cdp, built.locate);
  await setCamera(cdp, "2d");
  await clickTab(cdp, 0);
  // 截图里的情形：Seam Right 窄、右边紧挨着 Target（间隔 0.1 mm），标签比 Seam Right 的框宽
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.setParam(${lit(built.locate)}, 'template1SeamRightRoi', [1.5, 163, 4.5, 166]);
    window.__lyflow.stores.graph.getState().setParam(${lit(built.locate)}, 'template1TargetRoi', [4.6, 162.5, 15, 166]);
    return true;
  `);
  await sleep(400);
  const m = await cdp.eval(`
    const rect = (el) => { const r = el.getBoundingClientRect(); return { left: r.left, top: r.top, right: r.right, bottom: r.bottom }; };
    const out = {};
    for (const name of ['template1DatumRoi', 'template1TargetRoi', 'template1SeamLeftRoi', 'template1SeamRightRoi']) {
      const box = document.querySelector('[data-testid="roi-box-' + name + '"]');
      const label = document.querySelector('[data-testid="roi-label-' + name + '"]');
      if (!box || !label) return null;
      out[name] = { box: rect(box), label: rect(label), pos: label.getAttribute('data-pos'), text: label.textContent };
    }
    return out;
  `);
  mustOk(m !== null, "四个框和它们的标签都在");
  const sr = m.template1SeamRightRoi;
  const tg = m.template1TargetRoi;
  report.ok("Seam Right 与 Target 两个框是相邻的（屏幕上间隔 < 4 px）",
    tg.box.left - sr.box.right < 4 && tg.box.left >= sr.box.right - 1, JSON.stringify([sr.box, tg.box]));
  // 标签都放在框上方（老的摆法）会不会压住：证明这条断言不是白给的
  const naive = (b, l) => ({ left: b.left - 1, right: b.left - 1 + (l.right - l.left), top: b.top - (l.bottom - l.top), bottom: b.top });
  report.ok("对照：两个标签都放在框上方的话会相交", intersects(naive(sr.box, sr.label), naive(tg.box, tg.label)),
    JSON.stringify([naive(sr.box, sr.label), naive(tg.box, tg.label)]));
  // Seam Right 与 Target 这一对也在下面的两两检查里
  const names = Object.keys(m);
  const pairs = [];
  for (let i = 0; i < names.length; i += 1) {
    for (let j = i + 1; j < names.length; j += 1) {
      if (intersects(m[names[i]].label, m[names[j]].label)) pairs.push(`${names[i]}×${names[j]}`);
    }
  }
  report.eq("这一组四个标签两两都不相交", pairs, []);
  await cdp.eval(`window.__lyflow.stores.graph.getState().undo(); window.__lyflow.stores.graph.getState().undo(); return true;`);
}

/** §5 验收 14：把槽 3 的 datum 拖到 target 同侧，诊断标明「模板 3」。 */
async function suiteSlot3WrongSide(cdp, report) {
  if (!built) return;
  report.section("M8c 验收 14：槽 3 的 datum 拖到 target 同侧 → 诊断标明「模板 3」");
  await select(cdp, built.locate);
  await setCamera(cdp, "2d");
  const clean = await waitValidated(cdp, built.locate, false);
  mustOk(clean?.invalid === "0", "开始时校验干净", JSON.stringify(clean));
  await clickTab(cdp, 2);
  const g = await roiGeometry(cdp);
  const shift = SLOTS[2].shift;
  await dragMouse(cdp, g.boxes.template3DatumRoi.center, screenOf(g.map, 12 + shift, 160.5), { steps: 10 });
  // 标红与「同一侧」的措辞由 M8b 验收 10 验过；这里要的是诊断落在槽 3 上并写明「模板 3」
  const bad = await waitValidated(cdp, built.locate, true, 3000);
  const diag = (bad?.diags ?? []).find((d) => d.paramPath === "template3DatumRoi");
  report.ok("诊断指到 template3DatumRoi", diag !== undefined, JSON.stringify(bad?.diags));
  report.ok("诊断与节点上贴的诊断都标明「模板 3」",
    /模板 3/.test(diag?.message ?? "") && /模板 3/.test(bad?.text ?? ""),
    `诊断=${diag?.message ?? ""} 节点=${bad?.text ?? ""}`);
  report.ok("其它槽没被牵连（诊断只有这一条）", (bad?.diags ?? []).length === 1, JSON.stringify(bad?.diags));
  const st = await frameState(cdp);
  report.ok("Inspector 里「模板槽 3」这一节标红", st.groups.some((x) => x.name === "模板槽 3" && x.invalid),
    JSON.stringify(st.groups));
  await cdp.eval(`window.__lyflow.stores.graph.getState().undo(); return true;`);
  // 等撤销后的校验落定再切回 3D（撤销回到干净由 M8b 验收 10 验过）
  await waitValidated(cdp, built.locate, false, 3000);
  await setCamera(cdp, "3d");
}

export const m8cSuites = [suiteSlotSwitch, suiteLabelsApart, suiteSlot3WrongSide];
