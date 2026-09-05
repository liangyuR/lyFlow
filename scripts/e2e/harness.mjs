// 验收脚手架：起 app、连 CDP、记断言、收尾。见 ./README.md。

import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { Cdp, sleep, waitForTarget } from "./cdp.mjs";

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
export const CDP_PORT = Number(process.env.LYFLOW_CDP_PORT ?? 9222);

// ---------------------------------------------------------------- 断言记录

export class Report {
  #rows = [];
  #section = "";

  section(name) {
    this.#section = name;
    console.log(`\n── ${name}`);
  }

  ok(name, condition, detail = "") {
    const pass = Boolean(condition);
    this.#rows.push({ section: this.#section, name, pass, detail });
    const mark = pass ? "\x1b[32m✓\x1b[0m" : "\x1b[31m✗\x1b[0m";
    console.log(`  ${mark} ${name}${detail && !pass ? `\n      ${detail}` : ""}`);
    return pass;
  }

  eq(name, actual, expected) {
    const pass = JSON.stringify(actual) === JSON.stringify(expected);
    return this.ok(name, pass, `期望 ${JSON.stringify(expected)}，实际 ${JSON.stringify(actual)}`);
  }

  fail(name, detail) {
    return this.ok(name, false, detail);
  }

  get passed() {
    return this.#rows.filter((r) => r.pass).length;
  }
  get total() {
    return this.#rows.length;
  }
  get failures() {
    return this.#rows.filter((r) => !r.pass);
  }

  summary() {
    console.log(
      `\n${this.passed}/${this.total} 项通过` +
        (this.failures.length ? `，\x1b[31m${this.failures.length} 项失败\x1b[0m` : "，\x1b[32m全绿\x1b[0m"),
    );
    for (const f of this.failures) {
      console.log(`  \x1b[31m✗\x1b[0m [${f.section}] ${f.name}${f.detail ? ` — ${f.detail}` : ""}`);
    }
  }
}

// ------------------------------------------------------------------ 起 app

/** 把 `tauri build` 的产物复刻成一个**干净目录**里的安装结果：exe + 同目录的全部
 * DLL。验的是「DLL 随包 + 从 exe 同目录加载」，不等于干净机器 —— 见 ./README.md。 */
export function stagePackagedApp() {
  const release = path.join(ROOT, "bridge", "target", "release");
  // M4 起桌面壳叫 lyflow-app.exe：包名那个 bin 名让给了 CLI（ADR-0012）
  const exe = path.join(release, "lyflow-app.exe");
  if (!fs.existsSync(exe)) {
    throw new Error(`找不到 ${exe} —— 先跑 \`pnpm tauri build\``);
  }
  const dir = path.join(os.tmpdir(), `lyflow 安装目录 ${process.pid}`);
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  let dlls = 0;
  // CLI 也拷进去：无 GUI 机器上的验收跑的就是这一份（§6 第二条）
  const exes = ["lyflow-app.exe", "lyflow.exe"];
  for (const name of fs.readdirSync(release)) {
    if (name.endsWith(".dll")) dlls += 1;
    else if (!exes.includes(name)) continue;
    fs.copyFileSync(path.join(release, name), path.join(dir, name));
  }
  return { dir, exe: path.join(dir, "lyflow-app.exe"), cli: path.join(dir, "lyflow.exe"), dlls };
}

/** 启动 app 并连上它的 WebView2。三种模式（默认 `tauri dev` / `packagedExe` /
 * `LYFLOW_E2E_ATTACH`）与那条 WebView2 注入口的用法见 ./README.md。 */
export async function launchApp({ verbose = false, packagedExe = null } = {}) {
  const attach = process.env.LYFLOW_E2E_ATTACH === "1";
  let child = null;
  let killing = false;

  if (packagedExe && !attach) {
    console.log(`启动已打包的 ${packagedExe}（CDP 端口 ${CDP_PORT}）`);
    child = spawn(packagedExe, [], {
      cwd: path.dirname(packagedExe),
      env: {
        ...process.env,
        WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${CDP_PORT} --remote-allow-origins=*`,
      },
      stdio: verbose ? "inherit" : ["ignore", "pipe", "pipe"],
    });
    child.stdout?.on("data", (c) => verbose || process.stdout.write(c));
    child.stderr?.on("data", (c) => process.stderr.write(c));
  } else if (!attach) {
    console.log(`启动 tauri dev（CDP 端口 ${CDP_PORT}）…首次运行要编 C++ 与 Rust，请耐心`);
    child = spawn("pnpm", ["tauri", "dev"], {
      cwd: ROOT,
      shell: true,
      env: {
        ...process.env,
        WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${CDP_PORT} --remote-allow-origins=*`,
      },
      stdio: verbose ? "inherit" : ["ignore", "pipe", "pipe"],
    });
    if (!verbose) {
      // 不吞掉输出：编译错误、core 自检失败都在这里，静默是最坏的选择。
      // 但只在真的出问题时才打印，正常路径下几百行 cargo 输出没有信息量。
      const tail = [];
      const keep = (chunk) => {
        tail.push(chunk.toString());
        if (tail.length > 60) tail.shift();
      };
      child.stdout?.on("data", keep);
      child.stderr?.on("data", keep);
      child.on("exit", (code) => {
        // 收尾时是我们自己 taskkill 的，非零退出码在那里没有信息量
        if (!killing && code !== 0 && code !== null) {
          console.error(`\ntauri dev 退出（code ${code}），最后的输出：\n${tail.join("")}`);
        }
      });
    }
  } else {
    console.log("LYFLOW_E2E_ATTACH=1：连到已经开着的实例");
  }

  const target = await waitForTarget(CDP_PORT, {
    timeoutMs: attach ? 15_000 : 15 * 60_000,
    // Tauri 的 WebView2 里还会有 devtools 之类的 target，认页面 URL
    match: (t) => /localhost:5173|tauri\.localhost|index\.html/.test(t.url ?? ""),
  });
  const cdp = await Cdp.connect(target.webSocketDebuggerUrl);
  await cdp.send("Runtime.enable");
  await cdp.send("Console.enable").catch(() => {});

  // 收集控制台报错。「跑完没有红字」本身就是一条验收项。
  const consoleErrors = [];
  cdp.on("Runtime.consoleAPICalled", (p) => {
    if (p.type === "error") {
      consoleErrors.push(p.args.map((a) => a.value ?? a.description ?? "").join(" "));
    }
  });
  cdp.on("Runtime.exceptionThrown", (p) => {
    consoleErrors.push(p.exceptionDetails?.exception?.description ?? p.exceptionDetails?.text ?? "");
  });

  // 等 React 挂载并装好窗口桥
  await cdp.waitFor("window.__lyflow !== undefined", {
    timeoutMs: 60_000,
    what: "window.__lyflow（devbridge 没装上？）",
  });
  await cdp.waitFor("window.__lyflow.stores.manifest.getState().status === 'ready'", {
    timeoutMs: 60_000,
    what: "manifest 加载完成",
  });

  return {
    cdp,
    consoleErrors,
    async close() {
      cdp.close();
      killing = true;
      if (child) {
        // tauri dev 会拉起 vite 和真正的 exe，光 kill 父进程会留下孤儿。
        // Windows 上用 taskkill /T 整棵树带走。
        if (process.platform === "win32") {
          spawn("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" });
        } else {
          child.kill("SIGTERM");
        }
        await sleep(1500);
      }
    },
  };
}

// -------------------------------------------------------------- 临时工作区

/** 建一个名字里带中文和空格的临时目录，验收中文路径用。 */
export function makeChineseWorkspace() {
  const dir = path.join(os.tmpdir(), `lyflow 验收 中文目录 ${process.pid}`);
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  return {
    dir,
    cleanup() {
      fs.rmSync(dir, { recursive: true, force: true });
    },
  };
}
