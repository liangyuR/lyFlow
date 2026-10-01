// M3 验收：核心轨 §1.1–1.6 每项，编辑轨交互清单 P1 #17–#30 每项至少一条断言。
// 与 M2 的分组共用一个 app 实例，helper 在 ./page.mjs。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { UNTITLED_BACKUP } from "./harness.mjs";
import {
  buildGraph,
  centerOf,
  clickAt,
  clickUntilPicked,
  pressKey,
  openInspectorAdvanced,
  dragMouse,
  lit,
  mustOk,
  newDoc,
  pressCtrl,
  pressEscape,
  pressF5,
  pressQuestion,
  canvasBox,
  normalizeZoom,
  placeAtScreen,
  viewportScale,
  pressShiftF5,
  replan,
  restoreClipboard,
  runAndWait,
  saveGraphTo,
  select,
  selectAndReadViewer,
  stubClipboard,
} from "./page.mjs";

/** 一条三节点直链：生成 → 体素 → 透传。缓存与 stale 的分组都用它。 */
const CHAIN_NODES = [
  { key: "gen", op: "gen.synthetic", params: { pointCount: 30000, seed: 3 } },
  { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
  { key: "tail", op: "util.reroute" },
];
const CHAIN_EDGES = [
  { from: ["gen", "cloud"], to: ["voxel", "cloud"] },
  { from: ["voxel", "cloud"], to: ["tail", "in"] },
];

/** 等画布上正好剩下这些节点。不等的话拖动会抓到上一组留下的旧 handle。 */
async function waitForNodes(cdp, ids) {
  const list = Object.values(ids);
  await cdp.waitFor(
    `(() => {
       const rendered = [...document.querySelectorAll('[data-testid^="node-n_"]')]
         .map(el => el.getAttribute('data-testid').slice(5));
       return rendered.length === ${list.length} &&
              ${lit(list)}.every(id => rendered.includes(id));
     })()`,
    { timeoutMs: 8000, what: "画布重建完成" },
  );
  await sleep(150);
}

const domOf = (cdp, selector, expr) =>
  cdp.eval(`
    const el = document.querySelector(${lit(selector)});
    if (!el) return null;
    return (${expr});
  `);

// ------------------------------------------------------------- 1.1 缓存复用

async function suiteCache(cdp, report) {
  report.section("1.1 缓存复用：重跑全 skipped、改中间节点只重算下游");

  await cdp.eval(`await window.__lyflow.transport.clearCache(); return true;`);
  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);

  const cold = await replan(cdp);
  report.eq("冷启动时没有节点被判为已缓存", Object.values(cold.plan).filter((n) => n.cached).length, 0);
  report.ok(
    "plan_graph 给出了 32 位 cacheKey",
    Object.values(cold.plan).every((n) => typeof n.cacheKey === "string" && n.cacheKey.length === 32),
    JSON.stringify(cold.plan),
  );
  report.eq(
    "工具栏提示将重算 3 个节点",
    await domOf(cdp, '[data-testid="recompute-hint"]', "el.getAttribute('data-count')"),
    "3",
  );

  const first = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("第一次全部真算", Object.values(first.nodes).filter((n) => n.state === "done").length, 3);

  // 预测集合必须与实际 skipped 集合完全一致（§1.1 验收）
  const warm = await replan(cdp);
  const predicted = Object.entries(warm.plan)
    .filter(([, n]) => n.cached)
    .map(([id]) => id)
    .sort();
  report.eq(
    "工具栏改口说全部命中缓存",
    await domOf(cdp, '[data-testid="recompute-hint"]', "el.getAttribute('data-count')"),
    "0",
  );

  const second = await runAndWait(cdp, () => pressF5(cdp));
  const skipped = Object.entries(second.nodes)
    .filter(([, n]) => n.state === "skipped")
    .map(([id]) => id)
    .sort();
  report.eq("原图重跑全部 skipped", skipped.length, 3);
  report.eq("plan_graph 的预测集合与实际 skipped 完全一致", predicted, skipped);
  report.ok(
    "skipped 的原因标成了「缓存」而不是「静音」，且输出可取（outputsAvailable）",
    Object.values(second.nodes).every(
      (n) => n.cached === true && n.bypassed === false && n.outputsAvailable === true,
    ),
    JSON.stringify(second.nodes),
  );
  report.ok("重跑总耗时 < 50 ms", second.durationMs != null && second.durationMs < 50,
    `${second.durationMs} ms`);

  // 改中间节点参数 → 只重算它和它的下游
  await cdp.eval(`
    window.__lyflow.stores.graph.getState()
      .setParam(${lit(ids.voxel)}, 'leafSize', [0.05, 0.05, 0.05]);
    return true;
  `);
  const afterEdit = await replan(cdp);
  report.eq("改完之后 stale 精确到那两个节点", afterEdit.stale.sort(), [ids.tail, ids.voxel].sort());
  report.eq(
    "画布上恰好两个节点是虚线框",
    await cdp.eval(`return document.querySelectorAll('.node.is-stale').length;`),
    2,
  );
  report.eq(
    "上游那个没有被连坐",
    await domOf(cdp, `[data-testid="node-${ids.gen}"]`, "el.getAttribute('data-stale')"),
    "0",
  );
  // 原 run.mjs 的 suiteStale 并到这里：运行后改参数，工具栏摘要要挂出「已过时」
  report.ok(
    "工具栏显示「已过时」",
    await cdp.eval(`return !!document.querySelector('.toolbar__stat--stale');`),
  );

  const third = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("上游 gen 命中缓存", third.nodes[ids.gen]?.state, "skipped");
  report.eq("改过的 voxel 重算了", third.nodes[ids.voxel]?.state, "done");
  report.eq("下游 tail 跟着重算", third.nodes[ids.tail]?.state, "done");

  // 抽屉里的缓存统计 + 清空缓存
  await cdp.eval(`window.__lyflow.stores.ui.getState().toggleDrawer('cache'); return true;`);
  await sleep(200);
  const entries = await domOf(cdp, '[data-testid="cache-entries"]', "el.textContent");
  report.ok("抽屉里的缓存条目数 > 0", Number(entries) > 0, `entries=${entries}`);
  report.ok(
    "状态栏显示缓存占用",
    Boolean(await domOf(cdp, '[data-testid="statusbar-cache"]', "el.textContent")),
  );

  await cdp.eval(`document.querySelector('[data-testid="cache-clear"]').click(); return true;`);
  await sleep(400);
  report.eq(
    "清空缓存之后条目归零",
    await domOf(cdp, '[data-testid="cache-entries"]', "el.textContent"),
    "0",
  );
  await cdp.eval(`window.__lyflow.stores.ui.getState().toggleDrawer('cache'); return true;`);

  return ids;
}

// ------------------------------------------------------------- 1.2 并行执行

async function suiteParallel(cdp, report) {
  report.section("1.2 并行执行：菱形图跑通、事件不重不漏");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 40000, seed: 11 } },
      { key: "a", op: "filter.voxel_grid", params: { leafSize: [0.01, 0.01, 0.01] }, row: 0 },
      { key: "b", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] }, row: 1 },
      { key: "c", op: "filter.statistical_outlier", params: { meanK: 12 }, row: 2 },
      { key: "d", op: "filter.radius_outlier", row: 3 },
      { key: "m1", op: "util.merge", row: 0 },
      { key: "m2", op: "util.merge", row: 2 },
      { key: "m", op: "util.merge", row: 1 },
    ],
    [
      { from: ["gen", "cloud"], to: ["a", "cloud"] },
      { from: ["gen", "cloud"], to: ["b", "cloud"] },
      { from: ["gen", "cloud"], to: ["c", "cloud"] },
      { from: ["gen", "cloud"], to: ["d", "cloud"] },
      { from: ["a", "cloud"], to: ["m1", "a"] },
      { from: ["b", "cloud"], to: ["m1", "b"] },
      { from: ["c", "cloud"], to: ["m2", "a"] },
      { from: ["d", "cloud"], to: ["m2", "b"] },
      { from: ["m1", "cloud"], to: ["m", "a"] },
      { from: ["m2", "cloud"], to: ["m", "b"] },
    ],
  );

  const run = await runAndWait(cdp, () => pressF5(cdp));

  // 每个节点恰好走一遍 pending → running → done。并行下最容易坏的就是这个：
  // 事件顺序乱了、或者同一个节点被两个 worker 领走。序列走到 done，运行也就是 ok 的；
  // 头一条 idle 是 run_started 播下的占位。seq 的连续性由 execution store 的
  // console.warn 兜底，控制台分组会捕获它。
  const transitions = await cdp.eval(`return window.__lyflow.transitions;`);
  const expected = ["idle", "pending", "running", "done"];
  const badSeq = [];
  for (const key of Object.keys(ids)) {
    const seq = transitions.filter((t) => t.nodeId === ids[key]).map((t) => t.state);
    if (JSON.stringify(seq) !== JSON.stringify(expected)) badSeq.push(`${key}=${seq.join("→")}`);
  }
  report.ok(
    `${Object.keys(ids).length} 个节点的序列全是 idle→pending→running→done`,
    run.status === "ok" && badSeq.length === 0,
    `run.status=${run.status} 不对的：${badSeq.join("; ")}`,
  );
}

// --------------------------------------------------- 1.3 bypass 与 reroute

async function suiteBypassReroute(cdp, report) {
  report.section("1.3 / P1 #24 #25：静音透传、reroute 串联与类型推导");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);
  await waitForNodes(cdp, ids);
  const before = await runAndWait(cdp, () => pressF5(cdp));
  const rawCount = before.nodes[ids.gen]?.elementCount;
  mustOk(rawCount > 0, "先跑一次拿到原始点数", String(rawCount));
  report.ok(
    "体素确实降了采样",
    before.nodes[ids.voxel]?.elementCount < rawCount,
    `${before.nodes[ids.voxel]?.elementCount} < ${rawCount}`,
  );

  // Ctrl+M 静音（#25）。这是 change 动作，要进撤销栈。
  await select(cdp, ids.voxel);
  await pressCtrl(cdp, "M");
  await sleep(150);
  report.eq(
    "静音写进了 doc 的 bypass 字段",
    await cdp.eval(
      `return window.__lyflow.snapshot().doc.nodes.find(n => n.id === ${lit(ids.voxel)}).bypass;`,
    ),
    true,
  );
  report.eq(
    "画布上的节点标了静音",
    await domOf(cdp, `[data-testid="node-${ids.voxel}"]`, "el.getAttribute('data-bypass')"),
    "1",
  );
  report.eq(
    "静音进了撤销栈",
    await cdp.eval(`return window.__lyflow.snapshot().undoLabel;`),
    "静音节点",
  );

  const muted = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("静音的节点标 skipped", muted.nodes[ids.voxel]?.state, "skipped");
  report.eq("原因是静音而不是缓存", muted.nodes[ids.voxel]?.bypassed, true);
  report.eq("下游拿到的是原始点数", muted.nodes[ids.tail]?.elementCount, rawCount);

  await cdp.eval(`window.__lyflow.stores.graph.getState().undo(); return true;`);
  await sleep(150);
  report.ok(
    "撤销把静音撤掉了",
    !(await cdp.eval(
      `return window.__lyflow.snapshot().doc.nodes.find(n => n.id === ${lit(ids.voxel)}).bypass === true;`,
    )),
  );

  // #24：在边的右键菜单里插入 reroute（双击已让给连线查看器，见 edge-peek-plan P1）。
  // 摆成一条水平线，路径包围盒的中心才落在线上；
  // 命中要用 edge-interaction 那条粗路径，edge-path 的 pointer-events 是关的。
  await normalizeZoom(cdp, 0.6);
  const rerouteBox = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.gen]: { x: 16, y: 60 },
    [ids.voxel]: { x: Math.round(rerouteBox.w * 0.4), y: 60 },
    [ids.tail]: { x: Math.round(rerouteBox.w * 0.75), y: 60 },
  });
  const edgeId = await cdp.eval(`return window.__lyflow.snapshot().doc.edges[0].id;`);
  const point = await centerOf(
    cdp,
    `.react-flow__edge[data-id="${edgeId}"] .react-flow__edge-interaction`,
  );
  mustOk(Boolean(point), "拿到了连线中点", JSON.stringify(point));
  const pastBeforeReroute = await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
  const menu = await cdp.eval(`
    const el = document.elementFromPoint(${point.x}, ${point.y});
    if (!el) return 'no-element';
    el.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true, clientX: ${point.x}, clientY: ${point.y},
    }));
    await new Promise((done) => setTimeout(done, 200));
    const btn = document.querySelector('[data-testid="edge-ctx-reroute"]');
    if (!btn) return 'no-menu';
    btn.click();
    return 'ok';
  `);
  mustOk(menu === "ok", "边的右键菜单里有「在此插入 Reroute」", menu);
  await sleep(250);
  const doc = await cdp.eval(`return window.__lyflow.snapshot().doc;`);
  const reroutes = doc.nodes.filter((n) => n.op === "util.reroute");
  report.ok("右键菜单插入了一个 reroute", reroutes.length === 2, `${reroutes.length} 个 reroute 节点`);
  report.ok(
    "原来那条边被拆成了两条",
    doc.edges.length === 3 && !doc.edges.some((e) => e.id === edgeId),
    JSON.stringify(doc.edges.map((e) => `${e.from.node}.${e.from.port}→${e.to.node}.${e.to.port}`)),
  );

  // E6：Any 端口的实际类型由连线推导，端口着色跟着源走
  const inserted = reroutes.find((n) => n.id !== ids.tail);
  if (inserted) {
    const color = await domOf(
      cdp,
      `[data-testid="port-${inserted.id}-out"] .react-flow__handle`,
      "getComputedStyle(el).backgroundColor",
    );
    const source = await domOf(
      cdp,
      `[data-testid="port-${ids.gen}-cloud"] .react-flow__handle`,
      "getComputedStyle(el).backgroundColor",
    );
    report.eq("reroute 的端口颜色随源类型变化", color, source);
  }

  // 加 reroute 与插进连线一条撤销：以前两条，Ctrl+Z 一次连线回来了、reroute 还孤零零地留着
  await pressCtrl(cdp, "z");
  await sleep(200);
  const undoneReroute = await cdp.eval(`
    const d = window.__lyflow.snapshot().doc;
    return { reroutes: d.nodes.filter((n) => n.op === 'util.reroute').length, edge: d.edges.some((e) => e.id === ${lit(edgeId)}),
             past: window.__lyflow.stores.graph.getState().past.length };
  `);
  report.eq("Ctrl+Z 一次：插进去的 reroute 没了、原来那条边回来，撤销栈回到插入之前", undoneReroute,
    { reroutes: 1, edge: true, past: pastBeforeReroute });
  await pressCtrl(cdp, "y");
  await sleep(200);

  // 接不兼容的类型要被拒
  const verdict = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const plane = g.addNode('segment.ransac_plane', { x: 900, y: 400 });
    const v = g.connect(
      { node: ${lit(ids.tail)}, port: 'out' },
      { node: plane, port: 'cloud' },
    );
    const bad = window.__lyflow.stores.graph.getState().connect(
      { node: plane, port: 'inliers' },
      { node: ${lit(ids.tail)}, port: 'in' },
    );
    return { good: v.ok, bad: bad.ok, reason: bad.reason ?? '' };
  `);
  report.ok("推导成 PointCloud 之后能接到 PointCloud 端口", verdict.good, JSON.stringify(verdict));
  report.ok("接不兼容类型被拒", !verdict.bad, verdict.reason);
}

// ------------------------------------------------------------- 1.4 迁移

async function suiteMigration(cdp, report, ws) {
  report.section("1.4 迁移：v1 的图打开即改写、可撤销、再打开不再提示");

  const fixture = path.join(ws.dir, "老版本 随机抽样.lyflow.json");
  fs.writeFileSync(
    fixture,
    JSON.stringify(
      {
        schemaVersion: 1,
        id: "01J8XQZ4K7N3M2R5V8W1YB6TCD",
        name: "迁移 fixture",
        nodes: [
          {
            id: "g",
            op: "gen.synthetic",
            opVersion: "1.0.0",
            params: { pointCount: 5000 },
            ui: { position: { x: 40, y: 80 } },
          },
          {
            id: "s",
            op: "filter.random_sample",
            opVersion: "1.0.0",
            params: { count: 777, seed: 4 },
            ui: { position: { x: 300, y: 80 } },
          },
        ],
        edges: [
          { id: "e1", from: { node: "g", port: "cloud" }, to: { node: "s", port: "cloud" } },
        ],
      },
      null,
      2,
    ) + "\n",
    "utf8",
  );

  const loaded = await cdp.eval(`
    const b = window.__lyflow;
    const out = await b.transport.loadGraph(${lit(fixture)});
    b.stores.graph.getState().loadDoc(out.doc, ${lit(fixture)});
    return out.migrations;
  `);
  report.eq("load_graph 返回了一条迁移动作", loaded.length, 1);

  const applied = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const n = g.applyMigrations(${lit(loaded)});
    const s = window.__lyflow.snapshot();
    return { n, node: s.doc.nodes.find(x => x.id === 's'), dirty: s.dirty, undoLabel: s.undoLabel };
  `);
  report.eq("doc 里的参数换成了 keepCount", applied.node?.params?.keepCount, 777);
  report.eq("doc 里的 opVersion 升到了 2.0.0", applied.node?.opVersion, "2.0.0");
  report.eq("迁移置了 dirty", applied.dirty, true);
  report.eq("迁移是一条撤销记录", applied.undoLabel, "迁移 1 个节点");

  const undone = await cdp.eval(`
    window.__lyflow.stores.graph.getState().undo();
    return window.__lyflow.snapshot().doc.nodes.find(x => x.id === 's').params;
  `);
  report.eq("撤销之后回到旧参数", undone.count, 777);

  // 重做 → 保存 → 再打开：不该再提示迁移
  const again = await cdp.eval(`
    const b = window.__lyflow;
    b.stores.graph.getState().redo();
    await b.transport.saveGraph(${lit(fixture)}, b.stores.graph.getState().doc);
    const out = await b.transport.loadGraph(${lit(fixture)});
    return out.migrations;
  `);
  report.eq("保存后再打开不再提示迁移", again.length, 0);

  // 顶层图参数（M7 J7）：编辑器只原样保留，经 Tauri 后端保存再打开也不能丢。
  // 绑定目标 s.seed 从节点上拿掉 —— 同一个参数两处定义是 param_conflict。
  const graphParams = { seedAll: { type: "int", default: 4, binds: ["s.seed"], doc: "抽样种子" } };
  const kept = await cdp.eval(`
    const b = window.__lyflow;
    const doc = structuredClone(b.stores.graph.getState().doc);
    doc.params = ${lit(graphParams)};
    delete doc.nodes.find(x => x.id === 's').params.seed;
    await b.transport.saveGraph(${lit(fixture)}, doc);
    const out = await b.transport.loadGraph(${lit(fixture)});
    b.stores.graph.getState().loadDoc(out.doc, ${lit(fixture)});
    await b.transport.saveGraph(${lit(fixture)}, b.stores.graph.getState().doc);
    const back = await b.transport.loadGraph(${lit(fixture)});
    return JSON.stringify(back.doc.params);
  `);
  report.eq("顶层 params 经后端保存、打开、再保存原样保留", kept, JSON.stringify(graphParams));

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("迁移后的图跑得通", run.status, "ok");
  report.eq("抽样点数就是迁移过来的那个值", run.nodes["s"]?.elementCount, 777);
}

// ---------------------------------------------------------- 1.5 热重载装置

async function suiteHotReload(cdp, report) {
  report.section("1.5 热重载：开发期装置已就位");

  const info = await cdp.eval(`return await window.__lyflow.transport.getCoreInfo();`);
  // 安装包里不开（release 构建，bridge/src/core_ffi.rs 的 watch_source），那一条在 run.mjs 的「安装包」组里验
  const packaged = process.env.LYFLOW_E2E_PACKAGED === "1";
  report.ok(packaged ? "安装包：core 报告了代数，热重载关着" : "core 报告了热重载能力与代数",
    info?.hotReload === !packaged, JSON.stringify(info));
  report.eq("启动时是第 0 代", info?.generation, 0);
  // 状态栏的「热重载 · 第 N 代」只在开着时出现
  report.eq(
    packaged ? "状态栏没有热重载角标" : "状态栏的代数与 core 一致",
    await domOf(cdp, '[data-testid="statusbar-generation"]', "el.getAttribute('data-generation')"),
    packaged ? null : String(info?.generation ?? 0),
  );
  // 换代本身在 cargo test 里验（hot_reload_swaps_in_a_fresh_generation）：
  // CDP 没法在一次会话里重编 C++，手工复现步骤见 docs/m3-acceptance.md。
}

// ------------------------------------------------------------- 1.6 参数联动

async function suiteParamLinkage(cdp, report) {
  report.section("1.6 参数联动：visibleWhen 隐藏的参数不渲染也不校验必填");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 2000 } },
      { key: "rs", op: "filter.random_sample" },
    ],
    [{ from: ["gen", "cloud"], to: ["rs", "cloud"] }],
  );
  await select(cdp, ids.rs);
  await sleep(200);

  const byMode = async () =>
    cdp.eval(`
      return {
        keepCount: !!document.querySelector('[data-testid="param-keepCount"]'),
        keepRatio: !!document.querySelector('[data-testid="param-keepRatio"]'),
      };
    `);

  report.eq("mode=count 时只显示 keepCount", await byMode(), { keepCount: true, keepRatio: false });

  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.rs)}, 'mode', 'ratio');
    return true;
  `);
  await sleep(200);
  report.eq("换成 ratio 后只显示 keepRatio", await byMode(), { keepCount: false, keepRatio: true });

  // C++ 侧：隐藏的 path 参数不报「还没有选择文件」
  const diags = await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    return await b.transport.validateGraph(g.doc, g.filePath);
  `);
  report.eq("隐藏参数没有把图判成非法", diags.filter((d) => d.severity === "error").length, 0);

  // enabledWhen：passthrough 选了 intensity 时 keepOrganized 该禁用
  const pass = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const id = g.addNode('filter.passthrough', { x: 600, y: 80 });
    window.__lyflow.stores.graph.getState().setParam(id, 'field', 'intensity');
    window.__lyflow.stores.ui.getState().setSelection([id], []);
    return id;
  `);
  await sleep(250);
  // keepOrganized 在「高级」组里：Inspector 默认收起（与参数面板同一规则），展开才看得到
  const folded = await cdp.eval(`
    return {
      collapsed: !!document.querySelector('.insp__group--advanced[data-open="0"]'),
      rowShown: !!document.querySelector('[data-testid="param-keepOrganized"]'),
    };
  `);
  report.eq("高级组默认收起，里面的参数行不渲染", folded, { collapsed: true, rowShown: false });
  await openInspectorAdvanced(cdp);
  const disabled = await domOf(
    cdp,
    '[data-testid="param-keepOrganized"] input[type="checkbox"]',
    "el.disabled",
  );
  report.ok("enabledWhen 不满足时控件被禁用", disabled === true, `disabled=${disabled}`);
  void pass;
}

// ----------------------------------------------- P1 #17 #19 #20 #21 连线手感

/** #17：落点偏离端口中心 16 px 仍然能连上（connectionRadius = 24）。 */
async function suiteSnap(cdp, report) {
  report.section("P1 #17：连线端口吸附");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 3000 } },
      { key: "voxel", op: "filter.voxel_grid" },
    ],
    [],
  );
  await waitForNodes(cdp, ids);
  await normalizeZoom(cdp);
  await placeAtScreen(cdp, { [ids.gen]: { x: 20, y: 40 }, [ids.voxel]: { x: 300, y: 40 } });

  const source = await centerOf(cdp, `[data-testid="port-${ids.gen}-cloud"] .react-flow__handle`);
  const target = await centerOf(cdp, `[data-testid="port-${ids.voxel}-cloud"] .react-flow__handle`);
  mustOk(Boolean(source && target), "拿到了两个端口的位置", JSON.stringify({ source, target }));

  // connectionRadius 的单位是**画布坐标**，所以屏幕上的偏移量要乘缩放
  const k = await viewportScale(cdp);
  await dragMouse(cdp, source, {
    x: target.x - Math.round(16 * k),
    y: target.y - Math.round(10 * k),
  });
  const edges = await cdp.eval(`return window.__lyflow.snapshot().doc.edges;`);
  report.ok(
    "落点偏 16 个画布像素仍然吸附上了",
    edges.length === 1 && edges[0].to.node === ids.voxel,
    JSON.stringify(edges),
  );
}

/** #20：拖线中兼容端口高亮、不兼容端口置灰。 */
async function suitePortHints(cdp, report) {
  report.section("P1 #20：拖线时的端口兼容性可视化");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 1000 } },
      { key: "voxel", op: "filter.voxel_grid" },
      { key: "crop", op: "filter.crop_box", row: 1 },
    ],
    [],
  );
  await waitForNodes(cdp, ids);

  const highlight = await cdp.eval(`
    const b = window.__lyflow;
    b.stores.ui.getState().beginConnection(
      { node: ${lit(ids.gen)}, port: 'cloud', side: 'output' },
      new Set([${lit(`${ids.crop}:cloud`)}]),
    );
    await new Promise(r => setTimeout(r, 200));
    const ok = document.querySelector('[data-testid="port-${ids.crop}-cloud"]');
    const bad = document.querySelector('[data-testid="port-${ids.voxel}-cloud"]');
    const out = {
      compatible: ok ? ok.getAttribute('data-port-verdict') : null,
      incompatible: bad ? bad.getAttribute('data-port-verdict') : null,
      faded: bad ? Number(getComputedStyle(bad).opacity) : null,
    };
    b.stores.ui.getState().endConnection();
    await new Promise(r => setTimeout(r, 150));
    out.after = ok ? ok.getAttribute('data-port-verdict') : null;
    return out;
  `);
  report.eq("兼容端口被标成 compatible", highlight.compatible, "compatible");
  report.eq("不兼容端口被标成 incompatible", highlight.incompatible, "incompatible");
  report.ok("不兼容端口真的被压暗了", (highlight.faded ?? 1) < 0.5, String(highlight.faded));
  report.eq("松手之后标记清掉", highlight.after, null);
}

/** #19：拖离输入端 → 另一端跟着鼠标 → 落到别的兼容端口上。 */
async function suiteReconnect(cdp, report) {
  report.section("P1 #19：拖离输入端连线，另一端跟着鼠标");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 1000 } },
      { key: "voxel", op: "filter.voxel_grid" },
      { key: "crop", op: "filter.crop_box", row: 1 },
    ],
    [{ from: ["gen", "cloud"], to: ["voxel", "cloud"] }],
  );
  await waitForNodes(cdp, ids);
  // 两端拉够远：重连端点在端口外侧 reconnectRadius 处，离上游节点太近就会
  // 落在它的方块上，一按下去变成拖节点。缩放先压下去，否则塞不下两个节点。
  await normalizeZoom(cdp, 0.6);
  const box = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.gen]: { x: 16, y: 40 },
    [ids.voxel]: { x: Math.round(box.w * 0.62), y: 40 },
    [ids.crop]: { x: Math.round(box.w * 0.62), y: Math.round(box.h * 0.55) },
  });

  // React Flow 把可重连的端点画成一个 circle.react-flow__edgeupdater-target，
  // 位置在端口外侧 reconnectRadius 处 —— 从端口中心起手会被当成「新建连线」。
  const anchor = await centerOf(cdp, ".react-flow__edgeupdater-target");
  mustOk(Boolean(anchor), "连线的可重连端点存在", JSON.stringify(anchor));
  const hit = await cdp.eval(`
    const el = document.elementFromPoint(${anchor.x}, ${anchor.y});
    return el ? String(el.getAttribute('class') ?? el.tagName) : null;
  `);
  report.ok(
    "重连端点没有被上游节点盖住",
    String(hit).includes("edgeupdater"),
    `点上是 ${hit}`,
  );

  const newTarget = await centerOf(cdp, `[data-testid="port-${ids.crop}-cloud"] .react-flow__handle`);
  if (!newTarget) {
    report.fail("找不到新的落点端口", "");
    return;
  }
  await dragMouse(cdp, anchor, newTarget, { steps: 18 });
  await sleep(250);

  const after = await cdp.eval(`
    const s = window.__lyflow.snapshot();
    return { edges: s.doc.edges, undoLabel: s.undoLabel };
  `);
  report.ok(
    "拖离输入端后改接到了另一个端口",
    after.edges.length === 1 && after.edges[0].to.node === ids.crop,
    JSON.stringify(after.edges),
  );
  report.eq("改接是一条撤销记录", after.undoLabel, "改接连线");
  void ids.voxel;
}

/** #21：把一个孤立节点拖到连线上 → 自动插入到中间。 */
async function suiteInsertOnEdge(cdp, report) {
  report.section("P1 #21：拖节点到连线上自动插入");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 1000 } },
      { key: "crop", op: "filter.crop_box" },
      { key: "free", op: "filter.voxel_grid", row: 2 },
    ],
    [{ from: ["gen", "cloud"], to: ["crop", "cloud"] }],
  );
  await waitForNodes(cdp, ids);
  await normalizeZoom(cdp, 0.6);
  // 摆开：两端拉远、待插入的那个放在**上方**（下方会被抽屉和状态栏盖住），
  // 拖下来正好压在连线中点上
  const insertBox = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.gen]: { x: 16, y: Math.round(insertBox.h * 0.55) },
    [ids.crop]: { x: Math.round(insertBox.w * 0.62), y: Math.round(insertBox.h * 0.55) },
    [ids.free]: { x: Math.round(insertBox.w * 0.3), y: 30 },
  });
  await cdp.eval(`
    window.__lyflow.stores.ui.getState().setSelection([${lit(ids.free)}], []);
    return true;
  `);
  await sleep(200);

  // 命中判定在**画布坐标**里，用的是直线段而不是渲染出来的贝塞尔。落点按 doc 的
  // 位置 + 量测尺寸算准再换算成屏幕像素 —— 照视觉中点拖会差十几个像素。
  const plan = await cdp.eval(`
    const k = new DOMMatrixReadOnly(
      getComputedStyle(document.querySelector('.react-flow__viewport')).transform).a;
    const doc = window.__lyflow.snapshot().doc;
    const el = (id) => document.querySelector('[data-testid="node-' + id + '"]');
    const box = (id) => {
      const r = el(id).getBoundingClientRect();
      const p = doc.nodes.find(n => n.id === id).ui.position;
      return { x: p.x, y: p.y, w: r.width / k, h: r.height / k };
    };
    const a = box(${lit(ids.gen)}), b = box(${lit(ids.crop)}), me = box(${lit(ids.free)});
    const from = { x: a.x + a.w, y: a.y + a.h / 2 };
    const to = { x: b.x, y: b.y + b.h / 2 };
    const mid = { x: (from.x + to.x) / 2, y: (from.y + to.y) / 2 };
    const center = { x: me.x + me.w / 2, y: me.y + me.h / 2 };
    const head = el(${lit(ids.free)}).querySelector('.node__head').getBoundingClientRect();
    const grab = { x: Math.round(head.left + head.width / 2), y: Math.round(head.top + head.height / 2) };
    return {
      grab,
      drop: {
        x: Math.round(grab.x + (mid.x - center.x) * k),
        y: Math.round(grab.y + (mid.y - center.y) * k),
      },
    };
  `);
  if (!plan) {
    report.fail("拿不到拖动的起止点", "");
    return;
  }
  const freeAt = await cdp.eval(`return window.__lyflow.snapshot().doc.nodes.find((n) => n.id === ${lit(ids.free)}).ui.position;`);
  const pastBeforeDrag = await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
  await dragMouse(cdp, plan.grab, plan.drop, { steps: 20 });
  await sleep(400);

  const edges = await cdp.eval(`return window.__lyflow.snapshot().doc.edges;`);
  const inserted =
    edges.length === 2 &&
    edges.some((e) => e.to.node === ids.free) &&
    edges.some((e) => e.from.node === ids.free);
  report.ok("拖到连线上自动插入到中间", inserted, JSON.stringify(edges));
  report.ok(
    "插入之后原来那条边没了",
    !edges.some((e) => e.from.node === ids.gen && e.to.node === ids.crop),
    JSON.stringify(edges),
  );
  // 拖动与插入一条撤销：以前先记「移动节点」再记一条插入，Ctrl+Z 一次节点还插在线上
  await pressCtrl(cdp, "z");
  await sleep(200);
  const back = await cdp.eval(`
    const d = window.__lyflow.snapshot().doc;
    return { edges: d.edges.map((e) => e.from.node + '>' + e.to.node), at: d.nodes.find((n) => n.id === ${lit(ids.free)}).ui.position,
             past: window.__lyflow.stores.graph.getState().past.length };
  `);
  report.eq("Ctrl+Z 一次：原来那条边回来、节点回到拖之前的地方，撤销栈回到拖之前", back,
    { edges: [`${ids.gen}>${ids.crop}`], at: freeAt, past: pastBeforeDrag });
}

// ------------------------------------------------------------ P1 #18 搜索接上

async function suiteDropToSearch(cdp, report) {
  report.section("P1 #18：连线中途松手 → 搜索面板 → 自动接上");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [{ key: "gen", op: "gen.synthetic", params: { pointCount: 1000 } }],
    [],
  );
  await waitForNodes(cdp, ids);
  await normalizeZoom(cdp);
  await placeAtScreen(cdp, { [ids.gen]: { x: 40, y: 60 } });

  const source = await centerOf(cdp, `[data-testid="port-${ids.gen}-cloud"] .react-flow__handle`);
  if (!source) {
    report.fail("找不到源端口", "");
    return;
  }
  const pastBeforePick = await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
  await dragMouse(cdp, source, { x: source.x + 320, y: source.y + 160 }, { steps: 14 });
  await sleep(250);
  report.ok(
    "松手在空白处弹出了搜索面板",
    await cdp.eval(`return !!document.querySelector('.search-popup');`),
  );
  const popup = await cdp.eval(`return window.__lyflow.stores.ui.getState().searchPopup;`);
  report.ok(
    "面板记住了是从哪个端口拖出来的",
    popup?.pendingFrom?.node === ids.gen && popup?.pendingFrom?.port === "cloud",
    JSON.stringify(popup),
  );

  await cdp.eval(`
    const input = document.querySelector('.search-popup__input');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
    setter.call(input, 'voxel');
    input.dispatchEvent(new Event('input', { bubbles: true }));
    return true;
  `);
  await sleep(200);
  await cdp.eval(`
    const row = document.querySelector('.search-popup__row');
    if (row) row.click();
    return true;
  `);
  await sleep(250);

  const doc = await cdp.eval(`return window.__lyflow.snapshot().doc;`);
  report.eq("选中算子后落了一个新节点", doc.nodes.length, 2);
  report.ok(
    "新节点自动接上了拖出的那个端口",
    doc.edges.length === 1 && doc.edges[0].from.node === ids.gen,
    JSON.stringify(doc.edges),
  );
  // 加节点与接线一条撤销：以前两条，Ctrl+Z 一次线没了、节点还在
  await pressCtrl(cdp, "z");
  await sleep(200);
  report.eq("Ctrl+Z 一次：新节点与那条线一起撤掉，撤销栈回到加之前",
    await cdp.eval(`const d = window.__lyflow.snapshot().doc; return { nodes: d.nodes.length, edges: d.edges.length, past: window.__lyflow.stores.graph.getState().past.length };`),
    { nodes: 1, edges: 0, past: pastBeforePick });

  // 再打开搜索面板（空查询）：刚用过的那个排在第一，标着「最近」；面板顶上也多了一组
  const added = doc.nodes.find((n) => n.id !== ids.gen)?.op;
  await cdp.eval(`window.__lyflow.stores.ui.getState().openSearch({ screen: { x: 300, y: 200 }, flow: { x: 0, y: 0 } }); return true;`);
  await sleep(200);
  const first = await cdp.eval(`
    const row = document.querySelector('.search-popup__row');
    return row ? { op: row.dataset.opId, tag: row.querySelector('.search-popup__why')?.textContent ?? null } : null;
  `);
  report.eq("空查询时刚用过的算子排第一、标着「最近」", first, { op: added, tag: "最近" });
  await pressEscape(cdp);
  await sleep(150);
  report.ok("算子面板顶上有「最近用过」一组，里面有它",
    await cdp.eval(`return !!document.querySelector('[data-testid="palette-recent"] [data-op-id=${lit(added ?? "")}]');`));
}

// ------------------------------------------------ P1 #22 网格吸附与自动布局

async function suiteLayout(cdp, report) {
  report.section("P1 #22 + E8：网格吸附、对齐参考线、dagre 自动布局");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);
  await waitForNodes(cdp, ids);
  await normalizeZoom(cdp, 0.6);
  const layoutBox = await canvasBox(cdp);
  await placeAtScreen(cdp, {
    [ids.gen]: { x: 16, y: 40 },
    [ids.voxel]: { x: Math.round(layoutBox.w * 0.35), y: Math.round(layoutBox.h * 0.45) },
    [ids.tail]: { x: Math.round(layoutBox.w * 0.68), y: 40 },
  });

  // 拖一下：落点必须吸到 8 的倍数上
  const head = await centerOf(cdp, `[data-testid="node-${ids.voxel}"] .node__head`);
  mustOk(Boolean(head), "拿到了要拖的节点", JSON.stringify(head));
  await dragMouse(cdp, head, { x: head.x + 61, y: head.y + 37 }, { steps: 14 });
  await sleep(250);
  const pos = await cdp.eval(`
    return window.__lyflow.snapshot().doc.nodes.find(n => n.id === ${lit(ids.voxel)}).ui.position;
  `);
  report.ok(
    "拖动后位置吸到了 8 px 网格上",
    pos.x % 8 === 0 && pos.y % 8 === 0,
    JSON.stringify(pos),
  );

  // Ctrl+L 整理整图（M4 起 Ctrl+G 让给了「合成子图」）
  await cdp.eval(`window.__lyflow.stores.ui.getState().clearSelection(); return true;`);
  await pressCtrl(cdp, "L");
  await sleep(400);
  const laid = await cdp.eval(`
    const doc = window.__lyflow.snapshot().doc;
    return {
      undoLabel: window.__lyflow.snapshot().undoLabel,
      positions: doc.nodes.map(n => n.ui.position),
    };
  `);
  const xs = laid.positions.map((p) => p.x).sort((a, b) => a - b);
  report.ok("dagre 把三个节点排成了从左到右", xs[0] < xs[1] && xs[1] < xs[2], JSON.stringify(xs));
  report.ok("整理布局进了撤销栈", String(laid.undoLabel).includes("整理"), String(laid.undoLabel));
  // 文档缺 ui.position 时打开即布局：needsInitialLayout / layoutGraph 是纯逻辑，
  // 在 packages/editor/test/layout.test.mjs 里验
}

// ------------------------------------------ P1 #23 #26 #28 #29 编辑与输入

async function suiteEditing(cdp, report) {
  report.section("P1 #25 #26 #28 #29：折叠重命名、参数右键、拖动改值、快捷键面板；空画布的提示");

  await newDoc(cdp);
  // 空画布的提示：第一次打开就是一片空白时告诉人从哪开始；有了节点就消失
  const hint = () => cdp.eval(`return document.querySelector('[data-testid="canvas-empty-hint"]')?.textContent ?? null;`);
  const empty = await hint();
  report.ok("空画布上写着从哪开始（算子面板、双击、搜索键、打开）",
    typeof empty === "string" && empty.includes("算子面板") && empty.includes("双击") && empty.includes("Ctrl+O"), String(empty));
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);
  await sleep(150);
  report.eq("有了节点提示就消失", await hint(), null);
  await select(cdp, ids.voxel);
  await sleep(200);

  // #25 折叠
  await pressCtrl(cdp, "E");
  await sleep(200);
  report.ok(
    "Ctrl+E 折叠了节点",
    await cdp.eval(`return !!document.querySelector('[data-testid="node-collapsed-${ids.voxel}"]');`),
  );
  await pressCtrl(cdp, "E");
  await sleep(200);
  report.ok(
    "再按一次展开",
    !(await cdp.eval(`return !!document.querySelector('[data-testid="node-collapsed-${ids.voxel}"]');`)),
  );

  // #25 双击标题重命名
  const head = await centerOf(cdp, `[data-testid="node-${ids.voxel}"] .node__head`);
  if (head) {
    for (const type of ["mousePressed", "mouseReleased"]) {
      await cdp.send("Input.dispatchMouseEvent", {
        type, x: head.x, y: head.y, button: "left", clickCount: 2, buttons: 1,
      });
    }
    await sleep(250);
    const renamed = await cdp.eval(`
      const input = document.querySelector('[data-testid="node-rename-${ids.voxel}"]');
      if (!input) return null;
      const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
      setter.call(input, '粗降采样');
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
      await new Promise(r => setTimeout(r, 150));
      return window.__lyflow.snapshot().doc.nodes.find(n => n.id === ${lit(ids.voxel)}).ui.title;
    `);
    report.eq("双击标题改名写进了 ui.title", renamed, "粗降采样");
  }

  // #26 参数右键菜单
  await select(cdp, ids.voxel);
  await sleep(200);
  const menu = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-leafSize"]');
    if (!row) return 'no-row';
    const r = row.getBoundingClientRect();
    row.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true,
      clientX: r.left + 20, clientY: r.top + 8,
    }));
    await new Promise(r2 => setTimeout(r2, 200));
    return document.querySelector('[data-testid="param-menu"]') ? 'ok' : 'no-menu';
  `);
  report.eq("参数行右键弹出菜单", menu, "ok");
  // 点「重置」菜单就收起了，所以四项要在点之前数
  report.ok(
    "菜单里四项齐全",
    await cdp.eval(`
      return ['reset','copy','paste','path']
        .every(k => document.querySelector('[data-testid="param-menu-' + k + '"]') !== null);
    `),
  );
  const reset = await cdp.eval(`
    const before = JSON.stringify(window.__lyflow.snapshot().doc.nodes
      .find(n => n.id === ${lit(ids.voxel)}).params.leafSize ?? null);
    document.querySelector('[data-testid="param-menu-reset"]').click();
    await new Promise(r => setTimeout(r, 200));
    const after = window.__lyflow.snapshot().doc.nodes
      .find(n => n.id === ${lit(ids.voxel)}).params.leafSize;
    return { before, after: after ?? null };
  `);
  report.ok("重置为默认把稀疏键删掉了", reset.after === null, JSON.stringify(reset));

  // #28 数字框拖动改值，整段一条撤销
  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.gen)}, 'pointCount', 30000);
    window.__lyflow.stores.ui.getState().setSelection([${lit(ids.gen)}], []);
    return true;
  `);
  await sleep(250);
  const dragTarget = await centerOf(cdp, '[data-testid="param-drag-pointCount"]');
  if (dragTarget) {
    const undoBefore = await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
    await dragMouse(cdp, dragTarget, { x: dragTarget.x + 80, y: dragTarget.y }, { steps: 16 });
    await sleep(200);
    const after = await cdp.eval(`
      const g = window.__lyflow.stores.graph.getState();
      return {
        value: g.doc.nodes.find(n => n.id === ${lit(ids.gen)}).params.pointCount,
        past: g.past.length,
      };
    `);
    report.ok("水平拖动改了数值", after.value !== 30000, `pointCount=${after.value}`);
    report.eq("整段拖动只记一条撤销", after.past - undoBefore, 1);

    // 数字框里打了字、没失焦就按 F5：先提交再跑（以前跑的是打字之前的值 —— 数字框失焦才提交）
    await cdp.eval(`document.querySelector('[data-testid="param-drag-pointCount"]').focus(); return true;`);
    await pressKey(cdp, "a", 65, ["ctrl"]);
    await cdp.send("Input.insertText", { text: "4321" });
    const ranTyped = await runAndWait(cdp, () => pressF5(cdp));
    report.eq("数字框里打了字没失焦就按 F5：跑的是新值，焦点还在框里",
      { count: ranTyped.nodes[ids.gen]?.elementCount ?? null, focused: await cdp.eval(`return document.activeElement?.dataset?.testid ?? null;`) },
      { count: 4321, focused: "param-drag-pointCount" });
    await cdp.eval(`document.activeElement?.blur(); return true;`);
  } else {
    report.fail("找不到可拖动的数字框", "[data-testid=param-drag-pointCount]");
  }

  // 多选同一种算子：检查器里一起改。值不一样的行标「不同」，拖一下两个节点都变成同一个值，一条撤销
  const twin = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const id = g.addNode('gen.synthetic', { x: 40, y: 400 });
    window.__lyflow.stores.graph.getState().setParam(id, 'pointCount', 12000);
    window.__lyflow.stores.ui.getState().setSelection([${lit(ids.gen)}, id], []);
    return id;
  `);
  await sleep(250);
  report.ok("选中两个同算子节点：检查器换成一起改的表单，点数那一行标着「不同」",
    await cdp.eval(`return !!document.querySelector('[data-testid="inspector-multi"] [data-testid="multi-param-pointCount"][data-mixed="1"]');`));
  const multiDrag = await centerOf(cdp, '[data-testid="multi-param-pointCount"] [data-testid="param-drag-pointCount"]');
  if (multiDrag) {
    const undoBefore = await cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
    await dragMouse(cdp, multiDrag, { x: multiDrag.x + 60, y: multiDrag.y }, { steps: 12 });
    await sleep(200);
    const both = await cdp.eval(`
      const g = window.__lyflow.stores.graph.getState();
      const of = (id) => g.doc.nodes.find((n) => n.id === id).params.pointCount;
      return { a: of(${lit(ids.gen)}), b: of(${lit(twin)}), past: g.past.length };
    `);
    report.ok("拖一下两个节点都变成同一个值", both.a === both.b && both.a !== 12000, JSON.stringify(both));
    report.eq("两个节点一起改只记一条撤销", both.past - undoBefore, 1);
  } else {
    report.fail("多选表单里找不到可拖动的数字框", '[data-testid="multi-param-pointCount"]');
  }

  // F：把视图对准选中的节点 —— 选中的那个落到画布中间附近
  await cdp.eval(`window.__lyflow.stores.ui.getState().setSelection([${lit(twin)}], []); return true;`);
  await sleep(100);
  await pressKey(cdp, "f", 70);
  await sleep(600);
  const framed = await cdp.eval(`
    const pane = document.querySelector('.react-flow__pane').getBoundingClientRect();
    const node = document.querySelector('[data-testid="node-${twin}"]')?.getBoundingClientRect();
    if (!node) return null;
    return { dx: Math.round(node.left + node.width / 2 - (pane.left + pane.width / 2)),
             dy: Math.round(node.top + node.height / 2 - (pane.top + pane.height / 2)) };
  `);
  report.ok("按 F：选中的节点落到画布中间", framed != null && Math.abs(framed.dx) < 40 && Math.abs(framed.dy) < 40, JSON.stringify(framed));

  // 挪节点都进撤销栈：选中节点按方向键（React Flow 的键盘挪动）每按一下一条；拖框选出来的那个选区整段一条。
  // 以前这两种挪动都直接写进 doc、撤销栈里没有 —— Ctrl+Z 撤掉的是上一步
  await pressCtrl(cdp, "f", ["shift"]);  // 先看全图：gen 与 voxel 都在屏幕上
  await sleep(500);
  const posOf = (id) => cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.find((n) => n.id === ${lit(id)}).ui.position;`);
  const pastLen = () => cdp.eval(`return window.__lyflow.stores.graph.getState().past.length;`);
  await clickAt(cdp, await centerOf(cdp, `[data-testid="node-${ids.gen}"] .node__head`));
  const k0 = { pos: await posOf(ids.gen), past: await pastLen() };
  for (let i = 0; i < 2; i += 1) {
    await pressKey(cdp, "ArrowRight", 39);
    await sleep(80);
  }
  await sleep(150);
  const k1 = { pos: await posOf(ids.gen), past: await pastLen() };
  report.ok("选中节点按两下 →：挪了，撤销栈多两条", k1.pos.x > k0.pos.x && k1.past - k0.past === 2, JSON.stringify({ k0, k1 }));
  await pressCtrl(cdp, "z");
  await sleep(150);
  const k2 = await posOf(ids.gen);
  report.ok("Ctrl+Z 只撤掉最后那一下", k2.x < k1.pos.x && k2.x > k0.pos.x, JSON.stringify({ k0: k0.pos, k1: k1.pos, k2 }));

  // 框选 gen 与 voxel（从左上角外面的空白处拖到右下角外面），再拖那个选区。
  // 框的两个角都要离画布边 48 px 以上：框选时指针进了离边 40 px 那一圈，React Flow 会自动平移画布
  // （autoPanOnSelection），算好的框就套不住节点了。先缩小一点给四周留出地方
  await normalizeZoom(cdp, 0.7);
  const box = await cdp.eval(`
    const flow = document.querySelector('.react-flow').getBoundingClientRect();
    const clear = (x, y) => x - flow.left >= 48 && flow.right - x >= 48 && y - flow.top >= 48 && flow.bottom - y >= 48;
    const rects = [${lit(ids.gen)}, ${lit(ids.voxel)}].map((id) => document.querySelector('[data-testid="node-' + id + '"]').getBoundingClientRect());
    const left = Math.min(...rects.map((r) => r.left)), top = Math.min(...rects.map((r) => r.top));
    const right = Math.max(...rects.map((r) => r.right)), bottom = Math.max(...rects.map((r) => r.bottom));
    for (let d = 14; d < 120; d += 6) {
      const x = Math.round(left - d), y = Math.round(top - d), x2 = Math.round(right + d), y2 = Math.round(bottom + d);
      if (clear(x, y) && clear(x2, y2) && document.elementFromPoint(x, y)?.classList.contains('react-flow__pane')) {
        return { from: { x, y }, to: { x: x2, y: y2 } };
      }
    }
    return null;
  `);
  mustOk(box != null, "gen 与 voxel 左上角外面有离画布边够远的空白可以开始框选", box);
  // 以前框选只选上先碰到的那一个、选区也不出来：onSelectionChange 与 onNodesChange/onEdgesChange
  // 来回改连线的选中，控制台报 Maximum update depth exceeded（那条红字由 run.mjs 收尾的「控制台无报错」兜住）
  await dragMouse(cdp, box.from, box.to, { steps: 10 });
  await sleep(200);
  const boxed = await cdp.eval(`return [...window.__lyflow.stores.ui.getState().selectedNodes];`);
  report.ok("框选：gen 与 voxel 都选上了", boxed.includes(ids.gen) && boxed.includes(ids.voxel), JSON.stringify(boxed));
  const rect = await centerOf(cdp, ".react-flow__nodesselection-rect");
  mustOk(rect != null, "框选之后出现了选区", rect);
  const b0 = { gen: await posOf(ids.gen), voxel: await posOf(ids.voxel), past: await pastLen() };
  await dragMouse(cdp, rect, { x: rect.x + 48, y: rect.y + 32 }, { steps: 12 });
  await sleep(200);
  const b1 = { gen: await posOf(ids.gen), voxel: await posOf(ids.voxel), past: await pastLen() };
  report.ok("拖选区：两个节点一起挪了，整段只记一条撤销",
    b1.gen.x !== b0.gen.x && b1.voxel.x !== b0.voxel.x && b1.past - b0.past === 1, JSON.stringify({ b0, b1 }));
  await pressCtrl(cdp, "z");
  await sleep(150);
  report.eq("Ctrl+Z 一次两个都回去", { gen: await posOf(ids.gen), voxel: await posOf(ids.voxel) }, { gen: b0.gen, voxel: b0.voxel });

  // 多选批量删除，真按 Delete（删除只走键表；框选把相连的边也选上了，以前断边、删节点记成两条）
  const docCount = () => cdp.eval(`const d = window.__lyflow.stores.graph.getState().doc; return { nodes: d.nodes.length, edges: d.edges.length };`);
  const beforeDel = { ...(await docCount()), past: await pastLen() };
  await pressKey(cdp, "Delete", 46);
  await sleep(200);
  const del = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    return { gone: [${lit(ids.gen)}, ${lit(ids.voxel)}].every((id) => !g.doc.nodes.some((n) => n.id === id)),
             edges: g.doc.edges.length, past: g.past.length, selected: window.__lyflow.stores.ui.getState().selectedNodes.size };
  `);
  report.ok("Delete：框选的节点连同相连的边一起删掉，只记一条撤销，选中清空",
    del.gone && del.edges < beforeDel.edges && del.past - beforeDel.past === 1 && del.selected === 0, JSON.stringify({ beforeDel, del }));
  await pressCtrl(cdp, "z");
  await sleep(200);
  report.eq("Ctrl+Z 一次都回来", await docCount(), { nodes: beforeDel.nodes, edges: beforeDel.edges });

  // 右键拖动平移（中键与右键平移）：按在节点上拖也平移、松手不弹菜单；不挪的右键单击照样弹节点菜单。
  // 以前按在节点上拖不动（React Flow 只在空白处接右键平移），松手时指针在哪个节点 / 连线上就弹哪个的菜单
  const vpAt = () => cdp.eval(`
    const m = new DOMMatrixReadOnly(getComputedStyle(document.querySelector('.react-flow__viewport')).transform);
    return { x: Math.round(m.e), y: Math.round(m.f) };
  `);
  const anyMenu = () => cdp.eval(`return !!document.querySelector('[data-testid="node-context-menu"], [data-testid="edge-context-menu"]');`);
  const genHead = await centerOf(cdp, `[data-testid="node-${ids.gen}"] .node__head`);
  const v0 = await vpAt();
  await dragMouse(cdp, genHead, { x: genHead.x + 60, y: genHead.y + 40 }, { steps: 8, button: "right" });
  await sleep(200);
  const v1 = await vpAt();
  const menuAfterDrag = await anyMenu();
  report.ok("右键按在节点上拖：画布跟着平移，松手不弹菜单",
    Math.abs(v1.x - v0.x - 60) <= 2 && Math.abs(v1.y - v0.y - 40) <= 2 && !menuAfterDrag, JSON.stringify({ v0, v1, menuAfterDrag }));
  await clickAt(cdp, await centerOf(cdp, `[data-testid="node-${ids.gen}"] .node__head`), { button: "right" });
  await sleep(200);
  report.ok("右键单击节点照样弹节点菜单", await cdp.eval(`return !!document.querySelector('[data-testid="node-context-menu"]');`));
  await pressEscape(cdp);
  await pressEscape(cdp);

  // #12 复制粘贴走系统剪贴板（另一个窗口、重开之后也粘得进来）。系统剪贴板换成页面里的桩（stubClipboard）：
  // 这里要验的是「写的是什么、粘的是哪一份」，不该动用户真的剪贴板
  await stubClipboard(cdp);
  await cdp.eval(`
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    window.__lyflow.stores.ui.getState().setSelection([${lit(ids.gen)}, ${lit(twin)}], []);
    return true;
  `);
  await sleep(150);
  await pressCtrl(cdp, "c");
  await sleep(200);
  const copied = await cdp.eval(`
    try { const v = JSON.parse(window.__lyClip); return { kind: v.kind, nodes: v.nodes.length }; } catch { return null; }
  `);
  report.eq("Ctrl+C 往系统剪贴板写了一段带标记的 JSON（两个节点）", copied, { kind: "lyflow.nodes", nodes: 2 });
  const countNodes = () => cdp.eval(`return window.__lyflow.stores.graph.getState().doc.nodes.length;`);
  const before = await countNodes();
  // 清掉应用内的那一份：粘进来的只能是系统剪贴板里的
  await cdp.eval(`window.__lyflow.stores.ui.setState({ clipboard: null }); return true;`);
  await pressCtrl(cdp, "v");
  await sleep(300);
  report.eq("Ctrl+V 从系统剪贴板粘回两个节点（应用内的那份已经清掉）", (await countNodes()) - before, 2);
  // 文本编辑器里复制来的整张图也认
  await cdp.eval(`
    window.__lyClip = JSON.stringify({ schemaVersion: 1, id: 'clip', nodes: [{ id: 'p', op: 'gen.synthetic', ui: { position: { x: 0, y: 0 } } }], edges: [] });
    return true;
  `);
  await pressCtrl(cdp, "v");
  await sleep(300);
  report.eq("整张图的 JSON 也粘得进来", (await countNodes()) - before, 3);
  // 粘进来的那个是选中的；撤销掉这次粘贴，它没了，选中里也不该还挂着它的 id
  const pastedSel = await cdp.eval(`return [...window.__lyflow.stores.ui.getState().selectedNodes];`);
  await pressCtrl(cdp, "z");
  await sleep(200);
  const afterUndo = await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    const stale = [...window.__lyflow.stores.ui.getState().selectedNodes].filter((id) => !g.doc.nodes.some((n) => n.id === id));
    return { added: g.doc.nodes.length - ${before}, stale };
  `);
  report.ok("Ctrl+Z 撤掉粘贴：节点没了，选中里也没留下它的 id",
    pastedSel.length === 1 && afterUndo.added === 2 && afterUndo.stale.length === 0, JSON.stringify({ pastedSel, afterUndo }));
  // 内容是从 paste 事件里拿的：readText 在 WebView2 里会弹权限框，没人点就一直挂着，Ctrl+V 什么也粘不上
  report.eq("Ctrl+V 没去调 navigator.clipboard.readText", await cdp.eval(`return window.__lyClipReads;`), 0);
  await restoreClipboard(cdp);

  // #29 快捷键面板。先移开焦点：上一组把焦点留在了参数输入框里，
  // 而键表里 help 没标 inTextField，会被「打字时不拦键」的铁律挡掉。
  await cdp.eval(`
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    return true;
  `);
  await pressQuestion(cdp);
  await sleep(250);
  const sheet = await cdp.eval(`
    const el = document.querySelector('[data-testid="shortcut-panel"]');
    if (!el) return null;
    return {
      rows: el.querySelectorAll('[data-testid^="shortcut-"]').length,
      hasRun: !!el.querySelector('[data-testid="shortcut-run"]'),
      hasMute: !!el.querySelector('[data-testid="shortcut-mute"]'),
      hasLayout: !!el.querySelector('[data-testid="shortcut-layout"]'),
    };
  `);
  report.ok("? 打开了快捷键面板", Boolean(sheet), JSON.stringify(sheet));
  if (sheet) {
    report.ok("面板从键表生成，条目数与表一致", sheet.rows >= 20, `${sheet.rows} 条`);
    report.ok("表里有 F5 / Ctrl+M / Ctrl+G", sheet.hasRun && sheet.hasMute && sheet.hasLayout);
  }
  await cdp.eval(`window.__lyflow.stores.ui.getState().setHelpOpen(false); return true;`);
}

// ------------------------------------------------- §2.5 面板、最近文件、备份

async function suitePanels(cdp, report, ws) {
  report.section("§2.5：日志/诊断抽屉、最近文件、备份恢复、窗口标题");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 2000 } },
      { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0, 0.01, 0.01] } },
    ],
    [{ from: ["gen", "cloud"], to: ["voxel", "cloud"] }],
  );
  // 坏参数让这次运行失败；失败本身不单独断言，下面诊断抽屉列出那条错误就是证据
  await runAndWait(cdp, () => pressF5(cdp));

  await cdp.eval(`window.__lyflow.stores.ui.getState().toggleDrawer('diagnostics'); return true;`);
  await sleep(250);
  report.ok(
    "诊断抽屉列出了那条错误",
    await cdp.eval(`return !!document.querySelector('[data-testid="diag-${ids.voxel}"]');`),
  );
  await cdp.eval(`document.querySelector('[data-testid="diag-${ids.voxel}"]').click(); return true;`);
  await sleep(250);
  report.eq(
    "点诊断定位到了那个节点",
    await cdp.eval(`return window.__lyflow.snapshot().selected;`),
    [ids.voxel],
  );

  // 收起抽屉，后面的最近文件、备份不受它遮挡
  await cdp.eval(`window.__lyflow.stores.ui.setState({ drawer: null }); return true;`);

  // 最近文件
  const graphPath = path.join(ws.dir, "最近 文件.lyflow.json");
  await saveGraphTo(cdp, graphPath);
  await cdp.eval(`await window.__lyflow.transport.pushRecentFile(${lit(graphPath)}); return true;`);
  await cdp.eval(`document.querySelector('[data-testid="recent-toggle"]').click(); return true;`);
  await sleep(300);
  const recent = await cdp.eval(`
    const menu = document.querySelector('[data-testid="recent-menu"]');
    return menu ? [...menu.querySelectorAll('[data-testid="recent-item"]')].map(b => b.title) : null;
  `);
  report.ok("最近文件里有刚存的那个", (recent ?? []).includes(graphPath), JSON.stringify(recent));
  await cdp.eval(`document.querySelector('[data-testid="recent-toggle"]').click(); return true;`);

  // 备份：写一份比正文新的 `<file>~`，backup_status 要认出来
  await cdp.eval(`
    const b = window.__lyflow;
    await b.transport.writeBackup(${lit(graphPath)}, b.stores.graph.getState().doc);
    return true;
  `);
  report.ok("备份文件写出来了", fs.existsSync(`${graphPath}~`), `${graphPath}~`);
  fs.utimesSync(`${graphPath}~`, new Date(), new Date(Date.now() + 60_000));
  const status = await cdp.eval(`
    return await window.__lyflow.transport.backupStatus(${lit(graphPath)});
  `);
  report.ok("备份比正文新时被认出来", status.exists && status.newer, JSON.stringify(status));
  await cdp.eval(`await window.__lyflow.transport.discardBackup(${lit(graphPath)}); return true;`);
  report.ok("丢弃备份后文件没了", !fs.existsSync(`${graphPath}~`));

  // 参数框里打了字、没失焦就按 Ctrl+S：先提交再存（以前 Ctrl+S 在输入框里不响应，打的字也没进文件）
  await cdp.eval(`window.__lyflow.stores.ui.getState().setSelection([${lit(ids.gen)}], []); return true;`);
  await sleep(250);
  await cdp.eval(`document.querySelector('[data-testid="param-drag-pointCount"]').focus(); return true;`);
  await pressKey(cdp, "a", 65, ["ctrl"]);
  await cdp.send("Input.insertText", { text: "2468" });
  await pressCtrl(cdp, "s");
  const savedCount = () => JSON.parse(fs.readFileSync(graphPath, "utf8")).nodes.find((n) => n.id === ids.gen)?.params?.pointCount ?? null;
  for (let i = 0; i < 25 && savedCount() !== 2468; i += 1) await sleep(200);
  report.eq("参数框里打了字没失焦就按 Ctrl+S：文件里是新值，图不算没存", { file: savedCount(), dirty: await cdp.eval(`return window.__lyflow.stores.graph.getState().dirty;`) },
    { file: 2468, dirty: false });
  await cdp.eval(`document.activeElement?.blur(); return true;`);

  // 窗口标题
  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.gen)}, 'seed', 9);
    return true;
  `);
  await sleep(200);
  const title = await cdp.eval(`return document.title;`);
  report.ok("窗口标题带文件名与脏标记", /最近 文件\.lyflow\.json \*/.test(title), title);

  // 关窗口前问一句（app/src/closeGuard.ts）。原生对话框脚本点不了，tauri 的 invoke 也换不掉（只读属性），
  // 所以经 devbridge 换个对话框走一遍「要不要关」；监听装没装上、destroy 有没有权限另外查
  report.ok("关窗口前那一问装上了", await cdp.eval(`return window.__lyflow.shell.closeGuardInstalled();`));
  const shouldClose = (answer) => cdp.eval(`return await window.__lyflow.shell.shouldClose(${lit(answer)});`);
  report.eq("有没存的改动时点 ×：先问一句，选「取消」窗口不关", await shouldClose(false), { asked: true, close: false });
  report.eq("选「关闭」才关", await shouldClose(true), { asked: true, close: true });
  await cdp.eval(`const g = window.__lyflow.stores.graph.getState(); g.markSaved(g.filePath); return true;`);
  report.eq("没有改动时点 × 直接关、不问", await shouldClose(false), { asked: false, close: true });
  // 有监听时 tauri 不自己关、由 JS 来 destroy：壳得有这个权限，没有的话点 × 就再也关不掉。
  // 拿一个不存在的窗口名真调一次：权限不够是 ACL 先拒，够了才走到「找不到窗口」
  const destroyCheck = await cdp.eval(`
    try {
      await window.__TAURI_INTERNALS__.invoke('plugin:window|destroy', { label: 'lyflow-e2e-no-such-window' });
      return 'ok';
    } catch (e) { return String(e); }
  `);
  report.ok("壳有 destroy 窗口的权限（core:window:allow-destroy）", !/not allowed|denied|permission/i.test(destroyCheck), destroyCheck);

  // 没存过盘的图也定时备份（lib/autosave.ts；逻辑的单测在 packages/editor/test/autosave.test.mjs）：
  // 这里验真的 app data 位置与真的写盘。用户自己的那份已经被 harness 挪开了，收尾时挪回去
  await newDoc(cdp);
  await cdp.eval(`window.__lyflow.stores.graph.getState().addNode('gen.synthetic', { x: 0, y: 0 }); return true;`);
  const untitled = await cdp.eval(`return await window.__lyflow.autosave.untitledPath();`);
  report.ok("没存过盘的图的备份在 app data 里（untitled.lyflow.json 旁边的 ~）",
    typeof untitled === "string" && path.isAbsolute(untitled) && `${untitled}~` === UNTITLED_BACKUP, String(untitled));
  await cdp.eval(`await window.__lyflow.autosave.tick(); return true;`);
  report.ok("走一拍定时备份：真的写出了那个 ~ 文件", fs.existsSync(UNTITLED_BACKUP));
  await newDoc(cdp); // 相当于重开 app：一张空白的新图
  report.eq("找得回来", await cdp.eval(`return (await window.__lyflow.autosave.findUntitled())?.nodes ?? null;`), 1);
  const back = await cdp.eval(`
    const ok = await window.__lyflow.autosave.restoreUntitled();
    await new Promise((d) => setTimeout(d, 100));
    const g = window.__lyflow.stores.graph.getState();
    return { ok, nodes: g.doc.nodes.length, path: g.filePath, dirty: g.dirty, title: document.title };
  `);
  report.ok("换上：一个节点、没有路径、算没保存（标题带 *）",
    back.ok && back.nodes === 1 && back.path === null && back.dirty && / \* — LyFlow$/.test(back.title), JSON.stringify(back));
  await cdp.eval(`await window.__lyflow.autosave.discardUntitled(); return true;`);
  report.ok("删掉之后文件没了", !fs.existsSync(UNTITLED_BACKUP));
}

// ------------------------------------------------------------ #30 / 2.6 视图

async function suiteViewer(cdp, report) {
  report.section("P1 #30 / §2.6：着色模式、色带与范围、钉住、导出");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [{ key: "gen", op: "gen.synthetic", params: { pointCount: 8000, seed: 2 } },
     { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.03, 0.03, 0.03] } }],
    [{ from: ["gen", "cloud"], to: ["voxel", "cloud"] }],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "先跑一次", run.status);

  const view = await selectAndReadViewer(cdp, ids.gen);
  report.ok("3D 视图画出了点", view.count > 0 && view.hasCanvas, JSON.stringify(view));

  const controls = await cdp.eval(`
    return {
      shading: !!document.querySelector('[data-testid="viewer-shading"]'),
      ramp: !!document.querySelector('[data-testid="viewer-ramp"]'),
      min: !!document.querySelector('[data-testid="viewer-range-min"]'),
      max: !!document.querySelector('[data-testid="viewer-range-max"]'),
      pin: !!document.querySelector('[data-testid="viewer-pin"]'),
      exportBtn: !!document.querySelector('[data-testid="viewer-export"]'),
    };
  `);
  mustOk(Object.values(controls).every(Boolean), "着色/色带/范围/钉住/导出的控件都在",
    JSON.stringify(controls));

  const shaded = await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-shading"]');
    const setter = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, 'height');
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    await new Promise(r => setTimeout(r, 250));
    return document.querySelector('.viewer').getAttribute('data-shading');
  `);
  report.eq("切到高度着色", shaded, "height");

  const ramped = await cdp.eval(`
    // 上一步切到了高度着色，色带下拉此时一定可用（只有单色时才禁用）
    const sel = document.querySelector('[data-testid="viewer-ramp"]');
    if (!sel) return 'missing';
    if (sel.disabled) return 'disabled';
    const setter = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, 'gray');
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    await new Promise(r => setTimeout(r, 200));
    return sel.value;
  `);
  report.eq("色带可切换", ramped, "gray");

  // 钉住：钉住 gen 之后选中 voxel，视图不该切过去
  await cdp.eval(`document.querySelector('[data-testid="viewer-pin"]').click(); return true;`);
  await sleep(250);
  report.eq(
    "钉住状态写进了 DOM",
    await domOf(cdp, ".viewer", "el.getAttribute('data-pinned')"),
    "1",
  );
  await select(cdp, ids.voxel);
  await sleep(600);
  report.eq(
    "钉住后选别的节点视图不切换",
    await domOf(cdp, ".viewer", "el.getAttribute('data-node')"),
    ids.gen,
  );
  await cdp.eval(`document.querySelector('[data-testid="viewer-pin"]').click(); return true;`);
  await sleep(600);
  report.eq(
    "取消钉住后立刻跟随选中",
    await domOf(cdp, ".viewer", "el.getAttribute('data-node')"),
    ids.voxel,
  );

  // 导出：真正的下载由浏览器接管，这里只点一下，抛异常会被控制台分组逮到
  mustOk(
    await cdp.eval(`
      const btn = document.querySelector('[data-testid="viewer-export"]');
      if (!btn) return false;
      btn.click();
      await new Promise(r => setTimeout(r, 300));
      return true;
    `),
    "导出按钮点得到",
  );
}

/** 预览上的测量标记，一次读全。 */
function readMeasure(cdp) {
  return cdp.eval(`
    const v = document.querySelector('.viewer');
    const rows = [...document.querySelectorAll('[data-testid="measure-readout"] .measure-readout__row')]
      .map((r) => r.getAttribute('data-key'));
    const num3 = (s) => (s ? s.split(',').map(Number) : null);
    return {
      on: v.getAttribute('data-measuring'),
      count: Number(v.getAttribute('data-measure')),
      p1: num3(v.getAttribute('data-measure-p1')),
      p2: num3(v.getAttribute('data-measure-p2')),
      dist: v.getAttribute('data-measure-dist') === null ? null : Number(v.getAttribute('data-measure-dist')),
      stale: v.getAttribute('data-measure-stale'),
      node: v.getAttribute('data-node'),
      rows,
      readout: !!document.querySelector('[data-testid="measure-readout"]'),
    };
  `);
}

async function suiteMeasure(cdp, report) {
  report.section("测量（measure-plan）：M 开工具、单击选点、两点测距、拖动不选点、重跑标过期、换节点清掉");
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [{ key: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed: 5 } },
     { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.03, 0.03, 0.03] } }],
    [{ from: ["gen", "cloud"], to: ["voxel", "cloud"] }],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  mustOk(run.status === "ok", "先跑一次", run.status);
  const view = await selectAndReadViewer(cdp, ids.gen);
  mustOk(view.count > 0, "预览画出了 gen 的点", JSON.stringify(view));

  await pressKey(cdp, "m", 77);
  await sleep(200);
  const on = await readMeasure(cdp);
  report.eq("按 M：测量工具开着、读数框出来", [on.on, on.readout, on.count], ["1", true, 0]);

  const center = await centerOf(cdp, '[data-testid="viewer3d-canvas"]');
  const first = await clickUntilPicked(cdp, center, async () => (await readMeasure(cdp)).count === 1);
  mustOk(Boolean(first), "在画面中心附近点中了一个点");
  const one = await readMeasure(cdp);
  report.ok("第一次单击：P1 有坐标、读数只有 P1", Array.isArray(one.p1) && one.p1.length === 3 &&
    JSON.stringify(one.rows) === JSON.stringify(["p1"]), JSON.stringify(one));

  await dragMouse(cdp, { x: center.x + 40, y: center.y }, { x: center.x + 100, y: center.y + 30 });
  await sleep(300);
  report.eq("拖动转视角不算选点", (await readMeasure(cdp)).count, 1);

  const second = await clickUntilPicked(cdp, { x: center.x - 50, y: center.y + 20 },
    async () => (await readMeasure(cdp)).count === 2);
  mustOk(Boolean(second), "又点中了第二个点");
  const two = await readMeasure(cdp);
  const expect = Math.hypot(...two.p2.map((v, i) => v - two.p1[i]));
  report.ok("第二次单击：距离 = 两点坐标之差的模，读数有 |d| 与 Δ",
    two.dist > 0 && Math.abs(two.dist - expect) <= 1e-5 * Math.max(1, expect) &&
      two.rows.includes("dist") && two.rows.includes("delta"),
    JSON.stringify({ two, expect }));

  await cdp.eval(`window.__lyflow.stores.ui.getState().setViewerMode("2d"); return true;`);
  await sleep(300);
  report.ok("2D 剖面下多一行 XY 平面距离", (await readMeasure(cdp)).rows.includes("distXY"));
  await cdp.eval(`window.__lyflow.stores.ui.getState().setViewerMode("3d"); return true;`);

  await cdp.eval(`window.__lyflow.stores.graph.getState().setParam(${lit(ids.gen)}, 'seed', 6); return true;`);
  await runAndWait(cdp, () => pressF5(cdp));
  await cdp.waitFor(`document.querySelector('.viewer')?.getAttribute('data-measure-stale') === '1'`,
    { timeoutMs: 15_000, what: "重跑后测量标成过期" }).catch(() => {});
  const stale = await readMeasure(cdp);
  report.eq("同一节点重跑：两个点位留着、标「云已更新」", [stale.count, stale.stale], [2, "1"]);

  await select(cdp, ids.voxel);
  await cdp.waitFor(`document.querySelector('.viewer')?.getAttribute('data-node') === ${lit(ids.voxel)}`,
    { timeoutMs: 15_000, what: "预览切到 voxel" });
  await sleep(200);
  report.eq("换节点：测量清掉", (await readMeasure(cdp)).count, 0);

  await pressKey(cdp, "m", 77);
  await sleep(200);
  const off = await readMeasure(cdp);
  report.eq("再按 M 关掉工具、读数框收起", [off.on, off.readout], ["0", false]);
}

// ---------------------------------------------------------- Shift+F5 / #27

async function suiteRunToSelected(cdp, report) {
  report.section("P1 #27 + E7：Shift+F5 跑到选中节点");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, CHAIN_NODES, CHAIN_EDGES);
  await select(cdp, ids.voxel);
  const run = await runAndWait(cdp, () => pressShiftF5(cdp));
  report.eq("运行状态 ok", run.status, "ok");
  report.eq("目标就是选中的那个节点", run.targets, [ids.voxel]);
  report.ok("下游没进计划", run.nodes[ids.tail] === undefined, JSON.stringify(run.nodes[ids.tail]));
}

export const m3Suites = [
  suiteCache,
  suiteParallel,
  suiteBypassReroute,
  suiteMigration,
  suiteHotReload,
  suiteParamLinkage,
  suiteSnap,
  suitePortHints,
  suiteReconnect,
  suiteInsertOnEdge,
  suiteDropToSearch,
  suiteLayout,
  suiteEditing,
  suitePanels,
  suiteViewer,
  suiteMeasure,
  suiteRunToSelected,
];
