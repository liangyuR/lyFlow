// 「运行此节点」按钮的演示录制（docs/node-run-plan.md §6 修订一）：用 CDP 驱动真实的 Tauri app，
// 在一张 5 节点的链式图上依次演示七步，每步截一张画布区域的 PNG 到 docs/noderun-step-N.png（第 7 步 7a / 7b 两张）。
// 本机没有 ffmpeg，所以不合成 mp4 / gif（见 docs/node-run-acceptance.md）。不接进 run.mjs。
//
//   LYFLOW_PACKS=gap;dts node scripts/e2e/record-noderun.mjs          # 自己起 tauri dev
//   LYFLOW_E2E_ATTACH=1 node scripts/e2e/record-noderun.mjs           # 连到已经开着的实例
//
// 截图里看不到两样东西：系统鼠标指针（CDP 截的是页面，不含光标）和原生 title 气泡。
// 所以页面上临时叠两层（演示结束就撤掉）：一个跟着真鼠标走的指针标记，和画面顶部一行字幕 ——
// 字幕写当前是第几步、在做什么，hover 那一步把按钮的 title 原文抄进字幕里。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { launchApp, ROOT } from "./harness.mjs";
import {
  buildGraph,
  canvasBox,
  centerOf,
  lit,
  newDoc,
  placeAtScreen,
  pressF5,
  replan,
  runAndWait,
} from "./page.mjs";

const OUT = path.join(ROOT, "docs");
const btnSel = (id) => `[data-testid="node-run-${id}"]`;

/** 字幕与指针标记。都是 pointer-events: none，不挡真鼠标。 */
const OVERLAY = `
  if (!document.getElementById('__lyDemoCaption')) {
    const cap = document.createElement('div');
    cap.id = '__lyDemoCaption';
    const canvas = document.querySelector('.canvas').getBoundingClientRect();
    cap.style.cssText = 'position:fixed;left:' + (canvas.left + canvas.width / 2) + 'px;top:' + (canvas.top + 8) + 'px;' +
      'transform:translateX(-50%);z-index:2147483647;max-width:' + (canvas.width - 24) + 'px;padding:8px 16px;border-radius:8px;background:rgba(12,14,18,0.88);color:#fff;' +
      'font:600 15px/1.45 "Microsoft YaHei",system-ui,sans-serif;box-shadow:0 4px 18px rgba(0,0,0,.5);' +
      'border:1px solid rgba(74,158,255,.7);pointer-events:none;white-space:pre-wrap;text-align:center';
    document.body.appendChild(cap);
    const cur = document.createElement('div');
    cur.id = '__lyDemoCursor';
    cur.style.cssText = 'position:fixed;left:-100px;top:-100px;z-index:2147483647;width:0;height:0;pointer-events:none;' +
      'border-left:9px solid transparent;border-right:9px solid transparent;border-bottom:20px solid #ffd23f;' +
      'transform:translate(-9px,0) rotate(-28deg);transform-origin:9px 0;filter:drop-shadow(0 1px 2px #000)';
    document.body.appendChild(cur);
  }
  return true;
`;

async function caption(cdp, text) {
  await cdp.eval(`document.getElementById('__lyDemoCaption').textContent = ${lit(text)}; return true;`);
}

async function moveTo(cdp, p) {
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y, buttons: 0 });
  await cdp.eval(`const c = document.getElementById('__lyDemoCursor'); c.style.left = '${p.x}px'; c.style.top = '${p.y}px'; return true;`);
}

async function click(cdp, p, { shift = false, button = "left" } = {}) {
  await moveTo(cdp, p);
  const common = { x: p.x, y: p.y, button, buttons: button === "left" ? 1 : 2, clickCount: 1, modifiers: shift ? 8 : 0 };
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", ...common });
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...common, buttons: 0 });
}

/** 画布缩放到 target 附近：点 React Flow 自带的放大/缩小按钮，每下 1.2 倍。 */
async function zoomTo(cdp, target) {
  for (let i = 0; i < 20; i += 1) {
    const k = await cdp.eval(`return new DOMMatrixReadOnly(getComputedStyle(document.querySelector('.react-flow__viewport')).transform).a;`);
    if (Math.abs(k - target) / target < 0.1) break;
    const sel = k < target ? ".react-flow__controls-zoomin" : ".react-flow__controls-zoomout";
    await cdp.eval(`document.querySelector(${lit(sel)})?.click(); return true;`);
    await sleep(120);
  }
  await sleep(200);
}

/** 只截画布那一块（鼠标、字幕都在里面），1.5 倍像素，文字才看得清。 */
async function shot(cdp, n) {
  await sleep(120);
  const r = await cdp.eval(`const b = document.querySelector('.canvas').getBoundingClientRect(); return { x: b.left, y: b.top, w: b.width, h: b.height };`);
  const { data } = await cdp.send("Page.captureScreenshot", {
    format: "png",
    clip: { x: r.x, y: r.y, width: r.w, height: r.h, scale: 1.5 },
  });
  const file = path.join(OUT, `noderun-step-${n}.png`);
  fs.writeFileSync(file, Buffer.from(data, "base64"));
  console.log(`  第 ${n} 步 → ${path.relative(ROOT, file)}（${Math.round(fs.statSync(file).size / 1024)} KB）`);
}

const titleOf = (cdp, id) =>
  cdp.eval(`return document.querySelector(${lit(btnSel(id))})?.getAttribute('title') ?? '';`);

/** 在页面里逐帧等一个条件，拿到就返回（运行中的瞬间从 Node 侧轮询会错过）。 */
const until = (cdp, expr, timeoutMs = 30_000) =>
  cdp.eval(`
    const t0 = performance.now();
    while (performance.now() - t0 < ${timeoutMs}) {
      if (${expr}) return true;
      await new Promise((r) => requestAnimationFrame(r));
    }
    return false;
  `);

const stateOf = (id) => `document.querySelector('[data-testid="node-${id}"]')?.getAttribute('data-node-state')`;

const waitIdle = (cdp) =>
  cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState(); return s.runStatus !== 'running' && s.runStatus !== 'idle'; })()`,
    { timeoutMs: 120_000, what: "运行结束" },
  );

async function main() {
  const app = await launchApp({});
  const { cdp } = app;
  try {
    await newDoc(cdp);
    // 一条 5 节点的链：合成 → 体素（慢）→ 直通 → 体素 → Reroute。b 是那个「跑得够久」的节点
    const ids = await buildGraph(cdp, [
      { key: "a", op: "gen.synthetic", params: { pointCount: 3_000_000, seed: (Date.now() % 9973) + 1 } },
      { key: "b", op: "filter.voxel_grid", params: { leafSize: [0.0006, 0.0006, 0.0006] } },
      { key: "c", op: "filter.passthrough" },
      { key: "d", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
      { key: "e", op: "util.reroute" },
    ], [
      { from: ["a", "cloud"], to: ["b", "cloud"] },
      { from: ["b", "cloud"], to: ["c", "cloud"] },
      { from: ["c", "cloud"], to: ["d", "cloud"] },
      { from: ["d", "cloud"], to: ["e", "in"] },
    ]);
    // 画布缩放到 1：截图里的节点要看得清字。五个节点排成两行 Z 字（画布只有七八百像素宽）
    await zoomTo(cdp, 1);
    const box = await canvasBox(cdp);
    const x = (f) => Math.round(box.w * f);
    const y = (f) => Math.round(box.h * f);
    await placeAtScreen(cdp, {
      [ids.a]: { x: 24, y: y(0.14) },
      [ids.b]: { x: x(0.36), y: y(0.14) },
      [ids.c]: { x: x(0.69), y: y(0.14) },
      [ids.d]: { x: x(0.36), y: y(0.5) },
      [ids.e]: { x: x(0.69), y: y(0.5) },
    });
    await cdp.eval(OVERLAY);
    await sleep(400);

    console.log("录制「运行此节点」演示：");
    // 1. 全图运行
    await caption(cdp, "① 全图运行（F5）：五个节点依次跑完，每个按钮变成绿圈");
    await runAndWait(cdp, () => pressF5(cdp));
    await replan(cdp);
    await moveTo(cdp, { x: box.x + x(0.5), y: box.y + Math.round(box.h * 0.7) });
    await shot(cdp, 1);

    // 2. 改上游参数，hover 下游按钮
    await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.a)}, 'seed', ${(Date.now() % 7919) + 7}); return true;`);
    await replan(cdp);
    await sleep(250);
    const cBtn = await centerOf(cdp, btnSel(ids.c));
    await moveTo(cdp, cBtn);
    await sleep(350);
    const hoverTitle = await titleOf(cdp, ids.c);
    await caption(cdp, `② 改了上游「合成点云」的 seed，鼠标指到「直通滤波」的按钮上\nhover 提示：${hoverTitle}`);
    await shot(cdp, 2);

    // 3. 单击：上游与本节点依次运行，下游不进计划
    await caption(cdp, "③ 单击：只跑「直通滤波」与它过时的上游（合成 → 体素 → 直通 依次运行），下游不进计划");
    await click(cdp, cBtn);
    await until(cdp, `${stateOf(ids.b)} === 'running'`);
    await sleep(500);
    await shot(cdp, 3);
    await waitIdle(cdp);
    await replan(cdp);

    // 4. 再单击一次：已是最新
    await moveTo(cdp, { x: cBtn.x + 40, y: cBtn.y + 60 });
    await sleep(150);
    await moveTo(cdp, cBtn);
    await sleep(300);
    const upToDate = await titleOf(cdp, ids.c);
    await click(cdp, cBtn);
    await waitIdle(cdp);
    await sleep(300);
    await caption(cdp, `④ 再单击一次：${upToDate}\n节点上标「已缓存」，一个算子都没跑`);
    await shot(cdp, 4);

    // 5. Shift+单击：强制重算
    await caption(cdp, "⑤ Shift+单击：强制重算「直通滤波」—— 上游仍命中缓存，只有它真跑一遍");
    await click(cdp, cBtn, { shift: true });
    await waitIdle(cdp);
    await sleep(300);
    await shot(cdp, 5);

    // 6. 右键菜单三项
    await caption(cdp, "⑥ 右键菜单：运行到此节点（= 单击）/ 强制重算此节点（= Shift+单击）/ 仅此节点（用现有上游）");
    const head = await centerOf(cdp, `[data-testid="node-${ids.c}"] .node__head`);
    await moveTo(cdp, head);
    await cdp.eval(`
      const el = document.querySelector('[data-testid="node-${ids.c}"]');
      el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: ${head.x}, clientY: ${head.y} }));
      return true;
    `);
    await sleep(300);
    const item = await centerOf(cdp, '[data-testid="ctx-force-node"]');
    if (item) await moveTo(cdp, item);
    await sleep(200);
    await shot(cdp, 6);
    await cdp.eval(`document.querySelector('.react-flow__pane')?.click(); return true;`);
    await sleep(200);

    // 7. 运行中点 ■ 停止：Shift+单击慢节点「体素网格」让它真跑，趁它在跑点它自己
    const bBtn = await centerOf(cdp, btnSel(ids.b));
    await click(cdp, bBtn, { shift: true });
    await until(cdp, `document.querySelector(${lit(btnSel(ids.b))})?.getAttribute('data-run-own') === '1'`);
    await sleep(250);
    await caption(cdp, "⑦ 运行中：按钮是进度环 + ■，再点一下 = 停止");
    await shot(cdp, "7a");
    await click(cdp, bBtn);
    await waitIdle(cdp);
    await sleep(300);
    const status = await cdp.eval(`return window.__lyflow.stores.execution.getState().runStatus;`);
    await caption(cdp, `⑦ 点了 ■ 之后：这次运行 ${status}，「体素网格」停在「已取消」`);
    await shot(cdp, "7b");
  } finally {
    await cdp.eval(`
      document.getElementById('__lyDemoCaption')?.remove();
      document.getElementById('__lyDemoCursor')?.remove();
      return true;
    `).catch(() => {});
    await app.close();
  }
}

await main();
