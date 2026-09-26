// 图参数成形（docs/param-recipe-plan.md P1）的分组：验收 1–7。逐条结果见 docs/param-recipe-p1-acceptance.md。
//
// 1 schema / 往返：带完整规格的图参数经 Rust 存盘再读回不丢字段；完整规格与老格式在 core 里都能跑。
// 2 core：default 越过图参数自己的硬限位报 bad_param（paramPath = 名字，nodeId 为空）；params 传越界值同样；
//   老的无 type 图参数照常运行。
// 3 右键「纳入配方」：规格从 manifest 抄、default = 当前值、显式值被删；cacheKey 不变、结果命中缓存；一次 Ctrl+Z 还原。
// 4 子图内部参数纳入：子图参数 + 图参数两级；另一个实例不受影响；一次撤销还原两级；库算子进不去、没有这个动作。
// 5 被绑定参数的行上编辑：改的是图参数的 default，不产生 param_conflict。
// 6 RunOptions.params 传一个与 default 不同的值：被绑定节点的 cacheKey 与结果跟着变；传回 default 命中缓存。
// 7 P1.6：复制路径名带 nodeId；撤销回到保存点时 dirty 复原（visibleWhen 的 ne 在面板上由 P2 验收 11 的 enabledWhen（ne）覆盖）。
//
// 右键菜单与输入框走真实 DOM 事件（与 m4 的「提升为子图参数」同一个做法）；搭图走 store 的语义化动作。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { ROOT } from "./harness.mjs";
import { buildGraph, lit, mustOk, newDoc, pressCtrl, pressF5, runAndWait, select } from "./page.mjs";

// ------------------------------------------------------------ 页面侧的小工具

const snap = (cdp) => cdp.eval(`return window.__lyflow.snapshot();`);
const docOf = (cdp) => cdp.eval(`return window.__lyflow.stores.graph.getState().doc;`);

/** 在 Inspector 的参数行上右键，点菜单里的一项。返回 'ok' 或卡在哪一步。 */
async function paramMenu(cdp, param, item) {
  return cdp.eval(`
    const row = document.querySelector('[data-testid="param-${param}"]');
    if (!row) return 'no-row';
    const r = row.getBoundingClientRect();
    row.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true, clientX: r.left + 10, clientY: r.top + 10,
    }));
    await new Promise((done) => setTimeout(done, 120));
    const btn = document.querySelector('[data-testid="${item}"]');
    if (!btn) {
      document.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
      return 'no-item';
    }
    btn.click();
    await new Promise((done) => setTimeout(done, 150));
    return 'ok';
  `);
}

/** 在参数行的第 i 个输入框里打字再失焦（NumberInput 失焦才提交，一次编辑一条撤销）。 */
async function typeInto(cdp, param, text, i = 0) {
  return cdp.eval(`
    const row = document.querySelector('[data-testid="param-${param}"]');
    const input = row?.querySelectorAll('input')[${i}];
    if (!input) return 'no-input';
    input.focus();
    const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
    setter.call(input, ${lit(text)});
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.blur();
    await new Promise((done) => setTimeout(done, 120));
    return 'ok';
  `);
}

/** Inspector 里这一行的样子：是否由图参数提供、输入框里显示的值、是否只读。 */
const rowOf = (cdp, param) =>
  cdp.eval(`
    const row = document.querySelector('[data-testid="param-${param}"]');
    if (!row) return null;
    return {
      graphParam: row.getAttribute('data-graph-param'),
      tag: row.querySelector('[data-testid="param-graph-${param}"]')?.textContent ?? null,
      values: [...row.querySelectorAll('input')].map((i) => i.value),
      disabled: row.querySelector('input')?.disabled ?? null,
    };
  `);

/** 键盘快捷键之前先把焦点从输入框里拿出来，否则 Ctrl+Z 进的是输入框自己的撤销。 */
async function undoByKey(cdp) {
  await cdp.eval(`if (document.activeElement instanceof HTMLElement) document.activeElement.blur(); return true;`);
  await pressCtrl(cdp, "Z");
  await sleep(200);
}

/** 立刻编一次计划，返回 { 展开后的 id: cacheKey }。 */
async function planKeys(cdp) {
  await cdp.eval(`await window.__lyflow.plan(); return true;`);
  return cdp.eval(`
    const out = {};
    for (const [id, n] of window.__lyflow.stores.cache.getState().plan) out[id] = n.cacheKey;
    return out;
  `);
}

/** 最近一次运行开始时每个节点的 cacheKey（run_started.nodes）。 */
const ranKeys = (cdp) => cdp.eval(`return Object.fromEntries(window.__lyflow.stores.cache.getState().ranWith);`);

/** 这次运行里某个节点第一个输出的元素数。 */
const countOf = (run, id) => run.nodes[id]?.elementCount ?? null;

/** 直接问 core（不经 debounce）：transport.validateGraph，带编辑器合成的图参数取值。 */
const validateNow = (cdp, params) =>
  cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    const given = ${params === undefined ? "undefined" : lit(params)};
    return await b.transport.validateGraph(g.doc, g.filePath, given ?? b.snapshot().recipe.params ?? undefined);
  `);

const SEED = 60000 + (Date.now() % 9000);

// ------------------------------------------------------------ 验收 3 / 5 / 6 / 7（复制路径）

async function suiteIncludeTopLevel(cdp, report) {
  report.section("P1 验收 3：普通节点参数「纳入配方」——规格从 manifest 抄、default = 当前值、结果不变、一次撤销");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed: SEED } },
      { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
      { key: "cut", op: "filter.passthrough", params: { max: 0.8 } },
    ],
    [
      { from: ["gen", "cloud"], to: ["voxel", "cloud"] },
      { from: ["voxel", "cloud"], to: ["cut", "cloud"] },
    ],
  );
  const first = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(first.status === "ok", "纳入前先跑一遍", first.status);
  const keysBefore = await ranKeys(cdp);
  const docBefore = await docOf(cdp);

  await select(cdp, ids.voxel);
  await sleep(200);
  const viaMenu = await paramMenu(cdp, "leafSize", "param-menu-include");
  mustOk(viaMenu === "ok", "走真实右键菜单「纳入配方」", viaMenu);

  const s = await snap(cdp);
  const gp = s.doc.params?.leafSize;
  const { manifestDecl, opLabel } = await cdp.eval(`
    const op = window.__lyflow.stores.manifest.getState().operatorsById.get('filter.voxel_grid');
    return { manifestDecl: op.params.find((p) => p.name === 'leafSize'), opLabel: op.label };
  `);
  const voxelNode = s.doc.nodes.find((n) => n.id === ids.voxel);
  report.eq(
    "doc 里出现了图参数 leafSize：default = 纳入前的当前值、binds 指着这个节点参数、规格（type / min / max / step / unit / doc）" +
      "从 manifest 复制、label 是「节点标题 · 参数 label」、节点上的显式值被删了、一次撤销记录",
    {
      exists: Boolean(gp),
      default: gp?.default,
      binds: gp?.binds,
      type: gp?.type,
      limits: [gp?.min, gp?.max, gp?.step, gp?.unit, gp?.doc],
      label: gp?.label,
      explicitValue: voxelNode?.params?.leafSize ?? null,
      undoLabel: s.undoLabel,
    },
    {
      exists: true,
      default: [0.02, 0.02, 0.02],
      binds: [`${ids.voxel}.leafSize`],
      type: "vec3f",
      limits: [manifestDecl.min, manifestDecl.max, manifestDecl.step, manifestDecl.unit, manifestDecl.doc],
      label: `${opLabel} · ${manifestDecl.label}`,
      explicitValue: null,
      undoLabel: "纳入配方 leafSize",
    },
  );

  const row = await rowOf(cdp, "leafSize");
  report.ok("Inspector 这一行标着由图参数提供，文字是「由图参数 leafSize 提供」",
    row?.graphParam === "leafSize" && (row?.tag ?? "").includes("由图参数 leafSize 提供"), JSON.stringify(row));
  report.eq("行上显示的是图参数的有效值", row?.values, ["0.02", "0.02", "0.02"]);

  const keysAfter = await planKeys(cdp);
  report.eq(
    "cacheKey 与纳入前逐个相同（结果逐位相同）",
    [ids.gen, ids.voxel, ids.cut].map((id) => keysAfter[id] === keysBefore[id]),
    [true, true, true],
  );
  const again = await runAndWait(cdp, () => pressF5(cdp));
  report.eq(
    "再跑一遍全部命中缓存",
    [ids.gen, ids.voxel, ids.cut].map((id) => again.nodes[id]?.state),
    ["skipped", "skipped", "skipped"],
  );

  await undoByKey(cdp);
  const undone = await docOf(cdp);
  report.eq("一次 Ctrl+Z 完全还原（doc 与纳入前逐字段相同）", JSON.stringify(undone), JSON.stringify(docBefore));

  // ------------------------------------------------ 验收 5：在被绑定的行上编辑
  report.section("P1 验收 5：在被绑定参数的行上编辑 = 改图参数的 default，不产生 param_conflict");
  await select(cdp, ids.gen);
  await sleep(200);
  const included = await paramMenu(cdp, "pointCount", "param-menu-include");
  mustOk(included === "ok", "纳入 gen.pointCount", included);
  const typed = await typeInto(cdp, "pointCount", "30000");
  mustOk(typed === "ok", "在这一行的输入框里输 30000", typed);
  const edited = await snap(cdp);
  const genNode = edited.doc.nodes.find((n) => n.id === ids.gen);
  report.eq("doc 里改的是图参数的 default，节点上没有写成显式值，撤销记录是「修改图参数」",
    { default: edited.doc.params?.pointCount?.default, explicitValue: genNode?.params?.pointCount ?? null, undoLabel: edited.undoLabel },
    { default: 30000, explicitValue: null, undoLabel: "修改图参数 pointCount" });
  const diags = await validateNow(cdp);
  report.ok(
    "core 校验没有 param_conflict（也没有别的错）",
    Array.isArray(diags) && !diags.some((d) => d.code === "param_conflict") &&
      !diags.some((d) => d.severity === "error"),
    JSON.stringify(diags),
  );
  report.eq("行上显示新值（数字框）", (await rowOf(cdp, "pointCount"))?.values?.[0], "30000");
  const ran30k = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("运行用上了新的 default", countOf(ran30k, ids.gen), 30000);

  // ------------------------------------------------ 验收 6：RunOptions.params
  report.section("P1 验收 6：RunOptions.params 传另一个值 —— cacheKey 与结果跟着变，传回 default 命中缓存");
  const base = await ranKeys(cdp);
  const other = await runAndWait(cdp, () =>
    cdp.eval(`await window.__lyflow.run({ params: { pointCount: 12000 } }); return true;`),
  );
  const otherKeys = await ranKeys(cdp);
  report.eq("被绑定节点的结果跟着变", countOf(other, ids.gen), 12000);
  report.eq(
    "被绑定节点与下游的 cacheKey 跟着变",
    [ids.gen, ids.voxel, ids.cut].map((id) => otherKeys[id] !== base[id]),
    [true, true, true],
  );
  report.eq("doc 没被改（运行传参不进 GraphDoc）", (await docOf(cdp)).params.pointCount.default, 30000);
  const back = await runAndWait(cdp, () => pressF5(cdp));
  const backKeys = await ranKeys(cdp);
  report.eq(
    "传回 default：cacheKey 回到原来那一组、命中缓存",
    [ids.gen, ids.voxel, ids.cut].map((id) => [backKeys[id] === base[id], back.nodes[id]?.state]),
    [[true, "skipped"], [true, "skipped"], [true, "skipped"]],
  );

  // ------------------------------------------------ 验收 7（一）：复制路径名
  report.section("P1 验收 7：P1.6 三项 —— 复制路径名带 nodeId");
  await select(cdp, ids.cut);
  await sleep(200);
  await cdp.eval(`
    window.__lyCopied = null;
    // 剪贴板在 WebView2 里未必授权：换成一个记录器，菜单走的还是它自己的那条 copyText
    navigator.clipboard.writeText = async (t) => { window.__lyCopied = t; };
    return true;
  `);
  const copied = await paramMenu(cdp, "max", "param-menu-path");
  mustOk(copied === "ok", "右键 → 复制路径名", copied);
  await sleep(100);
  report.eq("剪贴板里是「节点.参数」", await cdp.eval(`return window.__lyCopied;`), `${ids.cut}.max`);
}

// ------------------------------------------------------------ 验收 4

async function suiteIncludeInSubgraph(cdp, report, ws) {
  report.section("P1 验收 4：子图内部参数纳入配方 —— 两级提升、另一个实例不变、一次撤销；库算子内部没有这个动作");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed: SEED + 1 } },
      { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] }, row: 0 },
    ],
    [{ from: ["gen", "cloud"], to: ["voxel", "cloud"] }],
  );
  // 合成子图，再复制出第二个实例，两个实例都接上 gen
  const made = await cdp.eval(`
    const g = window.__lyflow.stores.graph;
    const composed = g.getState().composeSubgraph([${lit(ids.voxel)}]);
    const dup = g.getState().duplicateNodes([composed.nodeId]).nodeIds[0];
    const v = g.getState().connect({ node: ${lit(ids.gen)}, port: 'cloud' }, { node: dup, port: 'cloud' });
    return { a: composed.nodeId, b: dup, subgraphId: composed.subgraphId, wired: v.ok };
  `);
  mustOk(made.wired && made.a !== made.b, "两个实例共用同一份子图定义", made);
  const first = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(first.status === "ok", "纳入前先跑一遍", first.status);
  const keysBefore = await ranKeys(cdp);
  const docBefore = await docOf(cdp);
  const innerA = `${made.a}/${ids.voxel}`;
  const innerB = `${made.b}/${ids.voxel}`;

  await cdp.eval(`
    window.__lyflow.stores.ui.getState().enterSubgraph({ nodeId: ${lit(made.a)}, subgraphId: ${lit(made.subgraphId)} });
    return true;
  `);
  await sleep(250);
  await select(cdp, ids.voxel);
  await sleep(200);
  const includedInner = await paramMenu(cdp, "minPointsPerVoxel", "param-menu-include");
  mustOk(includedInner === "ok", "在实例 A 里右键内参 → 纳入配方", includedInner);

  const s = await snap(cdp);
  const sp = s.doc.subgraphs[made.subgraphId].params.find((p) => p.name === "minPointsPerVoxel");
  const gp = s.doc.params?.minPointsPerVoxel;
  const label = gp?.label ?? "";
  report.eq(
    "两级提升：子图定义里多了提升参数（绑着内参、默认值 = 内参当前值）、图参数绑在实例 A 上（default = 当前值、label 带上实例标题）、" +
      "实例 B 上没写任何东西、一次撤销记录",
    {
      subgraphParam: sp ? { binds: sp.binds, default: sp.default } : null,
      graphParam: gp ? { binds: gp.binds, default: gp.default } : null,
      label,
      labelHasInstance: label.includes(" / ") && label.endsWith("· Min Points / Voxel"),
      instanceB: s.doc.nodes.find((n) => n.id === made.b)?.params ?? {},
      undoLabel: s.undoLabel,
    },
    {
      subgraphParam: { binds: [{ node: ids.voxel, param: "minPointsPerVoxel" }], default: 0 },
      graphParam: { binds: [`${made.a}.minPointsPerVoxel`], default: 0 },
      label,
      labelHasInstance: true,
      instanceB: {},
      undoLabel: "纳入配方 minPointsPerVoxel",
    },
  );
  const inner = await rowOf(cdp, "minPointsPerVoxel");
  report.eq("子图里这一行标着由图参数提供", inner?.graphParam, "minPointsPerVoxel");
  report.eq("而且可以在这里改（改的是图参数）", inner?.disabled, false);

  await cdp.eval(`window.__lyflow.stores.ui.getState().exitTo(0); return true;`);
  await sleep(200);
  const keysAfter = await planKeys(cdp);
  report.eq("纳入后两个实例的 cacheKey 都不变", [keysAfter[innerA] === keysBefore[innerA], keysAfter[innerB] === keysBefore[innerB]],
    [true, true]);

  // 改图参数：只有实例 A 变，实例 B 的结果不变
  await cdp.eval(`window.__lyflow.stores.graph.getState().editGraphParamValue('minPointsPerVoxel', 3); return true;`);
  const run = await runAndWait(cdp, () => pressF5(cdp));
  const keysRun = await ranKeys(cdp);
  report.eq("实例 A 的内部节点 cacheKey 变了", keysRun[innerA] !== keysBefore[innerA], true);
  report.eq("实例 B 的 cacheKey 不变", keysRun[innerB], keysBefore[innerB]);
  report.eq("实例 B 命中缓存、结果不变", [run.nodes[innerB]?.state, countOf(run, innerB)],
    ["skipped", countOf(first, innerB)]);
  await undoByKey(cdp); // 撤掉「改成 3」

  await undoByKey(cdp); // 撤掉「纳入配方」
  const undone = await docOf(cdp);
  report.eq("一次 Ctrl+Z 把两级一起还原", JSON.stringify(undone), JSON.stringify(docBefore));

  // ------------------------------------------------ 库算子：内部只读，没有这个动作
  const libId = `e2e_p1_${Date.now().toString(36)}`;
  const libReady = await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    await b.transport.saveAsLibrary(g.doc, ${lit(made.subgraphId)}, { id: ${lit(libId)}, category: 'E2E' });
    const r = await b.transport.refreshLibrary();
    b.stores.manifest.getState().replaceBundle(r.manifest, 0);
    const libNode = b.stores.graph.getState().addNode(${lit("lib." + libId)}, { x: 600, y: 400 });
    return { libNode, dirs: r.status.dirs };
  `);
  await cdp.eval(`
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    window.__lyflow.stores.ui.getState().setSelection([${lit(libReady.libNode)}], []);
    return true;
  `);
  await pressKeyCtrlEnter(cdp);
  const pathAfter = (await snap(cdp)).path;
  report.ok("库算子放进了画布，Ctrl+Enter 进不去（内部只读，看不到内参行）",
    Boolean(libReady?.libNode) && JSON.stringify(pathAfter) === "[]", JSON.stringify({ libReady, pathAfter }));
  const forged = await cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().setPath([{ nodeId: ${lit(libReady.libNode)}, subgraphId: ${lit(libId)} }]);
    const name = b.stores.graph.getState().promoteToGraphParam(${lit(ids.voxel)}, 'leafSize');
    const why = b.stores.graph.getState().lastRejection;
    b.stores.ui.getState().setPath([]);
    b.stores.graph.getState().clearRejection();
    return { name, why };
  `);
  report.eq("硬塞一个指进库算子的层级：store 也拒绝纳入", forged.name, null);
  // 收尾：删库文件，免得积压
  const libFile = libReady.dirs?.[0] ? path.join(libReady.dirs[0], `${libId}.lyflow-op.json`) : null;
  if (libFile && fs.existsSync(libFile)) {
    fs.rmSync(libFile, { force: true });
    await cdp.eval(`
      const r = await window.__lyflow.transport.refreshLibrary();
      window.__lyflow.stores.manifest.getState().replaceBundle(r.manifest, 0);
      return true;
    `);
  }
  void ws;
}

async function pressKeyCtrlEnter(cdp) {
  const base = { key: "Enter", code: "Enter", windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13, modifiers: 2 };
  await cdp.send("Input.dispatchKeyEvent", { type: "rawKeyDown", ...base });
  await cdp.send("Input.dispatchKeyEvent", { type: "keyUp", ...base });
  await sleep(250);
}

// ------------------------------------------------------------ 验收 1 / 2

async function suiteSpecAndCore(cdp, report, ws) {
  report.section("P1 验收 1：带完整规格的图参数 —— Rust 存盘往返不丢字段，完整规格与老格式都能跑");

  await newDoc(cdp);
  const fixture = path.join(ROOT, "schema", "examples", "graph-params.example.lyflow.json");
  const original = JSON.parse(fs.readFileSync(fixture, "utf8"));
  const saved = path.join(ws.dir, "图参数 往返.lyflow.json");
  const round = await cdp.eval(`
    const b = window.__lyflow;
    const loaded = await b.transport.loadGraph(${lit(fixture)});
    await b.transport.saveGraph(${lit(saved)}, loaded.doc);
    const back = await b.transport.loadGraph(${lit(saved)});
    b.stores.graph.getState().loadDoc(back.doc, ${lit(saved)});
    return back.doc.params;
  `);
  report.eq("经 Tauri 存盘再读回，params 逐字（含键顺序）相同", JSON.stringify(round), JSON.stringify(original.params));
  report.eq("core 校验：完整规格与老格式都干净", await validateNow(cdp), []);
  const ran = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("整张图跑通，gen 用的是图参数 pointCount 的 default", { status: ran.status, gen: countOf(ran, "n_gen") },
    { status: "ok", gen: 20000 });
  const table = await cdp.eval(`
    return [...document.querySelectorAll('[data-testid="graph-params"] [data-graph-param]')]
      .map((el) => el.getAttribute('data-graph-param'));
  `);
  report.eq("Inspector 顶上的图参数简表列出全部四个", table, ["leafSize", "cutField", "pointCount", "cutMax"]);

  report.section("P1 验收 2：core 按图参数自己的规格校验 default 与传入的值；无 type 的老图参数照常");
  await cdp.eval(`window.__lyflow.stores.graph.getState().setGraphParamDefault('pointCount', 30000000); return true;`);
  const bad = await validateNow(cdp);
  const hit = (bad ?? []).find((d) => d.code === "bad_param" && d.paramPath === "pointCount");
  report.ok("default 越过硬限位（max 20000000）：bad_param，paramPath 是图参数名、nodeId 为空", Boolean(hit) && !hit.nodeId,
    JSON.stringify(bad));
  await cdp.eval(`await window.__lyflow.validate(); return true;`);
  await sleep(100);
  const marked = await cdp.eval(`
    return document.querySelector('[data-testid="graph-param-pointCount"]')?.getAttribute('data-param-error') ?? null;
  `);
  report.eq("诊断贴在图参数简表的那一行上", marked, "1");
  const blocked = await runAndWait(cdp, () => pressF5(cdp));
  report.ok("有 error 时运行被拦下，一个节点都没进 running", blocked.status === "error" &&
    !Object.values(blocked.nodes).some((n) => n.state === "done"), JSON.stringify({ status: blocked.status, nodes: blocked.nodes }));
  await undoByKey(cdp);
  report.eq("撤销后 default 回到合法值", (await docOf(cdp)).params.pointCount.default, 20000);

  const low = await validateNow(cdp, { pointCount: 0, leafSize: [0.02, 0.02, 0.02], cutField: "z", cutMax: 1.5 });
  report.ok("params 传入越界值（0 < min 1）同样报 bad_param",
    (low ?? []).some((d) => d.code === "bad_param" && d.paramPath === "pointCount" && !d.nodeId),
    JSON.stringify(low));
  const lowRun = await runAndWait(cdp, () =>
    cdp.eval(`await window.__lyflow.run({ params: { pointCount: 0 } }); return true;`),
  );
  report.eq("传越界值运行：被拦下", lowRun.status, "error");
  const badEnum = await validateNow(cdp, { cutField: "w" });
  report.ok("options 也按图参数自己的规格查", (badEnum ?? []).some((d) => d.paramPath === "cutField" && !d.nodeId),
    JSON.stringify(badEnum));
  const old = await validateNow(cdp, { cutMax: 99 });
  report.eq("无 type 的老图参数 cutMax：不做图参数这一层的校验", old, []);
  const oldRun = await runAndWait(cdp, () =>
    cdp.eval(`await window.__lyflow.run({ params: { cutMax: 99 } }); return true;`),
  );
  report.eq("老图参数传值照常运行", oldRun.status, "ok");
}

// ------------------------------------------------------------ 验收 7（ne、dirty）

async function suiteP16(cdp, report, ws) {
  report.section("P1 验收 7：P1.6 —— 撤销回到保存点时 dirty 复原");

  // 一张带子图参数（visibleWhen 用 ne）的小图，dirty 的验证拿它当底子。ne 在 Inspector 上的显隐不再在这里断言：
  // 面板那一侧由 P2 验收 11 的 enabledWhen（ne）覆盖，core 那一侧（conditionHolds / manifest 导出）由 doctest 验
  await newDoc(cdp);
  const inst = await cdp.eval(`
    const b = window.__lyflow;
    const doc = {
      schemaVersion: 1, id: 'p1-ne', nodes: [
        { id: 'n_gen', op: 'gen.synthetic', params: {} },
        { id: 'n_s', op: 'sub:sg_ne', params: {} },
      ],
      edges: [{ id: 'e1', from: { node: 'n_gen', port: 'cloud' }, to: { node: 'n_s', port: 'cloud' } }],
      subgraphs: { sg_ne: {
        name: 'ne', nodes: [{ id: 'v', op: 'filter.voxel_grid', params: {} }], edges: [],
        inputs: [{ name: 'cloud', type: 'PointCloud', to: [{ node: 'v', port: 'cloud' }] }],
        outputs: [{ name: 'cloud', type: 'PointCloud', from: { node: 'v', port: 'cloud' } }],
        params: [
          { name: 'mode', type: 'enum', default: 'a', options: [{ value: 'a', label: 'A' }, { value: 'b', label: 'B' }], binds: [] },
          { name: 'extra', type: 'int', default: 0, visibleWhen: { param: 'mode', ne: 'a' },
            binds: [{ node: 'v', param: 'minPointsPerVoxel' }] },
        ] } },
    };
    b.stores.graph.getState().loadDoc(doc, null);
    return 'n_s';
  `);
  await select(cdp, inst);
  await sleep(250);

  // dirty：存盘 → 改 → Ctrl+Z 回到保存点 → dirty 复原；重做又脏
  const file = path.join(ws.dir, "P1 保存点.lyflow.json");
  await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    await b.transport.saveGraph(${lit(file)}, g.doc);
    g.markSaved(${lit(file)}, g.doc);
    return true;
  `);
  const marker = () => cdp.eval(`return !!document.querySelector('.toolbar__dirty');`);
  const clean = [(await snap(cdp)).dirty, await marker()];
  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam('n_gen', 'seed', 99); return true;`);
  await sleep(100);
  report.eq("存盘之后不脏，改一下就脏（[dirty, ●]）", { saved: clean, edited: [(await snap(cdp)).dirty, await marker()] },
    { saved: [false, false], edited: [true, true] });
  await undoByKey(cdp);
  report.eq("Ctrl+Z 回到保存点：dirty 复原、标题栏的 ● 消失", [(await snap(cdp)).dirty, await marker()], [false, false]);
  await cdp.eval(`
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    return true;
  `);
  await pressCtrl(cdp, "Y");
  await sleep(200);
  report.eq("Ctrl+Y 重做：又脏了", (await snap(cdp)).dirty, true);
}

export const paramsP1Suites = [suiteSpecAndCore, suiteIncludeTopLevel, suiteIncludeInSubgraph, suiteP16];
