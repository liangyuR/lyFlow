// 验收脚本 —— 用 CDP 驱动真实运行的 Tauri app。M2 的分组在本文件，M3 的在 ./m3.mjs。
// 跑法与「为什么是 CDP」见 ./README.md。

import fs from "node:fs";
import path from "node:path";

import { sleep } from "./cdp.mjs";
import { launchApp, makeChineseWorkspace, Report, stagePackagedApp } from "./harness.mjs";
import {
  buildGraph,
  lit,
  newDoc,
  pressEscape,
  pressF5,
  runAndWait,
  saveGraphTo,
  select,
  selectAndReadViewer,
} from "./page.mjs";
import { gapSuites } from "./gap.mjs";
import { m3Suites } from "./m3.mjs";
import { m4Suites } from "./m4.mjs";
import { m8bSuites } from "./m8b.mjs";
import { peekSuites } from "./peek.mjs";
import { phaseASuites } from "./phase_a.mjs";

// ------------------------------------------------------------------- 各分组

/** §11：中文目录下保存图 + 存 PCD。顺带生成后面演示 pipeline 要读的那个文件。 */
async function suiteChinesePath(cdp, report, ws) {
  report.section("中文路径：保存图 + 写 PCD");

  const graphPath = path.join(ws.dir, "演示 流程.lyflow.json");
  const pcdName = "合成 点云.pcd";

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 60000, seed: 7 } },
      { key: "save", op: "io.save_pcd", params: { path: pcdName, format: "binary" } },
    ],
    [{ from: ["gen", "cloud"], to: ["save", "cloud"] }],
  );

  const saved = await saveGraphTo(cdp, graphPath);
  report.ok("图存进中文目录", fs.existsSync(graphPath), `filePath=${saved}`);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("运行状态 ok", run.status, "ok");

  const pcdPath = path.join(ws.dir, pcdName);
  const exists = fs.existsSync(pcdPath);
  report.ok("中文文件名的 PCD 写出来了", exists, pcdPath);
  if (exists) {
    report.ok("PCD 非空", fs.statSync(pcdPath).size > 1000, `${fs.statSync(pcdPath).size} 字节`);
  }
  report.ok("生成节点报出了点数", run.nodes[ids.gen]?.elementCount === 60000,
    JSON.stringify(run.nodes[ids.gen]));

  return { graphPath, pcdName };
}

/** §11：演示 pipeline → F5 → 依次变色 → ok → 每个节点 3D 点数 > 0。 */
async function suiteDemoPipeline(cdp, report, ws, pcdName) {
  report.section("演示 pipeline：读 PCD → 裁剪 → 降采样 → 去噪 → 平面 → 分离 → 存盘");

  const graphPath = path.join(ws.dir, "演示 完整流程.lyflow.json");
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "load", op: "io.load_pcd", params: { path: pcdName } },
      { key: "crop", op: "filter.crop_box" },
      { key: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.006, 0.006, 0.006] } },
      { key: "sor", op: "filter.statistical_outlier", params: { meanK: 20 } },
      { key: "plane", op: "segment.ransac_plane", params: { distanceThreshold: 0.02 } },
      { key: "split", op: "segment.extract_indices" },
      { key: "out", op: "io.save_pcd", params: { path: "去平面 结果.pcd" } },
    ],
    [
      { from: ["load", "cloud"], to: ["crop", "cloud"] },
      { from: ["crop", "cloud"], to: ["voxel", "cloud"] },
      { from: ["voxel", "cloud"], to: ["sor", "cloud"] },
      { from: ["sor", "cloud"], to: ["plane", "cloud"] },
      { from: ["sor", "cloud"], to: ["split", "cloud"] },
      { from: ["plane", "inliers"], to: ["split", "indices"] },
      { from: ["split", "rest"], to: ["out", "cloud"] },
    ],
  );
  await saveGraphTo(cdp, graphPath);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("运行状态 ok", run.status, "ok");
  if (run.status !== "ok") {
    report.fail("失败详情", JSON.stringify(run.nodes));
  }

  for (const key of Object.keys(ids)) {
    report.eq(`${key} 节点 done`, run.nodes[ids[key]]?.state, "done");
  }

  // 「节点依次变色」：每个节点都走过 pending → running → done，
  // 而且上游的 done 一定排在下游的 running 之前。
  const transitions = await cdp.eval(`return window.__lyflow.transitions;`);
  const seq = (id) => transitions.filter((t) => t.nodeId === id).map((t) => t.state);
  // 头一条 idle 是 run_started 播下的占位：计划里的节点先全部亮成「排队中」，
  // 用户才知道这次要跑哪些节点。
  report.eq("load 的状态序列", seq(ids.load), ["idle", "pending", "running", "done"]);
  report.eq("out 的状态序列", seq(ids.out), ["idle", "pending", "running", "done"]);

  const indexOf = (id, state) => transitions.findIndex((t) => t.nodeId === id && t.state === state);
  report.ok(
    "上游 done 早于下游 running",
    indexOf(ids.load, "done") < indexOf(ids.crop, "running") &&
      indexOf(ids.voxel, "done") < indexOf(ids.sor, "running"),
    JSON.stringify(transitions.map((t) => `${t.nodeId}:${t.state}`)),
  );

  // 降采样确实降了：不然「跑通了」可能只是每个算子都原样透传。
  report.ok(
    "voxel 的点数少于 crop",
    run.nodes[ids.voxel].elementCount > 0 &&
      run.nodes[ids.voxel].elementCount < run.nodes[ids.crop].elementCount,
    `crop=${run.nodes[ids.crop].elementCount} voxel=${run.nodes[ids.voxel].elementCount}`,
  );

  // 每个有点云输出的节点，3D 视图都要画出点来
  for (const key of ["load", "crop", "voxel", "sor", "split"]) {
    const view = await selectAndReadViewer(cdp, ids[key]);
    report.ok(
      `选中 ${key} → 3D 视图点数 > 0`,
      view.count > 0 && view.hasCanvas,
      `count=${view.count} canvas=${view.hasCanvas} status=${view.status}`,
    );
  }

  // ransac_plane 没有点云输出，视图借上游最近的那片云当底图（不再是一片空白）
  const planeView = await selectAndReadViewer(cdp, ids.plane);
  report.eq("选中 ransac_plane → 底图取自上游的 sor", planeView.base, ids.sor);
  report.ok(
    "底图标签写明是谁的云",
    /^底图：/.test(planeView.baseText ?? ""),
    `baseText=${planeView.baseText} status=${planeView.status}`,
  );
  report.ok("底图真的画出了点", planeView.count > 0, `count=${planeView.count}`);

  const outPcd = path.join(ws.dir, "去平面 结果.pcd");
  report.ok("下游 PCD 写到中文目录", fs.existsSync(outPcd), outPcd);

  return { ids, graphPath };
}

/** §11：坏参数 → 红框 + 下游 cancelled + 旁支照常执行。 */
async function suiteBadParam(cdp, report, ids) {
  report.section("坏参数：leafSize = 0");

  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.voxel)}, 'leafSize', [0, 0.01, 0.01]);
    return true;
  `);
  const run = await runAndWait(cdp, () => pressF5(cdp));

  report.eq("运行状态 error", run.status, "error");
  report.eq("voxel 标 error", run.nodes[ids.voxel]?.state, "error");
  report.eq(
    "错误定位到 leafSize",
    run.nodes[ids.voxel]?.errors?.[0]?.paramPath,
    "leafSize",
  );
  // 上游不受影响：一次看到所有能看到的。缓存复用后它们是 skipped 而不是 done（ADR-0007）
  const upstreamOk = (id) => ["done", "skipped"].includes(run.nodes[id]?.state);
  report.ok("上游 load 仍然跑通", upstreamOk(ids.load), run.nodes[ids.load]?.state);
  report.ok("上游 crop 仍然跑通", upstreamOk(ids.crop), run.nodes[ids.crop]?.state);
  // 下游是 cancelled + upstream_failed，不是 skipped
  for (const key of ["sor", "plane", "split", "out"]) {
    report.eq(`下游 ${key} 标 cancelled`, run.nodes[ids[key]]?.state, "cancelled");
  }
  report.eq(
    "下游的原因是 upstream_failed",
    run.nodes[ids.sor]?.errors?.[0]?.code,
    "upstream_failed",
  );

  // 红框：选中出错的节点，leafSize 那一行必须带错误标记
  await select(cdp, ids.voxel);
  const box = await cdp.eval(`
    const row = document.querySelector('[data-testid="param-leafSize"]');
    const other = document.querySelector('[data-testid="param-minPointsPerVoxel"]');
    return {
      hasError: !!row && row.getAttribute('data-param-error') === '1',
      message: row ? (row.querySelector('.insp-param__error')?.textContent ?? '') : '',
      otherClean: !other || other.getAttribute('data-param-error') !== '1',
      errorList: !!document.querySelector('[data-testid="inspector-errors"]'),
    };
  `);
  report.ok("leafSize 输入框标红", box.hasError, JSON.stringify(box));
  report.ok("错误消息贴在控件下方", box.message.length > 0, box.message);
  report.ok("其它参数没有被连坐", box.otherClean);
  report.ok("检查器顶部列出该节点全部诊断", box.errorList);

  // 节点上的颜色也要对得上
  const domStates = await cdp.eval(`
    const out = {};
    for (const el of document.querySelectorAll('[data-node-state]')) {
      out[el.getAttribute('data-testid').replace('node-', '')] = el.getAttribute('data-node-state');
    }
    return out;
  `);
  report.eq("画布上 voxel 节点是 error", domStates[ids.voxel], "error");
  report.eq("画布上 sor 节点是 cancelled", domStates[ids.sor], "cancelled");

  // 复原，后面的分组还要用这张图
  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.voxel)}, 'leafSize', [0.006, 0.006, 0.006]);
    return true;
  `);
}

/** 只校验（不执行）与取输出信息两条 command。UI 目前没有调用点（`validate_graph`
 * 留给 M3 的边编辑边标红，`get_output_info` 留给日志抽屉），最容易悄悄坏掉。 */
async function suiteValidateAndInfo(cdp, report, ids) {
  report.section("validate_graph / get_output_info");

  const clean = await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    return await b.transport.validateGraph(g.doc, g.filePath);
  `);
  report.eq("干净的图没有诊断", clean, []);

  const dirty = await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    g.setParam(${lit(ids.voxel)}, 'leafSize', [0, 0.01, 0.01]);
    const out = await b.transport.validateGraph(
      window.__lyflow.stores.graph.getState().doc, g.filePath);
    window.__lyflow.stores.graph.getState()
      .setParam(${lit(ids.voxel)}, 'leafSize', [0.006, 0.006, 0.006]);
    return out;
  `);
  const hit = dirty.find((d) => d.nodeId === ids.voxel && d.paramPath === "leafSize");
  report.ok("坏参数被 validate_graph 逮到", Boolean(hit), JSON.stringify(dirty));
  report.eq("诊断带 severity", hit?.severity, "error");
  report.eq("诊断的 phase 是 validate", hit?.phase, "validate");

  // get_output_info 用的是上一次运行留下的结果
  const info = await cdp.eval(`
    const b = window.__lyflow;
    const runId = b.stores.execution.getState().runId;
    return await b.transport.getOutputInfo(runId, ${lit(ids.split)});
  `);
  const ports = (info ?? []).map((o) => o.port).sort();
  report.eq("extract_indices 报出两个输出端口", ports, ["rest", "selected"]);
  report.ok(
    "输出信息带类型与元素数",
    info.every((o) => o.type === "PointCloud" && o.elementCount > 0),
    JSON.stringify(info),
  );
}

/** §11：运行中 Esc → cancelled，UI 无残留 running。 */
async function suiteCancel(cdp, report) {
  report.section("运行中按 Esc 取消");

  await newDoc(cdp);
  // 三百万点 × 一串体素栅格：不取消要跑好几秒，取消才有意义
  const nodes = [{ key: "gen", op: "gen.synthetic", params: { pointCount: 3000000 } }];
  const edges = [];
  for (let i = 0; i < 12; i++) {
    nodes.push({
      key: `v${i}`,
      op: "filter.voxel_grid",
      params: { leafSize: [0.0008, 0.0008, 0.0008] },
      row: 0,
    });
    edges.push({ from: [i === 0 ? "gen" : `v${i - 1}`, "cloud"], to: [`v${i}`, "cloud"] });
  }
  const ids = await buildGraph(cdp, nodes, edges);

  const before = await cdp.eval(`return window.__lyflow.stores.execution.getState().runId;`);
  await pressF5(cdp);
  await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runId !== ${lit("__none__")} && s.runStatus === 'running'; })()`,
    { timeoutMs: 15_000, what: "运行开始" },
  );
  // 等它真的跑起来再取消，否则测的是「还没开始就取消」
  await sleep(400);
  await pressEscape(cdp);

  await cdp.waitFor(
    `(() => { const s = window.__lyflow.stores.execution.getState();
              return s.runStatus !== 'idle' && s.runStatus !== 'running'; })()`,
    { timeoutMs: 30_000, what: "取消后运行结束" },
  );
  const run = await cdp.eval(`return window.__lyflow.snapshot().run;`);
  void before;
  report.eq("运行状态 cancelled", run.status, "cancelled");

  const running = Object.entries(run.nodes).filter(([, n]) => n.state === "running");
  report.ok("状态里没有残留的 running", running.length === 0, JSON.stringify(running));

  const domRunning = await cdp.eval(`
    return {
      running: document.querySelectorAll('.node--running').length,
      cancelled: document.querySelectorAll('.node--cancelled').length,
      cancelBtnDisabled: document.querySelector('[data-testid="cancel-button"]')?.disabled ?? null,
      summary: document.querySelector('[data-testid="run-summary"]')?.getAttribute('data-run-status'),
    };
  `);
  report.eq("画布上没有 running 的节点", domRunning.running, 0);
  report.ok("画布上有 cancelled 的节点", domRunning.cancelled > 0, JSON.stringify(domRunning));
  report.eq("取消按钮已经变灰", domRunning.cancelBtnDisabled, true);
  report.eq("工具栏摘要显示 cancelled", domRunning.summary, "cancelled");

  return ids;
}

/** §11：运行后改任一参数 → 全部节点标 stale。 */
async function suiteStale(cdp, report) {
  report.section("改参数 → 结果标为过时");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 20000 } },
      { key: "voxel", op: "filter.voxel_grid" },
      { key: "pass", op: "filter.passthrough" },
    ],
    [
      { from: ["gen", "cloud"], to: ["voxel", "cloud"] },
      { from: ["voxel", "cloud"], to: ["pass", "cloud"] },
    ],
  );
  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("先跑一次 ok", run.status, "ok");

  const before = await cdp.eval(`
    return {
      stale: window.__lyflow.stores.execution.getState().stale,
      staleNodes: document.querySelectorAll('.node.is-stale').length,
    };
  `);
  report.ok("刚跑完不是过时状态", before.stale === false && before.staleNodes === 0,
    JSON.stringify(before));

  await cdp.eval(`
    window.__lyflow.stores.graph.getState().setParam(${lit(ids.gen)}, 'seed', 42);
    return true;
  `);
  await sleep(200);

  const after = await cdp.eval(`
    return {
      stale: window.__lyflow.stores.execution.getState().stale,
      staleNodes: document.querySelectorAll('.node.is-stale').length,
      totalNodes: document.querySelectorAll('[data-node-state]').length,
      summaryStale: !!document.querySelector('.toolbar__stat--stale'),
    };
  `);
  report.ok("store 标记为过时", after.stale === true, JSON.stringify(after));
  report.ok(
    "全部节点都变虚",
    after.totalNodes > 0 && after.staleNodes === after.totalNodes,
    JSON.stringify(after),
  );
  report.ok("工具栏显示「已过时」", after.summaryStale);
}

/** P1 #27：右键 → 运行到此节点。数据通路 M2 就位，这里验一遍。 */
async function suiteRunToNode(cdp, report) {
  report.section("Run to node（只跑上游闭包）");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 20000 } },
      { key: "voxel", op: "filter.voxel_grid" },
      { key: "tail", op: "filter.passthrough" },
    ],
    [
      { from: ["gen", "cloud"], to: ["voxel", "cloud"] },
      { from: ["voxel", "cloud"], to: ["tail", "cloud"] },
    ],
  );

  // 真的走右键菜单，不是直接调 store —— 菜单本身也是验收对象
  const opened = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${ids.voxel}"]');
    if (!el) return 'no-node';
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true,
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2,
    }));
    await new Promise((r2) => setTimeout(r2, 120));
    return document.querySelector('[data-testid="node-context-menu"]') ? 'ok' : 'no-menu';
  `);
  report.eq("右键弹出菜单", opened, "ok");

  const run = await runAndWait(cdp, () =>
    cdp.eval(`document.querySelector('[data-testid="run-to-node"]').click(); return true;`),
  );
  report.eq("运行状态 ok", run.status, "ok");
  report.eq("目标节点被记录", run.targets, [ids.voxel]);
  // M3 起这两个可能是 skipped（命中缓存），两种都算跑通（ADR-0007）
  const ran = (id) => ["done", "skipped"].includes(run.nodes[id]?.state);
  report.ok("上游 gen 执行了", ran(ids.gen), run.nodes[ids.gen]?.state);
  report.ok("目标 voxel 执行了", ran(ids.voxel), run.nodes[ids.voxel]?.state);
  report.ok("下游 tail 根本没进计划", run.nodes[ids.tail] === undefined,
    JSON.stringify(run.nodes[ids.tail]));
}

// ---------------------------------------------------------------------- main

async function main() {
  // --packaged：不起 tauri dev，而是把 `tauri build` 的产物拷进一个干净目录再启动。
  // 验的是 m2-plan §11 的「安装包在干净目录能启动并跑通演示 pipeline（DLL 随包）」。
  const packaged = process.argv.includes("--packaged");

  const report = new Report();
  const ws = makeChineseWorkspace();
  console.log(`中文工作目录：${ws.dir}`);

  let staged = null;
  if (packaged) {
    staged = stagePackagedApp();
    console.log(`干净安装目录：${staged.dir}（${staged.dlls} 个 DLL 随包）`);
  }

  const app = await launchApp({
    verbose: process.env.LYFLOW_E2E_VERBOSE === "1",
    packagedExe: staged?.exe ?? null,
  });
  const { cdp, consoleErrors } = app;

  if (packaged) {
    report.section("安装包（干净目录）");
    report.ok("打包产物启动成功并加载到了 core", staged.dlls >= 20,
      `${staged.dlls} 个 DLL`);
    const info = await cdp.eval(`return await window.__lyflow.transport.getCoreInfo();`);
    report.ok("core 版本可读", Boolean(info?.version), JSON.stringify(info));
    // 内置算子 16 个；库目录里可能还有别的，所以只断言下界
    report.ok("内置算子全在（≥ 16）", (info?.operatorCount ?? 0) >= 16,
      String(info?.operatorCount));
    report.ok("CLI 也随包", staged.cli != null && fs.existsSync(staged.cli), String(staged.cli));
    // ml.onnx_run 要这两个 DLL，而它们不在 vcpkg 里 —— applocal 看不见，
    // 是 packs/std-ml 的 cmake 拷进 core 的 bin/ 的（ADR-0015）。那个包默认开，所以永远随包。
    const ort = ["onnxruntime.dll", "onnxruntime_providers_shared.dll"];
    const present = ort.filter((n) => fs.existsSync(path.join(staged.dir, n)));
    report.eq("onnxruntime 两个 DLL 都在干净目录里", present, ort);
  }

  try {
    const { pcdName } = await suiteChinesePath(cdp, report, ws);
    const { ids } = await suiteDemoPipeline(cdp, report, ws, pcdName);
    // 顺序有意义：validate/info 要用演示 pipeline 那次成功运行留下的结果，
    // 所以必须排在把 leafSize 改坏的那一组之前。
    await suiteValidateAndInfo(cdp, report, ids);
    await suiteBadParam(cdp, report, ids);
    await suiteCancel(cdp, report);
    await suiteStale(cdp, report);
    await suiteRunToNode(cdp, report);

    const grouped = [
      ...m3Suites, ...m4Suites, ...phaseASuites, ...gapSuites, ...peekSuites, ...m8bSuites,
    ];
    for (const suite of grouped) {
      try {
        await suite(cdp, report, ws);
      } catch (e) {
        report.fail(`分组 ${suite.name || "匿名"} 中断`, e.stack ?? String(e));
      }
    }

    report.section("控制台");
    // React 的 StrictMode 在 dev 下会重复挂载并打一些 warning，
    // 只拦真正的 error —— 但一条都不许有。
    report.ok(
      "跑完全程没有控制台报错",
      consoleErrors.length === 0,
      consoleErrors.slice(0, 5).join("\n      "),
    );
  } catch (e) {
    report.section("脚本本身");
    report.fail("验收脚本中断", e.stack ?? String(e));
  } finally {
    report.summary();
    await app.close();
    ws.cleanup();
    if (staged) {
      // exe 刚被杀掉，文件句柄可能还没释放，删不掉就留着 —— 临时目录里的
      // 一份拷贝不值得为它加重试循环。
      try {
        fs.rmSync(staged.dir, { recursive: true, force: true });
      } catch {
        console.log(`（没能删掉 ${staged.dir}，手动清理即可）`);
      }
    }
  }

  process.exit(report.failures.length === 0 ? 0 : 1);
}

await main();
