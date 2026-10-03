import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import * as THREE from "three";

import { findBaseCloud, firstCloudPort, type BaseCloud } from "../../lib/basecloud";
import { cacheKey, cloudCache, dropOtherRuns, fetchCloud, putCache } from "../../lib/cloudCache";
import {
  applyViewPreset,
  boundsAttr,
  buildPoints,
  createScene,
  dataRangeOf,
  fitToBounds,
  overlayBoundsOf,
  paintPoints,
  restoreCameraView,
  saveCameraView,
  setPoints,
  unionBounds,
  type CameraMode,
  type CameraView,
  type Scene,
  type ShadingMode,
} from "../../lib/cloudScene";
import type { RampName } from "../../lib/ramps";
import { clampPointSize, POINT_SIZE_MAX, POINT_SIZE_MIN } from "../../lib/viewPrefs";
import { disposeOverlay, extentOf, shapesOf } from "../../lib/shapes2d";
import { registerPeekCanvas } from "../../lib/peekCanvas";
import { augmentOperators, levelOf, resolveOutput } from "../../lib/subgraph";
import { useGraphStore } from "../../store/graph";
import { useManifestStore } from "../../store/manifest";
import { PEEK_FROZEN, usePeekStore } from "../../store/peek";
import { transport } from "../../transport";
import { decodeCloud, type CloudPayload } from "../../types/execution";
import { useFocusOnDoubleClick } from "../../hooks/useFocusOnDoubleClick";
import { measureAttrs, useMeasure } from "../../hooks/useMeasure";
import { MeasureReadout } from "../MeasureReadout";
import { ViewPresetButtons, useViewPresetEvents } from "../ViewPresetButtons";
import { gridSpec, sameFrame, type ViewPreset } from "../../lib/viewFit";
import type { PeekViewProps } from "./types";

const MAX_POINTS_CHOICES = [100_000, 200_000, 500_000, 2_000_000];
const FROZEN = PEEK_FROZEN;

interface Display {
  runId: string | null;
  cloud: CloudPayload | null;
  status: string | null;
  base: BaseCloud | null;
}

interface CloudTarget {
  resolved: { nodeId: string; port: string } | null;
  base: BaseCloud | null;
}

/** 每个窗口最后的视角与那时取景量的范围。运行期间窗里换成「正在计算…」会卸掉画布，算完回来接着用 ——
 *  不然每次重跑都被拉回全貌。窗口关掉之后下次存的时候清掉。 */
const keptViews = new Map<string, { bounds: Float32Array; view: CameraView }>();

export function CloudView({ win, src }: PeekViewProps) {
  const hostRef = useRef<HTMLDivElement>(null);
  const peekRoot = useRef<HTMLDivElement>(null);
  const [gridText, setGridText] = useState<string | null>(null);
  const sceneRef = useRef<Scene | null>(null);
  /** 上次取景量的范围（下面取景的 effect 用）。 */
  const fitted = useRef<Float32Array | null>(null);

  const [display, setDisplay] = useState<Display>({
    runId: null,
    cloud: null,
    status: null,
    base: null,
  });
  const [loading, setLoading] = useState(false);
  const [overlayBounds, setOverlayBounds] = useState<Float32Array | null>(null);
  const [sceneHost, setSceneHost] = useState<Scene | null>(null);
  const [measuring, setMeasuring] = useState(false);
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
  const { maxPoints, shading, ramp, pointSize } = win.opts;
  const pointSizeRef = useRef(pointSize);
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
  const [lo, hi] = useMemo(
    () => dataRangeOf(cloud, effectiveShading),
    [cloud, effectiveShading],
  );

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;
    const scene = createScene(host);
    sceneRef.current = scene;
    fitted.current = null;
    setSceneHost(scene);
    return () => {
      const open = new Set(usePeekStore.getState().windows.map((w) => w.id));
      for (const id of keptViews.keys()) if (!open.has(id)) keptViews.delete(id);
      if (fitted.current && open.has(win.id)) keptViews.set(win.id, { bounds: fitted.current, view: saveCameraView(scene) });
      scene.dispose();
      sceneRef.current = null;
      setSceneHost(null);
    };
  }, [win.id]);

  useEffect(
    () =>
      registerPeekCanvas(win.id, () => {
        const scene = sceneRef.current;
        if (!scene) return null;
        scene.renderFrame();
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
        const payload = await fetchCloud(key, async () =>
          decodeCloud(await transport.getOutputCloud(runId, resolved.nodeId, resolved.port, maxPoints)),
        );
        if (cancelled) return;
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

    setPoints(scene, 0, buildPoints(cloud, pointSizeRef.current));
  }, [cloud]);

  // 点大小只改材质，不进上面那个几何 effect（拖一下就重建几何太亏）
  useEffect(() => {
    pointSizeRef.current = pointSize;
    const points = sceneRef.current?.points[0];
    if (points) (points.material as THREE.PointsMaterial).size = pointSize;
  }, [pointSize]);

  // 验收脚本读相机：拖过视角之后 ⤢ 能不能回到全貌
  useEffect(() => {
    const scene = sceneRef.current;
    const host = hostRef.current;
    if (!scene || !host) return;
    const write = () => {
      const c = scene.active().position;
      const text = [c.x, c.y, c.z].map((v) => Number(v.toFixed(3))).join(",");
      if (host.dataset.cameraPos !== text) host.dataset.cameraPos = text;
    };
    scene.frameListeners.add(write);
    return () => {
      scene.frameListeners.delete(write);
    };
  }, []);

  // 换着色模式/色带只重写 color 属性，positions 和 boundingSphere 原样留着。
  useEffect(() => {
    const points = sceneRef.current?.points[0];
    if (!points || !cloud || cloud.pointCount === 0) return;
    paintPoints(points, cloud, { shading: effectiveShading, ramp, lo, hi });
  }, [cloud, effectiveShading, ramp, lo, hi]);

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

  // 换了云或换了几何时看一眼要不要取景：还在同一个坐标系里（sameFrame，与主预览同一条）就不动相机 ——
  // 调参数重跑之后，在窗口里转好的视角、双击设好的转心都留着（运行期间画布卸掉过的，放回卸掉前的视角）。
  // 以前每次重跑都拉回斜 45° 的全貌。第一片云、换到差得远的一片时照旧取景。
  // 必须排在上面那个 effect 之后：取景要量的是它刚建好的那一组线。
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    setOverlayBounds(overlayBoundsOf(scene.overlay));
    const bounds = unionBounds(cloud, scene.overlay);
    if (!bounds) {
      setGridText(null);
      return;
    }
    const kept = fitted.current ? null : keptViews.get(win.id);
    if (kept && sameFrame(kept.bounds, bounds)) {
      restoreCameraView(scene, kept.view);
      fitted.current = kept.bounds;
    }
    if (fitted.current && sameFrame(fitted.current, bounds)) {
      // 不重新取景：网格跟到这片云下面，格子能不换档就不换
      scene.setGrid(gridSpec(bounds, scene.grid?.cell ?? null));
    } else {
      fitToBounds(scene, bounds);
      fitted.current = bounds;
    }
    setGridText(scene.grid?.text ?? null);
  }, [cloud, shapeStat, win.id]);

  useEffect(() => {
    sceneRef.current?.setMode(cameraMode);
  }, [cameraMode]);

  // 每个窗口自己一份测量；M 键只管主预览。有云才能量
  const measureOn = measuring && cloud !== null && cloud.pointCount > 0;
  const { measure, clear: clearMeasure } = useMeasure(
    sceneHost,
    measureOn,
    [cloud, null],
    `${win.id}|${target.resolved?.nodeId ?? ""}|${target.resolved?.port ?? ""}`,
  );
  // 双击一个点：转心挪到它上面（测量时不接）
  useFocusOnDoubleClick(sceneHost, [cloud, null], measureOn);

  const setOpts = usePeekStore((s) => s.setOpts);
  const empty = !cloud && !shapeStat;
  const pickPreset = useCallback((view: ViewPreset) => {
    const scene = sceneRef.current;
    return scene ? applyViewPreset(scene, view) : false;
  }, []);
  useViewPresetEvents(peekRoot, pickPreset);
  const resize = (factor: number) =>
    setOpts(win.id, { pointSize: clampPointSize(Math.round(pointSize * factor * 10) / 10) });
  // 转过视角后回到全貌（与主预览的 ⤢ 同一个：底图云 + 叠画几何）
  const fit = () => {
    const scene = sceneRef.current;
    const bounds = scene ? unionBounds(cloud, scene.overlay) : null;
    if (scene && bounds) {
      fitToBounds(scene, bounds);
      setGridText(scene.grid?.text ?? null);
    }
  };
  const status = loading ? "正在取点云…" : display.status;
  const pointChoices = MAX_POINTS_CHOICES.includes(maxPoints)
    ? MAX_POINTS_CHOICES
    : [...MAX_POINTS_CHOICES, maxPoints].sort((a, b) => a - b);

  return (
    <div
      ref={peekRoot}
      className="peek-cloud"
      data-view-presets={cameraMode === "3d" ? "1" : undefined}
      data-testid="peek-cloud"
      data-camera={cameraMode}
      data-shading={effectiveShading}
      data-point-size={pointSize}
      data-run={display.runId ?? ""}
      data-base={display.base?.localId ?? ""}
      data-cloud-bounds={boundsAttr(cloud && cloud.pointCount > 0 ? cloud.bounds : null)}
      data-overlay-bounds={boundsAttr(overlayBounds)}
      {...measureAttrs(measureOn, measure)}
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
        {gridText && (
          <span className="peek-cloud__count" data-testid="peek-grid" title="网格一格多大">
            {gridText}
          </span>
        )}
        <span className="peek-cloud__spacer" />
        <select
          className="peek-cloud__select"
          data-testid="peek-shading"
          value={effectiveShading}
          onChange={(e) => setOpts(win.id, { shading: e.target.value as ShadingMode })}
          title={
            shading === effectiveShading ? "着色方式" : "这片点云没有该通道，已换成它实际有的着色"
          }
        >
          <option value="intensity" disabled={!hasIntensity}>
            强度{hasIntensity ? "" : "（无）"}
          </option>
          <option value="height">高度</option>
          <option value="normal" disabled={!hasNormals}>
            法线{hasNormals ? "" : "（无）"}
          </option>
          <option value="rgb" disabled={!hasRgb} title="点云自带的颜色（PCD 的 rgb、模型分割的类别色）">
            RGB{hasRgb ? "" : "（无）"}
          </option>
          <option value="flat">单色</option>
        </select>
        <select
          className="peek-cloud__select"
          data-testid="peek-ramp"
          value={ramp}
          onChange={(e) => setOpts(win.id, { ramp: e.target.value as RampName })}
          disabled={noRamp}
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
        <button
          type="button"
          className="peek__btn"
          data-testid="peek-point-smaller"
          disabled={pointSize <= POINT_SIZE_MIN}
          onClick={() => resize(1 / 1.25)}
          title="点小一点"
        >
          −
        </button>
        <button
          type="button"
          className="peek__btn"
          data-testid="peek-point-bigger"
          disabled={pointSize >= POINT_SIZE_MAX}
          onClick={() => resize(1.25)}
          title="点大一点（降采样后只剩几千点时看得清）"
        >
          +
        </button>
        <button
          type="button"
          className="peek__btn"
          data-testid="peek-fit"
          disabled={empty}
          onClick={fit}
          title="缩放到全部（底图云 + 叠画几何）"
        >
          ⤢
        </button>
        <ViewPresetButtons className="peek__btn" disabled={cameraMode !== "3d"} onPick={pickPreset} />
        <button
          type="button"
          className="peek__btn"
          data-testid="peek-measure"
          data-on={measureOn ? "1" : "0"}
          disabled={!cloud || cloud.pointCount === 0}
          onClick={() => setMeasuring(!measuring)}
          title={measuring ? "关掉测量" : "测量：单击选点看坐标，再点一个量距离；拖动照旧转视角"}
        >
          测量
        </button>
      </div>

      <div className="peek-cloud__stage">
        <div className="peek-cloud__canvas" ref={hostRef} data-testid="peek-cloud-canvas" />
        {status !== null && (loading || empty) && (
          <div className="peek-cloud__empty" data-testid="peek-cloud-status">
            {status}
          </div>
        )}
        {measureOn && (
          <MeasureReadout
            measure={measure}
            mode={cameraMode}
            pointCount={cloud?.pointCount ?? 0}
            onClear={clearMeasure}
          />
        )}
      </div>
    </div>
  );
}
