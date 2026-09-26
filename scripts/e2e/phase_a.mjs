// 阶段 A 验收：惰性分支（plan_extended / not_demanded 半透明 / stale 不误报）
// 与图级命名输出。对应 docs/phase-a1-acceptance.md 的两条。与其余分组共用一个 app 实例。

import { buildGraph, lit, newDoc, pressF5, replan, runAndWait, select } from "./page.mjs";

/** 主路径成功的 fallback 图：a 来自 gen.synthetic，b 那一路整条都是 deferred。 */
const OK_NODES = [
  { key: "a", op: "gen.synthetic", params: { pointCount: 4096, seed: 3 } },
  { key: "b", op: "gen.synthetic", params: { pointCount: 777, seed: 4 } },
  { key: "bTail", op: "filter.passthrough", params: { min: -100, max: 100 } },
  { key: "fb", op: "flow.fallback" },
];
const OK_EDGES = [
  { from: ["b", "cloud"], to: ["bTail", "cloud"] },
  { from: ["a", "cloud"], to: ["fb", "a"] },
  { from: ["bTail", "cloud"], to: ["fb", "b"] },
];

const opacityOf = (cdp, nodeId) =>
  cdp.eval(`
    const el = document.querySelector('[data-testid=${lit(`node-${nodeId}`)}]');
    if (!el) return null;
    // 透明度会动（docs/motion-plan.md）：刚加的节点还在播进场，类名一变还有一段 S3 过渡，
    // 刚跑完就读会读到半路上的值。等有限的动画都播完、再过两帧（进场播完才擦 inline 值），
    // 直到节点上不再有动画。running 的呼吸光是无限循环，不等它。
    const finite = () => el.getAnimations().filter((a) => a.effect?.getTiming().iterations !== Infinity);
    for (let i = 0; i < 10 && (i === 0 || finite().length > 0); i += 1) {
      await Promise.all(finite().map((a) => a.finished.catch(() => {})));
      await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    }
    return {
      notDemanded: el.getAttribute('data-not-demanded'),
      opacity: getComputedStyle(el).opacity,
      state: el.getAttribute('data-node-state'),
    };
  `);

async function suiteLazyBranch(cdp, report) {
  report.section("惰性分支：not_demanded 半透明、stale 不误报（ADR-0016）");

  await newDoc(cdp);
  const ids = await buildGraph(cdp, OK_NODES, OK_EDGES);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("主路径成功时整轮仍是 ok", run.status, "ok");
  report.eq("fallback 跑完了", run.nodes[ids.fb]?.state, "done");
  report.eq("备用源报 skipped", run.nodes[ids.b]?.state, "skipped");
  report.eq("备用源的原因是 not_demanded", run.nodes[ids.b]?.reason, "not_demanded");
  report.eq("备用链尾也是 not_demanded", run.nodes[ids.bTail]?.reason, "not_demanded");

  const painted = await opacityOf(cdp, ids.b);
  report.eq("备用节点带 data-not-demanded", painted?.notDemanded, "1");
  report.ok(
    "备用节点画成半透明",
    painted !== null && Number(painted.opacity) < 0.6,
    JSON.stringify(painted),
  );

  const active = await opacityOf(cdp, ids.a);
  report.ok(
    "主路径节点不透明",
    active !== null && Number(active.opacity) > 0.95,
    JSON.stringify(active),
  );

  // stale 不误报：跑完立刻重编一次，一个节点都不该是 stale
  const cache = await replan(cdp);
  report.ok("重编后没有节点被误标 stale", cache.stale.length === 0, JSON.stringify(cache.stale));
  report.ok(
    "没被 demand 的节点没有进 ranWith",
    cache.ranWith[ids.b] === undefined,
    JSON.stringify(Object.keys(cache.ranWith)),
  );

  return ids;
}

async function suitePlanExtended(cdp, report) {
  report.section("惰性分支：主路径失败时经 plan_extended 追加（ADR-0016）");

  await newDoc(cdp);
  // 主路径读一个不存在的文件：路径非空所以过得了校验，在执行期报 io。
  // 必须写**绝对**路径 —— 没保存过的图带相对路径参数时 App 会拦下运行。
  // fallback 的 a 端口声明了 acceptsError，所以整轮不该被它拖成 error。
  const ids = await buildGraph(
    cdp,
    [
      { key: "bad", op: "io.load_pcd", params: { path: "Z:/lyflow-没有这个文件.pcd" } },
      { key: "b", op: "gen.synthetic", params: { pointCount: 999, seed: 2 } },
      { key: "fb", op: "flow.fallback" },
    ],
    [
      { from: ["bad", "cloud"], to: ["fb", "a"] },
      { from: ["b", "cloud"], to: ["fb", "b"] },
    ],
  );

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.eq("主路径失败了", run.nodes[ids.bad]?.state, "error");
  report.eq("fallback 仍然跑完", run.nodes[ids.fb]?.state, "done");
  report.eq("备用路径被 demand 之后跑完了", run.nodes[ids.b]?.state, "done");
  report.eq("整轮不因被吸收的失败而 error", run.status, "ok");
  report.eq("fallback 透传的是备用路径的点数", run.nodes[ids.fb]?.elementCount, 999);

  const painted = await opacityOf(cdp, ids.b);
  report.eq("被 demand 的节点不再半透明", painted?.notDemanded, "0");

  await select(cdp, ids.fb);
}

async function suiteGraphOutputs(cdp, report) {
  report.section("图级命名输出（ADR-0017）");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 2048, seed: 11 } },
      { key: "pipe", op: "util.reroute" },
    ],
    [{ from: ["gen", "cloud"], to: ["pipe", "in"] }],
  );

  // A2-5：走右键菜单的「标为输出」，不再直接往 doc 上写
  const marked = await cdp.eval(`
    const el = document.querySelector('[data-testid="node-${ids.pipe}"]');
    if (!el) return 'no-node';
    const r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true,
      clientX: r.left + r.width / 2, clientY: r.top + r.height / 2,
    }));
    await new Promise((r2) => setTimeout(r2, 120));
    const btn = document.querySelector('[data-testid="ctx-mark-output-out"]');
    if (!btn) return 'no-item';
    const label = btn.textContent;
    btn.click();
    await new Promise((r2) => setTimeout(r2, 120));
    return { label, outputs: window.__lyflow.stores.graph.getState().doc.outputs ?? {} };
  `);
  // 菜单项没找到时 marked 是 'no-node' / 'no-item'，下一条就拿不到 outputs
  report.ok("标出来的名字取自端口名", JSON.stringify(Object.keys(marked?.outputs ?? {})) === '["out"]',
    `右键「标为输出」返回 ${JSON.stringify(marked)}`);
  report.eq(
    "指向的是刚才那个端口",
    [marked.outputs?.out?.node, marked.outputs?.out?.port],
    [ids.pipe, "out"],
  );

  // Inspector 的图级输出列表（A2-5）
  const listed = await cdp.eval(`
    const row = document.querySelector('[data-testid="graph-output-out"]');
    return row ? { node: row.getAttribute('data-node'), port: row.getAttribute('data-port'),
                   canRemove: !!document.querySelector('[data-testid="remove-output-out"]') } : null;
  `);
  report.eq("Inspector 列出了这个图级输出，写明来自哪个端口", [listed?.node, listed?.port], [ids.pipe, "out"]);
  report.ok("列表里能取消", listed?.canRemove === true, JSON.stringify(listed));

  // 名字统一叫 cloud，后面的断言沿用 A1 的写法
  await cdp.eval(`
    const g = window.__lyflow.stores.graph.getState();
    g.removeGraphOutput('out');
    window.__lyflow.stores.graph.getState()
      .markGraphOutput({ node: ${lit(ids.pipe)}, port: 'out' }, 'cloud');
    return true;
  `);

  const run = await runAndWait(cdp, () => pressF5(cdp));
  report.ok(
    "run_started 带回了 outputs 声明",
    Array.isArray(run.outputs) && run.outputs.some((o) => o.name === "cloud"),
    JSON.stringify(run.outputs),
  );

  const outputs = await cdp.eval(`
    return await window.__lyflow.runOutputs(${lit(run.runId)});
  `);
  report.ok(
    "lyflow_run_outputs 按名字给出点云元信息",
    outputs?.cloud?.type === "PointCloud" && outputs.cloud.elementCount === 2048,
    JSON.stringify(outputs),
  );

  // 跑完之后列表里带上实测值，并且能删掉
  const withValue = await cdp.eval(`
    const row = document.querySelector('[data-testid="graph-output-cloud"]');
    return row ? { type: row.getAttribute('data-type'), text: row.textContent } : null;
  `);
  report.eq("列表里补上了输出类型", withValue?.type, "PointCloud");

  const removed = await cdp.eval(`
    document.querySelector('[data-testid="remove-output-cloud"]').click();
    await new Promise((r) => setTimeout(r, 120));
    return {
      outputs: window.__lyflow.stores.graph.getState().doc.outputs ?? {},
      section: !!document.querySelector('[data-testid="graph-outputs"]'),
    };
  `);
  report.eq("取消后 doc 上没有图级输出了", Object.keys(removed.outputs), []);
  report.ok("列表也跟着收起来了", removed.section === false, JSON.stringify(removed));
}

export const phaseASuites = [suiteLazyBranch, suitePlanExtended, suiteGraphOutputs];
