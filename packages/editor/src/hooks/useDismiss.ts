// 弹出层（下拉菜单）的收起规矩：Esc 收起（捕获阶段拦下，不再落到全局快捷键上 —— 在子图里按 Esc 是退出子图），
// 在它外面按下鼠标收起。box 是弹出层连同打开它的那个按钮的外框：点按钮本身归按钮（它自己切换开合）。
// 工具栏的最近文件、库目录以前只有再点一下按钮才收，配方下拉框点外面收、Esc 不收。

import { useEffect, type RefObject } from "react";

export function useDismiss(open: boolean, box: RefObject<HTMLElement | null>, close: () => void): void {
  useEffect(() => {
    if (!open) return;
    const owner = box.current?.ownerDocument ?? document;
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Escape") return;
      e.preventDefault();
      e.stopPropagation();
      close();
    };
    const onDown = (e: PointerEvent) => {
      if (box.current && !box.current.contains(e.target as Node)) close();
    };
    owner.addEventListener("keydown", onKey, true);
    owner.addEventListener("pointerdown", onDown, true);
    return () => {
      owner.removeEventListener("keydown", onKey, true);
      owner.removeEventListener("pointerdown", onDown, true);
    };
  }, [open, box, close]);
}
