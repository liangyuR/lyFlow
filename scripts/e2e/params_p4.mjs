import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { sleep } from './cdp.mjs';
import {
  buildGraph,
  lit,
  mustOk,
  newDoc,
  pickRecipe,
  clickSelector,
  pressF5,
  runAndWait,
  saveGraphTo,
} from './page.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const RECIPE = '车型A·对拍';

// ------------------------------------------------------------ 小工具


async function saveRecipe(cdp) {
  await cdp.eval(
    `if (document.activeElement instanceof HTMLElement) document.activeElement.blur(); return true;`,
  );
  mustOk((await clickSelector(cdp, '[data-testid="save-button"]')) === 'ok', '点击保存');
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.snapshot(); return !s.dirty && !s.recipes.dirty; })()`,
    { what: '保存完成（图与配方都不脏）', timeoutMs: 20_000 },
  );
  await sleep(150);
}

function cliOutputs(graph, ...args) {
  const profile = process.env.LYFLOW_E2E_PACKAGED === '1' ? 'release' : 'debug';
  const cli = process.env.LYFLOW_CLI ?? path.join(ROOT, 'bridge', 'target', profile, 'lyflow.exe');
  const run = spawnSync(cli, ['run', graph, ...args, '--outputs', '--no-cache'], {
    encoding: 'utf8',
    timeout: 120_000,
  });
  mustOk(run.status === 0, 'CLI 执行成功', run.error?.message ?? run.stderr);
  return JSON.parse(run.stdout.trim().split(/\r?\n/).at(-1));
}

async function suiteCliMatchesEditor(cdp, report, ws) {
  report.section('编辑器与 CLI 使用同一配方，显式图参数可以覆盖配方');
  const dir = fs.mkdtempSync(path.join(ws.dir, '配方对拍-'));
  const graph = path.join(dir, '限值判断.lyflow.json');
  fs.copyFileSync(path.join(ROOT, 'examples', 'p2-expression.lyflow.json'), graph);
  const recipeFile = path.join(dir, '限值判断.recipes', `${RECIPE}.lyflow-recipe.json`);
  const overrides = { leftValue: 20, upperLimit: 16 };

  await newDoc(cdp);
  await cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().setAutoRun(false);
    const file = ${lit(graph)};
    const loaded = await b.transport.loadGraph(file);
    b.stores.graph.getState().loadDoc(loaded.doc, file);
    await b.recipes.loaded();
    return true;
  `);
  const created = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    if (!g.createRecipe(${lit(RECIPE)})) return false;
    for (const [key, value] of Object.entries(${lit(overrides)})) {
      g.setRecipeValue(${lit(RECIPE)}, key, value);
    }
    return true;
  `);
  mustOk(created, '编辑器创建配方');
  mustOk((await pickRecipe(cdp, RECIPE)) === 'ok', '工具栏选中配方');
  await saveRecipe(cdp);
  report.eq('保存的配方只含覆盖值', JSON.parse(fs.readFileSync(recipeFile, 'utf8')).values, overrides);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === 'ok', '编辑器运行完成', run.status);
  const editor = await cdp.eval(`return window.__lyflow.runOutputs(${lit(run.runId)});`);
  const base = cliOutputs(graph);
  const recipe = cliOutputs(graph, '--recipe', recipeFile);
  for (const output of ['mean', 'withinLimit', 'limited']) {
    mustOk(editor[output]?.value?.data != null, `编辑器取得 ${output}`);
    report.eq(`CLI 与编辑器的 ${output} 一致`, recipe[output]?.value, editor[output].value);
  }
  report.ok('配方改变均值并切换阈值判定',
    base.mean.value.data.value !== recipe.mean.value.data.value &&
      base.withinLimit.value.data.value !== recipe.withinLimit.value.data.value);
  const baseLeft = JSON.parse(fs.readFileSync(graph, 'utf8')).params.leftValue.default;
  const pinned = cliOutputs(graph, '--recipe', recipeFile, '--param', `leftValue=${baseLeft}`);
  report.eq('显式参数覆盖配方后恢复基础均值', pinned.mean.value, base.mean.value);
}

// ------------------------------------------------------------ 工具栏宽度

const EIGHT = '车门缝隙检测左前';

/** 对拍分组留下的那种状态，用合成小图搭：图存在中文工作区里、一个图参数纳入了配方、选着配方、跑过一次。
 *  返回接下来改哪个图参数、改成多少（改了就是「已过时 + 将重算 + 配方没存」）。 */
async function crowdedRecipeState(cdp, ws) {
  const dir = path.join(ws.dir, '工具栏宽度');
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  const graphFile = path.join(dir, '合成小图.lyflow.json');
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(false); return true;`);
  const ids = await buildGraph(
    cdp,
    [
      { key: 'gen', op: 'gen.synthetic', params: { pointCount: 4000 } },
      { key: 'voxel', op: 'filter.voxel_grid' },
    ],
    [{ from: ['gen', 'cloud'], to: ['voxel', 'cloud'] }],
  );
  const param = await cdp.eval(
    `return window.__lyflow.stores.graph.getState().promoteToGraphParam(${lit(ids.voxel)}, 'minPointsPerVoxel');`,
  );
  mustOk(typeof param === 'string', 'minPointsPerVoxel 纳入配方（成为图参数）', param);
  await saveGraphTo(cdp, graphFile);
  const made = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    if (!g.createRecipe(${lit(RECIPE)})) return 'create';
    g.setRecipeValue(${lit(RECIPE)}, ${lit(param)}, 2);
    return 'ok';
  `);
  mustOk(made === 'ok', '新建配方并写进一个值', made);
  mustOk((await pickRecipe(cdp, RECIPE)) === 'ok', '工具栏下拉框选中配方');
  await saveRecipe(cdp);
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === 'ok', '合成小图跑过一次', run.status);
  return { param, value: 3 };
}

/** 1280–1440 宽时活动标签完整放下 8 个汉字；工具栏里什么都不溢出、运行区的状态字不被裁、配方下拉框在窗口里。
 *  状态尽量挤：跑过一次（done N）、改过参数（已过时 + 将重算 N 个）、有文件名、选着配方且没存。 */
async function suiteToolbarWidth(cdp, report, ws) {
  report.section('1280–1440 宽的窗口里，活动标签完整显示 8 个汉字，工具栏无溢出');
  const readState = () =>
    cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    return { file: g.filePath, recipe: b.snapshot().recipe?.current ?? null };
  `);
  let state = await readState();
  // 接着对拍分组的状态；它没跑就自己搭一个一样挤的，这一组照样验得到
  let tweak = { param: 'leftValue', value: 21 };
  if (!(state.file && state.recipe === RECIPE)) {
    tweak = await crowdedRecipeState(cdp, ws);
    state = await readState();
  }
  report.ok(
    '前置：图有路径、选着配方、跑过一次',
    Boolean(state.file) && state.recipe === RECIPE,
    JSON.stringify(state),
  );
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.setName(${lit(EIGHT)});
    g.setRecipeValue(${lit(RECIPE)}, ${lit(tweak.param)}, ${tweak.value});
    return true;
  `);
  await cdp.eval(`await window.__lyflow.plan(); return true;`);
  await sleep(300);

  const height = await cdp.eval(`return window.innerHeight;`);
  try {
    for (const width of [1280, 1366, 1440]) {
      await cdp.send('Emulation.setDeviceMetricsOverride', {
        width,
        height,
        deviceScaleFactor: 0,
        mobile: false,
      });
      await sleep(350);
      const m = await cdp.eval(`
        const tb = document.querySelector('.toolbar');
        const name = document.querySelector('[data-testid="flow-tab-select"][aria-selected="true"] .flow-tab__title');
        const run = document.querySelector('.toolbar__group--run');
        const menu = document.querySelector('.recipe-menu');
        const probe = document.createElement('span');
        const cs = getComputedStyle(name);
        probe.style.cssText = 'position:absolute;visibility:hidden;white-space:pre;font:' + cs.font;
        probe.textContent = name.textContent;
        document.body.appendChild(probe);
        const text = probe.getBoundingClientRect().width;
        probe.remove();
        const content = name.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
        return {
          innerWidth, value: name.textContent, text: Math.round(text * 10) / 10, content,
          nameOverflow: name.scrollWidth - name.clientWidth,
          toolbarOverflow: tb.scrollWidth - tb.clientWidth,
          runClipped: run.scrollWidth - run.clientWidth,
          menuRight: Math.round(menu.getBoundingClientRect().right),
          recompute: document.querySelector('[data-testid="recompute-hint"]')?.textContent ?? null,
          stale: !!document.querySelector('.toolbar__stat--stale'),
          dirty: !!document.querySelector('[data-testid="recipe-dirty"]'),
          undoIcon: !!document.querySelector('.toolbar button[aria-label="撤销"] svg'),
        };
      `);
      const at = `${width} 宽`;
      mustOk(m.innerWidth === width, `${at}：视口真的是这个宽度`, m);
      mustOk(
        Boolean(m.recompute && m.stale && m.dirty),
        `${at}：状态够挤（将重算、已过时、配方没存都在）`,
        m,
      );
      report.ok(
        `${at}：活动标签显示完整工作流名「${EIGHT}」`,
        m.value === EIGHT && m.content >= m.text && m.nameOverflow <= 0,
        JSON.stringify(m),
      );
      report.ok(
        `${at}：整条工具栏无溢出无裁切，撤销使用图标，配方下拉框整个在窗口里`,
        m.toolbarOverflow <= 0 && m.runClipped <= 0 && m.undoIcon && m.menuRight <= width,
        JSON.stringify(m),
      );
    }
  } finally {
    await cdp.send('Emulation.clearDeviceMetricsOverride');
    await sleep(200);
  }
}

async function suiteCleanup(cdp) {
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(true); return true;`);
}

export const paramsP4Suites = [suiteCliMatchesEditor, suiteToolbarWi