import { spawn } from "node:child_process";

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
  spawnError: string | null;
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

export function runCli(
  config: Config,
  argv: string[],
  options?: { timeoutMs?: number | undefined; cwd?: string | undefined },
): Promise<CliResult> {
  const exe = config.cli;
  if (!exe) {
    return Promise.resolve({
      code: -1,
      lines: [],
      skipped: [],
      stderr:
        "没有配置 LYFLOW_CLI：eval / perturb / diff_graphs 需要一个本地 lyflow 可执行文件的路径",
      timedOut: false,
      spawnError: "LYFLOW_CLI 未配置",
    });
  }
  const timeoutMs = options?.timeoutMs ?? DEFAULT_CLI_TIMEOUT_MS;
  const env: NodeJS.ProcessEnv = { ...process.env };
  if (config.packs) env["LYFLOW_PACKS"] = config.packs;

  return new Promise<CliResult>((resolve) => {
    const child = spawn(exe, argv, {
      ...(options?.cwd ? { cwd: options.cwd } : {}),
      env,
      windowsHide: true,
    });
    let stdout = "";
    let stderr = "";
    let timedOut = false;
    let spawnError: string | null = null;

    const timer = setTimeout(() => {
      timedOut = true;
      child.kill();
    }, timeoutMs);

    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (c: string) => {
      stdout += c;
    });
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (c: string) => {
      stderr += c;
    });
    child.on("error", (e) => {
      spawnError = e instanceof Error ? e.message : String(e);
    });
    child.on("close", (code) => {
      clearTimeout(timer);
      const parsed = parseJsonLines(stdout);
      resolve({
        code: spawnError !== null ? -1 : (code ?? -1),
        lines: parsed.lines,
        skipped: parsed.skipped,
        stderr: spawnError !== null ? `起 ${exe} 失败：${spawnError}\n${stderr}` : stderr,
        timedOut,
        spawnError,
      });
    });
  });
}
