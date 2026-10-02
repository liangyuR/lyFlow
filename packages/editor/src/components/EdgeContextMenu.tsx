// 连线的右键菜单：查看内容、在此插入 Reroute、删除连线。

import { useRef } from "react";

import { useMenuPlacement } from "../hooks/useMenuPlacement";
import { useGraphStore } from "../store/graph";

export interface EdgeMenuState {
  edgeId: string;
  x: number;
  y: number;
}

export function EdgeContextMenu({
  menu,
  onPeek,
  onReroute,
  onClose,
}: {
  menu: EdgeMenuState;
  onPeek: (edgeId: string, at: { x: number; y: number }) => void;
  onReroute: (edgeId: string, at: { x: number; y: number }) => void;
  onClose: () => void;
}) {
  const box = useRef<HTMLDivElement>(null);
  const style = useMenuPlacement(box, { x: menu.x, y: menu.y });
  const at = { x: menu.x, y: menu.y };
  return (
    <div ref={box} className="ctxmenu" style={style} data-testid="edge-context-menu" onClick={(e) => e.stopPropagation()}>
      <button
        type="button"
        data-testid="edge-ctx-peek"
        onClick={() => {
          onPeek(menu.edgeId, at);
          onClose();
        }}
      >
        查看内容
      </button>
      <button
        type="button"
        data-testid="edge-ctx-reroute"
        onClick={() => {
          onReroute(menu.edgeId, at);
          onClose();
        }}
      >
        在此插入 Reroute
      </button>
      <button
        type="button"
        data-testid="edge-ctx-delete"
        onClick={() => {
          useGraphStore.getState().disconnect([menu.edgeId]);
          onClose();
        }}
      >
        删除连线
      </button>
    </div>
  );
}
