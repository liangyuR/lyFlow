// `pnpm dev` = tauri dev + core-watch。改一行 C++ 到界面上出现新算子，中间不用重启。
// 只要 core-watch，用 `pnpm core:watch`；只要 app，用 `pnpm dev --no-core-watch`。

import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const withCore = !process.argv.includes("--no-core-watch");

const children = [];

function start(name, command, args, options = {}) {
  const child = spawn(command, args, { cwd: ROOT, stdio: "inherit", shell: true, ...options });
  child.on("error", (e) => console.error(`[${name}] 启动失败: ${e.message}`));
  children.push({ name, child });
  return child;
}

// core-watch 先起：它第一次可能要跑一次完整构建，而 tauri dev 那边 build.rs
// 走的是另一个构建目录，两边互不阻塞。
if (withCore) {
  start("core-watch", "powershell", [
    "-ExecutionPolicy", "Bypass", "-File", path.join("scripts", "core-watch.ps1"),
  ]);
}

const app = start("tauri", "pnpm", ["tauri", "dev"]);

function shutdown() {
  for (const { child } of children) {
    if (child.pid === undefined || child.killed) continue;
    // tauri dev 会拉起 vite 和真正的 exe，光 kill 父进程会留下孤儿
    if (process.platform === "win32") {
      spawn("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" });
    } else {
      child.kill("SIGTERM");
    }
  }
}

app.on("exit", (code) => {
  shutdown();
  process.exit(code ?? 0);
});
for (const signal of ["SIGINT", "SIGTERM"]) {
  process.on(signal, () => {
    shutdown();
    process.exit(0);
  });
}
