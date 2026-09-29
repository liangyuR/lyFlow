// 预览里的测量工具（docs/measure-plan.md）：单击选点、两点测距。主预览（Viewer3D，两栏时 A / B 各一片云）与
// 连线查看器的点云视图（peek/CloudView）共用这一份 —— 单击判定、吸附、标记点、生命周期都在这里。

import { useEffect, useRef, useState } from "react";

import { pickMatrixOf, setMeasureMarks, type Scene } from "../lib/cloudScene";
import { addPick, NO_MEASURE, pickCount, pickNearest, type Measure } from "../lib/pick";
import type { CloudPayload } from "../types/execution";

export interface MeasureState {
  measure: Measure;
  clear(): void;
}

/**
 * @param scene   画面所在的场景（还没建好是 null）
 * @param on      工具开着没有；关掉就清
 * @param clouds  每一栏正在画的云（单栏只给一片）；变了而点位还在 = 「云已更新」
 * @param resetKey 换了就清（换节点、换 B）
 */
export function useMeasure(
  scene: Scene | null,
  on: boolean,
  clouds: readonly [CloudPayload | null, CloudPayload | null],
  resetKey: string,
): MeasureState {
  const [measure, setMeasure] = useState<Measure>(NO_MEASURE);
  // 换节点就清掉；同一节点重跑保留坐标、标「云已更新」（M7）。两个 effect 的顺序不能换：
  // 换节点的那一次提交里云也变了，先清再标，标的是空测量、不起作用
  useEffect(() => {
    setMeasure(NO_MEASURE);
  }, [resetKey, on]);
  useEffect(() => {
    setMeasure((m) => (pickCount(m) > 0 && !m.stale ? { ...m, stale: true } : m));
  }, [clouds[0], clouds[1]]);
  useEffect(() => {
    if (!scene) return;
    setMeasureMarks(scene, [measure.p1, measure.p2].filter((p) => p !== null).map((p) => p.xyz));
  }, [scene, measure]);

  // 单击选点：按下到松开位移 < 4 px 且 < 400 ms 才算单击，拖动照旧交给 OrbitControls 转视角
  const cloudsRef = useRef(clouds);
  cloudsRef.current = clouds;
  useEffect(() => {
    if (!on || !scene) return;
    const el = scene.renderer.domElement;
    let down: { x: number; y: number; t: number } | null = null;
    const onDown = (e: PointerEvent) => {
      down = e.button === 0 ? { x: e.clientX, y: e.clientY, t: performance.now() } : null;
    };
    const onUp = (e: PointerEvent) => {
      const d = down;
      down = null;
      if (!d || e.button !== 0) return;
      if (Math.hypot(e.clientX - d.x, e.clientY - d.y) >= 4 || performance.now() - d.t > 400) return;
      const hit = scene.paneAt(e.clientX, e.clientY);
      const c = hit ? cloudsRef.current[hit.pane] : null;
      if (!hit || !c || c.pointCount === 0) return;
      const found = pickNearest(c.xyz, c.pointCount, pickMatrixOf(scene), hit.viewport, hit.local);
      if (!found) return;
      const i = found.index;
      const xyz: [number, number, number] = [c.xyz[i * 3]!, c.xyz[i * 3 + 1]!, c.xyz[i * 3 + 2]!];
      setMeasure((m) => addPick(m, { xyz, pane: hit.pane }));
    };
    el.addEventListener("pointerdown", onDown);
    el.addEventListener("pointerup", onUp);
    el.style.cursor = "crosshair";
    return () => {
      el.removeEventListener("pointerdown", onDown);
      el.removeEventListener("pointerup", onUp);
      el.style.cursor = "";
    };
  }, [scene, on]);

  return { measure, clear: () => setMeasure(NO_MEASURE) };
}

/** 根元素上给验收脚本读的几个属性（主预览与连线查看器同一套）。 */
export function measureAttrs(on: boolean, measure: Measure): Record<string, string | number | undefined> {
  const xyz = (p: Measure["p1"]) => (p ? p.xyz.map((v) => Number(v.toPrecision(6))).join(",") : undefined);
  const d = measure.p1 && measure.p2
    ? Math.hypot(...measure.p2.xyz.map((v, i) => v - measure.p1!.xyz[i]!))
    : null;
  return {
    "data-measuring": on ? "1" : "0",
    "data-measure": pickCount(measure),
    "data-measure-p1": xyz(measure.p1),
    "data-measure-p2": xyz(measure.p2),
    "data-measure-dist": d === null ? undefined : Number(d.toPrecision(6)),
    "data-measure-panes": measure.p1
      ? [measure.p1, measure.p2].filter((p) => p !== null).map((p) => (p.pane === 0 ? "A" : "B")).join(",")
      : undefined,
    "data-measure-stale": measure.stale ? "1" : undefined,
  };
}
