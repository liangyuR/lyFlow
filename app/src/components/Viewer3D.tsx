// 3D 点云预览（交互清单 P1 #30）。three.js 随包打、**不走 CDN**：桌面应用断网也得能用。
// 点云走二进制 IPC，`decodeCloud` 给的 Float32Array 是缓冲上的**视图**，全程零拷贝（ADR-0006）。

import { useEffect, useMemo, useRef, useState } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";

import { transport } from "../transport";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import { decodeCloud, type CloudPayload } from "../types/execution";
import "../styles.viewer.css";

export type ShadingMode = "intensity" | "height" | "normal" | "flat";
export type RampName = "viridis" | "gray" | "jet";

/** 载荷里还没有法线通道（ADR-0006 只有 xyz + intensity），法线着色先禁用。 */
const HAS_NORMAL: boolean = false;

/** 当前展示的东西。三者永远一起换，见 Viewer3D 里的注释。 */
interface Display {
  nodeId: string | null;
  cloud: CloudPayload | null;
  status: string | null;
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

/** 该节点的第一个 PointCloud 输出端口。没有就返回 null。 */
function firstCloudPort(opId: string): string | null {
  const op = useManifestStore.getState().operatorsById.get(opId);
  if (!op) return null;
  for (const p of op.outputs) {
    if (p.type === "PointCloud") return p.name;
  }
  return null;
}

interface Scene {
  renderer: THREE.WebGLRenderer;
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  controls: OrbitControls;
  points: THREE.Points | null;
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

  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.12;

  const grid = new THREE.GridHelper(4, 16, 0x33404f, 0x232a33);
  grid.rotation.x = Math.PI / 2; // GridHelper 默认躺在 XZ 面上，转到 XY
  scene.add(grid);
  const axes = new THREE.AxesHelper(0.5);
  scene.add(axes);

  let raf = 0;
  const tick = () => {
    raf = requestAnimationFrame(tick);
    controls.update();
    renderer.render(scene, camera);
  };
  tick();

  const state: Scene = {
    renderer,
    scene,
    camera,
    controls,
    points: null,
    dispose() {
      cancelAnimationFrame(raf);
      controls.dispose();
      if (state.points) {
        state.points.geometry.dispose();
        (state.points.material as THREE.Material).dispose();
      }
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
  return state;
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
  const [maxPoints, setMaxPoints] = useState(2_000_000);
  // 云、状态、以及**它属于哪个节点**必须一起换：拆成三个 useState 的话，切换节点时会出现
  // 「标题是新节点、点云还是旧节点」的中间态 —— 肉眼看不见，但验收脚本会稳定读到它。
  const [display, setDisplay] = useState<Display>({
    nodeId: null,
    cloud: null,
    status: "未运行",
  });
  const [loading, setLoading] = useState(false);
  const { cloud } = display;

  const selected = useUiStore((s) => s.selectedNodes);
  const nodes = useGraphStore((s) => s.doc.nodes);
  const runId = useExecutionStore((s) => s.runId);
  const runStatus = useExecutionStore((s) => s.runStatus);

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
    activeId ? s.nodes.get(activeId)?.state : undefined,
  );

  const hasIntensity = cloud?.intensity != null;
  // 选了「强度」但这片云没有强度通道时实际走高度着色，那就让下拉框也显示「高度」——
  // 下拉框写着强度、画面却是高度，用户只会以为强度数据本身有问题。
  const effectiveShading: ShadingMode =
    (shading === "intensity" && !hasIntensity) || (shading === "normal" && !HAS_NORMAL)
      ? "height"
      : shading;
  const isFlat = effectiveShading === "flat";

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
    const show = (status: string | null, payload: CloudPayload | null = null) => {
      if (cancelled) return;
      setDisplay({ nodeId: activeId, cloud: payload, status });
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
    const port = firstCloudPort(activeNode.op);
    if (!port) {
      setLoading(false);
      show("该节点无点云输出");
      return;
    }

    dropOtherRuns(runId);
    const key = cacheKey(runId, activeNode.id, port, maxPoints);
    const hit = cloudCache.get(key);
    if (hit) {
      // 命中也要 delete+set 一下，否则 LRU 的「最近使用」永远不更新
      putCache(key, hit);
      setLoading(false);
      show(hit.pointCount === 0 ? "该节点的点云是空的" : null, hit);
      return;
    }

    setLoading(true);
    void (async () => {
      try {
        const buffer = await transport.getOutputCloud(runId, activeNode.id, port, maxPoints);
        if (cancelled) return;
        const payload = decodeCloud(buffer);
        putCache(key, payload);
        show(payload.pointCount === 0 ? "该节点的点云是空的" : null, payload);
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
  }, [activeNode, activeId, selected.size, runId, runStatus, activeState, maxPoints]);

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
    writeColors(arr, cloud, effectiveShading, ramp, lo, hi);
    if (reuse) reuse.needsUpdate = true;
    else geometry.setAttribute("color", new THREE.BufferAttribute(arr, 3));
    material.vertexColors = true;
    material.color.setHex(0xffffff);
    material.needsUpdate = true;
  }, [cloud, effectiveShading, isFlat, ramp, lo, hi]);

  // 点大小只改材质，不重建几何体 —— 它曾经也在上面那个 effect 的依赖里，
  // 拖一下滑块就要重分配 24MB 颜色数组、重扫两百万点（见 README「踩过的坑」）。
  useEffect(() => {
    pointSizeRef.current = pointSize;
    const p = sceneRef.current?.points;
    if (p) (p.material as THREE.PointsMaterial).size = pointSize;
  }, [pointSize]);

  // 换了一片云就自动 fit 一次；同一片云里调参数不该把视角拉回去
  useEffect(() => {
    const scene = sceneRef.current;
    if (scene && cloud) fitToBounds(scene, cloud.bounds);
  }, [cloud]);

  const setRangeEnd = (end: 0 | 1, raw: string) => {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    setManualRange(end === 0 ? [v, hi] : [lo, v]);
    setRangeAuto(false);
  };

  const exportPng = () => {
    const scene = sceneRef.current;
    if (!scene) return;
    // 读 buffer 前立刻重画一帧：换成 preserveDrawingBuffer 的话每一帧都要多付一次代价。
    scene.renderer.render(scene.scene, scene.camera);
    const url = scene.renderer.domElement.toDataURL("image/png");
    const stamp = new Date().toISOString().replace(/[-:]/g, "").replace(/\..*$/, "");
    const name = (display.nodeId ?? "view").replace(/[^\w.-]+/g, "_");
    // 没装 Tauri fs 插件，用 <a download> 交给 webview 自己下载，浏览器模式下同样能用。
    const a = document.createElement("a");
    a.href = url;
    a.download = `${name}-${stamp}.png`;
    document.body.appendChild(a);
    a.click();
    a.remove();
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
    >
      <div className="viewer__bar">
        <span className="viewer__title">3D 预览</span>
        {cloud && (
          <span className="viewer__count" title="显示点数 / 总点数">
            {cloud.pointCount.toLocaleString()} / {cloud.totalPoints.toLocaleString()} 点
          </span>
        )}
        <span className="viewer__spacer" />
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
            if (scene && cloud) fitToBounds(scene, cloud.bounds);
          }}
          disabled={!cloud}
          title="缩放到全部"
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
          <option value="normal" disabled={!HAS_NORMAL} title="点云载荷暂无法线通道">
            法线（暂无通道）
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
          onClick={exportPng}
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
