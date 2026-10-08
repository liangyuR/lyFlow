import { spawn } from "node:child_process";
import fs from "node:fs";

import type { Config } from "./config.js";

export const DEFAULT_CLI_TIMEOUT_MS = 600000;

export interface JsonLine {
  text: string;
  value: Record<string, unknown>;
}

export interface CliResult {
  code: number;
  lines: JsonLine[];
  skipped: string[];
  stderr: string;
  timedOut: boolean;
  /** 调用方取消了（options.signal）：子进程已经结束，lines 是取消之前交出来的那些。 */
  cancelled: boolean;
  spawnError: string | null;
}

export interface CliOptions {
  timeoutMs?: number | undefined;
  cwd?: string | undefined;
  /** MCP 客户端取消了这次工具调用：结束子进程。以前子进程会在后台一直跑到完（最长 10 分钟）。 */
  signal?: AbortSignal | undefined;
  /** stdout 每出一行完整的 JSON 就调一次（进度通知用）；返回值里的 lines 照样是全部。 */
  onLine?: ((line: JsonLine) => void) | undefined;
  /** Persist complete JSON lines immediately; partial results survive cancellation or process failure. */
  outputPath?: string | undefined;
  /** Batch callers can keep summaries in memory and leave row payloads only on disk. */
  retainRows?: boolean | undefined;
}

export function parseJsonLines(chunk: string): { lines: JsonLine[]; skipped: string[] } {
  const lines: JsonLine[] = [];
  const skipped: string[] = [];
  for (const raw of chunk.split(/\r?\n/)) {
    const text = raw.trim();
    if (!text) continue;
    if (text[0] !== "{" && text[0] !== "[") {
      skipped.push(text);
      continue;
    }
    let value: unknown;
    try {
      value = JSON.parse(text);
    } catch {
      skipped.push(text);
      continue;
    }
    if (!value || typeof value !== "object" || Array.isArray(value)) {
      skipped.push(text);
      continue;
    }
    lines.push({ text, value: value as Record<string, unknown> });
  }
  return { lines, skipped };
}

export function stderrTail(stderr: string): string {
  const lines = stderr.split(/\r?\n/).filter((l) => l.trim().length > 0);
  return lines.length > 0 ? (lines[lines.length - 1] as string) : "";
}

export function runCli(config: Config, argv: string[], options?: CliOptions): Promise<CliResult> {
  const exe = config.cli;
  if (!exe) {
    return Promise.resolve({
      code: -1,
      lines: [],
      skipped: [],
      stderr:
        "没有配置 LYFLOW_CLI：eval / perturb / diff_graphs 需要一个本地 lyflow 可执行文件的路径",
      timedOut: false,
      cancelled: false,
      spawnError: "LYFLOW_CLI 未配置",
    });
  }
  const timeoutMs = options?.timeoutMs ?? DEFAULT_CLI_TIMEOUT_MS;
  const env: NodeJS.ProcessEnv = { ...process.env };
  if (config.packs) env["LYFLOW_PACKS"] = config.packs;
  if (options?.outputPath) fs.writeFileSync(options.outputPath, "", "utf8");

  return new Promise<CliResult>((resolve) => {
    const child = spawn(exe, argv, {
      ...(options?.cwd ? { cwd: options.cwd } : {}),
      env,
      windowsHide: true,
    });
    const lines: JsonLine[] = [];
    const skipped: string[] = [];
    let stderr = "";
    let timedOut = false;
    let cancelled = false;
    let spawnError: string | null = null;
    // onLine 只看完整的行：一次 data 可能停在半行上，剩下的等下一次
    let pending = "";
    const accept = (chunk: string) => {
      const parsed = parseJsonLines(chunk);
      skipped.push(...parsed.skipped);
      if (skipped.length > 200) skipped.splice(0, skipped.length - 200);
      for (const line of parsed.lines) {
        if (options?.outputPath) fs.appendFileSync(options.outputPath, `${line.text}\n`, "utf8");
        if (options?.retainRows !== false || !["eval_row", "perturb_row"].includes(String(line.value["kind"]))) lines.push(line);
        options?.onLine?.(line);
      }
    };

    const timer = setTimeout(() => {
      timedOut = true;
      child.kill();
    }, timeoutMs);
    const onAbort = () => {
      cancelled = true;
      child.kill();
    };
    const signal = options?.signal;
    if (signal?.aborted) onAbort();
    else signal?.addEventListener("abort", onAbort, { once: true });

    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (c: string) => {
      pending += c;
      const cut = pending.lastIndexOf("\n");
      if (cut < 0) return;
      const complete = pending.slice(0, cut);
      pending = pending.slice(cut + 1);
      try { accept(complete); } catch (e) { spawnError = String(e); child.kill(); }
    });
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (c: string) => {
      stderr = (stderr + c).slice(-1024 * 1024);
    });
    child.on("error", (e) => {
      spawnError = e instanceof Error ? e.message : String(e);
    });
    child.on("close", (code) => {
      clearTimeout(timer);
      signal?.removeEventListener("abort", onAbort);
      try { if (pending.trim()) accept(pending); } catch (e) { spawnError = String(e); }
      resolve({
        code: spawnError !== null ? -1 : (code ?? -1),
        lines,
        skipped,
        stderr: spawnError !== null ? `起 ${exe} 失败：${spawnError}\n${stderr}` : stderr,
        timedOut,
        cancelled,
        spawnError,
      });
    });
  });
}
