import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { pinRun, unpinRun } from "../lib/cloudCache";
import { exportCanvasPng } from "../lib/exportPng";
import { takePeekCanvas } from "../lib/peekCanvas";
import { defaultViewFor, usePeekSource, viewsFor, type PeekSource } from "../lib/peekSource";
import { useManifestStore } from "../store/manifest";
import {
  clampPeekScreen,
  isWebglView,
  usePeekStore,
  type PeekView,
  type PeekWindow,
} from "../store/peek";
import { useUiStore } from "../store/ui";

import { CloudView } from "./peek/CloudView";
import { FieldsView } from "./peek/FieldsView";
import { IndicesView } from "./peek/IndicesView";
import { TensorView } from "./peek/TensorView";
import { ValueView } from "./peek/ValueView";

const PEEK_Z_BASE = 30;

const VIEW_LABEL: Record<PeekView, string> = {
  cloud3d: "3D",
  cloud2d: "2D",
  tensor: "图像",
  value: "文本",
  indices: "列表",
  fields: "字段",
};

const VIEW_TITLE: Record<PeekView, string> = {
  cloud3d: "3D 自由视角点云",
  cloud2d: "正交俯视 XY（剖面 + 2D 几何）",
  tensor: "张量图像",
  value: "键值表 + 原始 JSON",
  indices: "下标列表",
  fields: "Bundle 的字段表，点进字段看它的内容",
};

function boundsOf(el: HTMLElement | null) {
  const canvas = el?.closest(".canvas");
  if (!canvas) return null;
  const r = canvas.getBoundingClientRect();
  return { left: r.left, top: r.top, right: r.right, bottom: r.bottom };
}

function samePath(a: PeekWindow["path"], b: PeekWindow["path"]): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i += 1) {
    if (a[i]!.nodeId !== b[i]!.nodeId || a[i]!.subgraphId !== b[i]!.subgraphId) return false;
  }
  return true;
}

function shortRun(runId: string): string {
  return runId.length > 6 ? runId.slice(-6) : runId;
}

function canvasView(view: PeekView): boolean {
  return isWebglView(view) || view === "tensor";
}

export function EdgePeek({ win, rank }: { win: PeekWindow; rank: number }) {
  const rootRef = useRef<HTMLDivElement>(null);
  const dragRef = useRef<{ dx: number; dy: number; pos: { x: number; y: number } } | null>(null);
  const [dragPos, setDragPos] = useState<{ x: number; y: number } | null>(null);

  const live = usePeekSource(win.path, win.from, win.field);
  const frozenRef = useRef<PeekSource | null>(null);
  useEffect(() => {
    if (win.locked) return;
    frozenRef.current = live.status === null ? live : null;
  }, [win.locked, live]);

  const frozen = win.locked ? frozenRef.current : null;
  const src = frozen ?? live;
  const lockedNoData = win.locked !== null && frozen === null;

  const typeColor = useManifestStore((s) =>
    src.type ? (s.typesByName.get(src.type)?.color ?? "#6b7280") : "#6b7280",
  );

  const close = usePeekStore((s) => s.close);
  const focus = usePeekStore((s) => s.focus);
  const move = usePeekStore((s) => s.move);
  const setLocked = usePeekStore((s) => s.setLocked);
  const setView = usePeekStore((s) => s.setView);
  const syncView = usePeekStore((s) => s.syncView);
  const setField = usePeekStore((s) => s.setField);

  const views = useMemo(() => viewsFor(src.type), [src.type]);
  const wanted = defaultViewFor(src.type);
  useEffect(() => {
    if (!win.viewAuto && views.includes(win.view)) return;
    if (win.view !== wanted) syncView(win.id, wanted);
  }, [win.id, win.view, win.viewAuto, views, wanted, syncView]);

  const lockedRun = win.locked?.runId ?? null;
  useEffect(() => {
    if (!lockedRun) return;
    pinRun(lockedRun);
    return () => unpinRun(lockedRun);
  }, [lockedRun]);

  useEffect(() => {
    const el = rootRef.current;
    if (!el) return;
    const observer = new ResizeObserver(() => {
      const w = el.offsetWidth;
      const h = el.offsetHeight;
      const current = usePeekStore.getState().windows.find((x) => x.id === win.id);
      if (!current || (current.size.w === w && current.size.h === h)) return;
      usePeekStore.getState().resize(win.id, { w, h });
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, [win.id]);

  const onTitlePointerDown = useCallback(
    (e: React.PointerEvent<HTMLElement>) => {
      if (e.button !== 0) return;
      if ((e.target as HTMLElement).closest("button")) return;
      e.preventDefault();
      e.currentTarget.setPointerCapture(e.pointerId);
      dragRef.current = {
        dx: e.clientX - win.screen.x,
        dy: e.clientY - win.screen.y,
        pos: win.screen,
      };
      setDragPos(win.screen);
    },
    [win.screen],
  );

  const onTitlePointerMove = useCallback(
    (e: React.PointerEvent<HTMLElement>) => {
      const drag = dragRef.current;
      if (!drag) return;
      const bounds = boundsOf(rootRef.current);
      const raw = { x: e.clientX - drag.dx, y: e.clientY - drag.dy };
      const next = bounds ? clampPeekScreen(bounds, raw, win.size) : raw;
      drag.pos = next;
      setDragPos(next);
    },
    [win.size],
  );

  const endDrag = useCallback(
    (e: React.PointerEvent<HTMLElement>) => {
      const drag = dragRef.current;
      if (!drag) return;
      dragRef.current = null;
      if (e.currentTarget.hasPointerCapture(e.pointerId)) {
        e.currentTarget.releasePointerCapture(e.pointerId);
      }
      setDragPos(null);
      move(win.id, drag.pos);
    },
    [move, win.id],
  );

  const copyJson = useCallback(() => {
    const value = src.stat?.value ?? src.stat;
    const ui = useUiStore.getState();
    if (!value) {
      ui.showToast("这个端口这次运行没有可复制的值", "warn");
      return;
    }
    void navigator.clipboard
      .writeText(JSON.stringify(value, null, 2))
      .then(() => ui.showToast("已复制"))
      .catch((err: unknown) =>
        ui.showToast(`复制失败：${err instanceof Error ? err.message : String(err)}`, "warn"),
      );
  }, [src.stat]);

  const gotoSource = useCallback(() => {
    const ui = useUiStore.getState();
    if (!samePath(ui.path, win.path)) ui.setPath(win.path);
    ui.setSelection([win.from.node], []);
  }, [win.path, win.from.node]);

  const openInMain = useCallback(() => {
    const ui = useUiStore.getState();
    if (!samePath(ui.path, win.path)) ui.setPath(win.path);
    ui.setPinnedNode(win.from.node);
    ui.showToast(`主 3D 视图已钉住 ${live.label}`);
  }, [win.path, win.from.node, live.label]);

  const exportPng = useCallback(() => {
    const ui = useUiStore.getState();
    const canvas = takePeekCanvas(win.id);
    if (!canvas) {
      ui.showToast("这个视图还没有画面可以导出", "warn");
      return;
    }
    void exportCanvasPng(canvas, `${win.from.node}-${win.from.port}`);
  }, [win.id, win.from.node, win.from.port]);

  const toggleLock = useCallback(() => {
    if (win.locked) {
      setLocked(win.id, null);
      return;
    }
    if (!src.runId) {
      useUiStore.getState().showToast("还没有运行结果可以锁定", "warn");
      return;
    }
    setLocked(win.id, { runId: src.runId });
  }, [win.locked, win.id, src.runId, setLocked]);

  const pos = dragPos ?? win.screen;

  return (
    <div
      ref={rootRef}
      className="peek"
      data-testid="edge-peek"
      data-peek-id={win.id}
      data-edge-id={win.edgeId}
      data-view={win.view}
      data-locked={win.locked ? "1" : "0"}
      data-type={src.type ?? undefined}
      data-field={src.field ?? undefined}
      style={{
        left: pos.x,
        top: pos.y,
        width: win.size.w,
        height: win.size.h,
        zIndex: PEEK_Z_BASE + rank,
      }}
      onPointerDown={() => focus(win.id)}
      onClick={(e) => e.stopPropagation()}
      onDoubleClick={(e) => e.stopPropagation()}
      onContextMenu={(e) => e.stopPropagation()}
    >
      <header
        className="peek__title"
        data-testid="peek-title"
        onPointerDown={onTitlePointerDown}
        onPointerMove={onTitlePointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
      >
        <span className="peek__name" title={`${win.from.node}.${win.from.port}`}>
          {src.label}
        </span>
        <span className="peek__sep">·</span>
        <span className="peek__port">
          {win.from.port}
          {src.field && <span className="peek__field">.{src.field}</span>}
        </span>
        <span
          className="peek__type"
          style={{ borderColor: typeColor, color: typeColor }}
          title={src.type ?? "类型未知"}
        >
          {src.type ?? "?"}
        </span>
        {win.locked && (
          <span
            className="peek__locked"
            data-testid="peek-locked-tag"
            data-run={win.locked.runId}
            title={`快照锁定在 run ${win.locked.runId}，不跟随新的运行`}
          >
            已锁定 · {shortRun(win.locked.runId)}
          </span>
        )}
        <span className="peek__spacer" />
        <button
          type="button"
          className="peek__btn"
          data-testid="peek-lock"
          data-on={win.locked ? "1" : "0"}
          title={win.locked ? "解除快照锁定" : "锁定当前快照"}
          onClick={toggleLock}
        >
          {win.locked ? "🔒" : "🔓"}
        </button>
        <button
          type="button"
          className="peek__btn"
          data-testid="peek-close"
          title="关闭"
          onClick={() => close(win.id)}
        >
          ✕
        </button>
      </header>

      <div className="peek__tools">
        {src.field && (
          <button
            type="button"
            className="peek__tool"
            data-testid="peek-field-back"
            title={`回到 ${src.bundle?.label ?? src.bundle?.kind ?? "Bundle"} 的字段表`}
            onClick={() => setField(win.id, null)}
          >
            ‹ 字段
          </button>
        )}
        {views.length > 1 && (
          <div className="peek__views" role="group" aria-label="视图">
            {views.map((v) => (
              <button
                key={v}
                type="button"
                className="peek__view"
                data-testid={`peek-view-${v}`}
                data-active={win.view === v ? "1" : "0"}
                title={VIEW_TITLE[v]}
                onClick={() => setView(win.id, v)}
              >
                {VIEW_LABEL[v]}
              </button>
            ))}
          </div>
        )}
        <button
          type="button"
          className="peek__tool"
          data-testid="peek-copy"
          onClick={copyJson}
        >
          复制 JSON
        </button>
        <button
          type="button"
          className="peek__tool"
          data-testid="peek-goto"
          onClick={gotoSource}
        >
          跳到源节点
        </button>
        <button
          type="button"
          className="peek__tool"
          data-testid="peek-open-main"
          title="在右侧主 3D 视图里钉住这个节点"
          onClick={openInMain}
        >
          在主 3D 视图打开
        </button>
        <button
          type="button"
          className="peek__tool"
          data-testid="peek-export-png"
          disabled={!canvasView(win.view)}
          title={
            canvasView(win.view)
              ? "把当前画面存成 PNG"
              : "这个视图没有画面，只有点云与张量图像能导出 PNG"
          }
          onClick={exportPng}
        >
          导出 PNG
        </button>
      </div>

      <div className="peek__body">
        {lockedNoData ? (
          <p className="peek__status" data-testid="peek-status">
            这一份快照没有数据
          </p>
        ) : src.status !== null ? (
          <p className="peek__status" data-testid="peek-status">
            {src.status}
          </p>
        ) : win.view === "cloud3d" || win.view === "cloud2d" ? (
          <CloudView win={win} src={src} />
        ) : win.view === "tensor" ? (
          <TensorView win={win} src={src} />
        ) : win.view === "indices" ? (
          <IndicesView win={win} src={src} />
        ) : win.view === "fields" ? (
          <FieldsView src={src} onPick={(f) => setField(win.id, f)} />
        ) : src.stat ? (
          <ValueView stat={src.stat} />
        ) : (
          <p className="peek__status" data-testid="peek-status">
            该节点尚未产出结果
          </p>
        )}
      </div>
    </div>
  );
}
