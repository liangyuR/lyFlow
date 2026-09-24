// HttpTransport 的验收：Node 桩服务器 + 系统 Chrome/Edge（CDP）驱动
// examples/host-react。跑法与前置条件见 ./README.md 的「e2e:http」。

import { spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { Cdp, sleep, waitForTarget } from "./cdp.mjs";
import { ROOT, Report } from "./harness.mjs";
import { buildGraph, lit, newDoc, runAndWait, selectAndReadViewer } from "./page.mjs";

const API_PORT = Number(process.env.LYFLOW_HTTP_PORT ?? 8788);
const HOST_PORT = Number(process.env.LYFLOW_HOST_PORT ?? 5174);
const CDP_PORT = Number(process.env.LYFLOW_HTTP_CDP_PORT ?? 9333);
const TOKEN = process.env.LYFLOW_HTTP_TOKEN ?? crypto.randomBytes(12).toString("hex");

const CLI = path.join(ROOT, "bridge", "target", "debug", "lyflow.exe");

const BROWSERS = [
  `${process.env["ProgramFiles"]}\\Google\\Chrome\\Application\\chrome.exe`,
  `${process.env["ProgramFiles(x86)"]}\\Google\\Chrome\\Application\\chrome.exe`,
  `${process.env.LOCALAPPDATA}\\Google\\Chrome\\Application\\chrome.exe`,
  `${process.env["ProgramFiles(x86)"]}\\Microsoft\\Edge\\Application\\msedge.exe`,
  `${process.env["ProgramFiles"]}\\Microsoft\\Edge\\Application\\msedge.exe`,
];

function findBrowser() {
  if (process.env.LYFLOW_BROWSER) return process.env.LYFLOW_BROWSER;
  for (const exe of BROWSERS) {
    if (exe && fs.existsSync(exe)) return exe;
  }
  throw new Error(
    "找不到 Chrome 或 Edge。装一个，或用 LYFLOW_BROWSER 指到浏览器可执行文件。",
  );
}

function waitForHttp(url, timeoutMs, what) {
  const deadline = Date.now() + timeoutMs;
  return (async () => {
    let last = "还没开始";
    while (Date.now() < deadline) {
      try {
        const res = await fetch(url, { headers: { Authorization: `Bearer ${TOKEN}` } });
        if (res.ok) return true;
        last = `HTTP ${res.status}`;
      } catch (e) {
        last = e.message;
      }
      await sleep(250);
    }
    throw new Error(`等不到 ${what}（${url}）：${last}`);
  })();
}

function tail(child, keep = 40) {
  const lines = [];
  const push = (c) => {
    lines.push(c.toString());
    if (lines.length > keep) lines.shift();
  };
  child.stdout?.on("data", push);
  child.stderr?.on("data", push);
  return lines;
}

// ---------------------------------------------------------------- 各分组

/** 宿主与包共用同一个 React：宿主组件里调包内 store 的 hook 不炸。 */
async function suiteHost(cdp, report) {
  report.section("宿主集成：@lyflow/editor + HttpTransport");

  const probe = await cdp.eval(`
    const el = document.querySelector('[data-testid="host-probe"]');
    return {
      host: window.__lyflowHost ?? null,
      hookOk: el ? el.getAttribute('data-hook-ok') : null,
      react: el ? el.getAttribute('data-react') : null,
      editors: document.querySelectorAll('[data-lyflow-editor="1"]').length,
    };
  `);
  report.ok(
    "宿主组件里调包内 store 的 hook 没炸（同一个 React 实例）",
    probe.host?.hookOk === true && probe.hookOk === "1",
    JSON.stringify(probe),
  );
  report.ok("宿主与包报同一个 React 版本", probe.host?.react === probe.react, JSON.stringify(probe));
  report.eq("页面上只有一个编辑器根元素", probe.editors, 1);

  const snap = await cdp.eval(`return window.__lyflow.snapshot();`);
  report.eq("传输层是 http", snap.transport, "http");
  report.eq("manifest 已就绪", snap.manifestStatus, "ready");
  report.ok("算子表读到了（≥16）", snap.operatorCount >= 16, String(snap.operatorCount));

  const info = await cdp.eval(`return await window.__lyflow.transport.getCoreInfo();`);
  report.ok("core 版本可读", Boolean(info?.version), JSON.stringify(info));

  // 主题变量有默认值，宿主不设也能显示
  const themed = await cdp.eval(`
    const el = document.querySelector('[data-lyflow-editor="1"]');
    const cs = getComputedStyle(el);
    return {
      bg: cs.getPropertyValue('--lyflow-bg-0').trim(),
      accent: cs.getPropertyValue('--lyflow-accent').trim(),
      background: cs.backgroundColor,
    };
  `);
  report.ok(
    "--lyflow-* 变量有默认值",
    themed.bg.length > 0 && themed.accent.length > 0,
    JSON.stringify(themed),
  );
}

/** 打开图 → 改参数 → 运行 → 看 3D → 取图级输出。 */
async function suiteEditAndRun(cdp, report, ws) {
  report.section("HttpTransport：打开图、改参数、运行、3D、图级输出");

  const rel = "demo.lyflow.json";
  const doc = {
    schemaVersion: 1,
    id: "http-demo",
    name: "http 演示",
    nodes: [
      { id: "gen", op: "gen.synthetic", params: { pointCount: 20000, seed: 5 }, ui: { position: { x: 40, y: 80 } } },
      { id: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] }, ui: { position: { x: 300, y: 80 } } },
    ],
    edges: [{ id: "e1", from: { node: "gen", port: "cloud" }, to: { node: "voxel", port: "cloud" } }],
    outputs: { thinned: { node: "voxel", port: "cloud" } },
  };
  fs.writeFileSync(path.join(ws, rel), JSON.stringify(doc, null, 2), "utf8");

  const opened = await cdp.eval(`
    const b = window.__lyflow;
    const loaded = await b.transport.loadGraph(${lit(rel)});
    b.stores.graph.getState().loadDoc(loaded.doc, ${lit(rel)});
    b.stores.ui.getState().clearSelection();
    const g = b.stores.graph.getState();
    return { nodes: g.doc.nodes.length, edges: g.doc.edges.length, outputs: Object.keys(g.doc.outputs ?? {}) };
  `);
  report.eq("经 HTTP 打开的图有两个节点", opened.nodes, 2);
  report.eq("边也回来了", opened.edges, 1);
  report.eq("图级输出声明回来了", opened.outputs, ["thinned"]);

  // 存盘也走 HTTP：改一次参数再写回工作区
  const saved = await cdp.eval(`
    const b = window.__lyflow;
    b.stores.graph.getState().setParam('gen', 'pointCount', 24000);
    const g = b.stores.graph.getState();
    await b.transport.saveGraph(${lit(rel)}, g.doc);
    b.stores.graph.getState().markSaved(${lit(rel)});
    return b.stores.graph.getState().doc.nodes.find((n) => n.id === 'gen').params.pointCount;
  `);
  report.eq("改参数后经 HTTP 存盘", saved, 24000);
  const onDisk = JSON.parse(fs.readFileSync(path.join(ws, rel), "utf8"));
  report.eq(
    "磁盘上的图确实变了",
    onDisk.nodes.find((n) => n.id === "gen").params.pointCount,
    24000,
  );

  const run = await runAndWait(cdp, () => cdp.eval(`await window.__lyflow.run(); return true;`));
  report.eq("运行状态 ok", run.status, "ok");
  report.eq("gen 节点 done", run.nodes.gen?.state, "done");
  report.eq("voxel 节点 done", run.nodes.voxel?.state, "done");
  report.eq("gen 报出的点数", run.nodes.gen?.elementCount, 24000);
  report.ok(
    "voxel 确实降了采样",
    run.nodes.voxel?.elementCount > 0 && run.nodes.voxel.elementCount < 24000,
    `voxel=${run.nodes.voxel?.elementCount}`,
  );
  report.eq("run_started 带回图级输出声明", (run.outputs ?? []).map((o) => o.name), ["thinned"]);

  const view = await selectAndReadViewer(cdp, "voxel");
  report.ok(
    "选中 voxel → 3D 视图画出了点",
    view.count > 0 && view.hasCanvas,
    `count=${view.count} canvas=${view.hasCanvas} status=${view.status}`,
  );

  const outputs = await cdp.eval(`
    const runId = window.__lyflow.stores.execution.getState().runId;
    return await window.__lyflow.runOutputs(runId);
  `);
  report.ok("图级输出按名字取到了", Boolean(outputs?.thinned), JSON.stringify(outputs));
  report.eq("输出指向 voxel.cloud", [outputs?.thinned?.node, outputs?.thinned?.port], ["voxel", "cloud"]);
  report.eq("输出类型是点云", outputs?.thinned?.type, "PointCloud");
  report.ok(
    "输出的元素数与节点报的一致",
    outputs?.thinned?.elementCount === run.nodes.voxel?.elementCount,
    `${outputs?.thinned?.elementCount} vs ${run.nodes.voxel?.elementCount}`,
  );
}

/** 从零搭一张图、走键盘 F5：编辑动作与快捷键在浏览器宿主里也能用。 */
async function suiteBuildAndShortcut(cdp, report) {
  report.section("HttpTransport：搭图 + 快捷键运行 + 坏参数");

  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      { key: "gen", op: "gen.synthetic", params: { pointCount: 12000, seed: 2 } },
      { key: "pass", op: "filter.passthrough" },
    ],
    [{ from: ["gen", "cloud"], to: ["pass", "cloud"] }],
  );

  // 焦点在编辑器根元素上，F5 才是编辑器的（A2-3）
  await cdp.eval(`document.querySelector('[data-lyflow-editor="1"]').focus(); return true;`);
  const run = await runAndWait(cdp, async () => {
    await cdp.send("Input.dispatchKeyEvent", {
      type: "rawKeyDown",
      key: "F5",
      code: "F5",
      windowsVirtualKeyCode: 116,
      nativeVirtualKeyCode: 116,
    });
    await cdp.send("Input.dispatchKeyEvent", {
      type: "keyUp",
      key: "F5",
      code: "F5",
      windowsVirtualKeyCode: 116,
      nativeVirtualKeyCode: 116,
    });
  });
  report.eq("F5 触发的运行 ok", run.status, "ok");
  report.eq("pass 节点 done", run.nodes[ids.pass]?.state, "done");

  const bad = await cdp.eval(`
    const b = window.__lyflow;
    const g = b.stores.graph.getState();
    return await b.transport.validateGraph(
      { ...g.doc, nodes: g.doc.nodes.map((n) =>
          n.id === ${lit(ids.gen)} ? { ...n, params: { ...n.params, pointCount: -1 } } : n) },
      g.filePath,
    );
  `);
  report.ok("坏参数被 HTTP 校验逮到", bad.length > 0, JSON.stringify(bad).slice(0, 200));
  report.eq("诊断带 nodeId", bad[0]?.nodeId, ids.gen);
}

/** 宿主关动效（docs/motion-plan.md A4、§3 验收 9 的 animations={false}）：真鼠标点宿主栏上的
 *  「动效」开关，编辑器根上挂 lyflow-motion-off，新节点不播进场、新连线不长；再点回来恢复。 */
async function suiteAnimationsProp(cdp, report) {
  report.section("宿主 animations={false}：关掉之后进场与生长都不播，打开恢复");

  const toggle = async () => {
    const p = await cdp.eval(`
      const r = document.querySelector('[data-testid="host-animations"]').getBoundingClientRect();
      return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
    `);
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y, buttons: 0 });
    await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: p.x, y: p.y, button: "left", buttons: 1, clickCount: 1 });
    await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: p.x, y: p.y, button: "left", buttons: 1, clickCount: 1 });
    await sleep(200);
  };
  /** 加两个节点、连一条线，记下进场/生长标记出现过几次，以及新节点两帧后的透明度。 */
  const probe = () => cdp.eval(`
    const hits = { entering: 0, growing: 0 };
    const obs = new MutationObserver((list) => {
      for (const m of list) {
        const els = m.type === 'attributes' ? [m.target]
          : [...m.addedNodes].filter((n) => n.nodeType === 1).flatMap((n) => [n, ...n.querySelectorAll('*')]);
        for (const el of els) {
          if (el.getAttribute('data-entering') !== null) hits.entering += 1;
          if (el.getAttribute('data-growing') !== null) hits.growing += 1;
        }
      }
    });
    obs.observe(document.querySelector('.react-flow'), { subtree: true, childList: true, attributes: true,
      attributeFilter: ['data-entering', 'data-growing'] });
    const g = () => window.__lyflow.stores.graph.getState();
    const a = g().addNode('gen.synthetic', { x: 40, y: 40 });
    const b = g().addNode('filter.passthrough', { x: 320, y: 40 });
    // 用 setTimeout 不用 rAF：浏览器窗口被挡住时页面是 hidden，rAF 根本不来，会一直等下去
    await new Promise((r) => setTimeout(r, 40));
    const opacity = Number(getComputedStyle(document.querySelector('[data-testid="node-' + a + '"]')).opacity);
    g().connect({ node: a, port: 'cloud' }, { node: b, port: 'cloud' });
    await new Promise((r) => setTimeout(r, 500));
    obs.disconnect();
    const root = document.querySelector('[data-lyflow-editor="1"]');
    return { ...hits, opacity, motion: root.getAttribute('data-motion'), off: root.classList.contains('lyflow-motion-off') };
  `);

  await newDoc(cdp);
  await sleep(500);
  await toggle();
  const off = await probe();
  report.ok("开关关掉：编辑器根上是 lyflow-motion-off", off.motion === "off" && off.off, JSON.stringify(off));
  report.eq("关动效时新节点 40 ms 后就是不透明的", off.opacity, 1);
  report.eq("关动效时没有进场标记", off.entering, 0);
  report.eq("关动效时新连线没有生长标记", off.growing, 0);

  await newDoc(cdp);
  await sleep(500);
  await toggle();
  const on = await probe();
  report.ok("开关打开：动效恢复", on.motion === "on" && !on.off, JSON.stringify(on));
  report.ok("恢复之后新节点又播进场、新连线又会长", on.entering > 0 && on.growing > 0, JSON.stringify(on));
}

// ------------------------------------------------------------------- main

async function main() {
  const report = new Report();

  if (!fs.existsSync(CLI)) {
    console.error(`找不到 ${CLI}\n先跑：cargo build --manifest-path bridge/Cargo.toml --bin lyflow --no-default-features`);
    process.exit(1);
  }

  const ws = fs.mkdtempSync(path.join(os.tmpdir(), "lyflow-http-e2e-"));
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "lyflow-http-chrome-"));
  console.log(`工作区 ${ws}`);

  const server = spawn(
    process.execPath,
    [
      path.join(ROOT, "packages", "editor", "test-server", "server.mjs"),
      "--port", String(API_PORT),
      "--root", ws,
      "--cli", CLI,
      "--token", TOKEN,
    ],
    { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] },
  );
  const serverLog = tail(server);

  const host = spawn(
    "pnpm",
    ["--filter", "lyflow-host-react", "dev", "--port", String(HOST_PORT), "--strictPort"],
    {
      cwd: ROOT,
      shell: true,
      stdio: ["ignore", "pipe", "pipe"],
      env: {
        ...process.env,
        VITE_LYFLOW_API: `http://127.0.0.1:${API_PORT}`,
        VITE_LYFLOW_TOKEN: TOKEN,
      },
    },
  );
  const hostLog = tail(host);

  let browser = null;
  let cdp = null;
  const consoleErrors = [];

  try {
    await waitForHttp(`http://127.0.0.1:${API_PORT}/lyflow/core-info`, 60_000, "桩服务器");
    await waitForHttp(`http://127.0.0.1:${HOST_PORT}/`, 120_000, "宿主 dev server");

    const exe = findBrowser();
    console.log(`浏览器 ${exe}（CDP 端口 ${CDP_PORT}）`);
    browser = spawn(
      exe,
      [
        `--remote-debugging-port=${CDP_PORT}`,
        `--user-data-dir=${profile}`,
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        "--remote-allow-origins=*",
        ...(process.env.LYFLOW_E2E_HEADLESS === "1" ? ["--headless=new"] : []),
        `http://127.0.0.1:${HOST_PORT}/`,
      ],
      { stdio: "ignore" },
    );

    const target = await waitForTarget(CDP_PORT, {
      timeoutMs: 60_000,
      match: (t) => (t.url ?? "").includes(`127.0.0.1:${HOST_PORT}`),
    });
    cdp = await Cdp.connect(target.webSocketDebuggerUrl);
    await cdp.send("Runtime.enable");
    await cdp.send("Console.enable").catch(() => {});
    cdp.on("Runtime.consoleAPICalled", (p) => {
      if (p.type === "error") {
        consoleErrors.push(p.args.map((a) => a.value ?? a.description ?? "").join(" "));
      }
    });
    cdp.on("Runtime.exceptionThrown", (p) => {
      consoleErrors.push(
        p.exceptionDetails?.exception?.description ?? p.exceptionDetails?.text ?? "",
      );
    });

    await cdp.waitFor("window.__lyflow !== undefined", {
      timeoutMs: 60_000,
      what: "window.__lyflow（宿主桥没装上？）",
    });
    await cdp.waitFor("window.__lyflow.stores.manifest.getState().status === 'ready'", {
      timeoutMs: 60_000,
      what: "manifest 加载完成",
    });

    await suiteHost(cdp, report);
    await suiteEditAndRun(cdp, report, ws);
    await suiteBuildAndShortcut(cdp, report);
    await suiteAnimationsProp(cdp, report);

    report.section("控制台");
    report.ok(
      "跑完全程没有控制台报错",
      consoleErrors.length === 0,
      consoleErrors.slice(0, 5).join("\n      "),
    );
  } catch (e) {
    report.section("脚本本身");
    report.fail("验收脚本中断", e.stack ?? String(e));
    if (serverLog.length) console.log(`\n桩服务器最后的输出：\n${serverLog.join("")}`);
    if (hostLog.length) console.log(`\n宿主 dev server 最后的输出：\n${hostLog.join("")}`);
  } finally {
    report.summary();
    cdp?.close();
    for (const child of [browser, host, server]) {
      if (!child) continue;
      if (process.platform === "win32") {
        spawn("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" });
      } else {
        child.kill("SIGTERM");
      }
    }
    await sleep(1200);
    for (const dir of [ws, profile]) {
      try {
        fs.rmSync(dir, { recursive: true, force: true });
      } catch {
        console.log(`（没能删掉 ${dir}，手动清理即可）`);
      }
    }
  }

  process.exit(report.failures.length === 0 ? 0 : 1);
}

await main();
