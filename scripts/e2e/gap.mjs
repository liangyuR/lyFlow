// 非点云输出的展示：3D 叠画 2D 几何、2D 剖面相机、Inspector 里的数值（G7）。
// 值从**执行事件**灌进去而不是靠某个算子：前端不该知道算子的名字（ADR-0003）。

import { sleep } from "./cdp.mjs";
import {
  buildGraph,
  lit,
  newDoc,
  pressF5,
  runAndWait,
  select,
  selectAndReadViewer,
} from "./page.mjs";

/** 把一条带几何输出的 node_state 灌进执行 store。走的是 transport 用的同一条 apply()。 */
function feedOutputs(cdp, nodeId, seq) {
  return cdp.eval(`
    const e = window.__lyflow.stores.execution.getState();
    e.apply({
      schemaVersion: 1,
      runId: e.runId,
      seq: ${seq},
      kind: 'node_state',
      nodeId: ${lit(nodeId)},
      state: 'done',
      durationMs: 1,
      stats: { elementCount: 1, byteSize: 64, outputs: ${lit(fakeOutputs())} },
    });
    return true;
  `);
}

/** 等 `.viewer` 上某个属性变成期望值，返回它最后读到的值。 */
async function waitAttr(cdp, name, want, tries = 50) {
  let got = null;
  for (let i = 0; i < tries; i += 1) {
    got = await cdp.eval(`
      const v = document.querySelector('.viewer');
      return v ? v.getAttribute(${lit(name)}) : null;
    `);
    if (String(got) === String(want)) break;
    await sleep(120);
  }
  return got;
}

/** 造一条 node_state 事件，端口上挂四种几何 + 一个 Measurement。 */
function fakeOutputs() {
  return [
    {
      port: "box",
      type: "Box2D",
      elementCount: 1,
      value: { kind: "Box2D", min: [-0.02, 0.16], max: [0.02, 0.18] },
    },
    {
      port: "line",
      type: "Line2D",
      elementCount: 1,
      value: {
        kind: "Line2D",
        point: [0, 0.17],
        dir: [1, 0],
        hasSegment: true,
        start: [-0.02, 0.17],
        end: [0.02, 0.17],
      },
    },
    {
      port: "circle",
      type: "Circle2D",
      elementCount: 1,
      value: { kind: "Circle2D", center: [0.005, 0.171], radius: 0.002 },
    },
    {
      port: "point",
      type: "Point2D",
      elementCount: 1,
      value: { kind: "Point2D", p: [0.012, 0.1725] },
    },
    {
      port: "value",
      type: "Measurement",
      elementCount: 1,
      value: {
        kind: "Measurement",
        value: 6.4138,
        ok: true,
        unit: "mm",
        verdict: "ok",
        nominal: 6.4,
        upper: 7.4,
        lower: 5.4,
      },
    },
  ];
}

/** §5：叠画几何、2D 剖面模式、Inspector 数值显示各至少一条断言。 */
async function suiteMeasurementOutputs(cdp, report) {
  report.section("量测输出：3D 叠画 + 2D 剖面 + Inspector 数值");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 5000, seed: 3 } },
      // 只输出 Indices/Plane，没有点云 —— 底图规则的最小复现
      { key: "plane", op: "segment.ransac_plane", params: { distanceThreshold: 0.02 } },
    ],
    [{ from: ["gen", "cloud"], to: ["plane", "cloud"] }],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("底图跑通了", run.status, "ok");

  await select(cdp, ids.gen);

  const applied = await feedOutputs(cdp, ids.gen, 100000);
  report.ok("事件灌进去了", applied === true);

  // -- 3D 叠画 -------------------------------------------------------------
  let overlay = 0;
  for (let i = 0; i < 50; i += 1) {
    overlay = await cdp.eval(`
      const v = document.querySelector('.viewer');
      return v ? Number(v.getAttribute('data-overlay') || 0) : -1;
    `);
    if (overlay === 4) break;
    await sleep(100);
  }
  report.eq("四个几何输出都叠上了（Box2D/Line2D/Circle2D/Point2D）", overlay, 4);

  const lines = await cdp.eval(`
    // three.js 的对象树从 store 里拿不到，退而求其次：确认画布还在、渲染没崩
    const canvas = document.querySelector('.viewer__canvas canvas');
    return canvas ? canvas.width > 0 && canvas.height > 0 : false;
  `);
  report.ok("叠画之后画布仍在渲染", lines === true);

  // -- 2D 剖面相机 ---------------------------------------------------------
  await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-camera"]');
    const setter = Object.getOwnPropertyDescriptor(
      window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, '2d');
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);
  let camera = "";
  for (let i = 0; i < 30; i += 1) {
    camera = await cdp.eval(
      `const v = document.querySelector('.viewer'); return v ? v.getAttribute('data-camera') : '';`,
    );
    if (camera === "2d") break;
    await sleep(80);
  }
  report.eq("切到 2D 剖面相机", camera, "2d");

  const noRotate = await cdp.eval(`
    const canvas = document.querySelector('.viewer__canvas canvas');
    return !!canvas;  // 相机换了之后画布没有被重建掉
  `);
  report.ok("2D 模式下画布仍在", noRotate === true);

  await cdp.eval(`
    const sel = document.querySelector('[data-testid="viewer-camera"]');
    const setter = Object.getOwnPropertyDescriptor(
      window.HTMLSelectElement.prototype, 'value').set;
    setter.call(sel, '3d');
    sel.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  `);

  // -- Inspector 数值 -------------------------------------------------------
  const inspector = await cdp.eval(`
    const section = document.querySelector('[data-testid="inspector-outputs"]');
    if (!section) return null;
    const row = section.querySelector('[data-testid="output-value"]');
    const box = section.querySelector('[data-testid="output-box"]');
    return {
      rows: section.querySelectorAll('.insp-out').length,
      valueText: row ? row.querySelector('.insp-out__value').textContent : null,
      verdict: row ? row.getAttribute('data-verdict') : null,
      boxText: box ? box.querySelector('.insp-out__value').textContent : null,
    };
  `);
  report.ok("Inspector 有「输出」一栏", inspector !== null, JSON.stringify(inspector));
  if (inspector) {
    report.eq("五个非点云输出都列出来了", inspector.rows, 5);
    report.eq("Measurement 显示成数值 + 单位", inspector.valueText, "6.4138 mm");
    report.eq("判定字段显示出来了", inspector.verdict, "ok");
    report.ok(
      "Box2D 显示成两个角点",
      typeof inspector.boxText === "string" && inspector.boxText.includes("→"),
      inspector.boxText,
    );
  }

  // -- 底图：几何节点借上游最近的那片云 ---------------------------------------
  const fed = await feedOutputs(cdp, ids.plane, 100001);
  report.ok("几何输出灌到没有点云输出的节点上", fed === true);

  const planeView = await selectAndReadViewer(cdp, ids.plane);
  report.eq("底图取自上游的 gen", planeView.base, ids.gen);
  report.ok(
    "底图标签写明是谁的云",
    /^底图：/.test(planeView.baseText ?? ""),
    `baseText=${planeView.baseText} status=${planeView.status}`,
  );
  report.ok("底图真的画出了点", planeView.count > 0, `count=${planeView.count}`);

  const planeOverlay = await waitAttr(cdp, "data-overlay", 4);
  report.eq("几何仍然叠在底图上", Number(planeOverlay), 4);
}

/** 可选：打开一张真实的 gap 图跑一遍，看 ROI 框有没有画出来。跑法见
 *  docs/gap-acceptance.md；不设 LYFLOW_GAP_GRAPH 时整组跳过。 */
async function suiteRealGapGraph(cdp, report) {
  const graphPath = process.env.LYFLOW_GAP_GRAPH;
  if (!graphPath) return;
  report.section("真实 gap 图：ROI 框 / 基准线 / 圆");

  const run = await openAndRun(cdp, report, graphPath);
  if (!run) return;
  const failed = Object.entries(run.nodes ?? {})
    .filter(([, n]) => n.state === "error")
    .map(([id, n]) => `${id}: ${(n.errors?.[0] ?? {}).message ?? ""}`);
  report.ok("跑完了（R1 那张图 gap 节点会红，属于预期）",
    run.status === "ok" || run.status === "error", `${run.status} ${failed.join(" | ")}`);

  const rois = await cdp.eval(`
    const nodes = window.__lyflow.stores.graph.getState().doc.nodes;
    return nodes.find((n) => n.op === 'gap.business_rois')?.id ?? null;
  `);
  report.ok("图里有 business_rois 节点", typeof rois === "string", String(rois));
  if (typeof rois === "string") {
    const view = await selectAndReadViewer(cdp, rois);
    const overlay = Number(await waitAttr(cdp, "data-overlay", 4));
    // 失败时把节点的输出一并打出来 —— 光看 0 猜不出是没跑还是没选中
    const detail = await cdp.eval(`
      const e = window.__lyflow.stores.execution.getState();
      const n = e.nodes.get(${lit(rois)});
      const u = window.__lyflow.stores.ui.getState();
      return JSON.stringify({
        path: u.path.length,
        selected: [...u.selectedNodes],
        state: n ? n.state : null,
        errors: n ? n.errors.map((x) => x.code + ':' + x.message) : null,
        outputs: n ? (n.stats?.outputs ?? []).map((o) => o.type + (o.value ? '+value' : '')) : null,
      });
    `);
    report.ok("四个业务 ROI 框都叠上了", overlay === 4, `overlay=${overlay} ${detail}`);

    // business_rois 只输出 Box2D，底图应当落在上游最近的那片云上
    const upstreamCloud = await cdp.eval(`
      const doc = window.__lyflow.stores.graph.getState().doc;
      const n = doc.nodes.find((x) => x.op === 'filter.radius_outlier');
      return n ? n.id : null;
    `);
    report.eq("底图落在上游的 filter.radius_outlier 上", view.base, upstreamCloud);
    report.ok(
      "底图标签写明是谁的云",
      /^底图：/.test(view.baseText ?? ""),
      `baseText=${view.baseText} status=${view.status}`,
    );
    report.ok("底图真的画出了点", view.count > 0, `count=${view.count} view=${view.view}`);
  }

  const circles = await cdp.eval(`
    const nodes = window.__lyflow.stores.graph.getState().doc.nodes;
    return nodes.find((n) => n.op === 'gap.fit_gap_circles')?.id ?? null;
  `);
  if (typeof circles === "string") {
    await select(cdp, circles);
    const state = await cdp.eval(`
      const e = window.__lyflow.stores.execution.getState();
      const n = e.nodes.get(${lit(circles)});
      return n ? { state: n.state, outputs: (n.stats?.outputs ?? []).map((o) => o.type) } : null;
    `);
    report.ok("圆拟合节点有结果或红框", state !== null, JSON.stringify(state));
  }
}

/** 打开一张真实的图并跑一遍。返回 run 结果；打不开返回 null。 */
async function openAndRun(cdp, report, graphPath) {
  await newDoc(cdp);
  const loaded = await cdp
    .eval(
      `
    const b = window.__lyflow;
    // 前面的分组可能停在某个子图里；层级不回到顶层的话，选中的节点在
    // aggregatedNodes 的前缀过滤里根本找不到（F2）
    b.stores.ui.getState().setPath([]);
    const loaded = await b.transport.loadGraph(${lit(graphPath)});
    b.stores.graph.getState().loadDoc(loaded.doc, ${lit(graphPath)});
    return true;
  `,
    )
    .catch((e) => String(e));
  report.ok("打开图", loaded === true, String(loaded));
  if (loaded !== true) return null;
  return runAndWait(cdp, () => pressF5(cdp));
}

/** 图里第一个用某个算子的节点 id。没有返回 null。 */
function nodeOfOp(cdp, op) {
  return cdp.eval(`
    const nodes = window.__lyflow.stores.graph.getState().doc.nodes;
    return nodes.find((n) => n.op === ${lit(op)})?.id ?? null;
  `);
}

/** §10：模型 ROI 路径的图。`LYFLOW_GAP_GRAPH_MODEL` 指向 R1 的模型图；未设时整组跳过。
 *  怎么生成那张图见 docs/gap-acceptance.md「第二部分」。 */
async function suiteModelGapGraph(cdp, report) {
  const graphPath = process.env.LYFLOW_GAP_GRAPH_MODEL;
  if (!graphPath) return;
  report.section("模型 ROI 图：四框 / 按类着色 / 裁剪窗状态");

  const run = await openAndRun(cdp, report, graphPath);
  if (!run) return;
  const failed = Object.entries(run.nodes ?? {})
    .filter(([, n]) => n.state === "error")
    .map(([id, n]) => `${id}: ${(n.errors?.[0] ?? {}).message ?? ""}`);
  report.ok("模型路径整张图跑通（一个红框都不该有）", run.status === "ok",
    `${run.status} ${failed.join(" | ")}`);

  // -- roi_from_labels：四框叠在底图上 --------------------------------------
  const rois = await nodeOfOp(cdp, "gap.roi_from_labels");
  report.ok("图里有 roi_from_labels 节点", typeof rois === "string", String(rois));
  if (typeof rois === "string") {
    const view = await selectAndReadViewer(cdp, rois);
    const overlay = Number(await waitAttr(cdp, "data-overlay", 4));
    report.eq("四个模型 ROI 框都叠上了", overlay, 4, `view=${view.view} count=${view.count}`);
    report.ok("框叠在一片真实的剖面上", view.count > 0, `count=${view.count} base=${view.base}`);
  }

  // -- labels_to_cloud：有点，且类别是逐点的一个通道 --------------------------
  const colored = await nodeOfOp(cdp, "gap.labels_to_cloud");
  report.ok("图里有 labels_to_cloud 节点", typeof colored === "string", String(colored));
  if (typeof colored === "string") {
    const view = await selectAndReadViewer(cdp, colored);
    report.ok("着色后的剖面有点", view.count > 0, `count=${view.count} view=${view.view}`);
    // 3D 视图没有 rgb 着色模式，所以算子把类 id 也写进 intensity：
    // 「强度」这一项没被禁用，就说明逐点的类别通道确实到了前端（见 gap-acceptance.md 偏离 20）
    const shading = await cdp.eval(`
      const sel = document.querySelector('[data-testid="viewer-shading"]');
      if (!sel) return null;
      const opt = [...sel.options].find((o) => o.value === 'intensity');
      return { disabled: opt ? opt.disabled : null, value: sel.value };
    `);
    report.ok(
      "逐点的类别通道到了前端（强度可选）",
      shading !== null && shading.disabled === false,
      JSON.stringify(shading),
    );
  }

  // -- roll_anchored_crop：status 显示在 Inspector ---------------------------
  const roll = await nodeOfOp(cdp, "gap.roll_anchored_crop");
  report.ok("图里有 roll_anchored_crop 节点", typeof roll === "string", String(roll));
  if (typeof roll === "string") {
    await selectAndReadViewer(cdp, roll);
    const status = await cdp.eval(`
      const row = document.querySelector('[data-testid="output-status"]');
      if (!row) return null;
      return {
        type: row.getAttribute('data-type'),
        text: row.querySelector('.insp-out__value').textContent,
      };
    `);
    report.ok("Inspector 里有 status 这一行", status !== null, JSON.stringify(status));
    if (status) {
      report.eq("status 是个 Record", status.type, "Record");
      report.ok(
        "status 写明了裁剪窗的结局与前后点数",
        /GapRollCrop/.test(status.text) &&
          /"status":"(applied|reverted:min_points|disabled|rejected:[a-z_]+)"/.test(status.text) &&
          /beforePrimary/.test(status.text),
        status.text,
      );
    }
    const windowRow = await cdp.eval(`
      const row = document.querySelector('[data-testid="output-window"]');
      return row ? row.querySelector('.insp-out__value').textContent : null;
    `);
    report.ok("窗本身也列出来了", typeof windowRow === "string" && windowRow.includes("→"),
      String(windowRow));
  }

  // -- 数值：R1 的模型路径基线 gap 3.7838 / flush 2.3504 ----------------------
  const values = await cdp.eval(`
    const e = window.__lyflow.stores.execution.getState();
    const doc = window.__lyflow.stores.graph.getState().doc;
    const pick = (op) => {
      const n = doc.nodes.find((x) => x.op === op && x.id.startsWith('n_'));
      return n ? n.id : null;
    };
    const read = (id, port) => {
      const n = id ? e.nodes.get(id) : null;
      const o = n ? (n.stats?.outputs ?? []).find((x) => x.port === port) : null;
      return o && o.value ? o.value.value : null;
    };
    return { gap: read('n_gap', 'value'), flush: read('n_flush', 'value'),
             ref: [read('n_ref', 'gap'), read('n_ref', 'flush')] };
  `);
  report.ok(
    "拆分算子与黑盒对照给出同一对数（差 ≤ 0.002 mm）",
    values.gap !== null && values.flush !== null &&
      Math.abs(values.gap - values.ref[0]) <= 0.002 &&
      Math.abs(values.flush - values.ref[1]) <= 0.002,
    JSON.stringify(values),
  );
}

export const gapSuites = [suiteMeasurementOutputs, suiteRealGapGraph, suiteModelGapGraph];
