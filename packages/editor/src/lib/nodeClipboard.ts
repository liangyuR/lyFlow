// 节点的复制粘贴（交互清单 P0 #12）也走系统剪贴板：一段带 kind 标记的 JSON。另一个窗口、关掉重开之后
// 照样粘得进来，也能整段贴进聊天、issue 里给人看。从文本编辑器复制来的整张图（GraphDoc 的 JSON）也认。
// 应用内的剪贴板（ui.clipboard）照旧留着：系统剪贴板读不到时用它。

import type { GraphDoc, GraphEdge, GraphNode } from "../types/graph";

export const NODE_CLIPBOARD_KIND = "lyflow.nodes";

export interface NodeClipboard {
  nodes: GraphNode[];
  edges: GraphDoc["edges"];
}

export function encodeNodeClipboard(clip: NodeClipboard): string {
  return JSON.stringify({ kind: NODE_CLIPBOARD_KIND, version: 1, nodes: clip.nodes, edges: clip.edges });
}

const isRecord = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

function isNode(v: unknown): v is GraphNode {
  return isRecord(v) && typeof v["id"] === "string" && typeof v["op"] === "string";
}

function isPortRef(v: unknown): boolean {
  return isRecord(v) && typeof v["node"] === "string" && typeof v["port"] === "string";
}

function isEdge(v: unknown): v is GraphEdge {
  return isRecord(v) && isPortRef(v["from"]) && isPortRef(v["to"]);
}

/** 剪贴板里的文字 → 要粘的节点与连线。认两种：encodeNodeClipboard 写的，与整张图（有 schemaVersion）。
 *  别的文字、没有一个像样节点的，返回 null。连线只留两端都在这批节点里的（与应用内复制同一个规则）。 */
export function decodeNodeClipboard(text: string): NodeClipboard | null {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return null;
  }
  if (!isRecord(value)) return null;
  const ours = value["kind"] === NODE_CLIPBOARD_KIND;
  const graphDoc = typeof value["schemaVersion"] === "number";
  if (!ours && !graphDoc) return null;
  const rawNodes = value["nodes"];
  const rawEdges = value["edges"] ?? [];
  if (!Array.isArray(rawNodes) || !Array.isArray(rawEdges)) return null;
  const nodes = rawNodes.filter(isNode);
  if (nodes.length === 0) return null;
  const ids = new Set(nodes.map((n) => n.id));
  const edges = rawEdges.filter(isEdge).filter((e) => ids.has(e.from.node) && ids.has(e.to.node));
  return { nodes, edges };
}
