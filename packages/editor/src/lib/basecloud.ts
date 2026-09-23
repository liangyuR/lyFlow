// 几何节点的底图：显示节点自己没有点云输出时，沿输入边往上游找**最近**的一片云当背景。
// 找到的 id 走 resolveOutput 展开成路径 id（F2），子图内外都能直接喂给 getOutputCloud。

import { levelOf, resolveOutput, pathPrefix, type SubPath } from "./subgraph";
import type { OutputStat } from "../types/execution";
import type { GraphDoc, GraphNode } from "../types/graph";
import { bundleKindOf, type BundleDesc, type OperatorDesc } from "../types/manifest";

/** 上游借来的那片云。 */
export interface BaseCloud {
  /** 出云节点在它所在层级里的本地 id。 */
  localId: string;
  /** 显示用的名字：节点标题优先，其次算子 label。 */
  label: string;
  /** 展开到叶子之后的路径 id + 端口，直接用于 getOutputCloud。 */
  resolved: { nodeId: string; port: string };
}

/** 该算子的第一个 PointCloud 输出端口。没有时再看 Bundle 输出里的点云字段，返回
 *  `<port>.<field>`（m8-plan L3 的寻址，取数函数原样认）：给了这次运行的输出统计就挑点最多的
 *  那个字段（ScanPair 里是合并云），没给就取声明里最后一个点云字段。都没有返回 null。 */
export function firstCloudPort(
  ops: ReadonlyMap<string, OperatorDesc>,
  opId: string,
  bundles?: readonly BundleDesc[],
  outputs?: readonly OutputStat[],
): string | null {
  const declared = ops.get(opId)?.outputs ?? [];
  for (const p of declared) {
    if (p.type === "PointCloud") return p.name;
  }
  for (const p of declared) {
    const kind = bundleKindOf(p.type);
    const desc = kind ? bundles?.find((b) => b.kind === kind) : undefined;
    const fields = (desc?.fields ?? []).filter((f) => f.type === "PointCloud");
    if (fields.length === 0) continue;
    let best = `${p.name}.${fields[fields.length - 1]!.name}`;
    let most = -1;
    for (const f of fields) {
      const stat = outputs?.find((o) => o.port === `${p.name}.${f.name}`);
      if (stat && stat.elementCount > most) {
        most = stat.elementCount;
        best = `${p.name}.${f.name}`;
      }
    }
    return best;
  }
  return null;
}

/** 一次搜索里的位置：哪一层、那一层的哪个节点。 */
interface Frame {
  path: SubPath;
  id: string;
}

function frameKey(frame: Frame): string {
  return pathPrefix(frame.path) + frame.id;
}

function nodeAt(doc: GraphDoc, path: SubPath, id: string): GraphNode | undefined {
  return levelOf(doc, path).nodes.find((n) => n.id === id);
}

function titleOf(node: GraphNode, ops: ReadonlyMap<string, OperatorDesc>): string {
  return node.ui?.title ?? ops.get(node.op)?.label ?? node.id;
}

/** 一个节点的直接上游，**按输入端口的声明顺序**排。多个输入时靠这个顺序定优先级。 */
function upstreamOf(
  doc: GraphDoc,
  path: SubPath,
  node: GraphNode,
  ops: ReadonlyMap<string, OperatorDesc>,
): Frame[] {
  const out: Frame[] = [];
  const level = levelOf(doc, path);
  for (const port of ops.get(node.op)?.inputs ?? []) {
    let linked = false;
    for (const e of level.edges) {
      if (e.to.node === node.id && e.to.port === port.name) {
        out.push({ path, id: e.from.node });
        linked = true;
      }
    }
    if (linked || path.length === 0) continue;
    // 这个入口在子图里没人接，那它是从外面喂进来的：翻过边界接着往上找。
    const seg = path[path.length - 1]!;
    const def = doc.subgraphs?.[seg.subgraphId];
    const entry = def?.inputs.find((i) =>
      i.to.some((t) => t.node === node.id && t.port === port.name),
    );
    if (!entry) continue;
    const parent = path.slice(0, -1);
    for (const e of levelOf(doc, parent).edges) {
      if (e.to.node === seg.nodeId && e.to.port === entry.name) {
        out.push({ path: parent, id: e.from.node });
      }
    }
  }
  return out;
}

/** 从 localId 出发，广度优先找最近的有点云输出的上游节点。找不到返回 null。 */
export function findBaseCloud(
  doc: GraphDoc,
  path: SubPath,
  localId: string,
  ops: ReadonlyMap<string, OperatorDesc>,
  bundles?: readonly BundleDesc[],
): BaseCloud | null {
  const start = nodeAt(doc, path, localId);
  if (!start) return null;

  const seen = new Set<string>([frameKey({ path, id: localId })]);
  const queue: Frame[] = [];
  const push = (frames: Frame[]) => {
    for (const f of frames) {
      const key = frameKey(f);
      if (seen.has(key)) continue;
      seen.add(key);
      queue.push(f);
    }
  };
  push(upstreamOf(doc, path, start, ops));

  // 队列是先进先出，所以先耗完同一层深度才往上走一层 —— 「最近的」由此保证，
  // 同深度之间的先后则来自 upstreamOf 的端口声明顺序。
  while (queue.length > 0) {
    const frame = queue.shift()!;
    const node = nodeAt(doc, frame.path, frame.id);
    if (!node) continue;
    const port = firstCloudPort(ops, node.op, bundles);
    // 解不开的（库算子的定义在库文件里）不算数，继续往上找
    const resolved = port ? resolveOutput(doc, frame.path, node.id, port) : null;
    if (port && resolved) {
      return { localId: node.id, label: titleOf(node, ops), resolved };
    }
    push(upstreamOf(doc, frame.path, node, ops));
  }
  return null;
}
