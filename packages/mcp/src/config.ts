import os from "node:os";
import path from "node:path";

export interface Config {
  httpBase: string;
  token: string | undefined;
  cli: string | undefined;
  packs: string | undefined;
  workDir: string;
}

function clean(value: string | undefined): string | undefined {
  const v = (value ?? "").trim();
  return v.length > 0 ? v : undefined;
}

export const MISSING_BASE =
  "缺 LYFLOW_HTTP_BASE。它是 /lyflow/* 契约的基址，例如 http://127.0.0.1:8787";

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const base = clean(env["LYFLOW_HTTP_BASE"]);
  if (!base) throw new Error(MISSING_BASE);
  return {
    httpBase: base.replace(/\/+$/, ""),
    token: clean(env["LYFLOW_HTTP_TOKEN"]),
    cli: clean(env["LYFLOW_CLI"]),
    packs: clean(env["LYFLOW_PACKS"]),
    workDir: clean(env["LYFLOW_WORK_DIR"]) ?? path.join(os.tmpdir(), "lyflow-mcp"),
  };
}
