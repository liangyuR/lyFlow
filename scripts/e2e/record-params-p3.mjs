// 配方（docs/param-recipe-plan.md P3）的验收截图：用 CDP 驱动真实的 Tauri app，搭一张「车门缝隙检测」的图、
// 建三个配方，截三张到 docs/：
//   params-p3-toolbar-row.png  工具栏的配方下拉框（当前「车型A·左前门」、有没存的改动 → 橙点），按节点页里被覆盖的行
//   params-p3-matrix.png       配方矩阵：三个配方、三态都在（沿用基础 / 配方的值 / 越界），选中两格
//   params-p3-manage.png       配方管理：选中有失配的「车型B·尾门」，失配报告与建议
// 不接进 run.mjs（截图会改动仓库文件）。图与配方放在临时目录里，跑完删掉。
//
//   LYFLOW_PACKS=gap;dts node scripts/e2e/record-params-p3.mjs     # 自己起 tauri dev
//   LYFLOW_E2E_ATTACH=1 node scripts/e2e/record-params-p3.mjs      # 连到已经开着的实例

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { launchApp, ROOT } from "./harness.mjs";
import { buildGraph, dragMouse, lit, newDoc, pressCtrl, runAndWait, saveGraphTo } from "./page.mjs";

const OUT = path.join(ROOT, "docs");
const LEFT = "车型A·左前门";
const RIGHT = "车型A·右前门";
const TAIL = "车型B·尾门";

async function shot(cdp, name) {
  const { data } = await cdp.send("Page.captureScreenshot", { format: "png" });
  const file = path.join(OUT, name);
  fs.writeFileSync(file, Buffer.from(data, "base64"));
  console.log(`  → ${path.relative(ROOT, file)}（${Math.round(fs.statSync(file).size / 1024)} KB）`);
}

async function main() {
  const work = path.join(os.tmpdir(), `lyflow 配方截图 ${process.pid}`);
  fs.rmSync(work, { recursive: true, force: true });
  fs.mkdirSync(work, { recursive: true });
  const file = path.join(work, "车门缝隙检测.lyflow.json");
  const dir = path.join(work, "车门缝隙检测.recipes");

  const app = await launchApp({ verbose: process.env.LYFLOW_E2E_VERBOSE === "1" });
  const { cdp } = app;
  try {
    await newDoc(cdp);
    await cdp.eval(`
      const ui = window.__lyflow.stores.ui.getState();
      ui.setAutoRun(false);
      ui.setParamPanelMaximized(false);
      ui.toggleParamPanel(false);
      return true;
    `);
    const ids = await buildGraph(
      cdp,
      [
        { key: "gen", op: "gen.synthetic", params: { pointCount: 60000, seed: 3 } },
        { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
        { key: "cut", op: "filter.passthrough", params: { min: -1, max: 1.2 } },
        { key: "sor", op: "filter.statistical_outlier" },
      ],
      [
        { from: ["gen", "cloud"], to: ["voxel", "cloud"] },
        { from: ["voxel", "cloud"], to: ["cut", "cloud"] },
        { from: ["cut", "cloud"], to: ["sor", "cloud"] },
      ],
    );
    await cdp.eval(`
      const g = () => window.__lyflow.stores.graph.getState();
      g().renameNode(${lit(ids.gen)}, '门缝扫描（合成）');
      g().renameNode(${lit(ids.voxel)}, '体素降采样');
      g().renameNode(${lit(ids.cut)}, 'Z 向裁切');
      g().renameNode(${lit(ids.sor)}, '离群点剔除');
      g().promoteToGraphParam(${lit(ids.voxel)}, 'leafSize');
      g().renameGraphParam(g().promoteToGraphParam(${lit(ids.cut)}, 'max'), 'cutMax');
      g().promoteToGraphParam(${lit(ids.cut)}, 'field');
      g().promoteToGraphParam(${lit(ids.sor)}, 'meanK');
      g().promoteToGraphParam(${lit(ids.sor)}, 'stddevMul');
      g().setGraphParamSpec('leafSize', { group: '降采样' });
      g().setGraphParamSpec('cutMax', { group: '裁切', min: 0, max: 5, unit: 'm' });
      g().setGraphParamSpec('field', { group: '裁切' });
      g().setGraphParamSpec('meanK', { group: '去噪' });
      g().setGraphParamSpec('stddevMul', { group: '去噪' });
      g().setName('车门缝隙检测');
      return true;
    `);
    await saveGraphTo(cdp, file);
    await cdp.eval(`
      const g = () => window.__lyflow.stores.graph.getState();
      g().createRecipe(${lit(LEFT)});
      g().createRecipe(${lit(RIGHT)});
      g().setRecipeValue(${lit(LEFT)}, 'leafSize', [0.015, 0.015, 0.015]);
      g().setRecipeValue(${lit(LEFT)}, 'cutMax', 0.8);
      g().setRecipeValue(${lit(RIGHT)}, 'cutMax', 0.95);
      g().setRecipeValue(${lit(RIGHT)}, 'meanK', 45);
      g().setDefaultRecipe(${lit(LEFT)});
      return true;
    `);
    await pressCtrl(cdp, "S");
    await cdp.waitFor(`!window.__lyflow.snapshot().recipes.dirty && !window.__lyflow.snapshot().dirty`, { what: "存盘" });
    // 尾门的配方是「外面来的」：一个越界值、一个类型不对、一个图里已经没有的名字
    const doc = await cdp.eval(`return window.__lyflow.stores.graph.getState().doc;`);
    const digest = await cdp.eval(`return window.__lyflow.recipes.specDigest();`);
    fs.writeFileSync(
      path.join(dir, `${TAIL}.lyflow-recipe.json`),
      `${JSON.stringify({
        schemaVersion: 1,
        name: TAIL,
        graph: { id: doc.id, specDigest: digest },
        values: { leafSize: [0.03, 0.03, 0.03], stddevMul: 150, field: 3, voxelSize: [0.05, 0.05, 0.05] },
        note: "产线 2 号工位导出",
        updatedAt: "2026-09-20T09:30:00.000Z",
      }, null, 2)}\n`,
    );
    const idx = JSON.parse(fs.readFileSync(path.join(dir, "index.json"), "utf8"));
    fs.writeFileSync(path.join(dir, "index.json"), `${JSON.stringify({ ...idx, order: [LEFT, RIGHT, TAIL] }, null, 2)}\n`);
    await newDoc(cdp);
    await cdp.eval(`
      const b = window.__lyflow;
      const loaded = await b.transport.loadGraph(${lit(file)});
      b.stores.graph.getState().loadDoc(loaded.doc, ${lit(file)});
      await b.recipes.loaded();
      return true;
    `);
    await sleep(300);
    await runAndWait(cdp, () => cdp.eval(`await window.__lyflow.run(); return true;`));
    // 一处没存的改动：下拉框上的橙点
    await cdp.eval(`window.__lyflow.stores.graph.getState().setRecipeValue(${lit(LEFT)}, 'stddevMul', 1.5); return true;`);

    // 面板开着、宽 760，按节点页滚到最上面（图参数分组）
    await cdp.eval(`
      try { localStorage.setItem('lyflow.paramPanel.width', '760'); } catch {}
      const ui = window.__lyflow.stores.ui.getState();
      ui.toggleParamPanel(true);
      ui.setParamPanelTab('nodes');
      return true;
    `);
    await sleep(400);
    const width = await cdp.eval(`return Math.round(document.querySelector('[data-testid="right-pane"]').getBoundingClientRect().width);`);
    if (Math.abs(width - 760) > 4) {
      const h = await cdp.eval(`
        const b = document.querySelector('[data-testid="right-splitter"]').getBoundingClientRect();
        return { x: Math.round(b.left + b.width / 2), y: Math.round(b.top + b.height / 2) };
      `);
      await dragMouse(cdp, h, { x: h.x + (width - 760), y: h.y }, { steps: 8 });
    }
    await cdp.eval(`document.querySelector('.react-flow__controls-fitview')?.click(); return true;`);
    await sleep(500);
    await cdp.eval(`window.__lyflow.stores.ui.getState().setSelection([${lit(ids.voxel)}], []); return true;`);
    await sleep(500);
    await cdp.eval(`
      const list = document.querySelector('[data-testid="pp-list"]');
      list.scrollTop = 0;
      return true;
    `);
    await sleep(400);
    await shot(cdp, "params-p3-toolbar-row.png");

    // 矩阵：选中左前门的两格
    await cdp.eval(`window.__lyflow.stores.ui.getState().setParamPanelTab('matrix'); return true;`);
    await sleep(400);
    await cdp.eval(`
      const c = (p, r) => document.querySelector('[data-testid="mx-cell-' + p + '-' + r + '"]');
      c('leafSize', ${lit(LEFT)}).dispatchEvent(new MouseEvent('click', { bubbles: true }));
      return true;
    `);
    await sleep(100);
    await cdp.eval(`
      const c = (p, r) => document.querySelector('[data-testid="mx-cell-' + p + '-' + r + '"]');
      c('cutMax', ${lit(LEFT)}).dispatchEvent(new MouseEvent('click', { bubbles: true, ctrlKey: true }));
      return true;
    `);
    await sleep(300);
    await shot(cdp, "params-p3-matrix.png");

    // 管理：选中尾门，看失配报告
    await cdp.eval(`window.__lyflow.stores.ui.getState().setParamPanelTab('recipes'); return true;`);
    await sleep(300);
    await cdp.eval(`document.querySelector('[data-testid="mg-item"][data-name=${lit(TAIL)}]').click(); return true;`);
    await sleep(400);
    await shot(cdp, "params-p3-manage.png");

    await cdp.eval(`
      const ui = window.__lyflow.stores.ui.getState();
      ui.setParamPanelTab('nodes');
      ui.toggleParamPanel(false);
      window.__lyflow.stores.graph.getState().newDoc();
      return true;
    `);
  } finally {
    await app.close();
    fs.rmSync(work, { recursive: true, force: true });
  }
}

await main();
