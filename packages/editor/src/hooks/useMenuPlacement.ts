// 右键菜单挂上之后量一下自己多大，再按 placeMenu 摆进窗口。在布局阶段做完，第一帧就在对的地方，不闪。
// 以前照鼠标位置往右下摆：在画布下半部分右键一个节点，二十来项的菜单大半截跑到窗口外面点不着。

import { useLayoutEffect, useState, type CSSProperties, type RefObject } from "react";

import { placeMenu, type Placement } from "../lib/placement";

export function useMenuPlacement(box: RefObject<HTMLElement>, at: { x: number; y: number }): CSSProperties {
  const key = `${at.x},${at.y}`;
  const [placed, setPlaced] = useState<{ key: string; p: Placement } | null>(null);

  // 换了位置（菜单在别处重新打开）先按鼠标位置、不限高摆一次，量到的才是它本来的大小
  useLayoutEffect(() => {
    if (placed?.key === key) return;
    const el = box.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    setPlaced({
      key,
      p: placeMenu(at, { width: r.width, height: r.height }, { width: window.innerWidth, height: window.innerHeight }),
    });
  });

  const p = placed?.key === key ? placed.p : { left: at.x, top: at.y };
  return p.maxHeight === undefined
    ? { left: p.left, top: p.top }
    : { left: p.left, top: p.top, maxHeight: p.maxHeight, overflowY: "auto" };
}
