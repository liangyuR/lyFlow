// 3D 点云预览（交互清单 P1 #30）。three.js 随包打、**不走 CDN**：桌面应用断网也得能用。
// 点云走二进制 IPC，`decodeCloud` 给的 Float32Array 是缓冲上的**视图**，全程零拷贝（ADR-0006）。

import { useEffect, useMemo, useRef, useState } from "react";
import * as THREE from "three";

import {
  boundsAttr,
  buildPoints,
  createScene,
  dataRangeOf,
  fitToBounds,
  overlayBoundsOf,
  paintPoints,
  round3,
  setPoints,
  unionBounds,
  type CameraMode,
  type Scene,
  type ShadingMode,
  type SplitMode,
} from "../lib/cloudScene";
import { diffSides, type CompareSide } from "../lib/compareDiff";
import { withBoundValues } from "../lib/graphParams";
import { effectiveParams } from "../lib/params";
import type { RampName } from "../lib/ramps";
import { copyFrameWrites, pickFrame, roiFramesOf } from "../lib/roiFrames";
import { rememberRoiBounds } from "../lib/roiThumbs";
import { disposeOverlay, extentOf, shapesOf } from "../lib/shapes2d";
import { fullId, levelOf, resolveOutput } from "../lib/subgraph";
import { compareContentFor } from "../lib/viewRule";
import { sameFrame } from "../lib/viewFit";
import { exportCanvasPng } from "../lib/exportPng";
import { MAX_POINTS_CHOICES, rangeFor, withRangeAuto, withRangeEnd, type ManualRanges } from "../lib/viewPrefs";
import { transport } from "../transport";
import { useCompareStore, type CompareSlot, type CompareSnapshot } from "../store/compare";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useGraphParamOverrides } from "../store/recipe";
import { useUiStore } from "../store/ui";
import { decodeCloud, type CloudPayload, type OutputStat } from "../types/execution";
import { CompareDiff } from "./CompareDiff";
import { CompareStage } from "./CompareStage";
import { ImagePane } from "./ImagePane";
import { ViewerStatus } from "./ViewerStatus";
import { MeasureReadout } from "./MeasureReadout";
import { RoiLayer, type RoiItem } from "./RoiLayer";
import { ValuePane } from "./ValuePane";
import { useFocusOnDoubleClick } from "../hooks/useFocusOnDoubleClick";
import { measureAttrs, useMeasure } from "../hooks/useMeasure";
import { useViewerSource, type ViewerSource } from "../hooks/useViewerSource";
import "../styles.viewer.css";

export type { ShadingMode, CameraMode } from "../lib/cloudScene";
export type { RampName } from "../lib/ramps";


// ------------------------------------------------------------ 2D 拖框（L15）

/** 四个角色框各一种颜色；多于四个时轮着用。 */
const ROI_COLORS = ["#34d399", "#f472b6", "#60a5fa", "#fbbf24", "#a78bfa", "#f87171"];

const NO_BACKDROP = { key: "", bounds: null, count: 0, error: null };

/** 没填过的框画在哪：底图（或数据云）的包围盒里一字排开，拖一下就落成真值。 */
function placeholderOf(
  i: number,
  n: number,
  bounds: ArrayLike<number> | null,
): [number, number, number, number] {
  if (!bounds) {
    const s = 0.005;
    const cx = (i - (n - 1) / 2) * s * 2.5;
    return [cx - s / 2, -s / 2, cx + s / 2, s / 2];
  }
  const w = bounds[3]! - bounds[0]!;
  const h = bounds[4]! - bounds[1]!;
  const cx = bounds[0]! + (w * (i + 0.5)) / n;
  const cy = (bounds[1]! + bounds[4]!) / 2;
  const hw = w / (n * 4);
  const hh = Math.max(h * 0.25, hw);
  return [cx - hw, cy - hh, cx + hw, cy + hh];
}

/** 对比时双栏的最小宽度：比它窄就改成上下叠（compare-plan §1.2）。 */
const COMPARE_MIN_LR_WIDTH = 480;

/** 这一侧此刻显示着的结果冻成快照；还没有结果（在取、没跑、出错……）返回 null。
 *  判据与 Edge Peek 的锁定条件相同：状态文字为空且不在取数（EdgePeek.tsx:76）。 */
function snapshotOf(src: ViewerSource, label: string, maxPoints: number): CompareSnapshot | null {
  const d = src.display;
  if (src.loading || d.status !== null || !d.runId || !d.nodeId) return null;
  return {
    runId: d.runId,
    preview: d.preview,
    label,
    content: src.content,
    outputs: src.outputs,
    cloud: d.cloud,
    cloudPort: d.port,
    base: d.base,
    maxPoints,
  };
}

/** 2D 几何叠画（G7）：整组重建，线与端口同色。对比时 A、B 各一组。 */
function drawShapes(
  group: THREE.Group,
  shapes: readonly OutputStat[],
  cloud: CloudPayload | null,
  typesByName: ReadonlyMap<string, { color?: string }>,
): void {
  disposeOverlay(group);
  if (shapes.length === 0) return;
  // 线的长度/十字的大小要有个尺度参照：优先用点云的跨度，没有云就用几何自己的。
  const span = cloud
    ? Math.max(cloud.bounds[3]! - cloud.bounds[0]!, cloud.bounds[4]! - cloud.bounds[1]!, 1e-3)
    : Math.max(...shapes.map(extentOf), 1e-3);
  for (const out of shapes) {
    const hex = typesByName.get(out.type)?.color ?? "#6b7280";
    const color = new THREE.Color(hex).getHex();
    for (const line of shapesOf(out, color, span)) group.add(line);
  }
}

function sideOf(src: ViewerSource): CompareSide {
  return { outputs: src.outputs, cloud: src.display.cloud, cloudPort: src.display.port, preview: src.display.preview };
}

/** 栏标题：节点标题优先，其次算子 label（与「底图：xxx」同一条规则）。 */
function nodeLabel(src: ViewerSource, fallback: string | null): string {
  return src.node?.ui?.title ?? src.op?.label ?? src.node?.id ?? fallback ?? "";
}

export function Viewer3D({ onRunToNode }: { onRunToNode?: ((nodeId: string) => void) | undefined } = {}) {
  const hostRef = useRef<HTMLDivElement>(null);
  const sceneRef = useRef<Scene | null>(null);

  // 着色、色带、点大小、显示点数在 ui store（落 localStorage，重启后还是上次的，lib/viewPrefs）
  const { shading, ramp, pointSize, maxPoints } = useUiStore((s) => s.viewerPrefs);
  const setPrefs = useUiStore((s) => s.setViewerPrefs);
  // 手动着色范围按着色模式各记一份：强度下填的 0–255 不该搬到高度上（不落盘：跟数据强相关）
  const [manualRanges, setManualRanges] = useState<ManualRanges>({});
  const pointSizeRef = useRef(pointSize);
  // 相机模式在 ui store：参数面板的 ROI 行「进入拖框」要能把它切到 2D（param-recipe P2.7）
  const cameraMode = useUiStore((s) => s.viewerMode);
  const setCameraMode = useUiStore((s) => s.setViewerMode);
  // 只读地暴露给验收脚本：底图云与叠画几何各自的包围盒，用来断言两者在同一个平面上。
  const [overlayBounds, setOverlayBounds] = useState<Float32Array | null>(null);

  const selected = useUiStore((s) => s.selectedNodes);
  const path = useUiStore((s) => s.path);
  const pinnedId = useUiStore((s) => s.pinnedNode);
  const setPinnedId = useUiStore((s) => s.setPinnedNode);
  const doc = useGraphStore((s) => s.doc);
  const nodes = useMemo(() => levelOf(doc, path).nodes, [doc, path]);
  // 底图取自输入端口时同样按节点表反映的那一次运行取（见 store/execution 的 resultRunId）
  const runId = useExecutionStore((s) => s.resultRunId);
  const runStatus = useExecutionStore((s) => s.runStatus);
  const isPreview = useExecutionStore((s) => s.preview);
  const previewMaxPoints = useUiStore((s) => s.previewMaxPoints);

  const selectedId = selected.size === 1 ? [...selected][0]! : null;
  // 钉住优先于选中：钉住期间在画布上点别的节点，视图不跟着走（§2.6）。
  const activeId = pinnedId ?? selectedId;
  const activeKey = activeId ? fullId(path, activeId) : null;
  // 手动选的「值 / 点云」只对当时那个节点有效，换节点就回到自动
  const contentPick = useUiStore((s) => s.viewerContentPick);
  const setContentPick = useUiStore((s) => s.setViewerContentPick);
  const slotA = useMemo(() => (activeId ? { path, nodeId: activeId } : null), [path, activeId]);
  // 取数（hooks/useViewerSource）：状态、输出统计、显示点云还是值、取云或借上游的底图
  const source = useViewerSource({
    slot: slotA,
    idleText: selected.size > 1 ? "选中了多个节点" : "选中一个节点查看它的输出",
    maxPoints,
    pick: contentPick && contentPick.nodeId === activeKey ? contentPick.content : null,
    frozen: null,
  });
  const { display, loading, node: activeNode, op: activeOp, outputs: activeOutputs, content, autoContent } = source;
  const { cloud } = display;

  // -- 对比（交互清单 #35）：A 就是上面那个（跟随选中 / 钉住），B 是一个显式的槽 ---------------
  const compareOn = useCompareStore((s) => s.on);
  const compareB = useCompareStore((s) => s.b);
  const frozenB = useCompareStore((s) => s.snapshot);
  const sourceB = useViewerSource({
    slot: compareOn ? compareB : null,
    idleText: "",
    maxPoints,
    pick: null,
    frozen: compareOn ? frozenB : null,
  });
  const cloudB = compareOn ? sourceB.display.cloud : null;
  // 一侧可画就两栏都是点云场景；两侧都只有值才换成两张值表格（§1.6）
  const stageContent = compareOn ? compareContentFor(content, sourceB.content) : content;
  const labelA = nodeLabel(source, activeId);
  const labelB = compareOn ? nodeLabel(sourceB, compareB?.nodeId ?? null) : "";
  const diff = useMemo(
    () => (compareOn ? diffSides(sideOf(source), sideOf(sourceB)) : null),
    // source / sourceB 每次渲染都是新对象；差异只随这四样变
    [compareOn, source.display, source.outputs, sourceB.display, sourceB.outputs],
  );
  const canFreezeB = compareOn && !frozenB && snapshotOf(sourceB, labelB, maxPoints) !== null;
  // 进入对比时顺手冻住 B（C2）：store 的 toggle 调这里，拿 A 此刻显示着的结果
  const captureRef = useRef<(slot: CompareSlot) => CompareSnapshot | null>(() => null);
  captureRef.current = (slot) =>
    slot.path === path && slot.nodeId === display.nodeId ? snapshotOf(source, labelA, maxPoints) : null;
  useEffect(() => {
    useCompareStore.getState().setCapture((slot) => captureRef.current(slot));
    return () => useCompareStore.getState().setCapture(null);
  }, []);
  // B 的节点被删（撤销也算）就退出对比
  useEffect(() => {
    useCompareStore.getState().prune(doc);
  }, [doc]);
  // 参数面板的 ROI 缩略图拿这片云的范围当底图（param-recipe P2.7）
  useEffect(() => {
    if (display.nodeId && cloud && cloud.pointCount > 0) {
      rememberRoiBounds(fullId(useUiStore.getState().path, display.nodeId), cloud.bounds);
    }
  }, [display.nodeId, cloud]);
  const typesByName = useManifestStore((s) => s.typesByName);
  const graphPath = useGraphStore((s) => s.filePath);
  // 2D 拖框（m8-plan L15）：选中节点带 roi 语义标记的参数按底图分组；一次只画选中的那一组
  // （L20：locate_template 的一个模板槽），切换条列出全部组
  // 框的位置按有效值画（param-recipe P1.4）：被图参数绑定的 roi 参数取图参数的值，
  // 拖动写回照旧走 setParam —— 它在 store 里自己路由到图参数
  const overrides = useGraphParamOverrides();
  const roiNode = useMemo(
    () =>
      activeNode && activeOp
        ? withBoundValues(doc, path, activeNode, activeOp.params.map((p) => p.name), overrides)
        : activeNode,
    [doc, path, activeNode, activeOp, overrides],
  );
  const roiFrames = useMemo(() => roiFramesOf(activeOp, roiNode), [activeOp, roiNode]);
  // 离开那个节点就忘掉手动选的：换走再回来也回到自动，而不是悄悄停在上次选的值表格上
  useEffect(() => {
    if (contentPick && contentPick.nodeId !== activeKey) setContentPick(null);
  }, [activeKey, contentPick]);
  const selectedFrame = useUiStore((s) => (activeId ? s.roiFrame[activeId] : undefined));
  const setRoiFrame = useUiStore((s) => s.setRoiFrame);
  const roi = useMemo(() => pickFrame(roiFrames, selectedFrame), [roiFrames, selectedFrame]);
  const [sceneHost, setSceneHost] = useState<Scene | null>(null);
  const [backdrop, setBackdrop] = useState<{
    key: string;
    bounds: Float32Array | null;
    count: number;
    error: string | null;
  }>(NO_BACKDROP);
  // 底图（L15）两种来源：roiBackdrop 指的文件，经宿主读；或者输入端口，沿边取上游那一次运行的结果
  // （gap.align_template 的模板是输入端口不是文件）。端口那种要带上 runId 与运行状态：跑完了要重取
  const portSources = useMemo(() => {
    if (!roi || roi.inputs.length === 0 || !activeNode) return null;
    const level = levelOf(doc, path);
    return roi.inputs.map((name) => {
      const e = level.edges.find((x) => x.to.node === activeNode.id && x.to.port === name);
      return e ? resolveOutput(doc, path, e.from.node, e.from.port) : null;
    });
  }, [roi, activeNode, doc, path]);
  // 对比模式下没有拖框，也就不要底图（C8）
  const backdropKey =
    compareOn
      ? ""
      : roi && roi.files.length > 0
      ? JSON.stringify({ files: roi.files, graphPath })
      : roi && portSources
        ? JSON.stringify({ ports: portSources, inputs: roi.inputs, runId: runId ?? null, runStatus })
        : "";

  // 对比时按两侧都有才算有（C5）：A 有强度 B 没有，两栏都退到高度 —— 同一个值涂同一种颜色
  const drawn = compareOn ? [cloud, cloudB].filter((c) => c && c.pointCount > 0) : [cloud];
  const allHave = (channel: "intensity" | "normals" | "rgb") =>
    drawn.length > 0 && drawn.every((c) => c?.[channel] != null);
  const hasIntensity = allHave("intensity");
  const hasNormals = allHave("normals");
  const hasRgb = allHave("rgb");
  // 选了「强度」但这片云没有强度通道时实际走别的着色，那就让下拉框也显示实际那一种 ——
  // 下拉框写着强度、画面却是高度，用户只会以为强度数据本身有问题。没有强度但自带颜色的
  // （模型分割的类别色、PCD 里的 rgb）先退到 RGB：那本来就是给人看的颜色
  const effectiveShading: ShadingMode =
    shading === "intensity" && !hasIntensity
      ? hasRgb
        ? "rgb"
        : "height"
      : (shading === "normal" && !hasNormals) || (shading === "rgb" && !hasRgb)
        ? "height"
        : shading;
  const isFlat = effectiveShading === "flat";
  const isRgbShading = effectiveShading === "rgb";
  // 色带与范围只对「标量 → 颜色」的着色有意义
  const noRamp = isFlat || isRgbShading;

  // 「自动」范围：对比时取两侧的并集（C5）
  const dataRange = useMemo<[number, number]>(() => {
    const a = dataRangeOf(cloud, effectiveShading);
    if (!cloudB || cloudB.pointCount === 0) return a;
    const b = dataRangeOf(cloudB, effectiveShading);
    if (!cloud || cloud.pointCount === 0) return b;
    return [Math.min(a[0], b[0]), Math.max(a[1], b[1])];
  }, [cloud, cloudB, effectiveShading]);
  const { range: shownRange, auto: rangeAuto } = rangeFor(manualRanges, effectiveShading, dataRange);
  const [lo, hi] = shownRange;

  const pinnedLabel = useMemo(() => {
    if (!pinnedId) return "";
    return nodes.find((n) => n.id === pinnedId)?.ui?.title ?? pinnedId;
  }, [pinnedId, nodes]);

  // 钉住的节点被删掉就自动松开，否则视图会一直停在一个不存在的节点上。
  useEffect(() => {
    if (pinnedId && !nodes.some((n) => n.id === pinnedId)) setPinnedId(null);
  }, [pinnedId, nodes]);

  // -- three.js 场景：只建一次 ---------------------------------------------
  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;
    const scene = createScene(host);
    sceneRef.current = scene;
    setSceneHost(scene);

    return () => {
      scene.dispose();
      sceneRef.current = null;
      setSceneHost(null);
    };
  }, []);

  // -- 对比的两栏：同一个场景切成两个视口，窄了就上下叠 ----------------------
  const [narrow, setNarrow] = useState(false);
  useEffect(() => {
    const host = hostRef.current;
    if (!compareOn || !host) return;
    const measure = () => setNarrow(host.clientWidth < COMPARE_MIN_LR_WIDTH);
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(host);
    return () => observer.disconnect();
  }, [compareOn]);
  const split: SplitMode = narrow ? "tb" : "lr";
  useEffect(() => {
    sceneRef.current?.setViews(compareOn ? 2 : 1, split);
  }, [compareOn, split]);
  // 验收脚本读相机：对比时两栏共用一台，拖动任一栏两边一起变（§6 第 3 条）；单栏时看重跑、换节点之后视角动没动
  useEffect(() => {
    const scene = sceneRef.current;
    const host = hostRef.current;
    if (!scene || !host) return;
    const write = () => {
      const c = scene.active().position;
      const text = [c.x, c.y, c.z].map(round3).join(",");
      if (host.dataset.cameraPos !== text) host.dataset.cameraPos = text;
    };
    scene.frameListeners.add(write);
    return () => {
      scene.frameListeners.delete(write);
      delete host.dataset.cameraPos;
    };
  }, [compareOn]);

  // -- 几何体：只随点云重建，着色参数一律不进这个 effect ---------------------
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;

    // 点大小从 ref 读初值：pointSize **不能**进这个 effect 的依赖，见下面那条注释
    setPoints(scene, 0, buildPoints(cloud, pointSizeRef.current));
  }, [cloud]);

  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    setPoints(scene, 1, buildPoints(cloudB, pointSizeRef.current));
  }, [cloudB]);

  // 换着色模式/色带/范围只重写 color 属性，positions 和 boundingSphere 原样留着。
  useEffect(() => {
    const points = sceneRef.current?.points[0];
    if (!points || !cloud || cloud.pointCount === 0) return;
    paintPoints(points, cloud, { shading: effectiveShading, ramp, lo, hi });
  }, [cloud, effectiveShading, ramp, lo, hi]);
  useEffect(() => {
    const points = sceneRef.current?.points[1];
    if (!points || !cloudB || cloudB.pointCount === 0) return;
    paintPoints(points, cloudB, { shading: effectiveShading, ramp, lo, hi });
  }, [cloudB, effectiveShading, ramp, lo, hi]);

  // 点大小只改材质，不重建几何体 —— 它曾经也在上面那个 effect 的依赖里，
  // 拖一下滑块就要重分配 24MB 颜色数组、重扫两百万点（见 README「踩过的坑」）。
  useEffect(() => {
    pointSizeRef.current = pointSize;
    for (const p of sceneRef.current?.points ?? []) {
      if (p) (p.material as THREE.PointsMaterial).size = pointSize;
    }
  }, [pointSize]);

  // -- 2D 几何叠画（G7）：整组重建，线与端口同色 -----------------------------
  const overlayShapes = useMemo(
    () => (activeOutputs ?? []).filter((o) => o.value !== undefined),
    [activeOutputs],
  );
  const overlayCount = useMemo(() => {
    let n = 0;
    for (const o of overlayShapes) {
      const k = o.value?.kind;
      if (k === "Box2D" || k === "Line2D" || k === "Circle2D" || k === "Point2D") n += 1;
    }
    return n;
  }, [overlayShapes]);

  const overlayShapesB = useMemo(
    () => (compareOn ? (sourceB.outputs ?? []).filter((o) => o.value !== undefined) : []),
    [compareOn, sourceB.outputs],
  );

  useEffect(() => {
    const scene = sceneRef.current;
    if (scene) drawShapes(scene.overlays[0], overlayShapes, cloud, typesByName);
  }, [overlayShapes, cloud, typesByName]);
  useEffect(() => {
    const scene = sceneRef.current;
    if (scene) drawShapes(scene.overlays[1], overlayShapesB, cloudB, typesByName);
  }, [overlayShapesB, cloudB, typesByName]);

  // -- 拖框的底图（L15）：roiBackdrop 指的文件（例如槽 1 的左右模板）不属于任何一次运行 ——
  // 框没填好时 locate_template 过不了校验，根本不会跑，底图却必须先看得见。指的是输入端口时
  // （align_template），底图就是上游那一次运行的结果，没跑过要说清楚。
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    disposeOverlay(scene.backdrop);
    if (!backdropKey) {
      setBackdrop(NO_BACKDROP);
      return;
    }
    const spec = JSON.parse(backdropKey) as {
      files?: string[];
      graphPath?: string | null;
      ports?: ({ nodeId: string; port: string } | null)[];
      inputs?: string[];
      runId?: string | null;
    };
    if (spec.files && typeof transport.loadCloudFile !== "function") {
      setBackdrop(NO_BACKDROP);
      return;
    }
    const load = (): Promise<CloudPayload[]> => {
      if (spec.files) {
        return Promise.all(
          spec.files.map((f) =>
            transport.loadCloudFile!(f, spec.graphPath ?? null, 200_000).then(decodeCloud),
          ),
        );
      }
      const inputs = spec.inputs ?? [];
      const unwired = inputs.filter((_, i) => !spec.ports?.[i]);
      if (unwired.length > 0) {
        return Promise.reject(new Error(`底图取自输入 ${unwired.join(" / ")} 的上游，但它没接上`));
      }
      if (!spec.runId) {
        return Promise.reject(new Error(`先运行一次：底图取自输入 ${inputs.join(" / ")} 上游的结果`));
      }
      return Promise.all(
        (spec.ports ?? []).map((src) =>
          transport.getOutputCloud(spec.runId!, src!.nodeId, src!.port, 200_000).then(decodeCloud),
        ),
      );
    };
    let cancelled = false;
    void (async () => {
      try {
        const payloads = await load();
        if (cancelled) return;
        // 几片拼成一片；NaN 槽不画（包围盒会被它毒成 NaN）
        const xyz: number[] = [];
        for (const p of payloads) {
          for (let i = 0; i < p.pointCount; i += 1) {
            const x = p.xyz[i * 3]!;
            const y = p.xyz[i * 3 + 1]!;
            if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
            xyz.push(x, y, 0);
          }
        }
        const geometry = new THREE.BufferGeometry();
        geometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array(xyz), 3));
        geometry.computeBoundingBox();
        geometry.computeBoundingSphere();
        const material = new THREE.PointsMaterial({
          size: pointSizeRef.current + 0.6,
          sizeAttenuation: false,
          color: 0xd1d5db,
          transparent: true,
          opacity: 0.85,
        });
        const points = new THREE.Points(geometry, material);
        points.renderOrder = 5;
        scene.backdrop.add(points);
        const bb = geometry.boundingBox;
        setBackdrop({
          key: backdropKey,
          bounds:
            xyz.length > 0 && bb
              ? new Float32Array([bb.min.x, bb.min.y, 0, bb.max.x, bb.max.y, 0])
              : null,
          count: xyz.length / 3,
          error: null,
        });
      } catch (e) {
        if (cancelled) return;
        setBackdrop({
          key: backdropKey,
          bounds: null,
          count: 0,
          error: e instanceof Error ? e.message : String(e),
        });
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [backdropKey]);

  const roiItems = useMemo<RoiItem[]>(() => {
    if (!roi || !activeOp || !roiNode) return [];
    const eff = effectiveParams(activeOp, roiNode);
    const bounds =
      backdrop.bounds ??
      (roi.files.length === 0 && roi.inputs.length === 0 && cloud && cloud.pointCount > 0
        ? cloud.bounds
        : null);
    const n = roi.params.length;
    return roi.params.map((param, i) => {
      const raw = eff[param.name];
      const value = Array.isArray(raw) && raw.length === 4 ? raw.map(Number) : [0, 0, 0, 0];
      return {
        param: param.name,
        label: param.label ?? param.name,
        // 颜色按组内位置取：每个槽的 datum 都是同一种颜色，切槽时不换色
        color: ROI_COLORS[i % ROI_COLORS.length]!,
        value: [value[0]!, value[1]!, value[2]!, value[3]!],
        scale: param.unit === "mm" ? 0.001 : 1,
        placeholder: placeholderOf(i, n, bounds),
      };
    });
  }, [roi, activeOp, roiNode, backdrop.bounds, cloud]);
  const roiEditing =
    !compareOn &&
    content === "cloud" &&
    cameraMode === "2d" &&
    roiItems.length > 0 &&
    activeNode !== undefined;
  // 切换条只给带底图的组（有名字的）；数据坐标系那一组只有一组，不需要切
  const roiTabs = roiEditing && roiFrames.some((f) => f.label) ? roiFrames : [];

  // -- 测量（docs/measure-plan.md）：单击选点、两点测距；逻辑在 hooks/useMeasure，与连线查看器共用 --
  const measuring = useUiStore((s) => s.viewerMeasuring);
  const setMeasuring = useUiStore((s) => s.setViewerMeasuring);
  // 只在点云场景里、且不在拖框时可用：拖框独占指针（M7）
  const measureOn = measuring && stageContent === "cloud" && !roiEditing;
  useEffect(() => {
    if (measuring && roiEditing) setMeasuring(false);
  }, [measuring, roiEditing]);
  const { measure, clear: clearMeasure } = useMeasure(
    sceneHost,
    measureOn,
    [cloud, cloudB],
    `${display.nodeId ?? ""}|${compareB?.nodeId ?? ""}`,
  );
  // 双击一个点：转心挪到它上面（测量、拖框时不接）
  useFocusOnDoubleClick(sceneHost, [cloud, cloudB], measureOn || roiEditing || stageContent !== "cloud");

  /** 把当前这组框原样写进其它启用的组（L20「复制到其它槽」），整个算一条撤销。 */
  const copyFrameToOthers = () => {
    if (!roi || !activeOp || !activeNode || !roiNode) return;
    const writes = copyFrameWrites(activeOp, roiNode, roi, roiFrames);
    if (writes.length === 0) return;
    const g = useGraphStore.getState();
    g.begin();
    for (const w of writes) useGraphStore.getState().setParam(activeNode.id, w.param, w.value);
    useGraphStore.getState().commit(`把${roi.label || "当前"}的框复制到其它槽`);
  };

  // 换了云或换了几何时看一眼要不要取景：还在同一个坐标系里（sameFrame）就不动相机 —— 调参数重跑、在链上逐个点节点时，
  // 视角连同双击设好的转心都留着。以前每片新云都取景一次，方向写死成斜 45°，正交相机的缩放也回到 1。
  // 第一片云、开关对比、换分栏方向时照旧取景。必须排在上面那个 effect 之后：取景要量的是它刚建好的那一组线。
  const fitted = useRef<{ bounds: Float32Array; compare: boolean; split: SplitMode } | null>(null);
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    setOverlayBounds(overlayBoundsOf(scene.overlay));
    // 对比时按 A ∪ B 取景（§1.3）：同一台相机，两栏里的东西都要装得下
    const bounds = unionBounds([cloud, cloudB], scene.overlays[0], scene.overlays[1], scene.backdrop);
    if (!bounds) return;
    const last = fitted.current;
    if (last && last.compare === compareOn && last.split === split && sameFrame(last.bounds, bounds)) return;
    fitToBounds(scene, bounds);
    fitted.current = { bounds, compare: compareOn, split };
  }, [cloud, cloudB, overlayShapes, overlayShapesB, backdrop.bounds, compareOn, split]);

  // 相机模式（G7）。只换 controls 挂的那台相机，场景与几何原封不动。
  useEffect(() => {
    sceneRef.current?.setMode(cameraMode);
  }, [cameraMode]);

  const setRangeEnd = (end: 0 | 1, raw: string) => {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    // 另一个界取框里看得到的那个数（自动时显示的是取整过的）：填完一个界，另一个框里的数不该跟着跳
    const shown: [number, number] = rangeAuto ? [round3(lo), round3(hi)] : [lo, hi];
    setManualRanges((m) => withRangeEnd(m, effectiveShading, end, v, shown));
  };

  const exportPng = async () => {
    const scene = sceneRef.current;
    if (!scene) return;
    // 读 buffer 前立刻重画一帧：换成 preserveDrawingBuffer 的话每一帧都要多付一次代价。
    scene.renderFrame();
    // 对比时一块画布就是两栏，导出的天然是一张并排图（C4）
    const name = compareOn ? `${display.nodeId ?? "A"}_vs_${compareB?.nodeId ?? "B"}` : (display.nodeId ?? "view");
    await exportCanvasPng(scene.renderer.domElement, name);
  };

  return (
    <div
      className="viewer"
      // 验收脚本靠这两个属性判断「视图已经切到这个节点了」，
      // 而不是去猜多久之后 React 会渲染完（scripts/e2e）。
      data-node={display.nodeId ?? ""}
      data-view={
        loading
          ? "loading"
          : (content === "value" || content === "image") && !display.status
            ? content
            : cloud
              ? "cloud"
              : "empty"
      }
      data-content={content}
      data-shading={effectiveShading}
      data-pinned={pinnedId ? "1" : "0"}
      data-run={display.runId ?? ""}
      data-busy={display.busy ? "1" : undefined}
      data-preview={isPreview ? "1" : "0"}
      data-camera={cameraMode}
      data-overlay={overlayCount}
      data-base={display.base?.localId ?? ""}
      data-cloud-bounds={boundsAttr(cloud && cloud.pointCount > 0 ? cloud.bounds : null)}
      data-overlay-bounds={boundsAttr(overlayBounds)}
      data-roi-edit={roiEditing ? roiItems.length : 0}
      data-roi-frame={roiEditing && roi ? roi.key : undefined}
      data-backdrop={backdrop.count}
      data-backdrop-bounds={boundsAttr(backdrop.bounds)}
      data-backdrop-error={backdrop.error ?? undefined}
      data-compare={compareOn ? "1" : "0"}
      data-compare-a={compareOn ? (activeKey ?? "") : undefined}
      data-compare-b={compareOn && compareB ? fullId(compareB.path, compareB.nodeId) : undefined}
      data-compare-frozen={compareOn ? (frozenB ? "1" : "0") : undefined}
      data-compare-run-b={compareOn ? (sourceB.display.runId ?? "") : undefined}
      data-split={compareOn ? split : undefined}
      {...measureAttrs(measureOn, measure)}
    >
      <div className="viewer__bar">
        <span className="viewer__title">预览</span>
        {isPreview && (
          <span className="viewer__preview" data-testid="viewer-preview-badge">
            预览 {Math.round(previewMaxPoints / 10000)} 万点
          </span>
        )}
        {display.base && (
          <span
            className="viewer__base"
            data-testid="viewer-base"
            data-node={display.base.localId}
            title={`该节点没有点云输出，底图取自上游最近的一片云（${display.base.localId}）`}
          >
            底图：{display.base.label}
          </span>
        )}
        {cloud && (
          <span className="viewer__count" title="显示点数 / 总点数">
            {cloud.pointCount.toLocaleString()} / {cloud.totalPoints.toLocaleString()} 点
          </span>
        )}
        <span className="viewer__spacer" />
        <select
          className="viewer__select"
          data-testid="viewer-camera"
          value={content === "value" || content === "image" ? content : cameraMode}
          onChange={(e) => {
            const v = e.target.value;
            if (v === "value" || v === "image") {
              if (activeKey) setContentPick({ nodeId: activeKey, content: v });
              return;
            }
            setCameraMode(v as CameraMode);
            if (activeKey && content !== "cloud") setContentPick({ nodeId: activeKey, content: "cloud" });
          }}
          title="3D 自由视角 / 2D 正交俯视 XY（剖面）/ 图像（图像域的节点）/ 输出值的表格。默认按节点的输入输出类型选，换节点就回到默认"
        >
          <option value="3d">3D</option>
          <option value="2d">2D 剖面</option>
          <option value="image" disabled={!activeKey || autoContent !== "image"}>
            图像
          </option>
          <option value="value" disabled={!activeKey}>
            值
          </option>
        </select>
        {content === "cloud" && (
          <>
            <select
              className="viewer__select"
              value={maxPoints}
              onChange={(e) => setPrefs({ maxPoints: Number(e.target.value) })}
              title="最多显示多少点（抽样在 C++ 侧做）"
            >
              {MAX_POINTS_CHOICES.map((n) => (
                <option key={n} value={n}>
                  {n >= 1_000_000 ? `${n / 1_000_000}M` : `${n / 1000}K`}
                </option>
              ))}
            </select>
            <input
              className="viewer__size"
              type="range"
              min={0.5}
              max={6}
              step={0.1}
              value={pointSize}
              onChange={(e) => setPrefs({ pointSize: Number(e.target.value) })}
              title="点大小"
            />
            <button
              type="button"
              className="viewer__fit"
              onClick={() => {
                const scene = sceneRef.current;
                const bounds = scene
                  ? unionBounds([cloud, cloudB], scene.overlays[0], scene.overlays[1], scene.backdrop)
                  : null;
                if (scene && bounds) {
                  fitToBounds(scene, bounds);
                  fitted.current = { bounds, compare: compareOn, split };
                }
              }}
              disabled={!cloud && overlayCount === 0 && backdrop.count === 0}
              title="缩放到全部（底图云 + 叠画几何）"
            >
              ⤢
            </button>
          </>
        )}
      </div>

      <div className="viewer__bar viewer__bar--tools">
        {content === "cloud" && (
          <>
            <span className="viewer__label">着色</span>
            <select
              className="viewer__select"
              data-testid="viewer-shading"
              value={effectiveShading}
              onChange={(e) => setPrefs({ shading: e.target.value as ShadingMode })}
              title={
                shading === effectiveShading
                  ? "着色方式"
                  : "这片点云没有该通道，已换成它实际有的着色"
              }
            >
              <option value="intensity" disabled={!hasIntensity}>
                强度{hasIntensity ? "" : "（无）"}
              </option>
              <option value="height">高度</option>
              <option value="normal" disabled={!hasNormals} title="需要点云带法线通道">
                法线{hasNormals ? "" : "（无）"}
              </option>
              <option value="rgb" disabled={!hasRgb} title="点云自带的颜色（PCD 的 rgb、模型分割的类别色）">
                RGB{hasRgb ? "" : "（无）"}
              </option>
              <option value="flat">单色</option>
            </select>
            <select
              className="viewer__select"
              data-testid="viewer-ramp"
              value={ramp}
              onChange={(e) => setPrefs({ ramp: e.target.value as RampName })}
              disabled={noRamp}
              title="色带"
            >
              <option value="viridis">viridis</option>
              <option value="gray">灰度</option>
              <option value="jet">蓝→红</option>
            </select>
            <input
              className="viewer__num"
              data-testid="viewer-range-min"
              type="number"
              step="any"
              value={rangeAuto ? round3(lo) : lo}
              onChange={(e) => setRangeEnd(0, e.target.value)}
              disabled={noRamp}
              title="着色范围下限"
            />
            <input
              className="viewer__num"
              data-testid="viewer-range-max"
              type="number"
              step="any"
              value={rangeAuto ? round3(hi) : hi}
              onChange={(e) => setRangeEnd(1, e.target.value)}
              disabled={noRamp}
              title="着色范围上限"
            />
            <button
              type="button"
              className="viewer__btn"
              data-testid="viewer-range-auto"
              onClick={() => setManualRanges((m) => withRangeAuto(m, effectiveShading))}
              disabled={noRamp || rangeAuto}
              title="范围回到数据实际的最小/最大"
            >
              自动
            </button>
          </>
        )}
        <span className="viewer__spacer" />
        {pinnedId && (
          <span className="viewer__pinned" title={`已钉住 ${pinnedId}`}>
            📌 {pinnedLabel}
          </span>
        )}
        <button
          type="button"
          className="viewer__btn"
          data-testid="viewer-pin"
          data-pinned={pinnedId ? "1" : "0"}
          onClick={() => setPinnedId(pinnedId ? null : selectedId)}
          disabled={!pinnedId && !selectedId}
          title={pinnedId ? "取消钉住，重新跟随选中" : "钉住当前节点，选别的节点也不切换"}
        >
          {pinnedId ? "已钉住" : "钉住"}
        </button>
        <button
          type="button"
          className="viewer__btn"
          data-testid="viewer-compare"
          data-on={compareOn ? "1" : "0"}
          onClick={() => useCompareStore.getState().toggle()}
          disabled={!compareOn && !activeId}
          title={
            compareOn
              ? "退出对比（Ctrl+Shift+D）"
              : "与基准对比（Ctrl+Shift+D）：B 冻结在当前结果上，之后改参数重跑只有 A 变"
          }
        >
          {compareOn ? "退出对比" : "对比"}
        </button>
        {stageContent === "cloud" && (
          <button
            type="button"
            className="viewer__btn"
            data-testid="viewer-measure"
            data-on={measureOn ? "1" : "0"}
            onClick={() => setMeasuring(!measuring)}
            disabled={roiEditing}
            title={
              roiEditing
                ? "拖框时不能测量"
                : measuring
                  ? "关掉测量（M）"
                  : "测量（M）：单击选点看坐标，再点一个量距离；拖动照旧转视角"
            }
          >
            {measuring ? "测量中" : "测量"}
          </button>
        )}
        {stageContent === "cloud" && (
          <button
            type="button"
            className="viewer__btn"
            data-testid="viewer-export"
            onClick={() => void exportPng()}
            title="把当前画面存成 PNG"
          >
            PNG
          </button>
        )}
      </div>

      {roiTabs.length > 0 && roi && activeNode && (
        <div className="viewer__bar viewer__bar--roi" data-testid="roi-frames" role="tablist">
          {roiTabs.map((f) => (
            <button
              key={f.key}
              type="button"
              role="tab"
              className={`viewer__tab${f.key === roi.key ? " is-active" : ""}`}
              data-testid="roi-frame-tab"
              data-frame={f.key}
              data-active={f.key === roi.key ? "1" : "0"}
              aria-selected={f.key === roi.key}
              onClick={() => setRoiFrame(activeNode.id, f.key)}
              title={`只显示${f.label}的模板与它的 ${f.params.length} 个框`}
            >
              {f.label}
            </button>
          ))}
          <span className="viewer__spacer" />
          <button
            type="button"
            className="viewer__btn"
            data-testid="roi-copy-frame"
            disabled={roiTabs.length < 2}
            onClick={copyFrameToOthers}
            title={`把${roi.label}的 ${roi.params.length} 个框原样复制到其它启用的槽（按角色一一对应）`}
          >
            复制到其它槽
          </button>
        </div>
      )}

      <div className="viewer__stage">
        <div className="viewer__canvas" ref={hostRef} data-testid="viewer3d-canvas" />
        {compareOn && (
          <CompareStage
            split={split}
            stage={stageContent}
            a={{ display, loading, content, outputs: activeOutputs, label: labelA }}
            b={{
              display: sourceB.display,
              loading: sourceB.loading,
              content: sourceB.content,
              outputs: sourceB.outputs,
              label: labelB,
            }}
            frozen={frozenB !== null}
            canFreeze={canFreezeB}
            frozenPoints={frozenB?.maxPoints ?? null}
            onFreeze={() => {
              const snap = snapshotOf(sourceB, labelB, maxPoints);
              if (snap) useCompareStore.getState().freeze(snap);
            }}
            onUnfreeze={() => useCompareStore.getState().unfreeze()}
            onExit={() => useCompareStore.getState().exit()}
          />
        )}
        {!compareOn && content === "value" && !display.status && !loading && (
          <ValuePane outputs={activeOutputs} />
        )}
        {!compareOn && content === "image" && activeNode && activeOp && roiNode && (
          // 状态（未运行 / 正在计算…）由 ImagePane 自己画：运行中它继续显示上一次的图，状态缩在角上
          <ImagePane
            doc={doc}
            path={path}
            node={activeNode}
            op={activeOp}
            roiNode={roiNode}
            runId={display.runId}
            status={display.status}
            nodeState={source.state}
            outputs={activeOutputs}
            onRunToNode={onRunToNode}
          />
        )}
        {roiEditing && activeNode && (
          <RoiLayer host={sceneHost} nodeId={activeNode.id} items={roiItems} />
        )}
        {!compareOn && !(content === "image" && activeNode && activeOp && roiNode) && (display.status || loading || (roiEditing && backdrop.error)) && (
          // 拖框时底图（模板）已经画出来了，状态只缩在角上，不盖住画面。底图取不到的原因也在这里说
          // （它比「未运行」有用：说的是要先跑哪一段）。说的是节点运行状态时跟着给下一步（运行到此、定位出错处）
          <ViewerStatus
            corner={roiEditing || !!display.busy}
            text={loading ? "正在取点云…" : roiEditing && backdrop.error ? backdrop.error : (display.status ?? "")}
            nodeId={activeNode && !loading && !(roiEditing && backdrop.error) ? display.nodeId : null}
            onRunToNode={onRunToNode}
          />
        )}
        {measureOn && (
          <MeasureReadout
            measure={measure}
            mode={cameraMode}
            compare={compareOn}
            pointCount={(cloud?.pointCount ?? 0) + (compareOn ? (cloudB?.pointCount ?? 0) : 0)}
            onClear={clearMeasure}
          />
        )}
      </div>
      {diff && <CompareDiff diff={diff} />}
    </div>
  );
}
