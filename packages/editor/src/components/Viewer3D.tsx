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
} from "../lib/cloudScene";
import { withBoundValues } from "../lib/graphParams";
import { effectiveParams } from "../lib/params";
import type { RampName } from "../lib/ramps";
import { copyFrameWrites, pickFrame, roiFramesOf } from "../lib/roiFrames";
import { rememberRoiBounds } from "../lib/roiThumbs";
import { disposeOverlay, extentOf, shapesOf } from "../lib/shapes2d";
import { fullId, levelOf, resolveOutput } from "../lib/subgraph";
import { exportCanvasPng } from "../lib/exportPng";
import { transport } from "../transport";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useGraphParamOverrides } from "../store/recipe";
import { useUiStore } from "../store/ui";
import { decodeCloud, type CloudPayload, type OutputStat } from "../types/execution";
import { RoiLayer, type RoiItem } from "./RoiLayer";
import { ValueView } from "./peek/ValueView";
import { useViewerSource } from "../hooks/useViewerSource";
import "../styles.viewer.css";

export type { ShadingMode, CameraMode } from "../lib/cloudScene";
export type { RampName } from "../lib/ramps";

const MAX_POINTS_CHOICES = [100_000, 500_000, 2_000_000, 8_000_000];

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

/** 主预览的「值」内容：每个输出端口一张表，与连线查看器的值视图是同一个组件。
 *  Bundle 字段展开出来的 `<port>.<field>` 不单列 —— 整个端口的值里已经有 fields。 */
function ValuePane({ outputs }: { outputs: readonly OutputStat[] | undefined }) {
  const shown = (outputs ?? []).filter((o) => !o.port.includes("."));
  return (
    <div className="viewer__values" data-testid="viewer-values">
      {shown.map((o) => (
        <section key={o.port} className="viewer__value" data-port={o.port}>
          <header className="viewer__value-head">
            <span>{o.port}</span>
            <span className="viewer__value-type">{o.type}</span>
          </header>
          <ValueView stat={o} />
        </section>
      ))}
    </div>
  );
}

export function Viewer3D() {
  const hostRef = useRef<HTMLDivElement>(null);
  const sceneRef = useRef<Scene | null>(null);

  const [shading, setShading] = useState<ShadingMode>("intensity");
  const [ramp, setRamp] = useState<RampName>("viridis");
  const [rangeAuto, setRangeAuto] = useState(true);
  const [manualRange, setManualRange] = useState<[number, number]>([0, 1]);
  const [pointSize, setPointSize] = useState(1.6);
  const pointSizeRef = useRef(1.6);
  // 相机模式在 ui store：参数面板的 ROI 行「进入拖框」要能把它切到 2D（param-recipe P2.7）
  const cameraMode = useUiStore((s) => s.viewerMode);
  const setCameraMode = useUiStore((s) => s.setViewerMode);
  const [maxPoints, setMaxPoints] = useState(2_000_000);
  // 只读地暴露给验收脚本：底图云与叠画几何各自的包围盒，用来断言两者在同一个平面上。
  const [overlayBounds, setOverlayBounds] = useState<Float32Array | null>(null);

  const selected = useUiStore((s) => s.selectedNodes);
  const path = useUiStore((s) => s.path);
  const pinnedId = useUiStore((s) => s.pinnedNode);
  const setPinnedId = useUiStore((s) => s.setPinnedNode);
  const doc = useGraphStore((s) => s.doc);
  const nodes = useMemo(() => levelOf(doc, path).nodes, [doc, path]);
  const runId = useExecutionStore((s) => s.runId);
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
  const { display, loading, node: activeNode, op: activeOp, outputs: activeOutputs, content } = source;
  const { cloud } = display;
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
  const backdropKey =
    roi && roi.files.length > 0
      ? JSON.stringify({ files: roi.files, graphPath })
      : roi && portSources
        ? JSON.stringify({ ports: portSources, inputs: roi.inputs, runId: runId ?? null, runStatus })
        : "";

  const hasIntensity = cloud?.intensity != null;
  const hasNormals = cloud?.normals != null;
  const hasRgb = cloud?.rgb != null;
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

  const dataRange = useMemo(
    () => dataRangeOf(cloud, effectiveShading),
    [cloud, effectiveShading],
  );
  const [lo, hi] = rangeAuto ? dataRange : manualRange;

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

  // -- 几何体：只随点云重建，着色参数一律不进这个 effect ---------------------
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;

    // 点大小从 ref 读初值：pointSize **不能**进这个 effect 的依赖，见下面那条注释
    setPoints(scene, 0, buildPoints(cloud, pointSizeRef.current));
  }, [cloud]);

  // 换着色模式/色带/范围只重写 color 属性，positions 和 boundingSphere 原样留着。
  useEffect(() => {
    const points = sceneRef.current?.points[0];
    if (!points || !cloud || cloud.pointCount === 0) return;
    paintPoints(points, cloud, { shading: effectiveShading, ramp, lo, hi });
  }, [cloud, effectiveShading, ramp, lo, hi]);

  // 点大小只改材质，不重建几何体 —— 它曾经也在上面那个 effect 的依赖里，
  // 拖一下滑块就要重分配 24MB 颜色数组、重扫两百万点（见 README「踩过的坑」）。
  useEffect(() => {
    pointSizeRef.current = pointSize;
    const p = sceneRef.current?.points[0];
    if (p) (p.material as THREE.PointsMaterial).size = pointSize;
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

  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    disposeOverlay(scene.overlay);
    if (overlayShapes.length === 0) return;
    // 线的长度/十字的大小要有个尺度参照：优先用点云的跨度，没有云就用几何自己的。
    const span = cloud
      ? Math.max(cloud.bounds[3]! - cloud.bounds[0]!, cloud.bounds[4]! - cloud.bounds[1]!, 1e-3)
      : Math.max(...overlayShapes.map(extentOf), 1e-3);
    for (const out of overlayShapes) {
      const hex = typesByName.get(out.type)?.color ?? "#6b7280";
      const color = new THREE.Color(hex).getHex();
      for (const line of shapesOf(out, color, span)) scene.overlay.add(line);
    }
  }, [overlayShapes, cloud, typesByName]);

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
    content === "cloud" && cameraMode === "2d" && roiItems.length > 0 && activeNode !== undefined;
  // 切换条只给带底图的组（有名字的）；数据坐标系那一组只有一组，不需要切
  const roiTabs = roiEditing && roiFrames.some((f) => f.label) ? roiFrames : [];

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

  // 换了云或换了几何就自动取景一次；同一份内容里调参数不该把视角拉回去。
  // 必须排在上面那个 effect 之后：取景要量的是它刚建好的那一组线。
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    setOverlayBounds(overlayBoundsOf(scene.overlay));
    const bounds = unionBounds(cloud, scene.overlay, scene.backdrop);
    if (bounds) fitToBounds(scene, bounds);
  }, [cloud, overlayShapes, backdrop.bounds]);

  // 相机模式（G7）。只换 controls 挂的那台相机，场景与几何原封不动。
  useEffect(() => {
    sceneRef.current?.setMode(cameraMode);
  }, [cameraMode]);

  const setRangeEnd = (end: 0 | 1, raw: string) => {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    setManualRange(end === 0 ? [v, hi] : [lo, v]);
    setRangeAuto(false);
  };

  const exportPng = async () => {
    const scene = sceneRef.current;
    if (!scene) return;
    // 读 buffer 前立刻重画一帧：换成 preserveDrawingBuffer 的话每一帧都要多付一次代价。
    scene.renderFrame();
    await exportCanvasPng(scene.renderer.domElement, display.nodeId ?? "view");
  };

  return (
    <div
      className="viewer"
      // 验收脚本靠这两个属性判断「视图已经切到这个节点了」，
      // 而不是去猜多久之后 React 会渲染完（scripts/e2e）。
      data-node={display.nodeId ?? ""}
      data-view={
        loading ? "loading" : content === "value" && !display.status ? "value" : cloud ? "cloud" : "empty"
      }
      data-content={content}
      data-shading={effectiveShading}
      data-pinned={pinnedId ? "1" : "0"}
      data-run={display.runId ?? ""}
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
          value={content === "value" ? "value" : cameraMode}
          onChange={(e) => {
            const v = e.target.value;
            if (v === "value") {
              if (activeKey) setContentPick({ nodeId: activeKey, content: "value" });
              return;
            }
            setCameraMode(v as CameraMode);
            if (activeKey && content === "value") setContentPick({ nodeId: activeKey, content: "cloud" });
          }}
          title="3D 自由视角 / 2D 正交俯视 XY（剖面）/ 输出值的表格。默认按节点的输出类型选，换节点就回到默认"
        >
          <option value="3d">3D</option>
          <option value="2d">2D 剖面</option>
          <option value="value" disabled={!activeKey}>
            值
          </option>
        </select>
        {content === "cloud" && (
          <>
            <select
              className="viewer__select"
              value={maxPoints}
              onChange={(e) => setMaxPoints(Number(e.target.value))}
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
              onChange={(e) => setPointSize(Number(e.target.value))}
              title="点大小"
            />
            <button
              type="button"
              className="viewer__fit"
              onClick={() => {
                const scene = sceneRef.current;
                const bounds = scene ? unionBounds(cloud, scene.overlay, scene.backdrop) : null;
                if (scene && bounds) fitToBounds(scene, bounds);
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
              onChange={(e) => setShading(e.target.value as ShadingMode)}
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
              onChange={(e) => setRamp(e.target.value as RampName)}
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
              value={rangeAuto ? round3(lo) : manualRange[0]}
              onChange={(e) => setRangeEnd(0, e.target.value)}
              disabled={noRamp}
              title="着色范围下限"
            />
            <input
              className="viewer__num"
              data-testid="viewer-range-max"
              type="number"
              step="any"
              value={rangeAuto ? round3(hi) : manualRange[1]}
              onChange={(e) => setRangeEnd(1, e.target.value)}
              disabled={noRamp}
              title="着色范围上限"
            />
            <button
              type="button"
              className="viewer__btn"
              data-testid="viewer-range-auto"
              onClick={() => setRangeAuto(true)}
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
        {content === "cloud" && (
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
        {content === "value" && !display.status && !loading && <ValuePane outputs={activeOutputs} />}
        {roiEditing && activeNode && (
          <RoiLayer host={sceneHost} nodeId={activeNode.id} items={roiItems} />
        )}
        {(display.status || loading || (roiEditing && backdrop.error)) && (
          // 拖框时底图（模板）已经画出来了，状态只缩在角上，不盖住画面。底图取不到的原因也在这里说
          <div
            className={`viewer__empty${roiEditing ? " viewer__empty--corner" : ""}`}
            data-testid="viewer3d-status"
          >
            {/* 拖框时底图取不到的原因比「未运行」有用：它说的是要先跑哪一段 */}
            {loading ? "正在取点云…" : roiEditing && backdrop.error ? backdrop.error : display.status}
          </div>
        )}
      </div>
    </div>
  );
}
