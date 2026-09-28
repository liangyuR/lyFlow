// 主预览的取数（从 Viewer3D 抽出来，docs/compare-plan.md §3.2）：给一个节点（路径 + 局部 id），
// 订阅它的状态与输出统计、决定显示点云还是值、取点云（自己的，或沿输入边借上游的当底图）。
// 普通模式调一次（A）；对比模式调两次（A、B）。B 冻结时给 frozen，原样返回快照，不订阅结果、不发请求。

import { useEffect, useMemo, useState } from "react";

import { findBaseCloud, firstCloudPort, type BaseCloud } from "../lib/basecloud";
import { cacheKey, cloudCache, dropOtherRuns, putCache } from "../lib/cloudCache";
import { augmentOperators, levelOf, resolveOutput } from "../lib/subgraph";
import { viewerContentFor, type ViewerContent } from "../lib/viewRule";
import { transport } from "../transport";
import type { CompareSlot, CompareSnapshot } from "../store/compare";
import { aggregatedNodes, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import { decodeCloud, type CloudPayload, type NodeState, type OutputStat } from "../types/execution";
import type { GraphNode } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

/** 当前展示的东西。永远一起换：拆成几个 useState 的话，切换节点时会出现「标题是新节点、
 *  点云还是旧节点」的中间态 —— 肉眼看不见，但验收脚本会稳定读到它。 */
export interface Display {
  nodeId: string | null;
  /** 这片云属于哪一次运行。验收脚本用它量「事件到渲染」的延迟。 */
  runId: string | null;
  cloud: CloudPayload | null;
  status: string | null;
  /** 这片云是从上游借来的底图时，借的是谁。自己有云时为 null。 */
  base: BaseCloud | null;
  /** 这片云是节点自己哪个输出端口的（`<port>.<field>` 也算）。借来的底图、没有云时为 null。 */
  port: string | null;
  /** 取数那一刻这次运行是不是预览运行（ADR-0011）。 */
  preview: boolean;
}

export interface ViewerSourceInput {
  /** null = 没有节点可看。 */
  slot: CompareSlot | null;
  /** 没有节点时的提示（「选中一个节点查看它的输出」/「选中了多个节点」）。 */
  idleText: string;
  maxPoints: number;
  /** 手动选的内容（ui.viewerContentPick 里对这个节点的那一条）；null = 按类型自动。 */
  pick: ViewerContent | null;
  /** 给了就原样返回快照：不订阅这次运行之后的结果，也不再向后端要东西（C3）。 */
  frozen: CompareSnapshot | null;
}

export interface ViewerSource {
  display: Display;
  loading: boolean;
  node: GraphNode | undefined;
  op: OperatorDesc | undefined;
  state: NodeState | undefined;
  outputs: readonly OutputStat[] | undefined;
  /** 实际显示的内容：手动选的优先，否则按类型自动。 */
  content: ViewerContent;
  autoContent: ViewerContent;
}

export function useViewerSource({ slot, idleText, maxPoints, pick, frozen }: ViewerSourceInput): ViewerSource {
  const path = slot?.path;
  const nodeId = slot?.nodeId ?? null;
  const doc = useGraphStore((s) => s.doc);
  const node = useMemo(
    () => (path && nodeId ? levelOf(doc, path).nodes.find((n) => n.id === nodeId) : undefined),
    [doc, path, nodeId],
  );
  const runId = useExecutionStore((s) => s.runId);
  const runStatus = useExecutionStore((s) => s.runStatus);
  const isPreview = useExecutionStore((s) => s.preview);
  const previewMaxPoints = useUiStore((s) => s.previewMaxPoints);
  // 只订阅**这一个节点的状态字符串**，不要订阅整张 nodes Map ——
  // 那张 Map 每来一条事件就是新引用，会排起一队几十兆的 IPC（见 README「踩过的坑」）。
  const liveState = useExecutionStore((s) =>
    path && nodeId ? aggregatedNodes(path, s.nodes).get(nodeId)?.state : undefined,
  );
  // 叠画用的非点云输出（G7）。stats 是事件里那一份，引用稳定，不会每帧新建。
  const liveOutputs = useExecutionStore((s) =>
    path && nodeId ? aggregatedNodes(path, s.nodes).get(nodeId)?.stats?.outputs : undefined,
  );
  const operatorsById = useManifestStore((s) => s.operatorsById);
  const ops = useMemo(
    () => augmentOperators(operatorsById, doc.subgraphs),
    [operatorsById, doc.subgraphs],
  );
  const bundles = useManifestStore((s) => s.bundle?.bundles);
  const op = node ? ops.get(node.op) : undefined;

  const outputs = frozen ? frozen.outputs : liveOutputs;
  // 显示点云场景还是值的表格（lib/viewRule）。类型取这次运行的实际类型，没跑过就用声明的 ——
  // 不必等运行结束才知道该显示什么。
  const autoContent = useMemo<ViewerContent>(
    () =>
      op
        ? viewerContentFor(
            op.outputs.map((o) => outputs?.find((st) => st.port === o.name)?.type ?? o.type),
            bundles,
          )
        : "cloud",
    [op, outputs, bundles],
  );
  const content: ViewerContent = frozen ? frozen.content : (pick ?? autoContent);

  const [display, setDisplay] = useState<Display>({
    nodeId: null,
    runId: null,
    cloud: null,
    status: "未运行",
    base: null,
    port: null,
    preview: false,
  });
  const [loading, setLoading] = useState(false);

  // -- 取点云 ---------------------------------------------------------------
  useEffect(() => {
    if (frozen) return;
    let cancelled = false;
    const show = (
      status: string | null,
      payload: CloudPayload | null = null,
      base: BaseCloud | null = null,
      port: string | null = null,
    ) => {
      if (cancelled) return;
      setDisplay({ nodeId, runId: runId ?? null, cloud: payload, status, base, port, preview: isPreview });
    };

    if (!node || !path) {
      setLoading(false);
      show(idleText);
      return;
    }
    if (!runId || runStatus === "idle") {
      setLoading(false);
      show("未运行");
      return;
    }
    if (liveState === "error") {
      setLoading(false);
      show("该节点运行出错");
      return;
    }
    if (liveState !== "done" && liveState !== "skipped") {
      setLoading(false);
      show(liveState === "running" ? "正在计算…" : "该节点尚未产出结果");
      return;
    }
    // 显示值时不取云：值就在事件里（stats.outputs），底图也用不上
    if (content === "value") {
      setLoading(false);
      show(null);
      return;
    }
    // 自己有云就用自己的；没有就沿输入边往上游借最近的一片当底图，几何叠在它上面 ——
    // 只输出 Box2D/Line2D 的节点若显示成空白，用户就看不出框压在剖面的哪里。
    // Bundle 里的点云字段也算「自己的云」（`<port>.<field>`，m8-plan L3）。
    const port = firstCloudPort(ops, node.op, bundles, liveOutputs);
    let base: BaseCloud | null = null;
    // 子图节点的结果在内部那个叶子上，按路径查结果仓（F2）
    let resolved = port ? resolveOutput(doc, path, node.id, port) : null;
    if (port && !resolved) {
      setLoading(false);
      show("这个算子的内部结果查不到（库算子的定义在库文件里）");
      return;
    }
    if (!port) {
      base = findBaseCloud(doc, path, node.id, ops, bundles);
      resolved = base?.resolved ?? null;
    }
    if (!resolved) {
      setLoading(false);
      show("该节点无点云输出，上游也没有可当底图的点云");
      return;
    }

    dropOtherRuns(runId);
    const key = cacheKey(runId, resolved.nodeId, resolved.port, maxPoints);
    const hit = cloudCache.get(key);
    if (hit) {
      // 命中也要 delete+set 一下，否则 LRU 的「最近使用」永远不更新
      putCache(key, hit);
      setLoading(false);
      show(hit.pointCount === 0 ? "该节点的点云是空的" : null, hit, base, port);
      return;
    }

    setLoading(true);
    void (async () => {
      try {
        // 预览时没必要拉超过预览点数的量：那条路径上本来就不会有更多点
        const cap = isPreview ? Math.min(maxPoints, previewMaxPoints) : maxPoints;
        const buffer = await transport.getOutputCloud(runId, resolved.nodeId, resolved.port, cap);
        if (cancelled) return;
        const payload = decodeCloud(buffer);
        putCache(key, payload);
        show(payload.pointCount === 0 ? "该节点的点云是空的" : null, payload, base, port);
      } catch (e) {
        show(e instanceof Error ? e.message : String(e));
      } finally {
        // 这里**不看 cancelled**：切换节点会作废旧请求，若那时不放下 loading，
        // 而新节点又不需要发请求（比如没有点云输出），界面就永远停在「正在取点云…」。
        setLoading(false);
      }
    })();

    return () => {
      cancelled = true;
    };
  }, [frozen, node, nodeId, idleText, runId, runStatus, liveState, maxPoints, doc, path,
      isPreview, previewMaxPoints, ops, bundles, liveOutputs, content]);

  const frozenDisplay = useMemo<Display | null>(
    () =>
      frozen
        ? {
            nodeId,
            runId: frozen.runId,
            cloud: frozen.cloud,
            status: null,
            base: frozen.base,
            port: frozen.cloudPort,
            preview: frozen.preview,
          }
        : null,
    [frozen, nodeId],
  );

  return {
    display: frozenDisplay ?? display,
    loading: frozen ? false : loading,
    node,
    op,
    state: frozen ? "done" : liveState,
    outputs,
    content,
    autoContent,
  };
}
