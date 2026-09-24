// 参数面板（docs/param-recipe-plan.md P2）的验收截图：用 CDP 驱动真实的 Tauri app，打开全类型示例图
// examples/param-showcase.lyflow.json，截两张到 docs/：
//   params-p2-panel.png  面板打开、与画布并排、选中「全类型示例」节点，看得到多种类型的控件
//   params-p2-types.png  面板最大化，transform 与 curve 两个新控件在画面里
// 不接进 run.mjs（截图会改动仓库文件）。
//
//   LYFLOW_PACKS=gap;dts node scripts/e2e/record-params-p2.mjs     # 自己起 tauri dev
//   LYFLOW_E2E_ATTACH=1 node scripts/e2e/record-params-p2.mjs      # 连到已经开着的实例（要带 LYFLOW_TEST_OPS=1 启动）

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { launchApp, ROOT } from "./harness.mjs";
import { lit, runAndWait } from "./page.mjs";

const OUT = path.join(ROOT, "docs");
const GRAPH = path.join(ROOT, "examples", "param-showcase.lyflow.json");

async function shot(cdp, name) {
  const { data } = await cdp.send("Page.captureScreenshot", { format: "png" });
  const file = path.join(OUT, name);
  fs.writeFileSync(file, Buffer.from(data, "base64"));
  console.log(`  → ${path.relative(ROOT, file)}（${Math.round(fs.statSync(file).size / 1024)} KB）`);
}

/** 把列表里某一行滚到视口顶上（虚拟化：先一屏一屏翻到它挂上）。 */
async function scrollRowToTop(cdp, selector, offset = 8) {
  return cdp.eval(`
    const frame = () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const list = document.querySelector('[data-testid="pp-list"]');
    list.scrollTop = 0;
    await frame();
    let el = document.querySelector(${lit(selector)});
    for (let i = 0; i < 200 && !el; i += 1) {
      list.scrollTop += list.clientHeight * 0.7;
      await frame();
      el = document.querySelector(${lit(selector)});
    }
    if (!el) return false;
    list.scrollTop += el.getBoundingClientRect().top - list.getBoundingClientRect().top - ${offset};
    await frame();
    return true;
  `);
}

async function main() {
  const app = await launchApp({ verbose: process.env.LYFLOW_E2E_VERBOSE === "1" });
  const { cdp } = app;
  try {
    const ok = await cdp.eval(`return !!window.__lyflow.stores.manifest.getState().operatorsById.get('test.param_showcase');`);
    if (!ok) throw new Error("core 里没有 test.param_showcase —— app 要带 LYFLOW_TEST_OPS=1 启动");

    // 打开示例图（与「打开文件」同一条 transport 路径），跑一遍：ROI 缩略图与 2D 视图要那片云
    await cdp.eval(`
      const b = window.__lyflow;
      const ui = b.stores.ui.getState();
      ui.setParamPanelMaximized(false);
      ui.toggleParamPanel(false);
      ui.setViewerMode('3d');
      const loaded = await b.transport.loadGraph(${lit(GRAPH)});
      b.stores.graph.getState().loadDoc(loaded.doc, ${lit(GRAPH)});
      return true;
    `);
    await sleep(500);
    await runAndWait(cdp, () => cdp.eval(`await window.__lyflow.run(); return true;`));

    await cdp.eval(`
      try { localStorage.setItem('lyflow.paramPanel.width', '700'); } catch {}
      window.__lyflow.stores.ui.getState().toggleParamPanel(true);
      return true;
    `);
    await sleep(400);
    // 宽度在面板挂载时读 localStorage；已经开过的实例里记着别的宽度就拖一下分栏把它摆到 700
    const width = await cdp.eval(`return Math.round(document.querySelector('[data-testid="right-pane"]').getBoundingClientRect().width);`);
    if (Math.abs(width - 700) > 4) {
      const h = await cdp.eval(`
        const b = document.querySelector('[data-testid="right-splitter"]').getBoundingClientRect();
        return { x: Math.round(b.left + b.width / 2), y: Math.round(b.top + b.height / 2) };
      `);
      const { dragMouse } = await import("./page.mjs");
      await dragMouse(cdp, h, { x: h.x + (width - 700), y: h.y }, { steps: 8 });
    }
    // 画布：适配视图，选中「全类型示例」—— 面板滚到它那一节并高亮
    await cdp.eval(`document.querySelector('.react-flow__controls-fitview')?.click(); return true;`);
    await sleep(600);
    await cdp.eval(`window.__lyflow.stores.ui.getState().setSelection(['n_show'], []); return true;`);
    await sleep(700);
    await shot(cdp, "params-p2-panel.png");

    // 最大化：transform 与 curve 在同一屏
    await cdp.eval(`window.__lyflow.stores.ui.getState().setParamPanelMaximized(true); return true;`);
    await sleep(400);
    await scrollRowToTop(cdp, '[data-testid="prow-n_show.pose"]', 4);
    await sleep(400);
    await shot(cdp, "params-p2-types.png");

    await cdp.eval(`
      const ui = window.__lyflow.stores.ui.getState();
      ui.setParamPanelMaximized(false);
      return true;
    `);
  } finally {
    await app.close();
  }
}

await main();
