//
// 3D 点云预览（交互清单 P1 #30）。
//
// three.js 随包打，**不走 CDN**：这是个桌面应用，装完就该能用；
// 依赖一个外部域名意味着断网或内网机器上打开就是黑屏。
//
// 数据路径见 ADR-0006：点云是二进制的，`decodeCloud` 拿到的 Float32Array
// 是 IPC 缓冲上的**视图**，直接交给 BufferAttribute，全程零拷贝。
// 一百万点走 JSON 的话是 30MB 文本加一次全量解析，那条路根本走不通。
//

import { useEffect, useMemo, useRef, useState } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";

import { transport } from "../transport";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import { decodeCloud, type CloudPayload } from "../types/execution";

export type ColorMode = "intensity" | "height" | "flat";

/** 当前展示的东西。三者永远一起换，见 Viewer3D 里的注释。 */
interface Display {
  nodeId: string | null;
  cloud: CloudPayload | null;
  status: string | null;
}

const MAX_POINTS_CHOICES = [100_000, 500_000, 2_000_000, 8_000_000];

/**
 * 已取回的点云缓存。
 *
 * 键是 runId+node+port+maxPoints —— 少了任何一段都会串味：
 * 少 runId 会在重跑之后拿到上一次的结果，少 maxPoints 会让滑块拖了没反应。
 * 切换选中节点来回看是最常见的操作，没有它每次都要走一遍 IPC。
 */
const cloudCache = new Map<string, CloudPayload>();

/**
 * 缓存的**字节**预算，不是条数预算。
 *
 * 按条数封顶是错的：maxPoints 最大到 8M，一条就是 96MB 坐标 + 32MB 强度，
 * 8 条能攒到 1GB。按字节算才对得上「大概占多少内存」这个人能理解的量。
 */
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
      // renderer.dispose() **不释放 WebGL 上下文**（那是 forceContextLoss）。
      // 少了这一句，每次挂载/卸载（StrictMode 的双挂载、HMR、面板开合）
      // 就漏一个上下文；浏览器攒够十几个之后开始逐出最老的，
      // 表现是视图突然变全黑而且不报错。
      renderer.dispose();
      renderer.forceContextLoss();
      host.removeChild(renderer.domElement);
    },
  };
  return state;
}

/** intensity → 蓝→青→黄→红 的调色。比灰度更容易看出结构。 */
function rampColor(t: number, out: THREE.Color) {
  const x = Math.max(0, Math.min(1, t));
  if (x < 0.5) out.setRGB(0.15 + 0.1 * x, 0.4 + 1.2 * x, 1.0 - 0.6 * x);
  else out.setRGB(0.35 + 1.3 * (x - 0.5), 1.0 - 1.2 * (x - 0.5), 0.4 - 0.7 * (x - 0.5));
}

function buildColors(cloud: CloudPayload, mode: ColorMode): Float32Array | null {
  const n = cloud.pointCount;
  if (mode === "flat") return null;

  const colors = new Float32Array(n * 3);
  const c = new THREE.Color();

  if (mode === "intensity" && cloud.intensity) {
    let lo = Infinity;
    let hi = -Infinity;
    for (let i = 0; i < n; i += 1) {
      const v = cloud.intensity[i]!;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    const span = hi - lo || 1;
    for (let i = 0; i < n; i += 1) {
      rampColor((cloud.intensity[i]! - lo) / span, c);
      colors[i * 3] = c.r;
      colors[i * 3 + 1] = c.g;
      colors[i * 3 + 2] = c.b;
    }
    return colors;
  }

  // 高度着色。用 bounds 而不是重新扫一遍：bounds 是**全量**点云算的，
  // 抽稀后的极值会让同一份数据在不同 maxPoints 下呈现不同的配色。
  const lo = cloud.bounds[2]!;
  const span = (cloud.bounds[5]! - lo) || 1;
  for (let i = 0; i < n; i += 1) {
    rampColor((cloud.xyz[i * 3 + 2]! - lo) / span, c);
    colors[i * 3] = c.r;
    colors[i * 3 + 1] = c.g;
    colors[i * 3 + 2] = c.b;
  }
  return colors;
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

  const [colorMode, setColorMode] = useState<ColorMode>("intensity");
  const [pointSize, setPointSize] = useState(1.6);
  const pointSizeRef = useRef(1.6);
  const [maxPoints, setMaxPoints] = useState(2_000_000);
  // 云、状态、以及**它属于哪个节点**必须一起更新。
  // 拆成三个 useState 的话，切换节点时会短暂出现「标题是新节点、点云还是旧节点」
  // 的中间态 —— 肉眼几乎看不见，但验收脚本会稳定地读到它。
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
  const selectedNode = useMemo(
    () => (selectedId ? nodes.find((n) => n.id === selectedId) : undefined),
    [selectedId, nodes],
  );
  // 只订阅**这一个节点的状态字符串**，不要订阅整张 nodes Map。
  // 那张 Map 每来一条事件就是一个新引用（包括每 50ms 一条的 node_progress），
  // 拿它当 effect 依赖的话：取云请求发出去 → 下一条进度事件让 effect 重跑 →
  // 旧请求作废、发新请求 → 循环。选中一个刚跑完的节点、而别的节点还在跑时，
  // 会排起一队几十兆的 IPC 往返。
  const selectedState = useExecutionStore((s) =>
    selectedId ? s.nodes.get(selectedId)?.state : undefined,
  );

  // 选了「强度」但这片云没有强度通道时，实际用的是高度着色 ——
  // 那就让下拉框也显示「高度」。下拉框写着强度、画面却是高度，
  // 用户只会以为强度数据本身有问题。
  const effectiveColorMode: ColorMode =
    colorMode === "intensity" && display.cloud?.intensity == null ? "height" : colorMode;

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
      setDisplay({ nodeId: selectedId, cloud: payload, status });
    };

    if (!selectedNode) {
      setLoading(false);
      show(selected.size > 1 ? "选中了多个节点" : "选中一个节点查看它的输出");
      return;
    }
    if (!runId || runStatus === "idle") {
      setLoading(false);
      show("未运行");
      return;
    }
    if (selectedState === "error") {
      setLoading(false);
      show("该节点运行出错");
      return;
    }
    if (selectedState !== "done" && selectedState !== "skipped") {
      setLoading(false);
      show(selectedState === "running" ? "正在计算…" : "该节点尚未产出结果");
      return;
    }
    const port = firstCloudPort(selectedNode.op);
    if (!port) {
      setLoading(false);
      show("该节点无点云输出");
      return;
    }

    dropOtherRuns(runId);
    const key = cacheKey(runId, selectedNode.id, port, maxPoints);
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
        const buffer = await transport.getOutputCloud(runId, selectedNode.id, port, maxPoints);
        if (cancelled) return;
        const payload = decodeCloud(buffer);
        putCache(key, payload);
        show(payload.pointCount === 0 ? "该节点的点云是空的" : null, payload);
      } catch (e) {
        show(e instanceof Error ? e.message : String(e));
      } finally {
        // 注意这里**不看 cancelled**：切换节点时旧请求会被作废，
        // 如果那时不把 loading 放下来，而新选中的节点又不需要发请求
        // （比如它根本没有点云输出），界面就会永远停在「正在取点云…」。
        setLoading(false);
      }
    })();

    return () => {
      cancelled = true;
    };
  }, [selectedNode, selectedId, selected.size, runId, runStatus, selectedState, maxPoints]);

  // -- 把点云放进场景 -------------------------------------------------------
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
    const colors = buildColors(cloud, effectiveColorMode);
    if (colors) geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));
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
      vertexColors: colors !== null,
      color: colors ? 0xffffff : 0x8fb8ff,
    });
    const points = new THREE.Points(geometry, material);
    scene.scene.add(points);
    scene.points = points;
  }, [cloud, effectiveColorMode]);

  // 点大小只改材质，不重建几何体。
  //
  // 这一条曾经形同虚设：pointSize 同时也在上面那个 effect 的依赖里，于是拖一下
  // 滑块就要重新分配一次 24MB 的颜色数组、重扫两百万个点、再传一次 GPU ——
  // 每个 input 事件一遍，拖动全程界面是卡死的。
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

  const hasIntensity = cloud?.intensity != null;

  return (
    <div
      className="viewer"
      // 验收脚本靠这两个属性判断「视图已经切到这个节点了」，
      // 而不是去猜多久之后 React 会渲染完（scripts/e2e）。
      data-node={display.nodeId ?? ""}
      data-view={loading ? "loading" : cloud ? "cloud" : "empty"}
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
          value={effectiveColorMode}
          onChange={(e) => setColorMode(e.target.value as ColorMode)}
          title={
            colorMode === "intensity" && !hasIntensity
              ? "这片点云没有强度通道，已退回高度着色"
              : "着色方式"
          }
        >
          <option value="intensity" disabled={!hasIntensity}>
            强度{hasIntensity ? "" : "（无）"}
          </option>
          <option value="height">高度</option>
          <option value="flat">单色</option>
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
            if (scene && cloud) fitToBounds(scene, cloud.bounds);
          }}
          disabled={!cloud}
          title="缩放到全部"
        >
          ⤢
        </button>
      </div>

      <div className="viewer__canvas" ref={hostRef} data-testid="viewer3d-canvas" />

      {(display.status || loading) && (
        <div className="viewer__empty" data-testid="viewer3d-status">
          {loading ? "正在取点云…" : display.status}
        </div>
      )}
    </div>
  );
}
