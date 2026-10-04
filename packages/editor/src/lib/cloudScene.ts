// 点云预览的 three.js 场景（从 Viewer3D 抽出来，docs/compare-plan.md §3.3）。一个 WebGLRenderer、
// 一块画布；对比模式下 views = 2，同一台相机按两个 scissor 视口各画一遍（C4）—— 相机同步不用「镜像」，
// 本来就是同一台，WebGL 上下文数也不变。views = 1 时与抽出来之前逐行同一行为。

import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";

import type { Viewport } from "./pick";
import { RAMPS, writeRgbColors, type RampName } from "./ramps";
import { disposeOverlay } from "./shapes2d";
import type { CloudPayload } from "../types/execution";
import { gridSpec, ISO_DIR, presetPosition, type GridSpec, type ViewPreset } from "./viewFit";

export type ShadingMode = "intensity" | "height" | "normal" | "rgb" | "flat";
/** 相机模式（G7）。2d = 正交俯视 XY，看剖面用。 */
export type CameraMode = "3d" | "2d";
/** 两栏怎么摆：左右（默认）或上下（容器太窄时）。 */
export type SplitMode = "lr" | "tb";

export interface Scene {
  renderer: THREE.WebGLRenderer;
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  /** 2D 剖面相机（G7）：正交、俯视 XY、不许旋转。 */
  ortho: THREE.OrthographicCamera;
  controls: OrbitControls;
  mode: CameraMode;
  /** 每栏一片云；views = 1 时只用第 0 个。 */
  points: [THREE.Points | null, THREE.Points | null];
  /** 每栏一组叠画的 2D 几何（G7）。整组一起换，不逐个增删。 */
  overlays: [THREE.Group, THREE.Group];
  /** = overlays[0]。单栏的代码只认它。 */
  overlay: THREE.Group;
  /** 拖框的底图（m8-plan L15）：roiBackdrop 指的那几个文件拼起来的云，例如模板。只在单栏用。 */
  backdrop: THREE.Group;
  /** 测量的标记点与连线（measure-plan M6）。不属于任何一栏：两栏都画，同一个世界坐标。 */
  measure: THREE.Group;
  /** 此刻的网格（跟着云走，lib/viewFit 的 gridSpec）。 */
  grid: GridSpec | null;
  /** 换网格：格子大小或格数变了才重建，位置每次都挪。null = 不动。 */
  setGrid(spec: GridSpec | null): void;
  /** 每帧渲染前调一遍。RoiLayer 靠它把 DOM 框跟着相机摆位。 */
  frameListeners: Set<() => void>;
  /** 正交相机的可视半宽，随 fit 改变；aspect 变了要重算上下边。 */
  halfWidth: number;
  /** **单栏**的宽高比：两栏时是半块画布的。 */
  aspect: number;
  views: 1 | 2;
  split: SplitMode;
  active(): THREE.Camera;
  applyOrtho(): void;
  setMode(mode: CameraMode): void;
  /** 切单栏 / 两栏。两栏时每栏的视口一样大，相机的宽高比按单栏算。 */
  setViews(views: 1 | 2, split?: SplitMode): void;
  /** 立刻画一帧（导出 PNG 前用：没开 preserveDrawingBuffer，读缓冲前得先画）。 */
  renderFrame(): void;
  /** 屏幕坐标落在哪一栏、那一栏在画布里的矩形（CSS px，左上为原点）。落在画布外返回 null。 */
  paneAt(clientX: number, clientY: number): { pane: 0 | 1; viewport: Viewport; local: { x: number; y: number } } | null;
  dispose(): void;
}

export function createScene(host: HTMLDivElement): Scene {
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
  // 转心（target）写在 canvas 上：验收脚本看平移、双击设转心有没有生效（转视角不动它，平移才动）
  controls.addEventListener("change", () => {
    const c = controls.target;
    renderer.domElement.dataset.target = [c.x, c.y, c.z].map((v) => Number(v.toPrecision(6))).join(",");
  });

  // 网格跟着云走（setGrid）：先放一个与以前差不多的 4 m 网格，第一片云一到就换
  let grid = new THREE.GridHelper(1, 2);
  const axes = new THREE.AxesHelper(1);
  scene.add(axes);
  const disposeHelper = (helper: THREE.LineSegments) => {
    helper.geometry.dispose();
    const m = helper.material;
    if (Array.isArray(m)) m.forEach((x) => x.dispose());
    else m.dispose();
  };
  const overlays: [THREE.Group, THREE.Group] = [new THREE.Group(), new THREE.Group()];
  scene.add(overlays[0], overlays[1]);
  const backdrop = new THREE.Group();
  scene.add(backdrop);
  const measure = new THREE.Group();
  scene.add(measure);
  const frameListeners = new Set<() => void>();

  // 画布的 CSS 尺寸，两栏切视口要用
  let width = 1;
  let height = 1;

  /** 第 i 栏的视口，CSS 像素、左下角为原点（three 的 setViewport 就是这么数的）。 */
  const paneRect = (i: 0 | 1): [number, number, number, number] => {
    if (state.split === "lr") {
      const w = width / 2;
      return [i * w, 0, w, height];
    }
    const h = height / 2;
    return [0, i === 0 ? h : 0, width, h];
  };

  const render = () => {
    const cam = state.active();
    if (state.views === 1) {
      renderer.render(scene, cam);
      return;
    }
    // 两栏：每一遍只让这一栏的云与几何可见。不用 THREE.Layers —— Group 的 layers 不传给子节点，
    // 叠画几何是一组线，逐条设层容易漏；visible 对整组生效。
    renderer.setScissorTest(true);
    for (const i of [0, 1] as const) {
      const other = (1 - i) as 0 | 1;
      const hidden = state.points[other];
      if (hidden) hidden.visible = false;
      overlays[other].visible = false;
      const [x, y, w, h] = paneRect(i);
      renderer.setViewport(x, y, w, h);
      renderer.setScissor(x, y, w, h);
      renderer.render(scene, cam);
      if (hidden) hidden.visible = true;
      overlays[other].visible = true;
    }
    renderer.setScissorTest(false);
    renderer.setViewport(0, 0, width, height);
  };

  let raf = 0;
  const tick = () => {
    raf = requestAnimationFrame(tick);
    controls.update();
    for (const fn of frameListeners) fn();
    render();
  };

  const resize = () => {
    width = host.clientWidth || 1;
    height = host.clientHeight || 1;
    renderer.setSize(width, height, false);
    const paneW = state.views === 2 && state.split === "lr" ? width / 2 : width;
    const paneH = state.views === 2 && state.split === "tb" ? height / 2 : height;
    camera.aspect = paneW / paneH;
    camera.updateProjectionMatrix();
    // 2D 剖面与透视相机一样保住竖直方向的范围：变宽了多看一点，而不是把上下裁掉、整体放大
    const prevAspect = state.aspect;
    state.aspect = paneW / paneH;
    if (prevAspect > 0 && Number.isFinite(prevAspect)) state.halfWidth *= state.aspect / prevAspect;
    state.applyOrtho();
  };

  const state: Scene = {
    renderer,
    scene,
    camera,
    ortho,
    controls,
    mode: "3d",
    points: [null, null],
    overlays,
    overlay: overlays[0],
    backdrop,
    measure,
    frameListeners,
    halfWidth: 2,
    aspect: 1,
    views: 1,
    split: "lr",
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
      // 2D 下左键拖改成平移：旋转关了之后左键原来什么也不干，只能右键拖着挪（俯视图里人人先拿左键拖）
      state.controls.mouseButtons.LEFT = mode === "3d" ? THREE.MOUSE.ROTATE : THREE.MOUSE.PAN;
      if (mode === "2d") {
        ortho.position.set(target.x, target.y, target.z + 10);
        ortho.zoom = 1;
        state.applyOrtho();
      }
      state.controls.update();
    },
    setViews(views, split = state.split) {
      if (state.views === views && state.split === split) return;
      state.views = views;
      state.split = split;
      if (views === 1) setPoints(state, 1, null);
      if (views === 1) disposeOverlay(overlays[1]);
      resize();
    },
    renderFrame() {
      render();
    },
    grid: null,
    setGrid(spec) {
      if (!spec) return;
      const prev = state.grid;
      if (!prev || prev.cell !== spec.cell || prev.divisions !== spec.divisions || !grid.parent) {
        scene.remove(grid);
        disposeHelper(grid);
        // 中心不在原点了：中心线与别的线同一个颜色（不然像是坐标轴）
        grid = new THREE.GridHelper(spec.cell * spec.divisions, spec.divisions, 0x232a33, 0x232a33);
        grid.rotation.x = Math.PI / 2; // GridHelper 默认躺在 XZ 面上，转到 XY
        // 永远先画、不写深度：2D 剖面里网格与最底下那层点的深度分不开（正交相机的深度精度不够），
        // 后建的网格会压在点上，重跑时还一会儿压一会儿不压
        grid.renderOrder = -1;
        (grid.material as THREE.Material).depthWrite = false;
        scene.add(grid);
      }
      grid.position.set(spec.center[0], spec.center[1], spec.z);
      // 坐标轴还在原点，长 3 格：云离原点远时它在画面外，不挡事
      axes.scale.setScalar(spec.cell * 3);
      state.grid = spec;
      renderer.domElement.dataset.grid = [spec.cell, spec.center[0], spec.center[1], spec.z].map((v) => Number(v.toPrecision(9))).join(",");
    },
    paneAt(clientX, clientY) {
      const r = renderer.domElement.getBoundingClientRect();
      const local = { x: clientX - r.left, y: clientY - r.top };
      if (local.x < 0 || local.y < 0 || local.x > r.width || local.y > r.height) return null;
      if (state.views === 1) return { pane: 0, viewport: { x: 0, y: 0, w: r.width, h: r.height }, local };
      if (state.split === "lr") {
        const w = r.width / 2;
        const pane = local.x < w ? 0 : 1;
        return { pane, viewport: { x: pane * w, y: 0, w, h: r.height }, local };
      }
      const h = r.height / 2;
      const pane = local.y < h ? 0 : 1;
      return { pane, viewport: { x: 0, y: pane * h, w: r.width, h }, local };
    },
    dispose() {
      cancelAnimationFrame(raf);
      observer.disconnect();
      controls.dispose();
      setPoints(state, 0, null);
      setPoints(state, 1, null);
      disposeOverlay(overlays[0]);
      disposeOverlay(overlays[1]);
      disposeOverlay(backdrop);
      disposeOverlay(measure);
      frameListeners.clear();
      // helper 自己也有 geometry 和 material。不放的话每次挂载都漏一份。
      disposeHelper(grid);
      disposeHelper(axes);
      // renderer.dispose() **不释放 WebGL 上下文**（那是 forceContextLoss），
      // 少了它每次挂载/卸载漏一个，攒够十几个后视图突然全黑（见 README「踩过的坑」）。
      renderer.dispose();
      renderer.forceContextLoss();
      host.removeChild(renderer.domElement);
    },
  };
  state.setGrid(gridSpec([-2, -2, 0, 2, 2, 0]));
  resize();
  const observer = new ResizeObserver(resize);
  observer.observe(host);
  tick();
  return state;
}

// ------------------------------------------------------------ 测量标记

/** 两个标记点的颜色：1 绿 2 粉，与 ROI 框的前两种同色（Viewer3D 的 ROI_COLORS）。 */
const MEASURE_COLORS = [0x34d399, 0xf472b6];

/** 重画测量标记：点固定像素大小、不被点云遮挡（depthTest 关、renderOrder 压在底图之上）。 */
export function setMeasureMarks(scene: Scene, points: readonly (readonly [number, number, number])[]): void {
  disposeOverlay(scene.measure);
  if (points.length === 0) return;
  const pos = new Float32Array(points.flat());
  const col = new Float32Array(points.length * 3);
  const c = new THREE.Color();
  points.forEach((_, i) => {
    c.setHex(MEASURE_COLORS[i % MEASURE_COLORS.length]!);
    col.set([c.r, c.g, c.b], i * 3);
  });
  if (points.length >= 2) {
    const lineGeom = new THREE.BufferGeometry();
    lineGeom.setAttribute("position", new THREE.BufferAttribute(pos.slice(0, 6), 3));
    const line = new THREE.Line(
      lineGeom,
      new THREE.LineBasicMaterial({ color: 0xf5f5f5, depthTest: false, transparent: true, opacity: 0.9 }),
    );
    line.renderOrder = 9;
    scene.measure.add(line);
  }
  const geom = new THREE.BufferGeometry();
  geom.setAttribute("position", new THREE.BufferAttribute(pos, 3));
  geom.setAttribute("color", new THREE.BufferAttribute(col, 3));
  const marks = new THREE.Points(
    geom,
    new THREE.PointsMaterial({ size: 9, sizeAttenuation: false, vertexColors: true, depthTest: false }),
  );
  marks.renderOrder = 10;
  scene.measure.add(marks);
}

/** 拾取要的投影矩阵：projection × view（列主序 16 个数）。 */
export function pickMatrixOf(scene: Scene): Float32Array {
  const cam = scene.active() as THREE.PerspectiveCamera | THREE.OrthographicCamera;
  cam.updateMatrixWorld();
  return Float32Array.from(new THREE.Matrix4().multiplyMatrices(cam.projectionMatrix, cam.matrixWorldInverse).elements);
}

// ------------------------------------------------------------ 点云几何与着色

/** 一片云的 Points。零拷贝：cloud.xyz 就是 IPC 缓冲上的视图。空云返回 null。 */
export function buildPoints(cloud: CloudPayload | null, size: number): THREE.Points | null {
  if (!cloud || cloud.pointCount === 0) return null;
  const geometry = new THREE.BufferGeometry();
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
    size,
    sizeAttenuation: false,
    vertexColors: false,
    color: 0x8fb8ff,
  });
  return new THREE.Points(geometry, material);
}

/** 换掉第 i 栏的云：旧的从场景摘下并释放。 */
export function setPoints(scene: Scene, i: 0 | 1, points: THREE.Points | null): void {
  const old = scene.points[i];
  if (old) {
    scene.scene.remove(old);
    old.geometry.dispose();
    (old.material as THREE.Material).dispose();
  }
  scene.points[i] = points;
  if (points) scene.scene.add(points);
}

export interface Paint {
  shading: ShadingMode;
  ramp: RampName;
  lo: number;
  hi: number;
}

/** 着色只重写 color 属性，positions 和 boundingSphere 原样留着；数组能复用就复用，
 *  换色带时不再分配几十兆。shading 是降级之后**实际**用的那一种。 */
export function paintPoints(points: THREE.Points, cloud: CloudPayload, paint: Paint): void {
  const geometry = points.geometry;
  const material = points.material as THREE.PointsMaterial;

  if (paint.shading === "flat") {
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
  if (paint.shading === "normal") writeNormalColors(arr, cloud);
  else if (paint.shading === "rgb") writeRgbColors(arr, cloud.rgb, cloud.pointCount);
  else writeColors(arr, cloud, paint.shading, paint.ramp, paint.lo, paint.hi);
  if (reuse) reuse.needsUpdate = true;
  else geometry.setAttribute("color", new THREE.BufferAttribute(arr, 3));
  material.vertexColors = true;
  material.color.setHex(0xffffff);
  material.needsUpdate = true;
}

/** 着色用的标量：强度模式取强度通道，其余取 Z。 */
function shadingValue(cloud: CloudPayload, mode: ShadingMode, i: number): number {
  if (mode === "intensity" && cloud.intensity) return cloud.intensity[i]!;
  return cloud.xyz[i * 3 + 2]!;
}

/** 法线着色：分量的绝对值直接当 RGB。色带对它没有意义，所以走单独一条路。 */
export function writeNormalColors(out: Float32Array, cloud: CloudPayload): void {
  const n = cloud.normals;
  if (!n) return;
  for (let i = 0; i < cloud.pointCount; i += 1) {
    out[i * 3] = Math.abs(n[i * 3] ?? 0);
    out[i * 3 + 1] = Math.abs(n[i * 3 + 1] ?? 0);
    out[i * 3 + 2] = Math.abs(n[i * 3 + 2] ?? 0);
  }
}

/** 就地写颜色。复用已有数组是为了换色带时不再分配几十兆。 */
export function writeColors(
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
export function dataRangeOf(cloud: CloudPayload | null, mode: ShadingMode): [number, number] {
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

// ------------------------------------------------------------ 包围盒与取景

export function round3(v: number) {
  return Number.isFinite(v) ? Math.round(v * 1000) / 1000 : 0;
}

/** 「云 + 叠画几何」的联合包围盒。取并集而不是二选一：几何再小也挤不掉云，
 *  云再大也不会把 ROI 框推出画面。对比模式把两栏的云都传进来（C4）。都空时返回 null。 */
export function unionBounds(
  clouds: CloudPayload | null | readonly (CloudPayload | null)[],
  ...groups: THREE.Group[]
): Float32Array | null {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  const list = Array.isArray(clouds) ? clouds : [clouds as CloudPayload | null];
  for (const cloud of list) {
    if (!cloud || cloud.pointCount === 0) continue;
    for (let i = 0; i < 3; i += 1) {
      min[i] = Math.min(min[i]!, cloud.bounds[i]!);
      max[i] = Math.max(max[i]!, cloud.bounds[i + 3]!);
    }
  }
  for (const overlay of groups) {
    if (overlay.children.length === 0) continue;
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
export function overlayBoundsOf(overlay: THREE.Group): Float32Array | null {
  if (overlay.children.length === 0) return null;
  const box = new THREE.Box3().setFromObject(overlay);
  if (box.isEmpty()) return null;
  return new Float32Array([box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z]);
}

/** 包围盒 → data- 属性上的六个数。null 是空串。 */
export function boundsAttr(bounds: ArrayLike<number> | null): string {
  if (!bounds) return "";
  return Array.from(bounds, round3).join(",");
}

/** 把视角的中心（OrbitControls 的 target）挪到 p 上：当前相机与 target 一起平移同一段，视线方向不变 ——
 *  画面平移到 p 居中、之后转视角绕着它转（useFocusOnDoubleClick）。正交俯视时也是平移。 */
export function focusOn(scene: Scene, p: [number, number, number]) {
  const t = scene.controls.target;
  const offset = new THREE.Vector3(p[0] - t.x, p[1] - t.y, p[2] - t.z);
  scene.controls.object.position.add(offset);
  t.add(offset);
  scene.controls.update();
}

/** 转到一个标准视角（只管 3D 的透视相机：2D 剖面本来就是俯视，正交相机不碰）。先把阻尼没走完的那点转动走完 ——
 *  不然刚甩了一下就按，相机会接着往外转。转心不动、距离不变。2D 时什么都不做、返回 false。 */
export function applyViewPreset(scene: Scene, view: ViewPreset): boolean {
  if (scene.mode === "2d") return false;
  const c = scene.controls;
  const damping = c.enableDamping;
  c.enableDamping = false;
  c.update();
  c.enableDamping = damping;
  const t = c.target;
  const p = scene.camera.position;
  const next = presetPosition(view, [t.x, t.y, t.z], [p.x, p.y, p.z]);
  p.set(next[0], next[1], next[2]);
  c.update();
  return true;
}

/** 相机此刻的样子（两台相机与转心）。场景重建之后放回去：连线查看器在运行期间换成「正在计算…」会卸掉画布。 */
export interface CameraView {
  target: [number, number, number];
  position: [number, number, number];
  near: number;
  far: number;
  orthoPosition: [number, number, number];
  orthoZoom: number;
  halfWidth: number;
}

export function saveCameraView(scene: Scene): CameraView {
  const t = scene.controls.target;
  const p = scene.camera.position;
  const o = scene.ortho.position;
  return {
    target: [t.x, t.y, t.z],
    position: [p.x, p.y, p.z],
    near: scene.camera.near,
    far: scene.camera.far,
    orthoPosition: [o.x, o.y, o.z],
    orthoZoom: scene.ortho.zoom,
    halfWidth: scene.halfWidth,
  };
}

export function restoreCameraView(scene: Scene, view: CameraView): void {
  scene.controls.target.set(...view.target);
  scene.camera.position.set(...view.position);
  scene.camera.near = view.near;
  scene.camera.far = view.far;
  scene.camera.updateProjectionMatrix();
  scene.ortho.position.set(...view.orthoPosition);
  scene.ortho.zoom = view.orthoZoom;
  scene.halfWidth = view.halfWidth;
  scene.applyOrtho();
  scene.controls.update();
}

export function fitToBounds(scene: Scene, bounds: Float32Array) {
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
  scene.camera.position.set(cx + d * ISO_DIR[0], cy + d * ISO_DIR[1], cz + d * ISO_DIR[2]);
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
  scene.setGrid(gridSpec(bounds));
}
