// 预览里双击一个点：视角的中心挪到它上面（相机跟着平移、不转），之后转视角就绕着它转 —— 看大云里的一小块
// （一条缝、一个孔）时不用来回拖着找转心。双击空白处不动。主预览（两栏时 A / B 各一片云）与连线查看器的点云视图共用。
// 测量开着时不接：双击 = 两次单击，测量那边已经各选了一个点；拖框时也不接（拖框独占指针）。

import { useEffect, useRef } from "react";

import { focusOn, pickMatrixOf, type Scene } from "../lib/cloudScene";
import { pickNearest } from "../lib/pick";
import type { CloudPayload } from "../types/execution";

export function useFocusOnDoubleClick(
  scene: Scene | null,
  clouds: readonly [CloudPayload | null, CloudPayload | null],
  disabled: boolean,
): void {
  const cloudsRef = useRef(clouds);
  cloudsRef.current = clouds;
  useEffect(() => {
    if (!scene || disabled) return;
    const el = scene.renderer.domElement;
    const onDouble = (e: MouseEvent) => {
      const hit = scene.paneAt(e.clientX, e.clientY);
      const c = hit ? cloudsRef.current[hit.pane] : null;
      if (!hit || !c || c.pointCount === 0) return;
      // 比测量的选点（8 px）宽一点：转心不用那么准，双击稀疏的云时别老落空
      const found = pickNearest(c.xyz, c.pointCount, pickMatrixOf(scene), hit.viewport, hit.local, 12);
      if (!found) return;
      const i = found.index;
      const p: [number, number, number] = [c.xyz[i * 3]!, c.xyz[i * 3 + 1]!, c.xyz[i * 3 + 2]!];
      focusOn(scene, p);
      // 验收脚本读它：转心落在了哪个点上
      el.dataset.focus = p.map((v) => Number(v.toPrecision(6))).join(",");
    };
    el.addEventListener("dblclick", onDouble);
    return () => el.removeEventListener("dblclick", onDouble);
  }, [scene, disabled]);
}
