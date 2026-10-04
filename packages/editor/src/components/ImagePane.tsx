// 主预览的图像模式（docs/image-plan.md 阶段 3）：
// - 节点输出图像 → 画那张图；
// - 节点输出的是像素几何（找圆、区域统计的框）→ 画它**输入**的那张图，几何叠在上面；
// - 节点带像素框参数（unit = px 的 roi，例如 image.crop）→ 画输入的那张图，框可拖、四角可拉（写回走 setParam，一次拖动一条撤销）。
// 画布、取数、缩放平移、悬停读数都是 ImageCanvas（与连线查看器共用）；这里只管「画哪张图」与叠画。
import { useEffect, useMemo, useRef, useState } from "react";

import { effectiveParams } from "../lib/params";
import { imageRoiParams } from "../lib/roiFrames";
import { levelOf, resolveOutput } from "../lib/subgraph";
import { transport } from "../transport";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import type { NodeState, OutputStat, OutputValue } from "../types/execution";
import type { GraphDoc, GraphNode } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";
import type { SubPath } from "../lib/subgraph";
import { ImageCanvas, type Depth, type ImageOverlayView } from "./ImageCanvas";
import { ViewerStatus } from "./ViewerStatus";

/** 与点云拖框同一组颜色（Viewer3D 的 ROI_COLORS）。 */
const ROI_COLORS = ["#34d399", "#f472b6", "#60a5fa", "#fbbf24", "#a78bfa", "#f87171"];
const CORNERS = ["nw", "ne", "sw", "se"] as const;
type Mode = "move" | (typeof CORNERS)[number];

interface Source {
  nodeId: string;
  port: string;
  /** 画的是节点自己的输出，还是它输入的那张图（叠画几何 / 拖框时）。 */
  from: "output" | "input";
}

/** 画哪张图。节点带像素框、或只输出几何时，画的是它 Image 输入上游的那张图。 */
function sourceOf(
  doc: GraphDoc,
  path: SubPath,
  node: GraphNode,
  op: OperatorDesc,
  wantInput: boolean,
): Source | null {
  const outPort = op.outputs.find((p) => p.type === "Image");
  if (outPort && !wantInput) {
    const r = resolveOutput(doc, path, node.id, outPort.name);
    return r ? { ...r, from: "output" } : null;
  }
  const inPort = op.inputs.find((p) => p.type === "Image");
  if (!inPort) return null;
  const edge = levelOf(doc, path).edges.find((e) => e.to.node === node.id && e.to.port === inPort.name);
  if (!edge) return null;
  const r = resolveOutput(doc, path, edge.from.node, edge.from.port);
  return r ? { ...r, from: "input" } : null;
}

interface Meta {
  key: string;
  width: number;
  height: number;
  channels: number;
  depth: Depth;
  /** 预览缩小过的图是 > 1（ADR-0028） */
  scale: number;
}

export interface ImagePaneProps {
  doc: GraphDoc;
  path: SubPath;
  node: GraphNode;
  op: OperatorDesc;
  /** 带图参数绑定后的节点（框按有效值画，与点云拖框同一口径）。 */
  roiNode: GraphNode;
  runId: string | null;
  /** 主预览此刻的状态文字（未运行 / 正在计算… / 出错……）；null = 这个节点这次运行的结果齐了。 */
  status: string | null;
  /** 节点这次运行的状态。画的是输入那张图时，节点自己出错不妨碍画图（见下）。 */
  nodeState: NodeState | undefined;
  outputs: readonly OutputStat[] | undefined;
  /** 空态里的「运行到此节点」。不给就不列。 */
  onRunToNode?: ((nodeId: string) => void) | undefined;
}

export function ImagePane({ doc, path, node, op, roiNode, runId, status, nodeState, outputs, onRunToNode }: ImagePaneProps) {
  const rois = useMemo(() => imageRoiParams(op, roiNode), [op, roiNode]);
  const shapes = useMemo(() => (outputs ?? []).filter((o) => o.value?.unit === "px"), [outputs]);
  // 有框要拖、或者要叠几何而自己没有图像输出时，画输入的那张图
  const wantInput = rois.length > 0 || !op.outputs.some((p) => p.type === "Image");
  const src = useMemo(() => sourceOf(doc, path, node, op, wantInput), [doc, path, node, op, wantInput]);
  const srcKey = src ? `${src.nodeId}|${src.port}` : "";
  const typesByName = useManifestStore((s) => s.typesByName);

  // 运行中（自动运行、拖参数时的预览运行）状态会短暂变成「正在计算…」：这时继续画上一次取成功的那张图，
  // 画布不卸掉 —— 否则每跑一次放大的位置就丢了，拖框拖到一半也会断。跑完换新结果，视角不动（ImageCanvas）
  const lastGood = useRef<{ runId: string; srcKey: string } | null>(null);
  // 画的是输入那张图时，节点自己出错（找圆找不到、裁剪框落在图外）照样画：这正是要看输入、把框拖回来的时候。
  // 上游没跑出来的话取元信息会失败，界面照实说（review 修正，PR #1）
  const ready = status === null || (src?.from === "input" && nodeState === "error");
  if (ready && runId && srcKey) lastGood.current = { runId, srcKey };
  const shownRunId =
    ready ? runId : lastGood.current && lastGood.current.srcKey === srcKey ? lastGood.current.runId : null;

  // 尺寸与位深：结果仓里那个输出的元信息（上游节点的 stats 不一定在当前层，按 runId 问一次最稳）
  const [meta, setMeta] = useState<Meta | null>(null);
  const [metaError, setMetaError] = useState<string | null>(null);
  const metaKey = src && shownRunId ? `${shownRunId}|${srcKey}` : "";
  /** 同一个节点、端口的旧元信息（只是换了 runId）：新的还没到时先顶着。 */
  const sameSource = (key: string) => srcKey !== "" && key.slice(key.indexOf("|") + 1) === srcKey;
  useEffect(() => {
    if (!src || !shownRunId) {
      setMetaError(null);
      return;
    }
    let cancelled = false;
    void (async () => {
      try {
        const infos = await transport.getOutputInfo(shownRunId, src.nodeId);
        const info = infos.find((o) => o.port === src.port);
        const v: OutputValue | undefined = info?.value;
        if (cancelled) return;
        if (!info || info.type !== "Image" || typeof v?.width !== "number") {
          setMeta(null);
          setMetaError(src.from === "input" ? "输入那张图还没有结果（上游没跑到）" : "这个输出不是图像");
          return;
        }
        setMetaError(null);
        setMeta({
          key: metaKey,
          width: v.width,
          height: v.height ?? 0,
          channels: v.channels ?? 0,
          depth: v.depth ?? "u8",
          scale: typeof v.scale === "number" && v.scale > 1 ? v.scale : 1,
        });
      } catch (e) {
        if (cancelled) return;
        setMeta((prev) => (prev && sameSource(prev.key) ? prev : null));
        setMetaError(e instanceof Error ? e.message : String(e));
      }
    })();
    return () => {
      cancelled = true;
    };
    // src 每次渲染可能是新对象；它的内容已经在 metaKey 里。节点状态变了也再问一次：同一个 runId
    // 先前问的时候结果还没到（取不到就停在「不是图像」），跑完之后 metaKey 不变、不重问就一直停在那（review 第二轮）
  }, [metaKey, ready, nodeState]);

  // 说的是节点运行状态（status 来自 useViewerSource）时跟着给下一步；没图、取图出错这些不给
  const centered = (text: string, fromRun: boolean) => (
    <ViewerStatus text={text} nodeId={fromRun ? node.id : null} onRunToNode={onRunToNode} />
  );
  if (!src) {
    return centered(
      status ?? (op.inputs.some((p) => p.type === "Image") ? "图像输入没接上，没有图可画" : "这个节点没有图像"),
      status !== null,
    );
  }
  const m = meta && (meta.key === metaKey || sameSource(meta.key)) ? meta : null;
  if (!m) return centered(status ?? metaError ?? (shownRunId ? "正在取图像…" : "未运行"), status !== null);

  const colorOf = (type: string) => typesByName.get(type)?.color ?? "#fbbf24";
  // 取图用元信息所属的那一次运行（review 修正）：新一次运行的元信息还没到时，尺寸与比例还是上一次的 ——
  // 拿新 runId 配旧尺寸，预览 ↔ 正式之间切换时取到的是另一种尺度的图，画面重新适配、叠画偏一倍
  const imageRun = m.key.slice(0, m.key.indexOf("|"));
  return (
    <div
      className="viewer__image"
      data-testid="viewer-image-pane"
      data-source={src.from}
      data-source-node={src.nodeId}
      data-shapes={shapes.length}
      data-rois={rois.length}
      data-showing-run={shownRunId ?? ""}
    >
      <ImageCanvas
        runId={imageRun}
        nodeId={src.nodeId}
        port={src.port}
        fullW={m.width}
        fullH={m.height}
        channels={m.channels}
        depth={m.depth}
        pixelScale={m.scale}
        testid="viewer-image"
        overlay={(view) => (
          <>
            <ShapeOverlay view={view} shapes={shapes} colorOf={colorOf} />
            {rois.length > 0 && (
              <RoiOverlay view={view} nodeId={node.id} op={op} roiNode={roiNode} rois={rois} />
            )}
          </>
        )}
      />
      {status !== null && (
        // 画面是上一次的结果：状态缩在角上，不盖住图
        <div className="viewer__empty viewer__empty--corner" data-testid="viewer3d-status">
          {status}
        </div>
      )}
    </div>
  );
}

// ------------------------------------------------------------ 像素几何

function ShapeOverlay({
  view,
  shapes,
  colorOf,
}: {
  view: ImageOverlayView;
  shapes: readonly OutputStat[];
  colorOf: (type: string) => string;
}) {
  const [W, H] = [view.fullWidth, view.fullHeight];
  return (
    <svg className="viewer-image__shapes" data-testid="viewer-image-shapes">
      {shapes.map((o) => {
        const v = o.value!;
        const color = colorOf(o.type);
        const common = { stroke: color, fill: "none", strokeWidth: 2, "data-port": o.port, "data-kind": v.kind };
        if (v.kind === "Box2D" && Array.isArray(v.min) && Array.isArray(v.max)) {
          const [x0, y0] = view.toStage(v.min[0], v.min[1]);
          const [x1, y1] = view.toStage(v.max[0], v.max[1]);
          return <rect key={o.port} {...common} x={x0} y={y0} width={x1 - x0} height={y1 - y0} />;
        }
        if (v.kind === "Circle2D" && v.center) {
          const [cx, cy] = view.toStage(v.center[0], v.center[1]);
          return <circle key={o.port} {...common} cx={cx} cy={cy} r={(v.radius ?? 0) * view.scale} />;
        }
        if (v.kind === "Point2D" && v.p) {
          const [x, y] = view.toStage(v.p[0], v.p[1]);
          return (
            <path key={o.port} {...common} d={`M${x - 6},${y}H${x + 6}M${x},${y - 6}V${y + 6}`} />
          );
        }
        if (v.kind === "Line2D" && v.point && v.dir) {
          // 没有端点就画一条穿过整张图的线
          const [a, b] = v.hasSegment && v.start && v.end
            ? [v.start, v.end]
            : [
                [v.point[0] - v.dir[0] * (W + H), v.point[1] - v.dir[1] * (W + H)],
                [v.point[0] + v.dir[0] * (W + H), v.point[1] + v.dir[1] * (W + H)],
              ];
          const [x0, y0] = view.toStage(a[0]!, a[1]!);
          const [x1, y1] = view.toStage(b[0]!, b[1]!);
          return <line key={o.port} {...common} x1={x0} y1={y0} x2={x1} y2={y1} />;
        }
        return null;
      })}
    </svg>
  );
}

// ------------------------------------------------------------ 可拖的像素框

function RoiOverlay({
  view,
  nodeId,
  op,
  roiNode,
  rois,
}: {
  view: ImageOverlayView;
  nodeId: string;
  op: OperatorDesc;
  roiNode: GraphNode;
  rois: ReturnType<typeof imageRoiParams>;
}) {
  const eff = effectiveParams(op, roiNode);
  const drag = useRef<{
    param: string;
    label: string;
    mode: Mode;
    start: [number, number];
    rect: [number, number, number, number];
  } | null>(null);
  const [active, setActive] = useState<string | null>(null);

  const down = (e: React.PointerEvent<HTMLElement>, param: string, label: string, rect: number[], mode: Mode) => {
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation(); // 不让画布开始平移
    e.currentTarget.setPointerCapture(e.pointerId);
    setActive(param);
    useGraphStore.getState().begin();
    drag.current = {
      param,
      label,
      mode,
      start: view.toImage(e.clientX, e.clientY),
      rect: [rect[0]!, rect[1]!, rect[2]!, rect[3]!],
    };
  };
  const move = (e: React.PointerEvent<HTMLElement>) => {
    const d = drag.current;
    if (!d) return;
    e.stopPropagation();
    const [px, py] = view.toImage(e.clientX, e.clientY);
    const dx = px - d.start[0];
    const dy = py - d.start[1];
    let [x0, y0, x1, y1] = d.rect;
    if (d.mode === "move") {
      x0 += dx;
      x1 += dx;
      y0 += dy;
      y1 += dy;
    } else {
      // 像素坐标 y 向下：n 是上边（y0），s 是下边（y1）
      if (d.mode === "nw" || d.mode === "sw") x0 += dx;
      else x1 += dx;
      if (d.mode === "nw" || d.mode === "ne") y0 += dy;
      else y1 += dy;
    }
    // 落到整像素、夹在图内（框的右下角可以等于宽高：右下角不含）
    const cx = (v: number) => Math.round(Math.min(Math.max(v, 0), view.fullWidth));
    const cy = (v: number) => Math.round(Math.min(Math.max(v, 0), view.fullHeight));
    const next = [cx(Math.min(x0, x1)), cy(Math.min(y0, y1)), cx(Math.max(x0, x1)), cy(Math.max(y0, y1))];
    useGraphStore.getState().setParam(nodeId, d.param, next);
  };
  const up = (e: React.PointerEvent<HTMLElement>) => {
    const d = drag.current;
    if (!d) return;
    drag.current = null;
    if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
    useGraphStore.getState().commit(`拖动 ${d.label}`);
  };

  return (
    <div className="roi-layer viewer-image__rois" data-testid="viewer-image-rois" data-node={nodeId}>
      {rois.map((p, i) => {
        const raw = eff[p.name];
        const r = Array.isArray(raw) && raw.length === 4 ? raw.map(Number) : [0, 0, 0, 0];
        const [x0, y0] = view.toStage(r[0]!, r[1]!);
        const [x1, y1] = view.toStage(r[2]!, r[3]!);
        const label = p.label ?? p.name;
        return (
          <div
            key={p.name}
            className={`roi-box${active === p.name ? " is-active" : ""}`}
            style={{
              ["--roi-color" as string]: ROI_COLORS[i % ROI_COLORS.length],
              left: x0,
              top: y0,
              width: Math.max(2, x1 - x0),
              height: Math.max(2, y1 - y0),
            }}
            data-testid={`viewer-image-roi-${p.name}`}
            data-roi={r.join(",")}
            onPointerDown={(e) => down(e, p.name, label, r, "move")}
            onPointerMove={move}
            onPointerUp={up}
            onPointerCancel={up}
            title={`${label}：拖框身平移，拖四角拉伸（像素）`}
          >
            <span className="roi-box__label">{label}</span>
            {CORNERS.map((c) => (
              <span
                key={c}
                className={`roi-box__handle roi-box__handle--${c}`}
                data-testid={`viewer-image-roi-${p.name}-${c}`}
                onPointerDown={(e) => down(e, p.name, label, r, c)}
                onPointerMove={move}
                onPointerUp={up}
                onPointerCancel={up}
              />
            ))}
          </div>
        );
      })}
    </div>
  );
}
