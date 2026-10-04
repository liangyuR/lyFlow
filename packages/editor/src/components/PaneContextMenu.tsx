// 画布空白处的右键菜单：添加算子、全选、整理布局、适配视图、快捷键面板。以前空白处右键毫无反应
// （React Flow 在「右键拖平移」模式下吞掉了空白处的 contextmenu，壳又挡掉了浏览器自己的菜单）。

import { useRef } from "react";

import { useMenuPlacement } from "../hooks/useMenuPlacement";
import { keyHint } from "../lib/keymap";

export interface PaneMenuState {
  x: number;
  y: number;
}

export function PaneContextMenu({
  menu,
  onAddOperator,
  onSelectAll,
  onLayout,
  onFitView,
  onHelp,
  onClose,
}: {
  menu: PaneMenuState;
  /** 在右键处打开算子搜索（与双击空白处一样）。 */
  onAddOperator: (at: { x: number; y: number }) => void;
  onSelectAll: () => void;
  onLayout?: (() => void) | undefined;
  onFitView: () => void;
  onHelp: () => void;
  onClose: () => void;
}) {
  const box = useRef<HTMLDivElement>(null);
  const style = useMenuPlacement(box, { x: menu.x, y: menu.y });
  const item = (testId: string, label: string, run: () => void, key?: string) => (
    <button
      type="button"
      data-testid={testId}
      onClick={() => {
        onClose();
        run();
      }}
    >
      {label} {key && <kbd>{keyHint(key)}</kbd>}
    </button>
  );
  return (
    <div ref={box} className="ctxmenu" style={style} data-testid="pane-context-menu" onClick={(e) => e.stopPropagation()}>
      {item("pane-ctx-add", "添加算子…", () => onAddOperator({ x: menu.x, y: menu.y }), "search")}
      <hr className="ctxmenu__sep" />
      {item("pane-ctx-select-all", "全选", onSelectAll, "selectAll")}
      {onLayout && item("pane-ctx-layout", "整理布局", onLayout, "layout")}
      {item("pane-ctx-fit", "适配视图", onFitView, "fitView")}
      <hr className="ctxmenu__sep" />
      {item("pane-ctx-help", "快捷键与鼠标用法", onHelp, "help")}
    </div>
  );
}
