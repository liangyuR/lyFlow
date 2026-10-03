// 节点的复制粘贴（交互清单 P0 #12）也走系统剪贴板：一段带 kind 标记的 JSON。另一个窗口、关掉重开之后
// 照样粘得进来，也能整段贴进聊天、issue 里给人看。从文本编辑器复制来的整张图（GraphDoc 的 JSON）也认。
// 应用内的剪贴板（ui.clipboard）照旧留着：系统剪贴板读不到时用它。
// 复制的里面有子图节点：它用到的子图定义一起带上（subgraphs），粘进另一张图才认得出来。

import type { GraphDoc, GraphEdge, GraphNode, SubgraphDef } from "../types/graph";

export const NODE_CLIPBOARD_KIND = "lyflow.nodes";

export interface NodeClipboard {
  nodes: GraphNode[];
  edges: GraphDoc["edges"];
  /** 这些节点（连同里面）用到的子图定义。没有子图节点时不写。 */
  subgraphs?: Record<string, SubgraphDef>;
}

export function encodeNodeClipboard(clip: NodeClipboard): string {
  const subgraphs = clip.subgraphs && Object.keys(clip.subgraphs).length > 0 ? { subgraphs: clip.subgraphs } : {};
  return JSON.stringify({ kind: NODE_CLIPBOARD_KIND, version: 1, nodes: clip.nodes, edges: clip.edges, ...subgraphs });
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

/** 子图定义：至少得有节点与连线两张表；入口、出口、参数缺了按空的补上。不像样的那一份丢掉。 */
function subgraphsOf(v: unknown): Record<string, SubgraphDef> | null {
  if (!isRecord(v)) return null;
  const out: Record<string, SubgraphDef> = {};
  for (const [id, def] of Object.entries(v)) {
    if (!isRecord(def) || !Array.isArray(def["nodes"]) || !Array.isArray(def["edges"])) continue;
    out[id] = { inputs: [], outputs: [], params: [], ...def } as unknown as SubgraphDef;
  }
  return Object.keys(out).length > 0 ? out : null;
}

/** 剪贴板里的文字 → 要粘的节点与连线（与子图定义）。认两种：encodeNodeClipboard 写的，与整张图（有 schemaVersion）。
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
  const subgraphs = subgraphsOf(value["subgraphs"]);
  return { nodes, edges, ...(subgraphs ? { subgraphs } : {}) };
}
