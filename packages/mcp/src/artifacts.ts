import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import readline from "node:readline";

import { projectValue, type Projection } from "./summary.js";

export interface ArtifactRef {
  artifactId: string;
  path: string;
  kind: "json" | "jsonl";
  bytes: number;
  sha256: string;
}

export class ArtifactStore {
  readonly dir: string;
  constructor(workDir: string) { this.dir = path.resolve(workDir, "artifacts"); }

  put(value: unknown): ArtifactRef {
    fs.mkdirSync(this.dir, { recursive: true });
    const id = crypto.randomUUID();
    const file = path.join(this.dir, `${id}.json`);
    fs.writeFileSync(file, JSON.stringify(value), { flag: "wx", encoding: "utf8" });
    return this.register(file, "json", id);
  }

  register(file: string, kind: "json" | "jsonl", id = crypto.randomUUID()): ArtifactRef {
    fs.mkdirSync(this.dir, { recursive: true });
    let resolved = fs.realpathSync(file);
    // References are immutable even when a cancelled evaluation later resumes.
    if (path.dirname(resolved) !== this.dir) {
      const copy = path.join(this.dir,`${id}.${kind}`); fs.copyFileSync(resolved,copy); resolved = fs.realpathSync(copy);
    }
    const digest = crypto.createHash("sha256");
    const fd = fs.openSync(resolved, "r");
    const buf = Buffer.alloc(64 * 1024);
    try { for (let n; (n = fs.readSync(fd, buf)) > 0;) digest.update(buf.subarray(0, n)); }
    finally { fs.closeSync(fd); }
    const ref: ArtifactRef = { artifactId: id, path: resolved, kind, bytes: fs.statSync(resolved).size, sha256: digest.digest("hex") };
    fs.writeFileSync(path.join(this.dir, `${id}.meta.json`), JSON.stringify(ref), { flag: "wx" });
    return ref;
  }

  async read(id: string, options: Projection = {}): Promise<Record<string, unknown>> {
    if (!/^[0-9a-f-]{36}$/.test(id)) throw new Error("不是有效的 artifactId");
    const ref = JSON.parse(fs.readFileSync(path.join(this.dir, `${id}.meta.json`), "utf8")) as ArtifactRef;
    if (ref.kind === "json") return { ...ref, ...projectValue(JSON.parse(fs.readFileSync(ref.path, "utf8")), options) };
    const offset = options.offset ?? 0;
    const limit = options.limit ?? 20;
    const rows: unknown[] = [];
    let total = 0;
    const input = readline.createInterface({ input: fs.createReadStream(ref.path), crlfDelay: Infinity });
    for await (const line of input) {
      if (!line.trim()) continue;
      if (total >= offset && rows.length < limit) rows.push(JSON.parse(line));
      total += 1;
    }
    return { ...ref, rows: rows.map((r) => projectValue(r, { fields: options.fields, limit: 8 }).value), offset, total, truncated: offset + rows.length < total };
  }
}
