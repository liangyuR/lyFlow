// 3D 点云预览（交互清单 P1 #30）。three.js 随包打、**不走 CDN**：桌面应用断网也得能用。
// 点云走二进制 IPC，`decodeCloud` 给的 Float32Array 是缓冲上的**视图**，全程零拷贝（ADR-0006）。

import { useEffect, useMemo, useRef, useState } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";

import { findBaseCloud, firstCloudPort, type BaseCloud } from "../lib/basecloud";
import { augmentOperators, levelOf, resolveOutput } from "../lib/subgraph";
import { dialogs } from "../lib/dialogs";
import { transport } from "../transport";
import { aggregatedNodes, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import { decodeCloud, type CloudPayload, type OutputStat } from "../types/execution";
import "../styles.viewer.css";

export type ShadingMode = "intensity" | "height" | "normal" | "flat";
export type RampName = "viridis" | "gray" | "jet";
/** 相机模式（G7）。2d = 正交俯视 XY，看剖面用。 */
export type CameraMode = "3d" | "2d";

/** 当前展示的东西。三者永远一起换，见 Viewer3D 里的注释。 */
interface Display {
  nodeId: string | null;
  /** 这片云属于哪一次运行。验收脚本用它量「事件到渲染」的延迟。 */
  runId: string | null;
  cloud: CloudPayload | null;
  status: string | null;
  /** 这片云是从上游借来的底图时，借的是谁。自己有云时为 null。 */
  base: BaseCloud | null;
}

const MAX_POINTS_CHOICES = [100_000, 500_000, 2_000_000, 8_000_000];

/** 已取回的点云缓存。键是 runId+node+port+maxPoints，少任何一段都会串味：
 *  少 runId 会在重跑后拿到上次结果，少 maxPoints 会让滑块拖了没反应。 */
const cloudCache = new Map<string, CloudPayload>();

/** 缓存的**字节**预算，不是条数预算：8M 点一条就是 96MB 坐标 + 32MB 强度，
 *  按条数封顶的话 8 条能攒到 1GB。 */
const CACHE_BYTES = 256 * 1024 * 1024;

function payloadBytes(p: CloudPayload) {
  return p.xyz.byteLength + (p.intensity?.byteLength ?? 0);
}

function cacheKey(runId: string, nodeId: string, port: string, maxPoints: number) {
  return `${runId}|${nodeId}|${port}|${maxPoints}`;
}

/** 换了一次运行就把旧运行的条目全丢掉 —— 它们再也不会被命中。 */
function dropOtherRuns(runId: string) {
  for (const key of [...cloudCache.keys()]) {
    if (!key.startsWith(`${runId}|`)) cloudCache.delete(key);
  }
}

function putCache(key: string, payload: CloudPayload) {
  // delete + set 让它变成真正的 LRU：Map.set 命中已有键时不会调整顺序，
  // 少了这一行，你来回切着看的那片云恰恰是最先被淘汰的那个。
  cloudCache.delete(key);
  cloudCache.set(key, payload);
  let total = 0;
  for (const p of cloudCache.values()) total += payloadBytes(p);
  while (total > CACHE_BYTES && cloudCache.size > 1) {
    const oldest = cloudCache.keys().next().value;
    if (oldest === undefined) break;
    total -= payloadBytes(cloudCache.get(oldest)!);
    cloudCache.delete(oldest);
  }
}

interface Scene {
  renderer: THREE.WebGLRenderer;
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  /** 2D 剖面相机（G7）：正交、俯视 XY、不许旋转。 */
  ortho: THREE.OrthographicCamera;
  controls: OrbitControls;
  mode: CameraMode;
  points: THREE.Points | null;
  /** 叠画的 2D 几何（G7）。整组一起换，不逐个增删。 */
  overlay: THREE.Group;
  /** 正交相机的可视半宽，随 fit 改变；aspect 变了要重算上下边。 */
  halfWidth: number;
  aspect: number;
  active(): THREE.Camera;
  applyOrtho(): void;
  setMode(mode: CameraMode): void;
  dispose(): void;
}

function createScene(host: HTMLDivElement): Scene {
  const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.setClearColor(0x11141a, 1);
  host.appendChild(renderer.domElement);

  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(50, 1, 0.001, 10_000);
  camera.up.set(0, 0, 1); // 点云世界里 Z 朝上，别用 three 默认的 Y 朝上
  camera.position.set(2, -2, 1.5);

  // 正交相机看 −Z 方向，up 是 +Y：屏幕上就是标准的 XY 平面。
  const ortho = new THREE.OrthographicCamera(-1, 1, 1, -1, -1000, 1000);
  ortho.up.set(0, 1, 0);
  ortho.position.set(0, 0, 10);

  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.12;

  const grid = new THREE.GridHelper(4, 16, 0x33404f, 0x232a33);
  grid.rotation.x = Math.PI / 2; // GridHelper 默认躺在 XZ 面上，转到 XY
  scene.add(grid);
  const axes = new THREE.AxesHelper(0.5);
  scene.add(axes);
  const overlay = new THREE.Group();
  scene.add(overlay);

  let raf = 0;
  const tick = () => {
    raf = requestAnimationFrame(tick);
    controls.update();
    renderer.render(scene, state.active());
  };

  const state: Scene = {
    renderer,
    scene,
    camera,
    ortho,
    controls,
    mode: "3d",
    points: null,
    overlay,
    halfWidth: 2,
    aspect: 1,
    active() {
      return state.mode === "2d" ? state.ortho : state.camera;
    },
    applyOrtho() {
      const h = state.halfWidth / Math.max(state.aspect, 1e-3);
      ortho.left = -state.halfWidth;
      ortho.right = state.halfWidth;
      ortho.top = h;
      ortho.bottom = -h;
      ortho.updateProjectionMatrix();
    },
    setMode(mode) {
      if (state.mode === mode) return;
      state.mode = mode;
      // OrbitControls 只认它构造时那台相机，换模式就换 object；
      // 2D 下关掉旋转，否则一拖就离开了 XY 平面，那这个模式就没意义了。
      const target = state.controls.target;
      state.controls.object = state.active() as THREE.PerspectiveCamera;
      state.controls.enableRotate = mode === "3d";
      if (mode === "2d") {
        ortho.position.set(target.x, target.y, target.z + 10);
        ortho.zoom = 1;
        state.applyOrtho();
      }
      state.controls.update();
    },
    dispose() {
      cancelAnimationFrame(raf);
      controls.dispose();
      if (state.points) {
        state.points.geometry.dispose();
        (state.points.material as THREE.Material).dispose();
      }
      disposeOverlay(overlay);
      // helper 自己也有 geometry 和 material。不放的话每次挂载都漏一份。
      for (const helper of [grid, axes]) {
        helper.geometry.dispose();
        const m = helper.material;
        if (Array.isArray(m)) m.forEach((x) => x.dispose());
        else m.dispose();
      }
      // renderer.dispose() **不释放 WebGL 上下文**（那是 forceContextLoss），
      // 少了它每次挂载/卸载漏一个，攒够十几个后视图突然全黑（见 README「踩过的坑」）。
      renderer.dispose();
      renderer.forceContextLoss();
      host.removeChild(renderer.domElement);
    },
  };
  tick();
  return state;
}

// --------------------------------------------------------- 2D 几何叠画（G7）

/** 叠画一条折线/线段集合。z 全部为 0：这些几何本来就定义在 XY 平面上。 */
function polyline(points: number[], color: number, loop: boolean): THREE.Line {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array(points), 3));
  const material = new THREE.LineBasicMaterial({ color, depthTest: false, transparent: true });
  const line = loop ? new THREE.LineLoop(geometry, material) : new THREE.Line(geometry, material);
  line.renderOrder = 10; // 永远画在点云之上，否则细线会被点糊掉
  return line;
}

function disposeOverlay(group: THREE.Group) {
  for (const child of [...group.children]) {
    group.remove(child);
    const line = child as THREE.Line;
    line.geometry?.dispose();
    const m = line.material as THREE.Material | THREE.Material[];
    if (Array.isArray(m)) m.forEach((x) => x.dispose());
    else m?.dispose();
  }
}

/** 这个几何自己有多大。没有点云做尺度参照时拿它当兜底。 */
function extentOf(out: OutputStat): number {
  const v = out.value;
  if (!v) return 0;
  if (v.kind === "Box2D" && Array.isArray(v.min) && Array.isArray(v.max)) {
    return Math.max(Math.abs(v.max[0] - v.min[0]), Math.abs(v.max[1] - v.min[1]));
  }
  if (v.kind === "Circle2D") return (v.radius ?? 0) * 4;
  if (v.kind === "Line2D" && v.hasSegment && v.start && v.end) {
    return Math.hypot(v.end[0] - v.start[0], v.end[1] - v.start[1]);
  }
  return 0;
}

/** 一个输出值 → 若干条线。认不出的 kind 返回空数组（前端不硬编码算子，也不该硬编码到崩）。 */
function shapesOf(out: OutputStat, color: number, span: number): THREE.Line[] {
  const v = out.value;
  if (!v) return [];
  switch (v.kind) {
    case "Box2D": {
      const [x0, y0] = Array.isArray(v.min) ? v.min : [0, 0];
      const [x1, y1] = Array.isArray(v.max) ? v.max : [0, 0];
      return [polyline([x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0], color, true)];
    }
    case "Line2D": {
      if (v.hasSegment && v.start && v.end) {
        return [polyline([v.start[0], v.start[1], 0, v.end[0], v.end[1], 0], color, false)];
      }
      const [px, py] = v.point ?? [0, 0];
      const [dx, dy] = v.dir ?? [1, 0];
      const n = Math.hypot(dx, dy) || 1;
      const h = span / 2;
      return [
        polyline(
          [px - (dx / n) * h, py - (dy / n) * h, 0, px + (dx / n) * h, py + (dy / n) * h, 0],
          color,
          false,
        ),
      ];
    }
    case "Circle2D": {
      const [cx, cy] = v.center ?? [0, 0];
      const r = v.radius ?? 0;
      const pts: number[] = [];
      const SEGMENTS = 72;
      for (let i = 0; i < SEGMENTS; i += 1) {
        const t = (i / SEGMENTS) * Math.PI * 2;
        pts.push(cx + r * Math.cos(t), cy + r * Math.sin(t), 0);
      }
      return [polyline(pts, color, true)];
    }
    case "Point2D": {
      const [x, y] = v.p ?? [0, 0];
      // 十字而不是一个点：单个 Point 在细线材质下根本看不见
      const s = span * 0.01 || 0.001;
      return [
        polyline([x - s, y, 0, x + s, y, 0], color, false),
        polyline([x, y - s, 0, x, y + s, 0], color, false),
      ];
    }
    default:
      return [];
  }
}

/** matplotlib viridis 的 11 个采样点，线性插值就够看。 */
const VIRIDIS: [number, number, number][] = [
  [0.267, 0.005, 0.329],
  [0.283, 0.141, 0.458],
  [0.254, 0.265, 0.53],
  [0.207, 0.372, 0.553],
  [0.164, 0.471, 0.558],
  [0.128, 0.567, 0.551],
  [0.135, 0.659, 0.518],
  [0.267, 0.749, 0.441],
  [0.478, 0.821, 0.318],
  [0.741, 0.873, 0.15],
  [0.993, 0.906, 0.144],
];

function viridisRamp(t: number, out: THREE.Color) {
  const x = Math.max(0, Math.min(1, t)) * (VIRIDIS.length - 1);
  const i = Math.min(VIRIDIS.length - 2, Math.floor(x));
  const f = x - i;
  const a = VIRIDIS[i]!;
  const b = VIRIDIS[i + 1]!;
  out.setRGB(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f);
}

/** 灰度。打印和做对比图时比彩色可靠。 */
function grayRamp(t: number, out: THREE.Color) {
  const v = 0.12 + 0.85 * Math.max(0, Math.min(1, t));
  out.setRGB(v, v, v);
}

/** 蓝→青→黄→红。饱和度高，找异常点最快。 */
function jetRamp(t: number, out: THREE.Color) {
  const x = Math.max(0, Math.min(1, t));
  if (x < 0.5) out.setRGB(0.15 + 0.1 * x, 0.4 + 1.2 * x, 1.0 - 0.6 * x);
  else out.setRGB(0.35 + 1.3 * (x - 0.5), 1.0 - 1.2 * (x - 0.5), 0.4 - 0.7 * (x - 0.5));
}

const RAMPS: Record<RampName, (t: number, out: THREE.Color) => void> = {
  viridis: viridisRamp,
  gray: grayRamp,
  jet: jetRamp,
};

/** 着色用的标量：强度模式取强度通道，其余取 Z。 */
function shadingValue(cloud: CloudPayload, mode: ShadingMode, i: number): number {
  if (mode === "intensity" && cloud.intensity) return cloud.intensity[i]!;
  return cloud.xyz[i * 3 + 2]!;
}

/** 法线着色：分量的绝对值直接当 RGB。色带对它没有意义，所以走单独一条路。 */
function writeNormalColors(out: Float32Array, cloud: CloudPayload): void {
  const n = cloud.normals;
  if (!n) return;
  for (let i = 0; i < cloud.pointCount; i += 1) {
    out[i * 3] = Math.abs(n[i * 3] ?? 0);
    out[i * 3 + 1] = Math.abs(n[i * 3 + 1] ?? 0);
    out[i * 3 + 2] = Math.abs(n[i * 3 + 2] ?? 0);
  }
}

/** 就地写颜色。复用已有数组是为了换色带时不再分配几十兆。 */
function writeColors(
  out: Float32Array,
  cloud: CloudPayload,
  mode: ShadingMode,
  ramp: RampName,
  lo: number,
  hi: number,
) {
  const paint = RAMPS[ramp];
  const span = hi - lo || 1;
  const c = new THREE.Color();
  for (let i = 0; i < cloud.pointCount; i += 1) {
    paint((shadingValue(cloud, mode, i) - lo) / span, c);
    out[i * 3] = c.r;
    out[i * 3 + 1] = c.g;
    out[i * 3 + 2] = c.b;
  }
}

/** 该模式下数据的实际取值范围，「自动」按钮和范围输入框的初值都用它。 */
function dataRangeOf(cloud: CloudPayload | null, mode: ShadingMode): [number, number] {
  if (!cloud || cloud.pointCount === 0) return [0, 1];
  if (mode === "intensity" && cloud.intensity) {
    let lo = Infinity;
    let hi = -Infinity;
    for (let i = 0; i < cloud.pointCount; i += 1) {
      const v = cloud.intensity[i]!;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    return Number.isFinite(lo) ? [lo, hi] : [0, 1];
  }
  // 高度用 bounds 而不是重新扫一遍：bounds 是**全量**点云算的，
  // 抽稀后的极值会让同一份数据在不同 maxPoints 下呈现不同的配色。
  return [cloud.bounds[2]!, cloud.bounds[5]!];
}

function round3(v: number) {
  return Number.isFinite(v) ? Math.round(v * 1000) / 1000 : 0;
}

/** 「底图云 + 叠画几何」的联合包围盒。取并集而不是二选一：几何再小也挤不掉云，
 *  云再大也不会把 ROI 框推出画面。两者都空时返回 null。 */
function unionBounds(cloud: CloudPayload | null, overlay: THREE.Group): Float32Array | null {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  if (cloud && cloud.pointCount > 0) {
    for (let i = 0; i < 3; i += 1) {
      min[i] = Math.min(min[i]!, cloud.bounds[i]!);
      max[i] = Math.max(max[i]!, cloud.bounds[i + 3]!);
    }
  }
  if (overlay.children.length > 0) {
    const box = new THREE.Box3().setFromObject(overlay);
    if (!box.isEmpty()) {
      const lo = [box.min.x, box.min.y, box.min.z];
      const hi = [box.max.x, box.max.y, box.max.z];
      for (let i = 0; i < 3; i += 1) {
        min[i] = Math.min(min[i]!, lo[i]!);
        max[i] = Math.max(max[i]!, hi[i]!);
      }
    }
  }
  if (!Number.isFinite(min[0]) || !Number.isFinite(max[0])) return null;
  return new Float32Array([min[0]!, min[1]!, min[2]!, max[0]!, max[1]!, max[2]!]);
}

/** 叠画几何自己的包围盒。空组返回 null。 */
function overlayBoundsOf(overlay: THREE.Group): Float32Array | null {
  if (overlay.children.length === 0) return null;
  const box = new THREE.Box3().setFromObject(overlay);
  if (box.isEmpty()) return null;
  return new Float32Array([box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z]);
}

/** 包围盒 → data- 属性上的六个数。null 是空串。 */
function boundsAttr(bounds: ArrayLike<number> | null): string {
  if (!bounds) return "";
  return Array.from(bounds, round3).join(",");
}

function fitToBounds(scene: Scene, bounds: Float32Array) {
  const cx = (bounds[0]! + bounds[3]!) / 2;
  const cy = (bounds[1]! + bounds[4]!) / 2;
  const cz = (bounds[2]! + bounds[5]!) / 2;
  const size = Math.max(
    bounds[3]! - bounds[0]!,
    bounds[4]! - bounds[1]!,
    bounds[5]! - bounds[2]!,
    1e-3,
  );
  const d = size * 1.8;
  scene.controls.target.set(cx, cy, cz);
  scene.camera.position.set(cx + d, cy - d, cz + d * 0.7);
  scene.camera.near = size / 1000;
  scene.camera.far = size * 100;
  scene.camera.updateProjectionMatrix();

  // 正交相机按 XY 的实际跨度取景，Z 不参与 —— 剖面视图里 Z 是「厚度」。
  const spanX = Math.max(bounds[3]! - bounds[0]!, 1e-4);
  const spanY = Math.max(bounds[4]! - bounds[1]!, 1e-4);
  scene.halfWidth = Math.max(spanX, spanY * Math.max(scene.aspect, 1e-3)) * 0.6;
  scene.ortho.position.set(cx, cy, cz + Math.max(size * 10, 1));
  scene.ortho.zoom = 1;
  scene.applyOrtho();
  scene.controls.update();
}

export function Viewer3D() {
  const hostRef = useRef<HTMLDivElement>(null);
  const sceneRef = useRef<Scene | null>(null);

  const [shading, setShading] = useState<ShadingMode>("intensity");
  const [ramp, setRamp] = useState<RampName>("viridis");
  const [rangeAuto, setRangeAuto] = useState(true);
  const [manualRange, setManualRange] = useState<[number, number]>([0, 1]);
  const [pinnedId, setPinnedId] = useState<string | null>(null);
  const [pointSize, setPointSize] = useState(1.6);
  const pointSizeRef = useRef(1.6);
  const [cameraMode, setCameraMode] = useState<CameraMode>("3d");
  const [maxPoints, setMaxPoints] = useState(2_000_000);
  // 云、状态、以及**它属于哪个节点**必须一起换：拆成三个 useState 的话，切换节点时会出现
  // 「标题是新节点、点云还是旧节点」的中间态 —— 肉眼看不见，但验收脚本会稳定读到它。
  const [display, setDisplay] = useState<Display>({
    nodeId: null,
    runId: null,
    cloud: null,
    status: "未运行",
    base: null,
  });
  const [loading, setLoading] = useState(false);
  // 只读地暴露给验收脚本：底图云与叠画几何各自的包围盒，用来断言两者在同一个平面上。
  const [overlayBounds, setOverlayBounds] = useState<Float32Array | null>(null);
  const { cloud } = display;

  const selected = useUiStore((s) => s.selectedNodes);
  const path = useUiStore((s) => s.path);
  const doc = useGraphStore((s) => s.doc);
  const nodes = useMemo(() => levelOf(doc, path).nodes, [doc, path]);
  const runId = useExecutionStore((s) => s.runId);
  const runStatus = useExecutionStore((s) => s.runStatus);
  const isPreview = useExecutionStore((s) => s.preview);
  const previewMaxPoints = useUiStore((s) => s.previewMaxPoints);

  const selectedId = selected.size === 1 ? [...selected][0]! : null;
  // 钉住优先于选中：钉住期间在画布上点别的节点，视图不跟着走（§2.6）。
  const activeId = pinnedId ?? selectedId;
  const activeNode = useMemo(
    () => (activeId ? nodes.find((n) => n.id === activeId) : undefined),
    [activeId, nodes],
  );
  // 只订阅**这一个节点的状态字符串**，不要订阅整张 nodes Map ——
  // 那张 Map 每来一条事件就是新引用，会排起一队几十兆的 IPC（见 README「踩过的坑」）。
  const activeState = useExecutionStore((s) =>
    activeId ? aggregatedNodes(path, s.nodes).get(activeId)?.state : undefined,
  );
  // 叠画用的非点云输出（G7）。stats 是事件里那一份，引用稳定，不会每帧新建。
  const activeOutputs = useExecutionStore((s) =>
    activeId ? aggregatedNodes(path, s.nodes).get(activeId)?.stats?.outputs : undefined,
  );
  const typesByName = useManifestStore((s) => s.typesByName);
  const operatorsById = useManifestStore((s) => s.operatorsById);
  const ops = useMemo(
    () => augmentOperators(operatorsById, doc.subgraphs),
    [operatorsById, doc.subgraphs],
  );

  const hasIntensity = cloud?.intensity != null;
  const hasNormals = cloud?.normals != null;
  // 选了「强度」但这片云没有强度通道时实际走高度着色，那就让下拉框也显示「高度」——
  // 下拉框写着强度、画面却是高度，用户只会以为强度数据本身有问题。
  const effectiveShading: ShadingMode =
    (shading === "intensity" && !hasIntensity) || (shading === "normal" && !hasNormals)
      ? "height"
      : shading;
  const isFlat = effectiveShading === "flat";
  const isNormalShading = effectiveShading === "normal";

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

    const resize = () => {
      const w = host.clientWidth || 1;
      const h = host.clientHeight || 1;
      scene.renderer.setSize(w, h, false);
      scene.camera.aspect = w / h;
      scene.camera.updateProjectionMatrix();
      scene.aspect = w / h;
      scene.applyOrtho();
    };
    resize();
    const observer = new ResizeObserver(resize);
    observer.observe(host);

    return () => {
      observer.disconnect();
      scene.dispose();
      sceneRef.current = null;
    };
  }, []);

  // -- 取点云 ---------------------------------------------------------------
  useEffect(() => {
    let cancelled = false;
    const show = (
      status: string | null,
      payload: CloudPayload | null = null,
      base: BaseCloud | null = null,
    ) => {
      if (cancelled) return;
      setDisplay({ nodeId: activeId, runId: runId ?? null, cloud: payload, status, base });
    };

    if (!activeNode) {
      setLoading(false);
      show(selected.size > 1 ? "选中了多个节点" : "选中一个节点查看它的输出");
      return;
    }
    if (!runId || runStatus === "idle") {
      setLoading(false);
      show("未运行");
      return;
    }
    if (activeState === "error") {
      setLoading(false);
      show("该节点运行出错");
      return;
    }
    if (activeState !== "done" && activeState !== "skipped") {
      setLoading(false);
      show(activeState === "running" ? "正在计算…" : "该节点尚未产出结果");
      return;
    }
    // 自己有云就用自己的；没有就沿输入边往上游借最近的一片当底图，几何叠在它上面 ——
    // 只输出 Box2D/Line2D 的节点若显示成空白，用户就看不出框压在剖面的哪里。
    const port = firstCloudPort(ops, activeNode.op);
    let base: BaseCloud | null = null;
    // 子图节点的结果在内部那个叶子上，按路径查结果仓（F2）
    let resolved = port ? resolveOutput(doc, path, activeNode.id, port) : null;
    if (port && !resolved) {
      setLoading(false);
      show("这个算子的内部结果查不到（库算子的定义在库文件里）");
      return;
    }
    if (!port) {
      base = findBaseCloud(doc, path, activeNode.id, ops);
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
      show(hit.pointCount === 0 ? "该节点的点云是空的" : null, hit, base);
      return;
    }

    setLoading(true);
    void (async () => {
      try {
        // 预览时没必要拉超过预览点数的量：那条路径上本来就不会有更多点
        const cap = isPreview ? Math.min(maxPoints, previewMaxPoints) : maxPoints;
        const buffer = await transport.getOutputCloud(
          runId,
          resolved.nodeId,
          resolved.port,
          cap,
        );
        if (cancelled) return;
        const payload = decodeCloud(buffer);
        putCache(key, payload);
        show(payload.pointCount === 0 ? "该节点的点云是空的" : null, payload, base);
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
  }, [activeNode, activeId, selected.size, runId, runStatus, activeState, maxPoints, doc, path,
      isPreview, previewMaxPoints, ops]);

  // -- 几何体：只随点云重建，着色参数一律不进这个 effect ---------------------
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;

    if (scene.points) {
      scene.scene.remove(scene.points);
      scene.points.geometry.dispose();
      (scene.points.material as THREE.Material).dispose();
      scene.points = null;
    }
    if (!cloud || cloud.pointCount === 0) return;

    const geometry = new THREE.BufferGeometry();
    // 零拷贝：cloud.xyz 就是 IPC 缓冲上的视图
    geometry.setAttribute("position", new THREE.BufferAttribute(cloud.xyz, 3));
    // 自己算 boundingSphere：让 three 从 attribute 里算一遍是白花的钱，
    // 而且 bounds 是全量点云的，比抽样后的更准。
    const cx = (cloud.bounds[0]! + cloud.bounds[3]!) / 2;
    const cy = (cloud.bounds[1]! + cloud.bounds[4]!) / 2;
    const cz = (cloud.bounds[2]! + cloud.bounds[5]!) / 2;
    const radius =
      Math.hypot(
        cloud.bounds[3]! - cloud.bounds[0]!,
        cloud.bounds[4]! - cloud.bounds[1]!,
        cloud.bounds[5]! - cloud.bounds[2]!,
      ) / 2 || 1;
    geometry.boundingSphere = new THREE.Sphere(new THREE.Vector3(cx, cy, cz), radius);

    const material = new THREE.PointsMaterial({
      // 从 ref 读初值：pointSize **不能**进这个 effect 的依赖，见下面那条注释
      size: pointSizeRef.current,
      sizeAttenuation: false,
      vertexColors: false,
      color: 0x8fb8ff,
    });
    const points = new THREE.Points(geometry, material);
    scene.scene.add(points);
    scene.points = points;
  }, [cloud]);

  // 换着色模式/色带/范围只重写 color 属性，positions 和 boundingSphere 原样留着。
  useEffect(() => {
    const points = sceneRef.current?.points;
    if (!points || !cloud || cloud.pointCount === 0) return;
    const geometry = points.geometry;
    const material = points.material as THREE.PointsMaterial;

    if (isFlat) {
      geometry.deleteAttribute("color");
      material.vertexColors = false;
      material.color.setHex(0x8fb8ff);
      material.needsUpdate = true;
      return;
    }

    const prev = geometry.getAttribute("color");
    const reuse =
      prev instanceof THREE.BufferAttribute &&
      prev.count === cloud.pointCount &&
      prev.array instanceof Float32Array
        ? prev
        : null;
    const arr = reuse ? (reuse.array as Float32Array) : new Float32Array(cloud.pointCount * 3);
    if (isNormalShading) writeNormalColors(arr, cloud);
    else writeColors(arr, cloud, effectiveShading, ramp, lo, hi);
    if (reuse) reuse.needsUpdate = true;
    else geometry.setAttribute("color", new THREE.BufferAttribute(arr, 3));
    material.vertexColors = true;
    material.color.setHex(0xffffff);
    material.needsUpdate = true;
  }, [cloud, effectiveShading, isFlat, isNormalShading, ramp, lo, hi]);

  // 点大小只改材质，不重建几何体 —— 它曾经也在上面那个 effect 的依赖里，
  // 拖一下滑块就要重分配 24MB 颜色数组、重扫两百万点（见 README「踩过的坑」）。
  useEffect(() => {
    pointSizeRef.current = pointSize;
    const p = sceneRef.current?.points;
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

  // 换了云或换了几何就自动取景一次；同一份内容里调参数不该把视角拉回去。
  // 必须排在上面那个 effect 之后：取景要量的是它刚建好的那一组线。
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    setOverlayBounds(overlayBoundsOf(scene.overlay));
    const bounds = unionBounds(cloud, scene.overlay);
    if (bounds) fitToBounds(scene, bounds);
  }, [cloud, overlayShapes]);

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
    scene.renderer.render(scene.scene, scene.camera);
    const url = scene.renderer.domElement.toDataURL("image/png");
    const stamp = new Date().toISOString().replace(/[-:]/g, "").replace(/\..*$/, "");
    const name = (display.nodeId ?? "view").replace(/[^\w.-]+/g, "_");
    const file = `${name}-${stamp}.png`;

    const pickPath = dialogs().pickPath;
    if (!pickPath) {
      // 宿主没有保存对话框，退回让浏览器自己下载
      const a = document.createElement("a");
      a.href = url;
      a.download = file;
      document.body.appendChild(a);
      a.click();
      a.remove();
      return;
    }
    try {
      const picked = await pickPath({
        mode: "save",
        defaultPath: file,
        filters: [{ name: "PNG", extensions: ["png"] }],
      });
      if (typeof picked !== "string") return;
      const base64 = url.slice(url.indexOf(",") + 1);
      const binary = atob(base64);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
      await transport.writeFileBytes(picked, bytes);
      useUiStore.getState().showToast(`已导出 ${picked}`);
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
    }
  };

  return (
    <div
      className="viewer"
      // 验收脚本靠这两个属性判断「视图已经切到这个节点了」，
      // 而不是去猜多久之后 React 会渲染完（scripts/e2e）。
      data-node={display.nodeId ?? ""}
      data-view={loading ? "loading" : cloud ? "cloud" : "empty"}
      data-shading={effectiveShading}
      data-pinned={pinnedId ? "1" : "0"}
      data-run={display.runId ?? ""}
      data-preview={isPreview ? "1" : "0"}
      data-camera={cameraMode}
      data-overlay={overlayCount}
      data-base={display.base?.localId ?? ""}
      data-cloud-bounds={boundsAttr(cloud && cloud.pointCount > 0 ? cloud.bounds : null)}
      data-overlay-bounds={boundsAttr(overlayBounds)}
    >
      <div className="viewer__bar">
        <span className="viewer__title">3D 预览</span>
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
          value={cameraMode}
          onChange={(e) => setCameraMode(e.target.value as CameraMode)}
          title="相机：3D 自由视角 / 2D 正交俯视 XY（剖面）"
        >
          <option value="3d">3D</option>
          <option value="2d">2D 剖面</option>
        </select>
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
            const bounds = scene ? unionBounds(cloud, scene.overlay) : null;
            if (scene && bounds) fitToBounds(scene, bounds);
          }}
          disabled={!cloud && overlayCount === 0}
          title="缩放到全部（底图云 + 叠画几何）"
        >
          ⤢
        </button>
      </div>

      <div className="viewer__bar viewer__bar--tools">
        <span className="viewer__label">着色</span>
        <select
          className="viewer__select"
          data-testid="viewer-shading"
          value={effectiveShading}
          onChange={(e) => setShading(e.target.value as ShadingMode)}
          title={
            shading === effectiveShading
              ? "着色方式"
              : "这片点云没有该通道，已退回高度着色"
          }
        >
          <option value="intensity" disabled={!hasIntensity}>
            强度{hasIntensity ? "" : "（无）"}
          </option>
          <option value="height">高度</option>
          <option value="normal" disabled={!hasNormals} title="需要点云带法线通道">
            法线{hasNormals ? "" : "（无）"}
          </option>
          <option value="flat">单色</option>
        </select>
        <select
          className="viewer__select"
          data-testid="viewer-ramp"
          value={ramp}
          onChange={(e) => setRamp(e.target.value as RampName)}
          disabled={isFlat}
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
          disabled={isFlat}
          title="着色范围下限"
        />
        <input
          className="viewer__num"
          data-testid="viewer-range-max"
          type="number"
          step="any"
          value={rangeAuto ? round3(hi) : manualRange[1]}
          onChange={(e) => setRangeEnd(1, e.target.value)}
          disabled={isFlat}
          title="着色范围上限"
        />
        <button
          type="button"
          className="viewer__btn"
          data-testid="viewer-range-auto"
          onClick={() => setRangeAuto(true)}
          disabled={isFlat || rangeAuto}
          title="范围回到数据实际的最小/最大"
        >
          自动
        </button>
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
          data-testid="viewer-export"
          onClick={() => void exportPng()}
          title="把当前画面存成 PNG"
        >
          PNG
        </button>
      </div>

      <div className="viewer__stage">
        <div className="viewer__canvas" ref={hostRef} data-testid="viewer3d-canvas" />
        {(display.status || loading) && (
          <div className="viewer__empty" data-testid="viewer3d-status">
            {loading ? "正在取点云…" : display.status}
          </div>
        )}
      </div>
    </div>
  );
}
