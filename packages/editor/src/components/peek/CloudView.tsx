import { useEffect, useMemo, useRef, useState } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";

import { findBaseCloud, firstCloudPort, type BaseCloud } from "../../lib/basecloud";
import { cacheKey, cloudCache, dropOtherRuns, putCache } from "../../lib/cloudCache";
import { RAMPS, type RampName } from "../../lib/ramps";
import { disposeOverlay, extentOf, shapesOf } from "../../lib/shapes2d";
import { registerPeekCanvas } from "../../lib/peekCanvas";
import { augmentOperators, levelOf, resolveOutput } from "../../lib/subgraph";
import { useGraphStore } from "../../store/graph";
import { useManifestStore } from "../../store/manifest";
import { PEEK_FROZEN, usePeekStore } from "../../store/peek";
import { transport } from "../../transport";
import { decodeCloud, type CloudPayload } from "../../types/execution";
import type { CameraMode, ShadingMode } from "../Viewer3D";
import type { PeekViewProps } from "./types";

const MAX_POINTS_CHOICES = [100_000, 200_000, 500_000, 2_000_000];
const POINT_SIZE = 1.6;
const FROZEN = PEEK_FROZEN;

interface Display {
  runId: string | null;
  cloud: CloudPayload | null;
  status: string | null;
  base: BaseCloud | null;
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

function round3(v: number) {
  return Number.isFinite(v) ? Math.round(v * 1000) / 1000 : 0;
}

function boundsAttr(bounds: ArrayLike<number> | null): string {
  if (!bounds) return "";
  return Array.from(bounds, round3).join(",");
}

function overlayBoundsOf(overlay: THREE.Group): Float32Array | null {
  if (overlay.children.length === 0) return null;
  const box = new THREE.Box3().setFromObject(overlay);
  if (box.isEmpty()) return null;
  return new Float32Array([box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z]);
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

interface CloudTarget {
  resolved: { nodeId: string; port: string } | null;
  base: BaseCloud | null;
}

export function CloudView({ win, src }: PeekViewProps) {
  const hostRef = useRef<HTMLDivElement>(null);
  const sceneRef = useRef<Scene | null>(null);

  const [display, setDisplay] = useState<Display>({
    runId: null,
    cloud: null,
    status: null,
    base: null,
  });
  const [loading, setLoading] = useState(false);
  const [overlayBounds, setOverlayBounds] = useState<Float32Array | null>(null);
  const { cloud } = display;

  const doc = useGraphStore((s) => s.doc);
  const operatorsById = useManifestStore((s) => s.operatorsById);
  const typesByName = useManifestStore((s) => s.typesByName);
  const ops = useMemo(
    () => augmentOperators(operatorsById, doc.subgraphs),
    [operatorsById, doc.subgraphs],
  );

  const path = win.path;
  const fromNode = win.from.node;
  const lockedRun = win.locked?.runId ?? null;
  const runId = lockedRun ?? src.runId;
  const { maxPoints, shading, ramp } = win.opts;
  const cameraMode: CameraMode = win.view === "cloud2d" ? "2d" : "3d";

  // 自己有云就画自己的；没有就先看本节点还有没有别的点云口，再沿输入边往上游借最近的一片，
  // 几何叠在它上面 —— 只输出 Box2D/Line2D 的节点若显示成空白，用户就看不出框压在剖面的哪里。
  const target: CloudTarget = useMemo(() => {
    if (src.type === "PointCloud" && src.resolved) return { resolved: src.resolved, base: null };
    const node = levelOf(doc, path).nodes.find((n) => n.id === fromNode);
    if (!node) return { resolved: null, base: null };
    const own = firstCloudPort(ops, node.op);
    if (own) {
      const resolved = resolveOutput(doc, path, node.id, own);
      if (resolved) {
        const label = node.ui?.title ?? ops.get(node.op)?.label ?? node.id;
        return { resolved, base: { localId: node.id, label, resolved } };
      }
    }
    const base = findBaseCloud(doc, path, node.id, ops);
    return { resolved: base?.resolved ?? null, base };
  }, [doc, path, fromNode, ops, src.type, src.resolved]);

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
  const [lo, hi] = useMemo(
    () => dataRangeOf(cloud, effectiveShading),
    [cloud, effectiveShading],
  );

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

  useEffect(
    () =>
      registerPeekCanvas(win.id, () => {
        const scene = sceneRef.current;
        if (!scene) return null;
        scene.renderer.render(scene.scene, scene.active());
        return scene.renderer.domElement;
      }),
    [win.id],
  );

  useEffect(() => {
    let cancelled = false;
    const show = (
      status: string | null,
      payload: CloudPayload | null = null,
      base: BaseCloud | null = null,
    ) => {
      if (cancelled) return;
      setDisplay({ runId: runId ?? null, cloud: payload, status, base });
    };

    if (src.status !== null) {
      setLoading(false);
      return;
    }
    const resolved = target.resolved;
    if (!runId || !resolved) {
      setLoading(false);
      show(resolved ? "未运行" : "该端口无点云，上游也没有可当底图的点云");
      return;
    }

    if (!lockedRun) dropOtherRuns(runId);
    const key = cacheKey(runId, resolved.nodeId, resolved.port, maxPoints);
    const hit = cloudCache.get(key);
    if (hit) {
      // 命中也要 delete+set 一下，否则 LRU 的「最近使用」永远不更新
      putCache(key, hit);
      setLoading(false);
      show(hit.pointCount === 0 ? "该端口的点云是空的" : null, hit, target.base);
      return;
    }

    setLoading(true);
    void (async () => {
      try {
        const buffer = await transport.getOutputCloud(
          runId,
          resolved.nodeId,
          resolved.port,
          maxPoints,
        );
        if (cancelled) return;
        const payload = decodeCloud(buffer);
        putCache(key, payload);
        show(payload.pointCount === 0 ? "该端口的点云是空的" : null, payload, target.base);
      } catch (e) {
        show(lockedRun ? FROZEN : e instanceof Error ? e.message : String(e));
      } finally {
        // 这里**不看 cancelled**：换窗口/换 run 会作废旧请求，若那时不放下 loading，
        // 而新的目标又不需要发请求（比如根本没有可画的云），界面就永远停在「正在取点云…」。
        setLoading(false);
      }
    })();

    return () => {
      cancelled = true;
    };
  }, [runId, lockedRun, target, maxPoints, src.status]);

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
      size: POINT_SIZE,
      sizeAttenuation: false,
      vertexColors: false,
      color: 0x8fb8ff,
    });
    const points = new THREE.Points(geometry, material);
    scene.scene.add(points);
    scene.points = points;
  }, [cloud]);

  // 换着色模式/色带只重写 color 属性，positions 和 boundingSphere 原样留着。
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

  // -- 2D 几何叠画（G7）：整组重建，线与端口同色 -----------------------------
  const shapeStat = useMemo(
    () => (src.type !== "PointCloud" && src.stat?.value !== undefined ? src.stat : null),
    [src.type, src.stat],
  );

  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    disposeOverlay(scene.overlay);
    if (!shapeStat) return;
    // 线的长度/十字的大小要有个尺度参照：优先用底图云的跨度，没有云就用几何自己的。
    const span = cloud
      ? Math.max(
          Math.hypot(
            cloud.bounds[3]! - cloud.bounds[0]!,
            cloud.bounds[4]! - cloud.bounds[1]!,
            cloud.bounds[5]! - cloud.bounds[2]!,
          ),
          extentOf(shapeStat),
          1e-3,
        )
      : Math.max(extentOf(shapeStat), 1e-3);
    const hex = typesByName.get(shapeStat.type)?.color ?? "#6b7280";
    const color = new THREE.Color(hex).getHex();
    for (const line of shapesOf(shapeStat, color, span)) scene.overlay.add(line);
  }, [shapeStat, cloud, typesByName]);

  // 换了云或换了几何就自动取景一次；同一份内容里调参数不该把视角拉回去。
  // 必须排在上面那个 effect 之后：取景要量的是它刚建好的那一组线。
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    setOverlayBounds(overlayBoundsOf(scene.overlay));
    const bounds = unionBounds(cloud, scene.overlay);
    if (bounds) fitToBounds(scene, bounds);
  }, [cloud, shapeStat]);

  useEffect(() => {
    sceneRef.current?.setMode(cameraMode);
  }, [cameraMode]);

  const setOpts = usePeekStore((s) => s.setOpts);
  const empty = !cloud && !shapeStat;
  const status = loading ? "正在取点云…" : display.status;
  const pointChoices = MAX_POINTS_CHOICES.includes(maxPoints)
    ? MAX_POINTS_CHOICES
    : [...MAX_POINTS_CHOICES, maxPoints].sort((a, b) => a - b);

  return (
    <div
      className="peek-cloud"
      data-testid="peek-cloud"
      data-camera={cameraMode}
      data-shading={effectiveShading}
      data-run={display.runId ?? ""}
      data-base={display.base?.localId ?? ""}
      data-cloud-bounds={boundsAttr(cloud && cloud.pointCount > 0 ? cloud.bounds : null)}
      data-overlay-bounds={boundsAttr(overlayBounds)}
    >
      <div className="peek-cloud__bar">
        {display.base && (
          <span
            className="peek-cloud__base"
            data-testid="peek-base"
            data-node={display.base.localId}
            title={`该端口没有点云，底图取自 ${display.base.localId}`}
          >
            底图：{display.base.label}
          </span>
        )}
        {cloud && (
          <span className="peek-cloud__count" title="显示点数 / 总点数">
            {cloud.pointCount.toLocaleString()} / {cloud.totalPoints.toLocaleString()}
          </span>
        )}
        <span className="peek-cloud__spacer" />
        <select
          className="peek-cloud__select"
          data-testid="peek-shading"
          value={effectiveShading}
          onChange={(e) => setOpts(win.id, { shading: e.target.value as ShadingMode })}
          title={
            shading === effectiveShading ? "着色方式" : "这片点云没有该通道，已退回高度着色"
          }
        >
          <option value="intensity" disabled={!hasIntensity}>
            强度{hasIntensity ? "" : "（无）"}
          </option>
          <option value="height">高度</option>
          <option value="normal" disabled={!hasNormals}>
            法线{hasNormals ? "" : "（无）"}
          </option>
          <option value="flat">单色</option>
        </select>
        <select
          className="peek-cloud__select"
          data-testid="peek-ramp"
          value={ramp}
          onChange={(e) => setOpts(win.id, { ramp: e.target.value as RampName })}
          disabled={isFlat}
          title="色带"
        >
          <option value="viridis">viridis</option>
          <option value="gray">灰度</option>
          <option value="jet">蓝→红</option>
        </select>
        <select
          className="peek-cloud__select"
          data-testid="peek-maxpoints"
          value={maxPoints}
          onChange={(e) => setOpts(win.id, { maxPoints: Number(e.target.value) })}
          disabled={lockedRun !== null}
          title={lockedRun ? FROZEN : "最多显示多少点（抽样在 C++ 侧做）"}
        >
          {pointChoices.map((n) => (
            <option key={n} value={n}>
              {n >= 1_000_000 ? `${n / 1_000_000}M` : `${n / 1000}K`}
            </option>
          ))}
        </select>
      </div>

      <div className="peek-cloud__stage">
        <div className="peek-cloud__canvas" ref={hostRef} data-testid="peek-cloud-canvas" />
        {status !== null && (loading || empty) && (
          <div className="peek-cloud__empty" data-testid="peek-cloud-status">
            {status}
          </div>
        )}
      </div>
    </div>
  );
}
