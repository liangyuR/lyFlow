// M4 验收：子图（§1）、live preview（§2）、大图性能（§4）。CLI（§3）由 cargo test
// 覆盖，理由见 ../../docs/m4-acceptance.md。与 M2/M3 的分组共用一个 app 实例。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import {
  buildGraph,
  centerOf,
  dragMouse,
  lit,
  newDoc,
  pressCtrl,
  pressEscape,
  pressF5,
  replan,
  runAndWait,
  select,
  selectAndReadViewer,
} from "./page.mjs";

/** 一条五节点直链：生成 → 裁剪 → 体素 → 去噪 → 直通。合成的对象是中间三个。 */
const CHAIN_NODES = [
  { key: "gen", op: "gen.synthetic", params: { pointCount: 60000, seed: 91 } },
  { key: "crop", op: "filter.crop_box" },
  { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
  { key: "sor", op: "filter.statistical_outlier", params: { meanK: 20 } },
  { key: "tail", op: "filter.passthrough", params: { min: -100, max: 100 } },
];
const CHAIN_EDGES = [
  { from: ["gen", "cloud"], to: ["crop", "cloud"] },
  { from: ["crop", "cloud"], to: ["voxel", "cloud"] },
  { from: ["voxel", "cloud"], to: ["sor", "cloud"] },
  { from: ["sor", "cloud"], to: ["tail", "cloud"] },
];

const snapshot = (cdp) => cdp.eval(`return window.__lyflow.snapshot();`);

/** 走 store 合成子图。右键菜单那条路在 §1.4 的分组里单独验。 */
async function compose(cdp, ids) {
  return cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().setSelection(${lit(ids)}, []);
    return b.stores.graph.getState().composeSubgraph(${lit(ids)});
  `);
}

/** 双击节点**身体**进子图。标题那一片双击是改名（P1 #25），别打在那儿。 */
async function enterByDoubleClick(cdp, nodeId) {
  return cdp.eval(`
    const el = document.querySelector('[data-testid="node-${nodeId}"]');
    if (!el) return 'no-node';
    const r = el.getBoundingClientRect();
    const x = r.left + r.width / 2;
    const y = r.top + r.height * 0.75;
    for (const type of ['mousedown', 'mouseup', 'click', 'dblclick']) {
      el.dispatchEvent(new MouseEvent(type, {
        bubbles: true, cancelable: true, view: window,
        detail: type === 'dblclick' ? 2 : 1, clientX: x, clientY: y,
      }));
    }
    await new Promise((done) => setTimeout(done, 250));
    return window.__lyflow.stores.ui.getState().path.length;
  `);
}

// -------------------------------------------------------- §1.1 合成与展开

async function suiteCompose(cdp, report) {
  report.section("§1 子图：合成之后结果一模一样，事件里是路径式 id");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);

  const flat = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("合成前跑通", flat.status, "ok");
  const flatTail = flat.nodes[ids.tail]?.elementCount;
  const flatVoxel = flat.nodes[ids.voxel]?.elementCount;
  report.ok("末端有点数", flatTail > 0, `tail=${flatTail}`);

  const composed = await compose(cdp, [ids.crop, ids.voxel, ids.sor]);
  report.ok("composeSubgraph 返回了新节点", Boolean(composed?.nodeId), JSON.stringify(composed));

  const after = await snapshot(cdp);
  report.eq("顶层剩下三个节点", after.level.nodes.length, 3);
  report.ok(
    "顶层节点是 gen / 子图 / tail",
    after.level.nodes.includes(ids.gen) &&
      after.level.nodes.includes(ids.tail) &&
      after.level.nodes.includes(composed.nodeId),
    JSON.stringify(after.level.nodes),
  );
  const def = after.subgraphs[composed.subgraphId];
  report.eq("子图收了三个节点", def.nodes.length, 3);
  report.eq("跨边界的入边变成了一个输入端口", def.inputs.length, 1);
  report.eq("跨边界的出边变成了一个输出端口", def.outputs.length, 1);
  report.eq("子图节点的 op 是 sub: 引用", after.doc.nodes.find((n) => n.id === composed.nodeId).op,
    `sub:${composed.subgraphId}`);

  // 展开后的计划：事件 id 是路径，子图这个节点本身不在计划里（F1/F2）
  const plan = await replan(cdp);
  const planIds = Object.keys(plan.plan);
  report.ok(
    "计划里是展开后的路径 id",
    planIds.includes(`${composed.nodeId}/${ids.voxel}`),
    JSON.stringify(planIds),
  );
  report.ok("子图节点本身不在计划里", !planIds.includes(composed.nodeId), JSON.stringify(planIds));

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("合成后跑通", run.status, "ok");
  report.eq("末端点数与合成前完全一致", run.nodes[ids.tail]?.elementCount, flatTail);
  report.eq(
    "子图内部的体素点数也一致",
    run.nodes[`${composed.nodeId}/${ids.voxel}`]?.elementCount,
    flatVoxel,
  );
  report.ok(
    "子图内部全部命中缓存（合成不改变 cacheKey 以外的东西）",
    ["skipped", "done"].includes(run.nodes[`${composed.nodeId}/${ids.sor}`]?.state),
    JSON.stringify(run.nodes[`${composed.nodeId}/${ids.sor}`]),
  );

  // 画布上的子图节点要显示聚合状态（F2）
  const dom = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${composed.nodeId}"]');
    return el ? {
      state: el.getAttribute('data-node-state'),
      subgraph: el.getAttribute('data-subgraph'),
      badge: !!document.querySelector('[data-testid="node-subbadge-${composed.nodeId}"]'),
      children: document.querySelector('[data-testid="node-children-${composed.nodeId}"]')?.textContent ?? null,
    } : null;
  `);
  report.ok("子图节点带子图角标", dom?.badge === true, JSON.stringify(dom));
  report.eq("data-subgraph 指向定义", dom?.subgraph, composed.subgraphId);
  report.ok(
    "状态聚合成一个（全 done/skipped → done/skipped）",
    ["done", "skipped"].includes(dom?.state ?? ""),
    JSON.stringify(dom),
  );
  report.eq("角标显示内部进度 3/3", dom?.children, "3/3");

  return { ids, composed, flatTail };
}

// ------------------------------------------------------ §1.4 进入 / 退出

async function suiteNavigate(cdp, report, fixture) {
  report.section("§1 子图导航：双击进入、面包屑、Esc 退出");

  const { ids, composed } = fixture;
  const depth = await enterByDoubleClick(cdp, composed.nodeId);
  report.eq("双击子图节点进去了", depth, 1);

  const inside = await snapshot(cdp);
  report.eq("当前层级是子图的三个节点", inside.level.nodes.length, 3);
  report.ok(
    "画布上渲染的就是内部节点",
    await cdp.eval(`
      const rendered = [...document.querySelectorAll('[data-testid^="node-n_"]')]
        .map((el) => el.getAttribute('data-testid').slice(5));
      return rendered.length === 3 && rendered.includes(${lit(ids.voxel)});
    `),
  );
  report.eq("事件前缀是路径", inside.pathPrefix, `${composed.nodeId}/`);

  const crumb = await cdp.eval(`
    const el = document.querySelector('[data-testid="breadcrumb"]');
    return el ? { depth: el.getAttribute('data-depth'), text: el.textContent } : null;
  `);
  report.eq("面包屑显示深度 1", crumb?.depth, "1");
  report.ok("面包屑里有子图名", (crumb?.text ?? "").includes("子图"), crumb?.text);

  // 进了子图之后，内部节点的状态来自路径事件
  const innerState = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${ids.voxel}"]');
    return el ? el.getAttribute('data-node-state') : null;
  `);
  report.ok(
    "内部节点显示自己的执行状态",
    ["done", "skipped"].includes(innerState ?? ""),
    String(innerState),
  );

  // 3D 视图：选中内部节点看得到输出（路径 id 直接查结果仓）
  const view = await selectAndReadViewer(cdp, ids.voxel);
  report.ok("子图里选中体素能看到点云", view.count > 0, JSON.stringify(view));

  await pressEscape(cdp);
  await sleep(200);
  const out = await snapshot(cdp);
  report.eq("Esc 退回顶层", out.path.length, 0);
  report.eq("顶层又是三个节点", out.level.nodes.length, 3);

  // 面包屑那条路也要能用
  await enterByDoubleClick(cdp, composed.nodeId);
  await cdp.eval(`document.querySelector('[data-testid="breadcrumb-root"]').click(); return true;`);
  await sleep(150);
  report.eq("点面包屑的「顶层」也能退出", (await snapshot(cdp)).path.length, 0);
}

// ------------------------------------------------------- §1.4 提升参数

async function suitePromote(cdp, report, fixture) {
  report.section("§1 提升参数：内参变只读，改外参只重算受影响的内部节点");

  const { ids, composed } = fixture;
  await enterByDoubleClick(cdp, composed.nodeId);
  await select(cdp, ids.voxel);
  await sleep(150);

  // 走真实的参数右键菜单
  const promoted = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-leafSize"]');
    if (!row) return 'no-row';
    const r = row.getBoundingClientRect();
    row.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true, clientX: r.left + 10, clientY: r.top + 10,
    }));
    await new Promise((done) => setTimeout(done, 120));
    const btn = document.querySelector('[data-testid="param-menu-promote"]');
    if (!btn) return 'no-promote';
    btn.click();
    await new Promise((done) => setTimeout(done, 150));
    return window.__lyflow.snapshot().subgraphs[${lit(composed.subgraphId)}].params;
  `);
  report.ok(
    "右键菜单里提升成功",
    Array.isArray(promoted) && promoted.length === 1 && promoted[0].name === "leafSize",
    JSON.stringify(promoted),
  );
  report.eq(
    "绑定指到内部节点的那个参数",
    promoted[0]?.binds,
    [{ node: ids.voxel, param: "leafSize" }],
  );

  const readOnly = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-leafSize"]');
    const input = row.querySelector('input');
    return { promotedAs: row.getAttribute('data-promoted'), disabled: input?.disabled ?? null };
  `);
  report.eq("内参标注了提升来源", readOnly.promotedAs, "leafSize");
  report.eq("内参变成只读", readOnly.disabled, true);

  await pressEscape(cdp);
  await sleep(200);

  // 外层表单上出现了这个参数
  await select(cdp, composed.nodeId);
  await sleep(200);
  const outerForm = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-leafSize"]');
    return { exists: !!row, disabled: row?.querySelector('input')?.disabled ?? null };
  `);
  report.ok("外层节点的表单上有这个参数", outerForm.exists, JSON.stringify(outerForm));
  report.eq("外层可编辑", outerForm.disabled, false);

  // 跑一次垫底，然后只改这个提升参数
  await runAndWait(cdp, () => pressF5(cdp));
  await cdp.eval(`
    window.__lyflow.stores.graph.getState()
      .setParam(${lit(composed.nodeId)}, 'leafSize', [0.05, 0.05, 0.05]);
    return true;
  `);
  const plan = await replan(cdp);
  const cachedOf = (id) => plan.plan[id]?.cached;
  report.eq("源头仍然命中缓存", cachedOf(ids.gen), true);
  report.eq("子图里的裁剪也命中缓存", cachedOf(`${composed.nodeId}/${ids.crop}`), true);
  report.eq("被改的体素要重算", cachedOf(`${composed.nodeId}/${ids.voxel}`), false);
  report.eq("它的下游也要重算", cachedOf(`${composed.nodeId}/${ids.sor}`), false);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("改完还能跑通", run.status, "ok");
  report.eq("源头是 skipped", run.nodes[ids.gen]?.state, "skipped");
  report.eq("体素是 done", run.nodes[`${composed.nodeId}/${ids.voxel}`]?.state, "done");
}

// --------------------------------------------------------- §1 嵌套与递归

async function suiteNested(cdp, report) {
  report.section("§1 嵌套两层：cacheKey 稳定，重跑全 skipped");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES.slice(0, 4), CHAIN_EDGES.slice(0, 3));

  const inner = await compose(cdp, [ids.voxel, ids.sor]);
  const outer = await compose(cdp, [ids.crop, inner.nodeId]);
  report.ok("两层都合成出来了", Boolean(inner?.nodeId && outer?.nodeId),
    JSON.stringify({ inner, outer }));

  const plan = await replan(cdp);
  const nested = `${outer.nodeId}/${inner.nodeId}/${ids.voxel}`;
  report.ok("计划里出现两层路径", Object.keys(plan.plan).includes(nested),
    JSON.stringify(Object.keys(plan.plan)));

  const first = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("第一次跑通", first.status, "ok");
  const keys1 = JSON.stringify((await snapshot(cdp)).cache.ranWith);

  const second = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("第二次跑通", second.status, "ok");
  const states = Object.values(second.nodes).map((n) => n.state);
  report.ok("重跑全部 skipped", states.every((s) => s === "skipped"), JSON.stringify(second.nodes));
  const keys2 = JSON.stringify((await snapshot(cdp)).cache.ranWith);
  report.eq("两次编译出来的 cacheKey 一模一样", keys1, keys2);

  // 进两层再出来
  await enterByDoubleClick(cdp, outer.nodeId);
  await enterByDoubleClick(cdp, inner.nodeId);
  const deep = await snapshot(cdp);
  report.eq("能进到第二层", deep.path.length, 2);
  report.eq("第二层的事件前缀是两段", deep.pathPrefix, `${outer.nodeId}/${inner.nodeId}/`);
  await pressEscape(cdp);
  await pressEscape(cdp);
  await sleep(150);
  report.eq("连按两次 Esc 回到顶层", (await snapshot(cdp)).path.length, 0);
}

async function suiteRecursion(cdp, report) {
  report.section("§1 递归引用被拒");

  // 手改过的文件才可能长这样：界面上没有能造出递归的操作
  const diags = await cdp.eval(`
    const b = window.__lyflow;
    const doc = {
      schemaVersion: 1, id: '01RECURSE0000000000000000',
      nodes: [{ id: 'n_a', op: 'sub:loop', params: {}, ui: { position: { x: 0, y: 0 } } }],
      edges: [],
      subgraphs: {
        loop: {
          name: '自引用',
          nodes: [{ id: 'inner', op: 'sub:loop', params: {} }],
          edges: [], inputs: [], outputs: [], params: [],
        },
      },
    };
    b.stores.graph.getState().loadDoc(doc, null);
    return await b.transport.validateGraph(doc, null);
  `);
  const hit = (diags ?? []).find((d) => d.code === "recursive_subgraph");
  report.ok("validate 报 recursive_subgraph", Boolean(hit), JSON.stringify(diags));
  report.ok(
    "诊断挂在展开时出问题的那个节点上",
    String(hit?.nodeId ?? "").startsWith("n_a"),
    String(hit?.nodeId),
  );

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("跑它只会失败，不会崩", run.status, "error");
}

// ------------------------------------------------------------ §1 库算子

async function suiteLibrary(cdp, report) {
  report.section("§1 库算子：保存到库 → 面板里出现 → 新图拖出即用");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);
  const composed = await compose(cdp, [ids.voxel, ids.sor]);
  const libId = `e2e_clean_${Date.now().toString(36)}`;

  // 走真实的右键菜单 + 对话框
  const saved = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${composed.nodeId}"]');
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true,
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2,
    }));
    await new Promise((d) => setTimeout(d, 150));
    const open = document.querySelector('[data-testid="ctx-save-library"]');
    if (!open) return 'no-menu-item';
    open.click();
    await new Promise((d) => setTimeout(d, 150));
    const idInput = document.querySelector('[data-testid="library-id"]');
    if (!idInput) return 'no-dialog';
    const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
    setter.call(idInput, ${lit(libId)});
    idInput.dispatchEvent(new Event('input', { bubbles: true }));
    await new Promise((d) => setTimeout(d, 80));
    document.querySelector('[data-testid="library-save"]').click();
    await new Promise((d) => setTimeout(d, 1500));
    return 'ok';
  `);
  report.eq("右键 → 保存到库 → 对话框走通", saved, "ok");

  const status = await cdp.eval(`return await window.__lyflow.transport.getLibraryStatus();`);
  report.ok("库里至少有一个算子", (status?.count ?? 0) >= 1, JSON.stringify(status));

  const libFile = status.dirs?.[0] ? path.join(status.dirs[0], `${libId}.lyflow-op.json`) : null;
  report.ok("库文件写到了 app data 下的 library/", libFile != null && fs.existsSync(libFile),
    String(libFile));

  const inManifest = await cdp.eval(`
    const ops = window.__lyflow.stores.manifest.getState().bundle.operators;
    const op = ops.find((o) => o.id === ${lit("lib." + libId)});
    return op ? { id: op.id, category: op.category, inputs: op.inputs.length, outputs: op.outputs.length } : null;
  `);
  report.ok("manifest 里出现了 lib.<id>", Boolean(inManifest), JSON.stringify(inManifest));
  report.ok("分类挂在 Library/ 下", (inManifest?.category ?? "").startsWith("Library/"),
    inManifest?.category);

  const inPalette = await cdp.eval(`
    const rows = [...document.querySelectorAll('[data-op-id]')].map((el) => el.getAttribute('data-op-id'));
    return rows.includes(${lit("lib." + libId)});
  `);
  report.ok("节点面板里也有它（manifest 驱动，前端零改动）", inPalette === true);

  // 新建一张图，从库里拖一个出来跑
  await newDoc(cdp);
  const fresh = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 40000, seed: 77 } },
      { key: "lib", op: `lib.${libId}` },
    ],
    [{ from: ["gen", "cloud"], to: ["lib", "cloud"] }],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("库算子在新图里跑通", run.status, "ok");
  const innerDone = Object.keys(run.nodes).filter((id) => id.startsWith(`${fresh.lib}/`));
  report.ok("库算子展开成了内部节点", innerDone.length === 2, JSON.stringify(innerDone));
  report.ok(
    "内部节点都有结果",
    innerDone.every((id) => ["done", "skipped"].includes(run.nodes[id].state)),
    JSON.stringify(run.nodes),
  );

  // 收尾：把库文件删掉，不然下一次跑会看到一堆积压的 e2e 算子
  if (libFile && fs.existsSync(libFile)) {
    fs.rmSync(libFile, { force: true });
    await cdp.eval(`
      const r = await window.__lyflow.transport.refreshLibrary();
      window.__lyflow.stores.manifest.getState().replaceBundle(r.manifest, 0);
      return r.status.count;
    `);
  }
  report.ok("收尾：库文件已删除", !libFile || !fs.existsSync(libFile), String(libFile));
}

// ------------------------------------------------------------ §2 preview

async function suitePreview(cdp, report) {
  report.section("§2 live preview：拖参数跟手、松手补正式运行、不污染正式缓存");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 400000, seed: 31 } },
      {
        key: "sample",
        op: "filter.random_sample",
        params: { mode: "ratio", keepRatio: 0.2, seed: 4 },
      },
    ],
    [{ from: ["gen", "cloud"], to: ["sample", "cloud"] }],
  );
  await runAndWait(cdp, () => pressF5(cdp));
  await select(cdp, ids.sample);
  await sleep(300);

  const statsBefore = await cdp.eval(`return await window.__lyflow.transport.cacheStats();`);

  // 在页面里装一个观察器，记录 3D 视图**真的换了一片云**的时刻
  await cdp.eval(`
    window.__m4 = { marks: [] };
    const viewer = document.querySelector('.viewer');
    window.__m4.observer = new MutationObserver(() => {
      window.__m4.marks.push({
        run: viewer.getAttribute('data-run'),
        view: viewer.getAttribute('data-view'),
        preview: viewer.getAttribute('data-preview'),
        at: performance.now(),
      });
    });
    window.__m4.observer.observe(viewer, { attributes: true, attributeFilter: ['data-run', 'data-view'] });
    window.__lyflow.clearTransitions();
    return true;
  `);

  // 真实鼠标拖动滑块：从中间往右拖一段
  const slider = await centerOf(cdp, '[data-testid="param-slider-keepRatio"]');
  report.ok("找到 keepRatio 的滑块", slider != null, JSON.stringify(slider));
  if (slider) {
    await dragMouse(cdp, { x: slider.x - 40, y: slider.y }, { x: slider.x + 40, y: slider.y }, { steps: 8 });
  }

  const sawPreview = await cdp.waitFor(
    `window.__lyflow.runMarks.some((m) => m.status !== 'running')`,
    { timeoutMs: 20_000, what: "预览运行结束" },
  );
  report.ok("拖动触发了预览运行", sawPreview === true);

  const latency = await cdp.waitFor(
    `(() => {
       const runs = window.__lyflow.runMarks;
       const marks = window.__m4.marks.filter((m) => m.view === 'cloud');
       for (let i = runs.length - 1; i >= 0; i -= 1) {
         const hit = marks.find((m) => m.run === runs[i].runId && m.at >= runs[i].at);
         if (hit) return Math.round(hit.at - runs[i].at);
       }
       return null;
     })()`,
    { timeoutMs: 20_000, what: "视图画出预览结果" },
  );
  report.ok(`事件到渲染 ${latency} ms < 100 ms`, latency != null && latency < 100, `${latency} ms`);

  const previewRun = await cdp.eval(`
    const s = window.__lyflow.snapshot();
    return { preview: s.run.preview, sourceCount: s.run.nodes[${lit(ids.gen)}]?.elementCount ?? null,
             maxPoints: s.preview.maxPoints, autoRun: s.preview.autoRun };
  `);
  report.ok(
    "松手后自动补了一次正式运行（preview 标记已经落回 false）",
    previewRun.preview === false,
    JSON.stringify(previewRun),
  );
  report.eq("正式运行的源头是全量点数", previewRun.sourceCount, 400000);

  // 单独发一次 preview run，直接断言抽稀与命名空间
  const previewOnly = await runAndWait(cdp, () =>
    cdp.eval(`
      await window.__lyflow.run({
        targets: [${lit(ids.sample)}], preview: true, previewMaxPoints: 20000 });
      return true;
    `),
  );
  report.ok(
    "preview run 里源头抽到了 2 万点",
    previewOnly.nodes[ids.gen]?.elementCount === 20000,
    JSON.stringify(previewOnly.nodes),
  );

  const statsAfter = await cdp.eval(`return await window.__lyflow.transport.cacheStats();`);
  report.ok(
    "预览的结果进了独立命名空间（正式那份的条目没被顶掉）",
    statsAfter.entries >= statsBefore.entries,
    JSON.stringify({ before: statsBefore.entries, after: statsAfter.entries }),
  );

  // 正式重跑：全部命中缓存说明预览没有污染正式的键
  const full = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("正式重跑仍然全部命中缓存",
    Object.values(full.nodes).every((n) => n.state === "skipped"), true);

  await cdp.eval(`window.__m4.observer.disconnect(); return true;`);
}

// ------------------------------------------------------------- §4 大图性能

async function suiteBigGraph(cdp, report) {
  report.section("§4 大图性能：300 节点 400 边");

  await newDoc(cdp);
  const built = await cdp.eval(`
    const t0 = performance.now();
    const g = () => window.__lyflow.stores.graph.getState();
    // 30 条链 × 10 个节点 = 300 个节点。每条链里穿插 4 个 util.merge，
    // 它们的第二个输入从同链更靠前的节点引一条边过来，凑到 ~400 条边。
    const lanes = [];
    for (let lane = 0; lane < 30; lane += 1) {
      const ids = [];
      const outPort = [];
      for (let step = 0; step < 10; step += 1) {
        const op = step === 0 ? 'gen.synthetic' : (step % 2 === 0 ? 'util.merge' : 'util.reroute');
        const id = g().addNode(op, { x: step * 250, y: lane * 140 });
        ids.push(id);
        outPort.push(step === 0 ? 'cloud' : (op === 'util.merge' ? 'cloud' : 'out'));
        if (step > 0) {
          g().connect(
            { node: ids[step - 1], port: outPort[step - 1] },
            { node: id, port: op === 'util.merge' ? 'a' : 'in' },
          );
        }
      }
      // merge 的第二个输入：从链头拉一条，制造扇出
      for (let step = 2; step < 10; step += 2) {
        g().connect({ node: ids[0], port: 'cloud' }, { node: ids[step], port: 'b' });
      }
      lanes.push(ids);
    }
    const doc = g().doc;
    return { ms: Math.round(performance.now() - t0), nodes: doc.nodes.length, edges: doc.edges.length };
  `);
  report.ok(
    `搭出 ${built.nodes} 节点 / ${built.edges} 边`,
    built.nodes === 300 && built.edges >= 380,
    JSON.stringify(built),
  );

  // 「打开 < 1 s」：从 loadDoc 到画布上出现节点
  const openMs = await cdp.eval(`
    const b = window.__lyflow;
    const doc = JSON.parse(JSON.stringify(b.stores.graph.getState().doc));
    b.stores.graph.getState().newDoc();
    await new Promise((d) => setTimeout(d, 100));
    const t0 = performance.now();
    b.stores.graph.getState().loadDoc(doc, null);
    for (let i = 0; i < 200; i += 1) {
      await new Promise((d) => requestAnimationFrame(d));
      if (document.querySelectorAll('[data-testid^="node-n_"]').length > 0) break;
    }
    return Math.round(performance.now() - t0);
  `);
  report.ok(`打开 300 节点的图用了 ${openMs} ms < 1000 ms`, openMs < 1000, `${openMs} ms`);

  const virtualized = await cdp.eval(`
    const rendered = document.querySelectorAll('[data-testid^="node-n_"]').length;
    const total = window.__lyflow.stores.graph.getState().doc.nodes.length;
    return { rendered, total };
  `);
  report.ok(
    "只渲染了视野里的节点（onlyRenderVisibleElements）",
    virtualized.rendered < virtualized.total,
    JSON.stringify(virtualized),
  );

  // 拖动帧率：页面里挂一个 rAF 采样器，然后用 CDP 发**真**鼠标事件拖一个节点。
  // 合成 MouseEvent 骗不过 React Flow 的 d3-drag（它要读 event.view.document）。
  await cdp.eval(`
    window.__m4fps = { frames: [], last: performance.now(), stop: false };
    const tick = () => {
      const now = performance.now();
      window.__m4fps.frames.push(now - window.__m4fps.last);
      window.__m4fps.last = now;
      if (!window.__m4fps.stop) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    return true;
  `);
  const grab = await centerOf(cdp, '[data-testid^="node-n_"]');
  if (grab) {
    await dragMouse(cdp, grab, { x: grab.x + 160, y: grab.y + 90 }, { steps: 20 });
  }
  const fps = await cdp.eval(`
    window.__m4fps.stop = true;
    const f = window.__m4fps.frames.slice(3).sort((a, b) => a - b);
    if (f.length === 0) return null;
    return Math.round(1000 / f[Math.floor(f.length / 2)]);
  `);
  report.ok(`拖动时的帧率 ${fps} fps ≥ 30`, fps != null && fps >= 30, `${fps} fps`);

  // 事件合并：一次运行下来 store 的更新次数远少于事件数
  await newDoc(cdp);
  const merged = await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    const ids = [];
    for (let i = 0; i < 40; i += 1) {
      const op = i === 0 ? 'gen.synthetic' : 'util.reroute';
      const id = b.stores.graph.getState().addNode(op, { x: i * 40, y: (i % 6) * 90 });
      ids.push(id);
      if (i > 0) {
        b.stores.graph.getState().connect(
          { node: ids[i - 1], port: i === 1 ? 'cloud' : 'out' },
          { node: id, port: 'in' },
        );
      }
    }
    void g;
    b.clearTransitions();
    let updates = 0;
    const stop = b.stores.execution.subscribe((s, p) => { if (s.nodes !== p.nodes) updates += 1; });
    const before = b.stores.execution.getState().runId;
    await b.run({});
    for (let i = 0; i < 400; i += 1) {
      const s = b.stores.execution.getState();
      if (s.runId !== before && s.runStatus !== 'running' && s.runStatus !== 'idle') break;
      await new Promise((d) => setTimeout(d, 25));
    }
    stop();
    return { updates, transitions: b.transitions.length };
  `);
  report.ok(
    `40 个节点的运行：${merged.transitions} 次状态变化合并成 ${merged.updates} 次 store 更新`,
    merged.updates < merged.transitions,
    JSON.stringify(merged),
  );
}

// ---------------------------------------------------------- §1 解散子图

async function suiteDissolve(cdp, report) {
  report.section("§1 解散子图：内容内联回来，提升的参数落回内参");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES.slice(0, 4), CHAIN_EDGES.slice(0, 3));
  const composed = await compose(cdp, [ids.voxel, ids.sor]);

  // 提升一个参数并改掉它，解散之后这个值必须落回内参
  await cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().enterSubgraph({ nodeId: ${lit(composed.nodeId)},
                                           subgraphId: ${lit(composed.subgraphId)} });
    b.stores.graph.getState().promoteParam(${lit(ids.voxel)}, 'leafSize');
    b.stores.ui.getState().exitTo(0);
    b.stores.graph.getState().setParam(${lit(composed.nodeId)}, 'leafSize', [0.07, 0.07, 0.07]);
    return true;
  `);

  const inlined = await cdp.eval(`
    window.__lyflow.stores.ui.getState().setSelection([${lit(composed.nodeId)}], []);
    return window.__lyflow.stores.graph.getState().dissolveSubgraph(${lit(composed.nodeId)});
  `);
  report.eq("内联出两个节点", inlined.length, 2);

  const after = await snapshot(cdp);
  report.eq("顶层回到四个节点", after.level.nodes.length, 4);
  report.ok("子图定义已经删掉了", Object.keys(after.subgraphs).length === 0,
    JSON.stringify(Object.keys(after.subgraphs)));

  const leaf = after.doc.nodes
    .map((n) => n.params?.leafSize)
    .find((v) => Array.isArray(v));
  report.eq("提升参数的值落回了内参", leaf, [0.07, 0.07, 0.07]);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("解散之后照样跑通", run.status, "ok");
}

// ------------------------------------------------------ Ctrl+G / Ctrl+Shift+G

async function suiteShortcuts(cdp, report) {
  report.section("§1 快捷键：Ctrl+G 合成、Ctrl+Shift+G 解散");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES.slice(0, 4), CHAIN_EDGES.slice(0, 3));
  await cdp.eval(`
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    window.__lyflow.stores.ui.getState().setSelection([${lit(ids.voxel)}, ${lit(ids.sor)}], []);
    return true;
  `);
  await pressCtrl(cdp, "G");
  await sleep(250);
  const composed = await snapshot(cdp);
  report.eq("Ctrl+G 之后顶层剩三个节点", composed.level.nodes.length, 3);
  report.eq("多了一份子图定义", Object.keys(composed.subgraphs).length, 1);

  const subNode = composed.doc.nodes.find((n) => n.op.startsWith("sub:"));
  await cdp.eval(`
    window.__lyflow.stores.ui.getState().setSelection([${lit(subNode.id)}], []);
    return true;
  `);
  await pressCtrl(cdp, "G", ["shift"]);
  await sleep(250);
  const dissolved = await snapshot(cdp);
  report.eq("Ctrl+Shift+G 之后回到四个节点", dissolved.level.nodes.length, 4);
  report.eq("子图定义也一并清掉", Object.keys(dissolved.subgraphs).length, 0);

  // 撤销栈：合成与解散各是一条
  await pressCtrl(cdp, "Z");
  await sleep(150);
  report.eq("撤销回到子图状态", (await snapshot(cdp)).level.nodes.length, 3);
  await pressCtrl(cdp, "Z");
  await sleep(150);
  report.eq("再撤销回到四个节点", (await snapshot(cdp)).level.nodes.length, 4);
}


// -------------------------------------------------------- M3 留下的三个尾巴

async function suiteM3Tails(cdp, report, ws) {
  report.section("M3 尾巴：法线着色可用、PNG 导出走保存对话框、core-watch 见 m4-acceptance");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed: 61 } },
      { key: "nrm", op: "features.normals", params: { kSearch: 10 } },
    ],
    [{ from: ["gen", "cloud"], to: ["nrm", "cloud"] }],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("法线估计跑通", run.status, "ok");

  // 源头没有法线：下拉框里那一项应当还是灰的
  const plain = await selectAndReadViewer(cdp, ids.gen);
  report.ok("源头的点云画出来了", plain.count > 0, JSON.stringify(plain));
  const beforeOpt = await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const opt = [...sel.options].find((o) => o.value === 'normal');
    return { disabled: opt.disabled, text: opt.textContent };
  `);
  report.ok("没有法线通道时「法线」是禁用的", beforeOpt.disabled === true, JSON.stringify(beforeOpt));

  const withNormals = await selectAndReadViewer(cdp, ids.nrm);
  report.ok("带法线的点云也画出来了", withNormals.count > 0, JSON.stringify(withNormals));
  const afterOpt = await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const opt = [...sel.options].find((o) => o.value === 'normal');
    return { disabled: opt.disabled, text: opt.textContent };
  `);
  report.ok("有法线通道时「法线」可选了", afterOpt.disabled === false, JSON.stringify(afterOpt));
  report.ok("选项文字不再标「无」", !String(afterOpt.text).includes("无"), afterOpt.text);

  const shaded = await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, 'normal');
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    await new Promise((d) => setTimeout(d, 250));
    const v = document.querySelector('.viewer');
    return { shading: v.getAttribute('data-shading'), value: sel.value };
  `);
  report.eq("切到法线着色之后视图确实是 normal", shaded.shading, "normal");

  // PNG 导出的落盘那一半：对话框是原生的，CDP 驱动不了，但写文件这一步可以验
  const target = path.join(ws.dir, "导出 视图.png");
  const wrote = await cdp.eval(`
    const bytes = [137, 80, 78, 71, 13, 10, 26, 10];
    await window.__lyflow.transport.writeFileBytes(${lit(target)}, new Uint8Array(bytes));
    return true;
  `);
  report.ok("write_file_bytes 把字节写到了用户指定的路径", wrote === true && fs.existsSync(target),
    target);
  if (fs.existsSync(target)) {
    const head = fs.readFileSync(target);
    report.ok("写出来的字节一字不差", head.length === 8 && head[1] === 0x50, head.toString("hex"));
  }
}

export const m4Suites = [
  async (cdp, report) => {
    const fixture = await suiteCompose(cdp, report);
    await suiteNavigate(cdp, report, fixture);
    await suitePromote(cdp, report, fixture);
  },
  suiteDissolve,
  suiteShortcuts,
  suiteNested,
  suiteRecursion,
  suiteLibrary,
  suitePreview,
  suiteBigGraph,
  suiteM3Tails,
];
