// 配方 P4（docs/param-recipe-plan.md P4：CLI、MCP、宿主接入）的分组。逐条结果见 docs/param-recipe-p4-acceptance.md。
//
// 26 `lyflow run --recipe A.lyflow-recipe.json` 的结果等于编辑器选配方 A 运行的结果：真实的 gap 图（KUN10 的点 2，
//    软装 V 缝开口 + 面差）与真实的样本（KUN10 的几帧），逐位比较图级输出 gap / flush；`--param` 覆盖配方里的同名值。
//    27（失配退出码 4）、28（MCP）在 cargo test 与 packages/mcp 的测试里，不在这里。
// 顺手修：1280–1440 宽的窗口里工具栏的图名框至少完整放下 8 个汉字（P3 截图里「车门缝隙检测」被截断）。
//
// 数据：LYFLOW_GAP_KUN10 指到 luoshi 目录（下面有 database/KUN10 与 cloud/KUN10），缺省是这台机器上的位置。
// 找不到数据或 core 里没有 gap 包时，这一组标「未验」并说明原因（grep 得到，不算通过）。
// CLI 用 bridge/target/debug/lyflow.exe；它没带 gap 包时这一组记一条失败并给出重编命令（不在 e2e 里现编，也不静默跳过）。

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { sleep } from "./cdp.mjs";
import { lit, mustOk, newDoc, pressCtrl, pressF5, runAndWait, saveGraphTo } from "./page.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const CLI = path.join(ROOT, "bridge", "target", "debug", "lyflow.exe");
const KUN10 = process.env.LYFLOW_GAP_KUN10 ?? "C:\\Users\\11601\\OneDrive\\Desktop\\DTS\\luoshi";
const POINT = "2";
const FRAMES = 3;
const RECIPE = "车型A·P4";

// ------------------------------------------------------------ 小工具

const snap = (cdp) => cdp.eval(`return window.__lyflow.snapshot();`);

async function click(cdp, selector) {
  return cdp.eval(`
    const el = document.querySelector(${lit(selector)});
    if (!el) return 'missing';
    el.click();
    await new Promise((d) => setTimeout(d, 150));
    return 'ok';
  `);
}

async function pickRecipe(cdp, name) {
  await click(cdp, '[data-testid="recipe-toggle"]');
  const r = await click(cdp, `[data-testid="recipe-option"][data-name=${lit(name)}]`);
  await sleep(120);
  return r;
}

/** 真按 Ctrl+S，等图与配方都存完。 */
async function saveByKey(cdp) {
  await cdp.eval(`if (document.activeElement instanceof HTMLElement) document.activeElement.blur(); return true;`);
  await pressCtrl(cdp, "S");
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.snapshot(); return !s.dirty && !s.recipes.dirty; })()`,
    { what: "Ctrl+S 存完（图与配方都不脏）", timeoutMs: 20_000 },
  );
  await sleep(150);
}

/** stdout 的 JSON Lines；最后一行是 --outputs 的图级输出。 */
function cli(args) {
  const r = spawnSync(CLI, args, { encoding: "utf8", env: process.env, timeout: 120_000 });
  const lines = (r.stdout ?? "")
    .split(/\r?\n/)
    .filter((l) => l.startsWith("{") || l.startsWith("["))
    .map((l) => JSON.parse(l));
  return { code: r.status, lines, stderr: r.stderr ?? "" };
}

function cliHasGap() {
  const r = cli(["manifest"]);
  return r.code === 0 && (r.lines[0]?.operators ?? []).some((o) => o.id === "gap.notch_width");
}

/** CLI 带没带 gap 包。不带就返回原因与重编命令：e2e 里不现编（一编几十分钟），也不静默跳过（跳过的分组照样「全绿」）。 */
function gapCliProblem() {
  const rebuild = '先在仓库根重编：$env:LYFLOW_PACKS="gap;dts"; cargo build --manifest-path bridge/Cargo.toml --bin lyflow --no-default-features';
  if (!fs.existsSync(CLI)) return `找不到 ${CLI}。${rebuild}`;
  if (!cliHasGap()) return `${CLI} 里没有 gap.notch_width（编它时 LYFLOW_PACKS 没带 gap）。${rebuild}`;
  return null;
}

/** 这次运行的图级输出里 gap / flush 两个量测，整段 JSON 文本（逐位比较用）。 */
const measurementsOf = (outputs) => ({
  gap: JSON.stringify(outputs?.gap ?? null),
  flush: JSON.stringify(outputs?.flush ?? null),
});

// ------------------------------------------------------------ 验收 26

async function suiteCliMatchesEditor(cdp, report, ws) {
  const graphSrc = path.join(KUN10, "database", "KUN10", "device_0", POINT, `${POINT}.lyflow.json`);
  const cloudRoot = path.join(KUN10, "cloud", "KUN10");
  const title = `P4 验收 26：CLI --recipe 与编辑器选配方运行逐位相同（真实 gap 图 ${POINT}，KUN10 的 ${FRAMES} 帧）`;
  if (!fs.existsSync(graphSrc) || !fs.existsSync(cloudRoot)) {
    report.section(`${title}（未验：找不到 ${graphSrc} 或 ${cloudRoot}，设 LYFLOW_GAP_KUN10）`);
    report.fail("真实 gap 数据在", `${graphSrc} / ${cloudRoot}`);
    return;
  }
  const hasGap = await cdp.eval(`return window.__lyflow.stores.manifest.getState().operatorsById.has('gap.notch_width');`);
  if (!hasGap) {
    report.section(`${title}（未验：app 的 core 没有 gap 包，设 LYFLOW_PACKS=gap;dts）`);
    report.fail("app 里有 gap.notch_width", "LYFLOW_PACKS 没带 gap");
    return;
  }
  report.section(title);
  const cliProblem = gapCliProblem();
  if (cliProblem !== null) {
    report.fail("CLI 带着 gap 包（bridge/target/debug/lyflow.exe）", cliProblem);
    return;
  }

  const frames = fs
    .readdirSync(cloudRoot)
    .sort()
    .map((f) => path.join(cloudRoot, f, "device_0", `${POINT}_0`))
    .filter((d) => fs.existsSync(d))
    .slice(0, FRAMES);
  mustOk(frames.length === FRAMES, `找到 ${FRAMES} 帧点 ${POINT} 的样本`, `只找到 ${frames.length} 帧（${cloudRoot}）`);

  // 图拷进中文工作区（配方目录跟着图走），打开、把三个 notch_width 参数纳入配方
  const dir = path.join(ws.dir, "车门缝隙 P4");
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  const graphFile = path.join(dir, "点2 开口与面差.lyflow.json");
  // 线上那张图原样拷来。09-25 起它自己就是 result_bundle v2 的接法、n_load.layout = profile
  // （docs/kun10-graphs-migration-acceptance.md），这里不再替它改任何东西
  const src = JSON.parse(fs.readFileSync(graphSrc, "utf8"));
  fs.writeFileSync(graphFile, JSON.stringify(src, null, 2), "utf8");
  const recipeFile = path.join(dir, "点2 开口与面差.recipes", `${RECIPE}.lyflow-recipe.json`);

  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(false); return true;`);
  await cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().setPath([]);
    const loaded = await b.transport.loadGraph(${lit(graphFile)});
    b.stores.graph.getState().loadDoc(loaded.doc, ${lit(graphFile)});
    await b.recipes.loaded();
    return true;
  `);
  const promoted = await cdp.eval(`
    const g = () => window.__lyflow.stores.graph.getState();
    const names = ['gapOffset', 'levelDepth', 'flushOffset'].map((p) => g().promoteToGraphParam('n_notch', p));
    g().setParam('n_load', 'dir', ${lit(frames[0])});
    return names;
  `);
  mustOk(JSON.stringify(promoted) === JSON.stringify(["gapOffset", "levelDepth", "flushOffset"]),
    "gapOffset、levelDepth、flushOffset 纳入配方（成为图参数）", promoted);
  await saveGraphTo(cdp, graphFile);

  // 建配方 A、改三个值、选中它、Ctrl+S（图与配方文件一起落盘）
  const made = await cdp.eval(`
    const g = () => window.__lyflow.stores.graph.getState();
    if (!g().createRecipe(${lit(RECIPE)})) return 'create';
    g().setRecipeValue(${lit(RECIPE)}, 'gapOffset', -0.3);
    g().setRecipeValue(${lit(RECIPE)}, 'levelDepth', 1);
    g().setRecipeValue(${lit(RECIPE)}, 'flushOffset', 0.05);
    return 'ok';
  `);
  mustOk(made === "ok", "新建配方并写进三个值", made);
  const picked = await pickRecipe(cdp, RECIPE);
  mustOk(picked === "ok", "工具栏下拉框选中配方", picked);
  await saveByKey(cdp);
  const onDisk = fs.existsSync(recipeFile) ? JSON.parse(fs.readFileSync(recipeFile, "utf8")) : null;
  report.eq("配方文件落盘，只存三个与基础不同的值", onDisk?.values, { gapOffset: -0.3, levelDepth: 1, flushOffset: 0.05 });
  const st = await snap(cdp);
  report.eq("编辑器交给 core 的取值 = default ← 配方", st.recipe?.params, { gapOffset: -0.3, levelDepth: 1, flushOffset: 0.05 });

  const rows = [];
  for (const [i, frame] of frames.entries()) {
    await cdp.eval(`window.__lyflow.stores.graph.getState().setParam('n_load', 'dir', ${lit(frame)}); return true;`);
    await saveByKey(cdp);
    const run = await runAndWait(cdp, () => pressF5(cdp));
    const editor = measurementsOf(await cdp.eval(`return await window.__lyflow.runOutputs(${lit(run.runId)});`));
    const viaRecipe = cli(["run", graphFile, "--recipe", recipeFile, "--outputs", "--no-cache"]);
    const cliOut = measurementsOf(viaRecipe.lines[viaRecipe.lines.length - 1]);
    const tag = `第 ${i + 1} 帧（${path.basename(path.dirname(path.dirname(frame)))}）`;
    const hasValue = (v) => v !== "null" && !v.includes('"failed"');
    report.ok(`${tag}：编辑器运行 ok、CLI 退出码 0，gap / flush 都有值`,
      run.status === "ok" && viaRecipe.code === 0 && hasValue(editor.gap) && hasValue(editor.flush),
      JSON.stringify({ status: run.status, cliCode: viaRecipe.code, gap: editor.gap, flush: editor.flush,
        nodes: run.status === "ok" ? undefined : run.nodes, stderr: viaRecipe.code === 0 ? undefined : viaRecipe.stderr.slice(-400) }));
    report.eq(`${tag}：gap / flush 逐位相同（编辑器选配方 = CLI --recipe）`, cliOut, editor);
    const row = { frame: tag, gap: editor.gap, flush: editor.flush };

    if (i === 0) {
      // 与基础比只在第一帧做一次：证明配方真的起了作用，不必每帧再多跑一遍 CLI
      const base = cli(["run", graphFile, "--outputs", "--no-cache"]);
      const baseOut = measurementsOf(base.lines[base.lines.length - 1]);
      report.ok(`${tag}：与基础不同（配方真的起了作用）`, baseOut.gap !== editor.gap && baseOut.flush !== editor.flush,
        `基础 ${baseOut.gap} / ${baseOut.flush}`);
      row.baseGap = baseOut.gap;
      row.baseFlush = baseOut.flush;
      // --param 覆盖配方里的同名值：gapOffset 盖回基础，结果等于「只改 levelDepth、flushOffset」
      const pinned = cli(["run", graphFile, "--recipe", recipeFile, "--param", "gapOffset=-0.42", "--outputs", "--no-cache"]);
      const same = cli(["run", graphFile, "--param", "levelDepth=1", "--param", "flushOffset=0.05", "--outputs", "--no-cache"]);
      const a = measurementsOf(pinned.lines[pinned.lines.length - 1]);
      const b = measurementsOf(same.lines[same.lines.length - 1]);
      report.eq("--param 覆盖配方里的 gapOffset：gap 等于只用配方另外两个值的结果", a.gap, b.gap);
      report.ok("被覆盖之后的 gap 与配方 A 的不同", a.gap !== editor.gap, `${a.gap} vs ${editor.gap}`);
      report.ok("CLI 在 stderr 报了用的是哪个配方", viaRecipe.stderr.includes(`配方「${RECIPE}」：3 个值，3 个与基础不同`), viaRecipe.stderr);
    }
    rows.push(row);
  }
  console.log(`      逐帧：${JSON.stringify(rows)}`);
}

// ------------------------------------------------------------ 工具栏宽度

const EIGHT = "车门缝隙检测左前";

/** 1280–1440 宽时图名框完整放下 8 个汉字；工具栏里什么都不溢出、运行区的状态字不被裁、配方下拉框在窗口里。
 *  状态照 P3 截图那样挤：跑过一次（done N）、改过参数（已过时 + 将重算 N 个）、有文件名、选着配方且没存。 */
async function suiteToolbarWidth(cdp, report) {
  report.section("顺手修：1280–1440 宽的窗口里，工具栏的图名框完整放下 8 个汉字");
  const state = await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    return { file: g.filePath, recipe: b.snapshot().recipe?.current ?? null };
  `);
  report.ok("前置：图有路径、选着配方（接着验收 26 的状态）", Boolean(state.file) && state.recipe === RECIPE, JSON.stringify(state));
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.setName(${lit(EIGHT)});
    g.setRecipeValue(${lit(RECIPE)}, 'levelDepth', 1.1);
    return true;
  `);
  await cdp.eval(`await window.__lyflow.plan(); return true;`);
  await sleep(300);

  const height = await cdp.eval(`return window.innerHeight;`);
  try {
    for (const width of [1280, 1366, 1440]) {
      await cdp.send("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 0, mobile: false });
      await sleep(350);
      const m = await cdp.eval(`
        const tb = document.querySelector('.toolbar');
        const name = document.querySelector('[data-testid="doc-name"]');
        const run = document.querySelector('.toolbar__group--run');
        const menu = document.querySelector('.recipe-menu');
        // 8 个字按图名框自己的字体量一遍，与框的内容区比
        const probe = document.createElement('span');
        const cs = getComputedStyle(name);
        probe.style.cssText = 'position:absolute;visibility:hidden;white-space:pre;font:' + cs.font;
        probe.textContent = name.value;
        document.body.appendChild(probe);
        const text = probe.getBoundingClientRect().width;
        probe.remove();
        const content = name.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
        return {
          innerWidth, value: name.value, text: Math.round(text * 10) / 10, content,
          nameOverflow: name.scrollWidth - name.clientWidth,
          toolbarOverflow: tb.scrollWidth - tb.clientWidth,
          runClipped: run.scrollWidth - run.clientWidth,
          menuRight: Math.round(menu.getBoundingClientRect().right),
          recompute: document.querySelector('[data-testid="recompute-hint"]')?.textContent ?? null,
          stale: !!document.querySelector('.toolbar__stat--stale'),
          dirty: !!document.querySelector('[data-testid="recipe-dirty"]'),
          undoText: document.querySelector('.toolbar button[aria-label="撤销"]')?.innerText ?? null,
        };
      `);
      const at = `${width} 宽`;
      mustOk(m.innerWidth === width, `${at}：视口真的是这个宽度`, m);
      mustOk(Boolean(m.recompute && m.stale && m.dirty), `${at}：状态够挤（将重算、已过时、配方没存都在）`, m);
      report.ok(`${at}：图名框的内容区放得下「${EIGHT}」`, m.value === EIGHT && m.content >= m.text && m.nameOverflow <= 0,
        JSON.stringify(m));
      report.ok(`${at}：整条工具栏无溢出无裁切（运行区的状态字不被裁、撤销按钮收成箭头 ↶），配方下拉框整个在窗口里`,
        m.toolbarOverflow <= 0 && m.runClipped <= 0 && m.undoText?.trim() === "↶" && m.menuRight <= width, JSON.stringify(m));
    }
  } finally {
    await cdp.send("Emulation.clearDeviceMetricsOverride");
    await sleep(200);
  }
}

async function suiteCleanup(cdp) {
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.ui.getState().setAutoRun(true); return true;`);
}

export const paramsP4Suites = [suiteCliMatchesEditor, suiteToolbarWidth, suiteCleanup];
