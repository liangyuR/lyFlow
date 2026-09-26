// 配方（docs/param-recipe-plan.md P3）的分组：验收 17–24。逐条结果见 docs/param-recipe-p3-acceptance.md。
//
// 17 新建两个配方、各改几项、Ctrl+S：磁盘上两个配方文件与 index.json（只存与基础不同的值），图文件同时保存；
//    重开后下拉框（默认配方）、矩阵、值都还原。
// 18 K6 ① ② ③：改已纳入的参数只有当前配方文件变；改没纳入的图变、行上提示，「改为只在本配方生效」；
//    切走再切回来未保存的改动还在；保存前发现文件被外部修改 → 提示覆盖 / 重新载入。
// 19 K4：A 覆盖 x、B 覆盖 y，A → B 之后 x 回到基础。
// 20 切换配方后运行：受影响的节点重算、其余命中缓存；切回原配方全部命中缓存；「将重算 N 个」跟着变；自动运行照常触发。
// 21 K7：配方值的改动能 Ctrl+Z / Ctrl+Y；切换配方不产生撤销记录。
// 22 矩阵：三态、只显示差异、单元格编辑写对配方、多选复制到另一配方、隐藏列、越界为红且运行被阻止并给出原因。
// 23 失配：手工构造的配方文件分别触发四类，条目与建议正确；「全部按建议修复」后能运行，一次撤销还原。
// 24 管理：新建、复制、重命名、删除、设为默认、导入（重名处理）、导出，各看磁盘结果；自动备份写与恢复。
//
// 配方目录是图文件旁边的 <图名>.recipes/：每组用自己的图名，开始前把上一轮的目录删掉。
// 对话框（起名、确认删除、外部修改的三选一）是编辑器自己画的，照常点；导入导出在界面上要弹系统文件对话框，
// 脚本经窗口桥直接给路径（window.__lyflow.recipes）。Ctrl+S 之前图必须已经有路径（saveGraphTo），否则会弹另存为。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { buildGraph, lit, mustOk, newDoc, pressCtrl, pressF5, replan, runAndWait, saveGraphTo } from "./page.mjs";

// ------------------------------------------------------------ 小工具

const snap = (cdp) => cdp.eval(`return window.__lyflow.snapshot();`);
/** 当前配方与取值（snapshot().recipe）合上配方集合（snapshot().recipes）。 */
const recipeState = async (cdp) => {
  const s = await snap(cdp);
  return { ...s.recipe, ...s.recipes };
};
const docOf = (cdp) => cdp.eval(`return window.__lyflow.stores.graph.getState().doc;`);
const valuesOf = async (cdp, name) => (await recipeState(cdp)).recipes.find((r) => r.name === name)?.values ?? null;
const readJson = (file) => JSON.parse(fs.readFileSync(file, "utf8"));
const listDir = (dir) => (fs.existsSync(dir) ? fs.readdirSync(dir).sort() : []);
const rowSel = (key) => `[data-testid="prow-${key}"]`;

/** 返回 'ok' 的界面动作（打字、点按钮、对话框）当前提：不是 'ok' 就中断这一组，原因写明卡在哪。 */
async function must(what, action) {
  const r = await action;
  mustOk(r === "ok", what, r);
}

async function openPanel(cdp, tab) {
  await cdp.eval(`
    const ui = window.__lyflow.stores.ui.getState();
    ui.toggleParamPanel(true);
    ui.setParamPanelTab(${lit(tab)});
    return true;
  `);
  await sleep(250);
}

async function closePanel(cdp) {
  await cdp.eval(`
    const ui = window.__lyflow.stores.ui.getState();
    ui.setParamPanelTab('nodes');
    ui.setParamPanelMaximized(false);
    ui.toggleParamPanel(false);
    return true;
  `);
}

/** 虚拟化列表里把某一行滚到挂上为止（同 params_p2.mjs 的 reveal）。 */
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
      for (let i = 0; i < 200 && !(el = find()); i += 1) {
        if (list.scrollTop + list.clientHeight >= list.scrollHeight - 1) break;
        list.scrollTop += Math.max(80, list.clientHeight * 0.7);
        await frame();
      }
    }
    if (!el) return null;
    el.scrollIntoView({ block: 'center' });
    await frame();
    return true;
  `);
}

/** 在一个元素（或它里面第 index 个 input）里「打字」再失焦：原生 setter + input 事件，失焦才提交。 */
async function typeInto(cdp, selector, text, { index = 0, inList = true } = {}) {
  if (inList && !(await reveal(cdp, selector))) return "no-row";
  return cdp.eval(`
    const host = document.querySelector(${lit(selector)});
    const el = host?.matches('input, textarea') ? host : host?.querySelectorAll('input, textarea')[${index}];
    if (!el) return 'no-input';
    el.focus();
    const proto = el instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, ${lit(text)});
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.blur();
    await new Promise((d) => setTimeout(d, 120));
    return 'ok';
  `);
}

async function click(cdp, selector, { inList = false } = {}) {
  if (inList && !(await reveal(cdp, selector))) return "no-row";
  return cdp.eval(`
    const el = document.querySelector(${lit(selector)});
    if (!el) return 'missing';
    el.click();
    await new Promise((d) => setTimeout(d, 150));
    return 'ok';
  `);
}

/** 编辑器自己的起名对话框：填名字、点确定。返回 'ok' 或卡在哪。 */
async function answerText(cdp, value, { check = null } = {}) {
  await cdp.waitFor(`!!document.querySelector('[data-testid="modal"][data-kind="text"]')`, { what: "起名对话框" });
  const r = await typeInto(cdp, '[data-testid="modal-input"]', value, { inList: false });
  if (r !== "ok") return r;
  if (check !== null) {
    await cdp.eval(`
      const c = document.querySelector('[data-testid="modal-check"]');
      if (c && c.checked !== ${check}) c.click();
      return true;
    `);
  }
  await sleep(80);
  const ok = await click(cdp, '[data-testid="modal-ok"]');
  await sleep(150);
  return ok;
}

async function answerChoice(cdp, id) {
  await cdp.waitFor(`!!document.querySelector('[data-testid="modal"][data-kind="choice"]')`, { what: "选择对话框" });
  return click(cdp, `[data-testid="modal-choice-${id}"]`);
}

/** 工具栏下拉框里点一项（名字 "" = 基础）。 */
async function pickRecipe(cdp, name) {
  await click(cdp, '[data-testid="recipe-toggle"]');
  const r = await click(cdp, `[data-testid="recipe-option"][data-name=${lit(name)}]`);
  await sleep(120);
  return r;
}

async function newRecipeFromMenu(cdp, name, { copy = false } = {}) {
  await click(cdp, '[data-testid="recipe-toggle"]');
  await click(cdp, '[data-testid="recipe-new"]');
  return answerText(cdp, name, { check: copy });
}

async function blurActive(cdp) {
  await cdp.eval(`if (document.activeElement instanceof HTMLElement) document.activeElement.blur(); return true;`);
}

/** 真按 Ctrl+S，等图与配方都存完（两个脏标记都落下）。 */
async function saveByKey(cdp) {
  await blurActive(cdp);
  await pressCtrl(cdp, "S");
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.snapshot(); return !s.dirty && !s.recipes.dirty; })()`,
    { what: "Ctrl+S 存完（图与配方都不脏）", timeoutMs: 20_000 },
  );
  await sleep(150);
}

async function undoByKey(cdp) {
  await blurActive(cdp);
  await pressCtrl(cdp, "Z");
  await sleep(200);
}

async function redoByKey(cdp) {
  await blurActive(cdp);
  await pressCtrl(cdp, "Y");
  await sleep(200);
}

/** 按 F5，等这次运行被拦下（配方有失配：startRun 直接 failRun，不会有 runId）。返回执行 store 的 error。 */
async function f5Blocked(cdp) {
  await cdp.eval(`window.__lyflow.stores.execution.getState().reset(); return true;`);
  await pressF5(cdp);
  await cdp.waitFor(`window.__lyflow.stores.execution.getState().runStatus === 'error'`, { what: "运行被拦下" });
  return cdp.eval(`return window.__lyflow.stores.execution.getState().error;`);
}

/** 经 transport 读图、loadDoc（与「打开文件」同一条路：订阅跟着读配方目录），等配方读完。 */
async function reopen(cdp, file) {
  await cdp.eval(`
    const b = window.__lyflow;
    const loaded = await b.transport.loadGraph(${lit(file)});
    b.stores.graph.getState().loadDoc(loaded.doc, ${lit(file)});
    await b.recipes.loaded();
    return true;
  `);
  await sleep(250);
}

/** 这几组共用的图：gen → 体素 → 直通，gen → 统计离群（另一条支路，切配方时它该一直命中缓存）。
 *  leafSize、直通的 max（改名 cutMax）、meanK 纳入配方；存盘到工作区（有路径才能建配方）。 */
async function recipeGraph(cdp, ws, name, { seed = 7 } = {}) {
  const dir = path.join(ws.dir, `${name}.recipes`);
  fs.rmSync(dir, { recursive: true, force: true });
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(false); return true;`);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed } },
      { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
      { key: "cut", op: "filter.passthrough", params: { min: -1, max: 1.2 } },
      { key: "sor", op: "filter.statistical_outlier", row: 1 },
    ],
    [
      { from: ["gen", "cloud"], to: ["voxel", "cloud"] },
      { from: ["voxel", "cloud"], to: ["cut", "cloud"] },
      { from: ["gen", "cloud"], to: ["sor", "cloud"] },
    ],
  );
  await cdp.eval(`
    const g = () => window.__lyflow.stores.graph.getState();
    if (g().promoteToGraphParam(${lit(ids.voxel)}, 'leafSize') !== 'leafSize') throw new Error('纳入 leafSize 失败');
    const m = g().promoteToGraphParam(${lit(ids.cut)}, 'max');
    if (!g().renameGraphParam(m, 'cutMax')) throw new Error('改名 cutMax 失败');
    if (g().promoteToGraphParam(${lit(ids.sor)}, 'meanK') !== 'meanK') throw new Error('纳入 meanK 失败');
    return true;
  `);
  const file = path.join(ws.dir, `${name}.lyflow.json`);
  await saveGraphTo(cdp, file);
  await sleep(150);
  return { ids, file, dir };
}

const recipeFile = (dir, name) => path.join(dir, `${name}.lyflow-recipe.json`);

// ------------------------------------------------------------ 验收 17

const A = "车型A·左前门";
const B = "车型B·尾门";

async function suiteCreateSaveReopen(cdp, report, ws) {
  report.section("P3 验收 17：新建两个配方、各改几项、Ctrl+S → 两个配方文件 + index.json，图同时保存；重开后都还原");
  const { ids, file, dir } = await recipeGraph(cdp, ws, "车门缝隙 17");

  await must("起名对话框：新建配方 A", newRecipeFromMenu(cdp, A));
  await openPanel(cdp, "nodes");
  // 在按节点页被绑定的行上打字：选着配方 → 写进配方（K6 ①）
  await must("A：leafSize 第一个分量改成 0.03", typeInto(cdp, rowSel(`${ids.voxel}.leafSize`), "0.03"));
  await must("A：cutMax 改成 2.5", typeInto(cdp, `[data-testid="pp-gp-cutMax"]`, "2.5"));
  await must("起名对话框：新建配方 B", newRecipeFromMenu(cdp, B));
  await must("B：meanK 改成 40", typeInto(cdp, rowSel(`${ids.sor}.meanK`), "40"));
  // 在 B 里把 cutMax 改成与基础相同的值：等于基础 = 不存
  await must("B：cutMax 写成与基础相同的 1.2", typeInto(cdp, `[data-testid="pp-gp-cutMax"]`, "1.2"));
  const doc0 = await docOf(cdp);
  report.eq("default 一个都没动", [doc0.params.leafSize.default, doc0.params.cutMax.default, doc0.params.meanK.default], [[0.02, 0.02, 0.02], 1.2, 30]);

  // 设 B 为默认（管理页的按钮），然后真按 Ctrl+S
  await openPanel(cdp, "recipes");
  await click(cdp, `[data-testid="mg-item"][data-name=${lit(B)}]`);
  await must("管理页：设 B 为默认", click(cdp, '[data-testid="mg-default"]'));
  const before = await snap(cdp);
  const dot = await cdp.eval(`return !!document.querySelector('[data-testid="recipe-dirty"]');`);
  report.ok("存盘前：图与配方合并的脏标记亮着（工具栏 ● 与下拉框的橙色小圆点）", (before.dirty || before.recipes.dirty) && dot,
    JSON.stringify({ dirty: before.dirty, recipesDirty: before.recipes.dirty, dot }));
  await saveByKey(cdp);

  const files = listDir(dir);
  report.eq("磁盘：配方目录里两个配方文件 + index.json", files, [`${A}.lyflow-recipe.json`, `${B}.lyflow-recipe.json`, "index.json"].sort());
  const fa = readJson(recipeFile(dir, A));
  const fb = readJson(recipeFile(dir, B));
  const digest = await cdp.eval(`return window.__lyflow.recipes.specDigest();`);
  report.eq("A 的文件只存与基础不同的两个值；B 的文件只有 meanK 一个值（cutMax 等于基础，没存）", { A: fa.values, B: fb.values },
    { A: { leafSize: [0.03, 0.02, 0.02], cutMax: 2.5 }, B: { meanK: 40 } });
  report.ok("文件头：schemaVersion 1、name 与文件名一致、graph.id 与摘要是当前图的、有 updatedAt",
    fa.schemaVersion === 1 && fa.name === A && fa.graph.id === doc0.id && fa.graph.specDigest === digest && typeof fa.updatedAt === "string",
    JSON.stringify({ ...fa, values: undefined }));
  report.eq("index.json：default 是 B，order 是建的顺序", readJson(path.join(dir, "index.json")), { default: B, order: [A, B] });
  const onDisk = readJson(file);
  report.ok("图文件同时保存（磁盘上的 doc 带着三个图参数），工具栏脏标记落下",
    Object.keys(onDisk.params ?? {}).join() === "leafSize,cutMax,meanK" &&
      !(await cdp.eval(`return !!document.querySelector('[data-testid="toolbar-dirty"]');`)),
    Object.keys(onDisk.params ?? {}).join());

  // 重开：换一张空图再打开这张
  await newDoc(cdp);
  report.eq("新建空图：配方清空、回到基础", (await recipeState(cdp)).recipes.length, 0);
  await reopen(cdp, file);
  const st = await recipeState(cdp);
  report.eq("重开：两个配方按 index.json 的顺序读回", st.recipes.map((r) => r.name), [A, B]);
  report.eq("重开：有默认配方就选它（B），下拉框显示 B",
    { current: st.current, shown: await cdp.eval(`return document.querySelector('[data-testid="recipe-current"]')?.textContent;`) },
    { current: B, shown: B });
  report.eq("重开：值还原", st.recipes.map((r) => r.values), [{ leafSize: [0.03, 0.02, 0.02], cutMax: 2.5 }, { meanK: 40 }]);
  report.eq("重开：交给 core 的取值 = default ← B", st.params, { leafSize: [0.02, 0.02, 0.02], cutMax: 1.2, meanK: 40 });
  report.ok("重开：不脏", !st.dirty && !(await snap(cdp)).dirty);
  await openPanel(cdp, "matrix");
  const cells = await cdp.eval(`
    const at = (p, c) => document.querySelector('[data-testid="mx-cell-' + p + '-' + c + '"]')?.getAttribute('data-state');
    return [at('leafSize', ${lit(A)}), at('cutMax', ${lit(A)}), at('meanK', ${lit(B)}), at('cutMax', ${lit(B)})];
  `);
  report.eq("重开：矩阵里 A 的两格、B 的一格是配方存的值，B 的 cutMax 沿用基础", cells, ["override", "override", "override", "inherit"]);
  await closePanel(cdp);
  return { ids, file, dir };
}

// ------------------------------------------------------------ 验收 18

async function suiteEditSemantics(cdp, report, ws) {
  report.section("P3 验收 18：K6 ① 只写当前配方、② 改图 + 提示 +「改为只在本配方生效」、③ 切走切回改动还在；外部修改检测");
  const { ids, file, dir } = await recipeGraph(cdp, ws, "车门缝隙 18");
  await newRecipeFromMenu(cdp, A);
  await newRecipeFromMenu(cdp, B);
  await pickRecipe(cdp, A);
  await openPanel(cdp, "nodes");
  await typeInto(cdp, `[data-testid="pp-gp-cutMax"]`, "2");
  await saveByKey(cdp);
  const graphText0 = fs.readFileSync(file, "utf8");
  const bText0 = fs.readFileSync(recipeFile(dir, B), "utf8");
  const aText0 = fs.readFileSync(recipeFile(dir, A), "utf8");

  // ① 选着 A 改已纳入的参数 → 只有 A 的文件变
  await must("① A：cutMax 改成 3", typeInto(cdp, `[data-testid="pp-gp-cutMax"]`, "3"));
  const row = await cdp.eval(`
    const r = document.querySelector('[data-testid="pp-gp-cutMax"]');
    return { override: r?.getAttribute('data-recipe-override'), tag: document.querySelector('[data-testid="pp-recipe-tag-gp-cutMax"]')?.textContent ?? null,
             stripe: r ? getComputedStyle(r.querySelector('.prow__stripe')).backgroundColor : null };
  `);
  report.ok("① 被当前配方覆盖的行：橙色竖条 +「配方 · 基础 1.2」标签", row.override === "1" && row.tag === "配方 · 基础 1.2" && row.stripe === "rgb(224, 160, 48)", JSON.stringify(row));
  await saveByKey(cdp);
  report.ok("① Ctrl+S 之后：只有 A 的文件变了，B 与图文件逐字节不变",
    fs.readFileSync(recipeFile(dir, A), "utf8") !== aText0 && fs.readFileSync(recipeFile(dir, B), "utf8") === bText0 && fs.readFileSync(file, "utf8") === graphText0);
  report.eq("① A 的文件里 cutMax = 3", readJson(recipeFile(dir, A)).values, { cutMax: 3 });

  // ② 选着 A 改一个没纳入的参数（统计离群的 stddevMul）→ 改的是图，行上提示
  const key = `${ids.sor}.stddevMul`;
  await must("② A：stddevMul 改成 2.5", typeInto(cdp, rowSel(key), "2.5"));
  const sorParams = (await docOf(cdp)).nodes.find((n) => n.id === ids.sor).params;
  report.eq("② 照常改图：节点上的显式值是 2.5", sorParams.stddevMul, 2.5);
  await reveal(cdp, rowSel(key));
  report.ok("② 行上提示「此改动影响所有配方」",
    (await cdp.eval(`return document.querySelector('[data-testid="pp-basehint-${key}"]')?.textContent ?? '';`)).includes("此改动影响所有配方"));
  await must("② 点「改为只在本配方生效」", click(cdp, `[data-testid="pp-only-recipe-${key}"]`, { inList: true }));
  const doc2 = await docOf(cdp);
  report.ok("② 图里多了图参数 stddevMul，default = 改之前的 1（别的配方行为不变），节点上的显式值删了",
    doc2.params.stddevMul?.default === 1 && doc2.params.stddevMul.binds.join() === key && doc2.nodes.find((n) => n.id === ids.sor).params.stddevMul === undefined,
    JSON.stringify(doc2.params.stddevMul));
  report.eq("② 新值只在 A 里", [await valuesOf(cdp, A), await valuesOf(cdp, B)], [{ cutMax: 3, stddevMul: 2.5 }, {}]);
  report.eq("② 撤销记录是一步", (await snap(cdp)).undoLabel, `改为只在配方 ${A} 生效：stddevMul`);

  // ③ 切到 B 再切回 A：不弹窗，未保存的改动还在
  report.ok("③ 有未保存的改动", (await recipeState(cdp)).dirty || (await snap(cdp)).dirty);
  await pickRecipe(cdp, B);
  report.ok("③ 切到 B：没有弹任何对话框", !(await cdp.eval(`return !!document.querySelector('[data-testid="modal"]');`)));
  report.eq("③ B 下 stddevMul 是基础 1", (await recipeState(cdp)).params.stddevMul, 1);
  await pickRecipe(cdp, A);
  report.eq("③ 切回 A：没存的 2.5 还在", (await recipeState(cdp)).params.stddevMul, 2.5);
  await saveByKey(cdp);
  report.eq("③ Ctrl+S 一次把图（新图参数）与 A 一起存下", [readJson(file).params.stddevMul?.default, readJson(recipeFile(dir, A)).values], [1, { cutMax: 3, stddevMul: 2.5 }]);

  // 外部修改：A 的文件被别人改了，编辑器里也改了 A，Ctrl+S → 提示；先选「重新载入」
  const external = { ...readJson(recipeFile(dir, A)), values: { cutMax: 4.5 } };
  fs.writeFileSync(recipeFile(dir, A), `${JSON.stringify(external, null, 2)}\n`);
  await typeInto(cdp, `[data-testid="pp-gp-cutMax"]`, "3.3");
  await blurActive(cdp);
  await pressCtrl(cdp, "S");
  const prompt = await cdp.eval(`
    for (let i = 0; i < 60; i += 1) {
      const m = document.querySelector('[data-testid="modal"][data-kind="choice"]');
      if (m) return m.textContent;
      await new Promise((d) => setTimeout(d, 100));
    }
    return null;
  `);
  const choices = await cdp.eval(`return ['overwrite','reload','cancel'].every((id) => !!document.querySelector('[data-testid="modal-choice-' + id + '"]'));`);
  report.ok("外部修改：Ctrl+S 前弹出「配方文件已被外部修改」并列出文件，给了覆盖 / 重新载入 / 取消三个选项",
    (prompt ?? "").includes("已被外部修改") && (prompt ?? "").includes(`${A}.lyflow-recipe.json`) && choices, JSON.stringify({ prompt, choices }));
  await click(cdp, '[data-testid="modal-choice-reload"]');
  await cdp.waitFor(`!window.__lyflow.snapshot().recipes.dirty`, { what: "重新载入之后存完", timeoutMs: 15_000 });
  report.eq("重新载入：内存里 A 换成磁盘上的版本，磁盘上的文件没被编辑器的版本覆盖",
    { memory: await valuesOf(cdp, A), disk: readJson(recipeFile(dir, A)).values }, { memory: { cutMax: 4.5 }, disk: { cutMax: 4.5 } });
  await undoByKey(cdp);
  report.eq("重新载入是一步撤销：Ctrl+Z 回到编辑器里的 3.3", (await valuesOf(cdp, A))?.cutMax, 3.3);
  // 再来一次，这次选「覆盖」
  fs.writeFileSync(recipeFile(dir, A), `${JSON.stringify({ ...external, values: { cutMax: 0.5 } }, null, 2)}\n`);
  await blurActive(cdp);
  await pressCtrl(cdp, "S");
  await answerChoice(cdp, "overwrite");
  await cdp.waitFor(`!window.__lyflow.snapshot().recipes.dirty`, { what: "覆盖之后存完", timeoutMs: 15_000 });
  report.eq("覆盖：磁盘上是编辑器里的版本", readJson(recipeFile(dir, A)).values.cutMax, 3.3);
  // 取消：什么都不写，图也不存
  fs.writeFileSync(recipeFile(dir, A), `${JSON.stringify({ ...external, values: { cutMax: 0.7 } }, null, 2)}\n`);
  await typeInto(cdp, `[data-testid="pp-gp-cutMax"]`, "3.9");
  await blurActive(cdp);
  await pressCtrl(cdp, "S");
  await answerChoice(cdp, "cancel");
  await sleep(400);
  report.ok("取消：文件保持外部的版本，编辑器里仍是没存的状态",
    readJson(recipeFile(dir, A)).values.cutMax === 0.7 && (await recipeState(cdp)).dirty);
  await cdp.eval(`window.__lyflow.stores.graph.getState().setRecipeValue(${lit(A)}, 'cutMax', 3.3); return true;`);
  await blurActive(cdp);
  await pressCtrl(cdp, "S");
  await answerChoice(cdp, "overwrite");
  await cdp.waitFor(`!window.__lyflow.snapshot().recipes.dirty`, { what: "收尾存盘", timeoutMs: 15_000 });
  await closePanel(cdp);
}

// ------------------------------------------------------------ 验收 19、21

async function suiteBaseAndUndo(cdp, report, ws) {
  report.section("P3 验收 19 / 21：K4 总是从基础重算；K7 配方值能 Ctrl+Z / Ctrl+Y，切换配方不进撤销栈");
  const { ids } = await recipeGraph(cdp, ws, "车门缝隙 19");
  await newRecipeFromMenu(cdp, A);
  await openPanel(cdp, "nodes");
  await typeInto(cdp, rowSel(`${ids.voxel}.leafSize`), "0.05");
  await newRecipeFromMenu(cdp, B);
  await typeInto(cdp, `[data-testid="pp-gp-cutMax"]`, "0.8");
  await pickRecipe(cdp, A);
  await pickRecipe(cdp, B);
  const p = (await recipeState(cdp)).params;
  report.eq("A → B：x 回到基础（不残留 A 的值）、y 是 B 的", [p.leafSize, p.cutMax], [[0.02, 0.02, 0.02], 0.8]);
  await pickRecipe(cdp, "");

  // K7
  await pickRecipe(cdp, B);
  const past0 = await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
  await pickRecipe(cdp, A);
  await pickRecipe(cdp, B);
  report.eq("切换配方两次：撤销栈长度不变", await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`), past0);
  await typeInto(cdp, `[data-testid="pp-gp-cutMax"]`, "0.6");
  report.eq("B：cutMax 改成 0.6，撤销记录写明是配方里的值", (await snap(cdp)).undoLabel, `配方 ${B}：修改 cutMax`);
  await undoByKey(cdp);
  report.eq("Ctrl+Z：B 的 cutMax 回到 0.8，当前配方还是 B",
    { cutMax: (await valuesOf(cdp, B)).cutMax, current: (await recipeState(cdp)).current }, { cutMax: 0.8, current: B });
  await redoByKey(cdp);
  report.eq("Ctrl+Y：又是 0.6", (await valuesOf(cdp, B)).cutMax, 0.6);
  // 「恢复基础」「写回基础」各一步
  await click(cdp, '[data-testid="pp-recipe-reset-gp-cutMax"]', { inList: true });
  report.eq("恢复基础：B 里的 cutMax 覆盖删掉", await valuesOf(cdp, B), {});
  await undoByKey(cdp);
  await click(cdp, '[data-testid="pp-recipe-tobase-gp-cutMax"]', { inList: true });
  report.eq("写回基础：default 变成 0.6、B 的覆盖删掉",
    [(await docOf(cdp)).params.cutMax.default, await valuesOf(cdp, B)], [0.6, {}]);
  await undoByKey(cdp);
  report.eq("写回基础一次 Ctrl+Z 同时还原 default 与配方", [(await docOf(cdp)).params.cutMax.default, (await valuesOf(cdp, B)).cutMax], [1.2, 0.6]);
  await closePanel(cdp);
}

// ------------------------------------------------------------ 验收 20

async function suiteSwitchAndRun(cdp, report, ws) {
  report.section("P3 验收 20：切换配方后运行 —— 受影响的节点重算、其余命中缓存；切回原配方全部命中缓存");
  // 种子每次不同：结果仓按内容寻址，同一个 app 里跑过一样的图，「该重算的」也会命中缓存
  const { ids } = await recipeGraph(cdp, ws, "车门缝隙 20", { seed: 1000 + (Date.now() % 100000) });
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.createRecipe(${lit(A)}); g.createRecipe(${lit(B)});
    g.setRecipeValue(${lit(A)}, 'leafSize', [0.03, 0.03, 0.03]);
    g.setRecipeValue(${lit(B)}, 'cutMax', 0.9);
    return true;
  `);
  await pickRecipe(cdp, A);
  const runA = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(runA.status === "ok", "A 下跑一遍：成功", runA.status);
  await pickRecipe(cdp, B);
  const plan = await replan(cdp);
  const cached = (id) => plan.plan[id]?.cached;
  report.eq("切到 B 之后的计划：体素与直通要重算，gen 与离群支路命中缓存",
    [cached(ids.gen), cached(ids.voxel), cached(ids.cut), cached(ids.sor)], [true, false, false, true]);
  await sleep(200);
  report.eq("工具栏「将重算 N 个节点」跟着变成 2",
    await cdp.eval(`return document.querySelector('[data-testid="recompute-hint"]')?.getAttribute('data-count');`), "2");
  report.ok("结果标成已过时", (await snap(cdp)).run.stale);
  const runB = await runAndWait(cdp, () => pressF5(cdp));
  const st = (run, id) => run.nodes[id]?.state;
  report.eq("B 下运行：体素、直通重算（done），gen、离群命中缓存（skipped）",
    [st(runB, ids.gen), st(runB, ids.voxel), st(runB, ids.cut), st(runB, ids.sor)], ["skipped", "done", "done", "skipped"]);
  await pickRecipe(cdp, A);
  const back = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("切回 A 再运行：全部命中缓存",
    [ids.gen, ids.voxel, ids.cut, ids.sor].map((id) => st(back, id)), ["skipped", "skipped", "skipped", "skipped"]);
  // 自动运行打开时，切换配方照常触发一次运行
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(true); return true;`);
  const auto = await runAndWait(cdp, () => pickRecipe(cdp, B));
  report.ok("自动运行开着：切到 B 自动跑了一遍（全部命中缓存）",
    auto.status === "ok" && [ids.gen, ids.voxel, ids.cut, ids.sor].every((id) => st(auto, id) === "skipped"), JSON.stringify(auto.nodes));
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(false); return true;`);
}

// ------------------------------------------------------------ 验收 22

async function suiteMatrix(cdp, report, ws) {
  report.section("P3 验收 22：矩阵 —— 三态、只显示差异、单元格编辑、多选复制、隐藏列、越界为红且运行被阻止");
  const { ids } = await recipeGraph(cdp, ws, "车门缝隙 22");
  const C = "车型C·右前门";
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.createRecipe(${lit(A)}); g.createRecipe(${lit(B)}); g.createRecipe(${lit(C)});
    g.setRecipeValue(${lit(A)}, 'leafSize', [0.03, 0.03, 0.03]);
    g.setRecipeValue(${lit(A)}, 'cutMax', 2);
    return true;
  `);
  await openPanel(cdp, "matrix");
  const cell = (p, c) => `[data-testid="mx-cell-${p}-${c}"]`;
  const stateAt = (p, c) => cdp.eval(`return document.querySelector(${lit(cell(p, c))})?.getAttribute('data-state') ?? null;`);
  report.eq("列：参数 | 基础 | A | B | C", await cdp.eval(`return [...document.querySelectorAll('[data-testid="mx-table"] thead th')].map((t) => t.getAttribute('data-col') ?? 'param');`), ["param", ":base", A, B, C]);
  report.eq("A 的 leafSize 是配方存的值（override），B 的 leafSize 沿用基础（inherit）",
    [await stateAt("leafSize", A), await stateAt("leafSize", B)], ["override", "inherit"]);

  // 就地编辑：双击 B 的 meanK，填 1（越过它的 min 2）→ 红
  await cdp.eval(`document.querySelector(${lit(cell("meanK", B))}).dispatchEvent(new MouseEvent('dblclick', { bubbles: true })); return true;`);
  await sleep(150);
  await must("双击单元格：就地出现 meanK 的数字框", typeInto(cdp, `${cell("meanK", B)} [data-testid="mx-editor"]`, "1", { inList: false }));
  await sleep(200);
  report.eq("单元格编辑写进 B（A、C 不变）", [await valuesOf(cdp, A), await valuesOf(cdp, B), await valuesOf(cdp, C)],
    [{ leafSize: [0.03, 0.03, 0.03], cutMax: 2 }, { meanK: 1 }, {}]);
  const invalidState = await stateAt("meanK", B);
  const redTitle = await cdp.eval(`
    const el = document.querySelector(${lit(cell("meanK", B))});
    return { title: el.getAttribute('title'), color: getComputedStyle(el).color };
  `);
  report.ok("越界的单元格是红的（invalid、红字），悬停写原因（越界：不能小于 2）",
    invalidState === "invalid" && redTitle.color === "rgb(229, 72, 77)" && /越界.*不能小于 2/.test(redTitle.title),
    JSON.stringify({ state: invalidState, ...redTitle }));
  // 「越界阻止运行、其余配方照常运行」在验收 23 里对同一条路径断言（③ 的配方被阻止、④ 的配方照常运行），这里不再重跑

  // 只显示差异 / 全部
  await click(cdp, '[data-testid="mx-diff"]');
  const rowsDiff = await cdp.eval(`return [...document.querySelectorAll('tr.mx-row')].map((r) => r.getAttribute('data-param'));`);
  report.eq("只显示差异：有配方存了值的三行（leafSize、cutMax、meanK）", rowsDiff, ["leafSize", "cutMax", "meanK"]);
  await cdp.eval(`window.__lyflow.stores.graph.getState().clearRecipeValue(${lit(A)}, 'cutMax'); return true;`);
  await sleep(150);
  report.eq("只显示差异随值变化：清掉 A 的 cutMax 之后 cutMax 那行不见了",
    await cdp.eval(`return [...document.querySelectorAll('tr.mx-row')].map((r) => r.getAttribute('data-param'));`), ["leafSize", "meanK"]);
  await click(cdp, '[data-testid="mx-all"]');
  report.eq("全部：三个图参数都在", await cdp.eval(`return document.querySelectorAll('tr.mx-row').length;`), 3);
  await cdp.eval(`window.__lyflow.stores.graph.getState().setRecipeValue(${lit(A)}, 'cutMax', 2); return true;`);

  // 复杂值：双击 C 的 leafSize → 弹出编辑，改第二个分量
  await cdp.eval(`document.querySelector(${lit(cell("leafSize", C))}).dispatchEvent(new MouseEvent('dblclick', { bubbles: true })); return true;`);
  await sleep(150);
  await typeInto(cdp, `${cell("leafSize", C)} [data-testid="mx-editor"]`, "0.07", { index: 1, inList: false });
  await click(cdp, '[data-testid="mx-edit-done"]');
  await sleep(150);
  report.eq("弹出编辑写进 C", await valuesOf(cdp, C), { leafSize: [0.02, 0.07, 0.02] });

  // 多选：单击 A 的 leafSize，Ctrl+单击 A 的 cutMax → 复制到 B
  await cdp.eval(`
    document.querySelector(${lit(cell("leafSize", A))}).dispatchEvent(new MouseEvent('click', { bubbles: true }));
    document.querySelector(${lit(cell("cutMax", A))}).dispatchEvent(new MouseEvent('click', { bubbles: true, ctrlKey: true }));
    return true;
  `);
  await sleep(150);
  const sel = await cdp.eval(`
    const el = document.querySelector(${lit(cell("cutMax", A))});
    return { n: document.querySelectorAll('[data-selected="1"]').length, shadow: getComputedStyle(el).boxShadow,
             label: document.querySelector('[data-testid="mx-copy"]').textContent };
  `);
  report.ok("选中两格：蓝色内描边，按钮写着「复制选中 2 格 → 配方」", sel.n === 2 && sel.shadow.includes("inset") && sel.label.includes("复制选中 2 格"), JSON.stringify(sel));
  await click(cdp, '[data-testid="mx-copy"]');
  await click(cdp, `[data-testid="mx-copy-to-${B}"]`);
  report.eq("复制到 B：B 的 leafSize、cutMax 等于 A 的（meanK 不动）", await valuesOf(cdp, B), { meanK: 1, leafSize: [0.03, 0.03, 0.03], cutMax: 2 });
  report.eq("复制是一步撤销", (await snap(cdp)).undoLabel, `复制 2 格到配方 ${B}`);
  // 选基础列的格复制 = 让目标回到基础（删掉覆盖）
  await cdp.eval(`document.querySelector('[data-testid="mx-cell-meanK-base"]').dispatchEvent(new MouseEvent('click', { bubbles: true })); return true;`);
  await sleep(100);
  await click(cdp, '[data-testid="mx-copy"]');
  await click(cdp, `[data-testid="mx-copy-to-${B}"]`);
  report.eq("把基础列的 meanK 复制到 B：B 的越界值被基础替掉（不再存），B 的 meanK 回到沿用基础",
    { values: await valuesOf(cdp, B), meanK: await stateAt("meanK", B) },
    { values: { leafSize: [0.03, 0.03, 0.03], cutMax: 2 }, meanK: "inherit" });

  // 隐藏列
  await click(cdp, `[data-testid="mx-hide-${C}"]`);
  report.ok("隐藏 C 列", !(await cdp.eval(`return !!document.querySelector('[data-testid="mx-col-${C}"]');`)));
  await click(cdp, '[data-testid="mx-unhide"]');
  report.ok("显示隐藏的列：C 回来了", await cdp.eval(`return !!document.querySelector('[data-testid="mx-col-${C}"]');`));
  await closePanel(cdp);
  return { ids };
}

// ------------------------------------------------------------ 验收 23

async function suiteMismatch(cdp, report, ws) {
  report.section("P3 验收 23：失配 —— 手工构造的配方文件触发四类，条目与建议正确；全部按建议修复后能运行，一次撤销还原");
  const { file, dir } = await recipeGraph(cdp, ws, "车门缝隙 23");
  const doc = await docOf(cdp);
  const digest = await cdp.eval(`return window.__lyflow.recipes.specDigest();`);
  const ref = { id: doc.id, specDigest: digest };
  const write = (name, values, graph = ref) =>
    fs.writeFileSync(recipeFile(dir, name), `${JSON.stringify({ schemaVersion: 1, name, graph, values, updatedAt: "2026-09-24T08:00:00.000Z" }, null, 2)}\n`);
  fs.mkdirSync(dir, { recursive: true });
  write("失配-多出", { voxelSize: [0.05, 0.05, 0.05], cutMax: 2 });
  write("失配-类型", { leafSize: "abc", meanK: "35" });
  write("失配-越界", { leafSize: [0.02, 20, 0.00001], meanK: 1 });
  write("失配-规格", { cutMax: 2 }, { id: "01JSOMEOTHERGRAPH000000000", specDigest: `sha256:${"0".repeat(64)}` });
  fs.writeFileSync(path.join(dir, "坏文件.lyflow-recipe.json"), "{ 这不是 JSON");
  await reopen(cdp, file);
  const st = await recipeState(cdp);
  report.eq("四个配方读进来，读不出来的那个列在问题里", [st.recipes.map((r) => r.name).sort(), st.problems.map((p) => p.file)],
    [["失配-多出", "失配-类型", "失配-越界", "失配-规格"].sort(), ["坏文件.lyflow-recipe.json"]]);
  await openPanel(cdp, "recipes");
  report.ok("管理页底部列出读不出来的文件", (await cdp.eval(`return document.querySelector('[data-testid="mg-problems"]')?.textContent ?? '';`)).includes("坏文件"));
  const reportOf = async (name) => {
    await click(cdp, `[data-testid="mg-item"][data-name=${lit(name)}]`);
    return cdp.eval(`return [...document.querySelectorAll('[data-testid="mg-mismatch"]')].map((li) => ({
      kind: li.getAttribute('data-kind'), param: li.getAttribute('data-param'),
      badge: li.querySelector('.mg-kind').textContent, fix: li.querySelector('[data-testid="mg-fix"]').textContent }));`);
  };
  const mismatches = {};
  for (const name of ["失配-多出", "失配-类型", "失配-越界", "失配-规格"]) mismatches[name] = await reportOf(name);
  report.eq("四类失配的条目与建议：① 多出 voxelSize → 删除；② 类型不符 leafSize \"abc\" → 删除、meanK \"35\" → 改为 35；" +
    "③ 越界 leafSize 逐分量夹到 [0.0001, 10]、meanK 夹到 2；④ 规格变了 → 按当前图更新记录", mismatches, {
    "失配-多出": [{ kind: "extra", param: "voxelSize", badge: "多出", fix: "删除这个值" }],
    "失配-类型": [
      { kind: "type", param: "leafSize", badge: "类型不符", fix: "删除这个值（用基础）" },
      { kind: "type", param: "meanK", badge: "类型不符", fix: "改为 35" },
    ],
    "失配-越界": [
      { kind: "range", param: "leafSize", badge: "越界", fix: "夹到限位：[0.02,10,0.0001]" },
      { kind: "range", param: "meanK", badge: "越界", fix: "夹到限位：2" },
    ],
    "失配-规格": [{ kind: "spec", param: "", badge: "规格变了", fix: "按当前图更新记录" }],
  });
  const badges = await cdp.eval(`return [...document.querySelectorAll('[data-testid="mg-item"]')].map((b) => b.getAttribute('data-name') + '=' + (b.querySelector('[data-testid="mg-item-bad"]')?.textContent ?? '0')).sort();`);
  report.eq("列表上的失配数红徽标（①–③ 的条数）", badges, ["失配-多出=1", "失配-类型=2", "失配-规格=0", "失配-越界=2"].sort());

  // ④ 不阻止运行；③ 阻止，修完能跑，一次撤销还原
  await pickRecipe(cdp, "失配-规格");
  report.eq("④ 的配方照常运行", (await runAndWait(cdp, () => pressF5(cdp))).status, "ok");
  await pickRecipe(cdp, "失配-越界");
  const blocked = await f5Blocked(cdp);
  report.ok("③ 的配方运行被阻止，原因写明配方与参数", /失配-越界/.test(blocked ?? "") && /leafSize/.test(blocked ?? ""), blocked);
  await click(cdp, `[data-testid="mg-item"][data-name="失配-越界"]`);
  await click(cdp, '[data-testid="mg-fix-all"]');
  report.eq("全部按建议修复：值夹进限位", await valuesOf(cdp, "失配-越界"), { leafSize: [0.02, 10, 0.0001], meanK: 2 });
  report.eq("修完能运行", (await runAndWait(cdp, () => pressF5(cdp))).status, "ok");
  await undoByKey(cdp);
  report.eq("一次 Ctrl+Z 还原成修之前", await valuesOf(cdp, "失配-越界"), { leafSize: [0.02, 20, 0.00001], meanK: 1 });
  // 单条修：② 里只修 meanK
  await click(cdp, `[data-testid="mg-item"][data-name="失配-类型"]`);
  await cdp.eval(`[...document.querySelectorAll('[data-testid="mg-mismatch"]')].find((li) => li.getAttribute('data-param') === 'meanK').querySelector('[data-testid="mg-fix"]').click(); return true;`);
  await sleep(150);
  report.eq("单条「改为 35」：只改 meanK", await valuesOf(cdp, "失配-类型"), { leafSize: "abc", meanK: 35 });
  await pickRecipe(cdp, "");
  await closePanel(cdp);
}

// ------------------------------------------------------------ 验收 24

async function suiteManage(cdp, report, ws) {
  report.section("P3 验收 24：管理 —— 新建、复制、重命名、删除、设为默认、导入（重名）、导出，看磁盘；自动备份");
  const { file, dir } = await recipeGraph(cdp, ws, "车门缝隙 24");
  await openPanel(cdp, "recipes");
  await click(cdp, '[data-testid="mg-new"]');
  await must("新建：起名对话框", answerText(cdp, A));
  await cdp.eval(`window.__lyflow.stores.graph.getState().setRecipeValue(${lit(A)}, 'cutMax', 2.2); return true;`);
  await saveByKey(cdp);
  report.ok("新建 → 存盘：A 的文件出现", fs.existsSync(recipeFile(dir, A)));

  await click(cdp, `[data-testid="mg-item"][data-name=${lit(A)}]`);
  await click(cdp, '[data-testid="mg-dup"]');
  await must("复制：起名", answerText(cdp, B));
  await saveByKey(cdp);
  report.eq("复制 → 存盘：B 的文件、值与 A 相同", readJson(recipeFile(dir, B)).values, { cutMax: 2.2 });

  await click(cdp, `[data-testid="mg-item"][data-name=${lit(B)}]`);
  await click(cdp, '[data-testid="mg-rename"]');
  const R = "车型B·尾门（改）";
  await must("重命名：起名对话框", answerText(cdp, R));
  await saveByKey(cdp);
  report.ok("重命名 → 存盘：旧文件没了、新文件在、文件里的 name 跟着改",
    !fs.existsSync(recipeFile(dir, B)) && fs.existsSync(recipeFile(dir, R)) && readJson(recipeFile(dir, R)).name === R, listDir(dir).join(" | "));

  await click(cdp, `[data-testid="mg-item"][data-name=${lit(R)}]`);
  await click(cdp, '[data-testid="mg-default"]');
  await saveByKey(cdp);
  report.eq("设为默认 → 存盘：index.json 的 default", readJson(path.join(dir, "index.json")).default, R);

  await click(cdp, '[data-testid="mg-delete"]');
  await must("删除：确认对话框点「删除」", answerChoice(cdp, "delete"));
  await saveByKey(cdp);
  report.ok("删除 → 存盘：文件没了，index.json 里的 default 一并去掉",
    !fs.existsSync(recipeFile(dir, R)) && readJson(path.join(dir, "index.json")).default === undefined, JSON.stringify(readJson(path.join(dir, "index.json"))));

  // 导入：外部文件的名字与现有的 A 重名 → 让用户改名
  const outside = path.join(ws.dir, "外部配方");
  fs.mkdirSync(outside, { recursive: true });
  const importFile = path.join(outside, "来自产线.lyflow-recipe.json");
  fs.writeFileSync(importFile, `${JSON.stringify({ schemaVersion: 1, name: A, graph: { id: "x", specDigest: `sha256:${"1".repeat(64)}` }, values: { meanK: 55 }, updatedAt: "2026-09-01T00:00:00.000Z" }, null, 2)}\n`);
  await cdp.eval(`window.__importDone = null; window.__lyflow.recipes.importFrom(${lit(importFile)}).then((n) => { window.__importDone = n ?? '(取消)'; }); return true;`);
  await cdp.waitFor(`!!document.querySelector('[data-testid="modal"][data-kind="text"]')`, { what: "导入重名的改名对话框" });
  const problem = await cdp.eval(`return document.querySelector('[data-testid="modal-problem"]')?.textContent ?? '';`);
  report.ok("导入重名：对话框说明已经有同名配方，确定按钮置灰", problem.includes("已经有配方") && (await cdp.eval(`return document.querySelector('[data-testid="modal-ok"]').disabled;`)), problem);
  const C = "车型A·左前门（产线）";
  await answerText(cdp, C);
  await cdp.waitFor(`window.__importDone !== null`, { what: "导入完成" });
  await saveByKey(cdp);
  const imported = readJson(recipeFile(dir, C));
  report.ok("导入 → 存盘：复制进配方目录，name 是新名字、值原样（图的记录保留外部的，失配 ④ 照常提示）",
    imported.name === C && imported.values.meanK === 55 && imported.graph.id === "x", JSON.stringify(imported));
  report.ok("导入不动外部文件", readJson(importFile).name === A);

  // 导出：任意位置，立即写
  const exportFile = path.join(ws.dir, "导出", `${A}.lyflow-recipe.json`);
  await cdp.eval(`await window.__lyflow.recipes.exportTo(${lit(A)}, ${lit(exportFile)}); return true;`);
  const exported = fs.existsSync(exportFile) ? readJson(exportFile) : null;
  report.ok("导出：文件写到指定位置，内容就是 A", exported?.name === A && exported.values.cutMax === 2.2, JSON.stringify(exported));

  // 自动备份：内存里没存的改动写进 autosave~.json，重开后能恢复
  await cdp.eval(`window.__lyflow.stores.graph.getState().setRecipeValue(${lit(A)}, 'cutMax', 3.7); return true;`);
  await cdp.eval(`await window.__lyflow.recipes.autosave(); return true;`);
  report.ok("自动备份：写出 autosave~.json", fs.existsSync(path.join(dir, "autosave~.json")));
  await newDoc(cdp);
  await reopen(cdp, file);
  report.eq("重开：磁盘上的 A 仍是 2.2", (await valuesOf(cdp, A)).cutMax, 2.2);
  report.ok("从自动备份恢复：A 回到没存的 3.7，而且是未保存状态",
    (await cdp.eval(`return await window.__lyflow.recipes.restoreAutosave();`)) && (await valuesOf(cdp, A)).cutMax === 3.7 && (await recipeState(cdp)).dirty);
  await saveByKey(cdp);
  report.ok("存盘后自动备份删掉", !fs.existsSync(path.join(dir, "autosave~.json")));
  await closePanel(cdp);
}

// ------------------------------------------------------------ 收尾

async function suiteCleanup(cdp) {
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(true); return true;`);
  await closePanel(cdp);
}

export const paramsP3Suites = [
  suiteCreateSaveReopen,
  suiteEditSemantics,
  suiteBaseAndUndo,
  suiteSwitchAndRun,
  suiteMatrix,
  suiteMismatch,
  suiteManage,
  suiteCleanup,
];
