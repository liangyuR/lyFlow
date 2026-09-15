import fs from "node:fs";
import path from "node:path";

import type { GraphDoc } from "./types.js";

export function checkStructure(doc: GraphDoc, where: string): void {
  const missing: string[] = [];
  if (typeof doc["schemaVersion"] !== "number") missing.push("schemaVersion");
  if (typeof doc["id"] !== "string") missing.push("id");
  if (!Array.isArray(doc.nodes)) missing.push("nodes");
  if (!Array.isArray(doc["edges"])) missing.push("edges");
  if (missing.length > 0) {
    throw new Error(
      `${where} 缺 GraphDoc 的必填字段：${missing.join("、")}（见 lyflow://schema/graph-doc）`,
    );
  }
  for (const [i, node] of doc.nodes.entries()) {
    if (!node || typeof node.id !== "string" || typeof node.op !== "string") {
      throw new Error(`${where} 的第 ${i} 个节点缺 id 或 op`);
    }
  }
}

export function readGraphDoc(file: string): GraphDoc {
  let text: string;
  try {
    text = fs.readFileSync(file, "utf8");
  } catch (e) {
    throw new Error(`读取 ${file} 失败：${e instanceof Error ? e.message : String(e)}`);
  }
  let doc: unknown;
  try {
    doc = JSON.parse(text);
  } catch (e) {
    throw new Error(`${file} 不是合法 JSON：${e instanceof Error ? e.message : String(e)}`);
  }
  if (!doc || typeof doc !== "object" || !Array.isArray((doc as GraphDoc).nodes)) {
    throw new Error(`${file} 不像 GraphDoc：缺 nodes 数组`);
  }
  return doc as GraphDoc;
}

export function applySet(doc: GraphDoc, set: Record<string, unknown>): GraphDoc {
  for (const [key, value] of Object.entries(set)) {
    const dot = key.lastIndexOf(".");
    if (dot <= 0 || dot === key.length - 1) {
      throw new Error(`set 的键写法是 <nodeId>.<param>，收到 ${key}`);
    }
    const nodeId = key.slice(0, dot);
    const param = key.slice(dot + 1);
    const node = doc.nodes.find((n) => n.id === nodeId);
    if (!node) throw new Error(`图里没有节点 ${nodeId}`);
    node.params = { ...(node.params ?? {}), [param]: value };
  }
  return doc;
}

export interface GraphInput {
  graph?: unknown;
  graphPath?: string | undefined;
  baseDir?: string | undefined;
  set?: Record<string, unknown> | undefined;
}

export interface ResolvedGraph {
  doc: GraphDoc;
  graphPath: string | null;
}

export function envelopeGraphPath(
  graphPath: string | undefined,
  baseDir: string | undefined,
): string | null {
  if (baseDir) return `${baseDir.replace(/[\\/]+$/, "")}/graph.lyflow.json`;
  if (!graphPath) return null;
  if (path.isAbsolute(graphPath)) return null;
  return graphPath.replace(/\\/g, "/");
}

export function resolveGraph(input: GraphInput): ResolvedGraph {
  const hasGraph = input.graph !== undefined && input.graph !== null;
  if (hasGraph && input.graphPath) {
    throw new Error("graph 与 graphPath 只能给一个");
  }
  let doc: GraphDoc;
  if (hasGraph) {
    const g = input.graph;
    if (!g || typeof g !== "object" || !Array.isArray((g as GraphDoc).nodes)) {
      throw new Error("graph 不像 GraphDoc：缺 nodes 数组");
    }
    doc = JSON.parse(JSON.stringify(g)) as GraphDoc;
  } else if (input.graphPath) {
    doc = readGraphDoc(input.graphPath);
  } else {
    throw new Error("给 graph（内联的 GraphDoc）或 graphPath（本地图文件）其中一个");
  }
  checkStructure(doc, hasGraph ? "graph" : (input.graphPath as string));
  if (input.set) applySet(doc, input.set);
  return { doc, graphPath: envelopeGraphPath(input.graphPath, input.baseDir) };
}
