import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const MARKER = path.join("schema", "operator-manifest.schema.json");

export function findRepoRoot(start?: string): string | null {
  let dir = start ?? path.dirname(fileURLToPath(import.meta.url));
  for (let i = 0; i < 8; i += 1) {
    if (fs.existsSync(path.join(dir, MARKER))) return dir;
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return null;
}

export const NO_REPO =
  "找不到仓库里的 schema/ 与 docs/：这一份 @lyflow/mcp 是从仓库外的副本跑起来的，" +
  "本地文件类的 resource 拿不到。工具（list_operators / run_graph 等）不受影响。";

export function readRepoFile(root: string | null, relative: string): string {
  if (!root) throw new Error(NO_REPO);
  const file = path.join(root, relative);
  if (!fs.existsSync(file)) throw new Error(`仓库里没有 ${relative}（找的是 ${file}）`);
  return fs.readFileSync(file, "utf8");
}

export function listPacks(root: string | null): string[] {
  if (!root) return [];
  const dir = path.join(root, "packs");
  if (!fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir, { withFileTypes: true })
    .filter((e) => e.isDirectory() && fs.existsSync(path.join(dir, e.name, "README.md")))
    .map((e) => e.name)
    .sort();
}
