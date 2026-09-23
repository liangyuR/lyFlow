import { create } from "zustand";

import { newLocalId } from "../lib/ids";
import { levelOf, type SubPath } from "../lib/subgraph";
import { useUiStore } from "./ui";
import type { RampName } from "../lib/ramps";
import type { ShadingMode } from "../components/Viewer3D";
import type { GraphDoc, PortRef } from "../types/graph";

/** fields = Bundle 的字段表（m8-plan L17）：先列字段，点进字段再按字段类型换视图。 */
export type PeekView = "cloud3d" | "cloud2d" | "tensor" | "value" | "indices" | "fields";

export type TensorLayout = "auto" | "HWC" | "CHW" | "NHWC" | "NCHW";

export interface PeekOpts {
  maxPoints: number;
  shading: ShadingMode;
  ramp: RampName;
  layout: TensorLayout;
  sliceIndex: number;
  channel: number | "rgb";
  range: [number, number] | "auto";
}

export interface PeekWindow {
  id: string;
  edgeId: string;
  path: SubPath;
  from: PortRef;
  screen: { x: number; y: number };
  size: { w: number; h: number };
  z: number;
  locked: { runId: string } | null;
  view: PeekView;
  viewAuto: boolean;
  opts: PeekOpts;
  /** 看的是 Bundle 端口里的哪个字段（`<port>.<field>` 寻址，m8-plan L3 / L17）。null = 整个端口。 */
  field: string | null;
}

export type PeekOpenRequest = Omit<
  PeekWindow,
  "id" | "z" | "locked" | "size" | "opts" | "viewAuto" | "field"
> &
  Partial<Pick<PeekWindow, "size" | "opts" | "locked" | "viewAuto" | "field">>;

export const PEEK_FROZEN = "快照已冻结：这次运行的结果已被后端回收（见 ADR-0019）";

export const PEEK_MAX_WINDOWS = 6;
export const PEEK_MAX_WEBGL = 4;
export const PEEK_DEFAULT_SIZE = { w: 360, h: 280 };

export function isWebglView(view: PeekView): boolean {
  return view === "cloud3d" || view === "cloud2d";
}

function countWebgl(windows: readonly PeekWindow[], exceptId?: string): number {
  let n = 0;
  for (const w of windows) {
    if (w.id !== exceptId && isWebglView(w.view)) n += 1;
  }
  return n;
}

export const PEEK_DEFAULT_OPTS: PeekOpts = {
  maxPoints: 200_000,
  shading: "intensity",
  ramp: "viridis",
  layout: "auto",
  sliceIndex: 0,
  channel: 0,
  range: "auto",
};

interface PeekState {
  windows: PeekWindow[];
  topZ: number;

  open(win: PeekOpenRequest): string | null;
  close(id: string): void;
  closeAll(): void;
  focus(id: string): void;
  move(id: string, screen: { x: number; y: number }): void;
  resize(id: string, size: { w: number; h: number }): void;
  setView(id: string, view: PeekView): void;
  syncView(id: string, view: PeekView): void;
  setOpts(id: string, partial: Partial<PeekOpts>): void;
  setLocked(id: string, locked: { runId: string } | null): void;
  /** 进 / 出 Bundle 的一个字段。视图回到自动，跟着字段类型换。 */
  setField(id: string, field: string | null): void;
  prune(doc: GraphDoc, path: SubPath): void;
}

export const PEEK_MIN_VISIBLE = 40;

export interface PeekBounds {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

export function clampPeekScreen(
  bounds: PeekBounds,
  screen: { x: number; y: number },
  size: { w: number; h: number },
): { x: number; y: number } {
  const minX = bounds.left + PEEK_MIN_VISIBLE - size.w;
  const maxX = Math.max(minX, bounds.right - PEEK_MIN_VISIBLE);
  const maxY = Math.max(bounds.top, bounds.bottom - PEEK_MIN_VISIBLE);
  return {
    x: Math.min(Math.max(screen.x, minX), maxX),
    y: Math.min(Math.max(screen.y, bounds.top), maxY),
  };
}

function samePath(a: SubPath, b: SubPath): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i += 1) {
    if (a[i]!.nodeId !== b[i]!.nodeId || a[i]!.subgraphId !== b[i]!.subgraphId) return false;
  }
  return true;
}

export function findWindowForEdge(
  windows: readonly PeekWindow[],
  edgeId: string,
  path: SubPath,
): PeekWindow | undefined {
  return windows.find((w) => w.edgeId === edgeId && samePath(w.path, path));
}

export const usePeekStore = create<PeekState>((set, get) => ({
  windows: [],
  topZ: 0,

  open(win) {
    const state = get();
    let windows = state.windows;
    if (windows.length >= PEEK_MAX_WINDOWS) {
      const victim = windows.find((w) => w.locked === null);
      if (!victim) {
        useUiStore.getState().showToast("窗口太多了，先关掉几个", "warn");
        return null;
      }
      windows = windows.filter((w) => w.id !== victim.id);
    }
    let view = win.view;
    let viewAuto = win.viewAuto ?? true;
    if (isWebglView(view) && countWebgl(windows) >= PEEK_MAX_WEBGL) {
      const victim = windows.find((w) => w.locked === null && isWebglView(w.view));
      if (victim) {
        windows = windows.filter((w) => w.id !== victim.id);
      } else {
        useUiStore.getState().showToast("3D 窗口太多了，先关掉几个", "warn");
        view = "value";
        viewAuto = false;
      }
    }
    const taken = new Set(windows.map((w) => w.id));
    const id = newLocalId("peek", taken);
    const z = state.topZ + 1;
    const created: PeekWindow = {
      id,
      edgeId: win.edgeId,
      path: win.path,
      from: win.from,
      screen: win.screen,
      size: win.size ?? PEEK_DEFAULT_SIZE,
      z,
      locked: win.locked ?? null,
      view,
      viewAuto,
      opts: { ...PEEK_DEFAULT_OPTS, ...win.opts },
      field: win.field ?? null,
    };
    set({ windows: windows.concat(created), topZ: z });
    return id;
  },

  close(id) {
    const windows = get().windows;
    if (!windows.some((w) => w.id === id)) return;
    set({ windows: windows.filter((w) => w.id !== id) });
  },

  closeAll() {
    if (get().windows.length === 0) return;
    set({ windows: [] });
  },

  focus(id) {
    const state = get();
    const win = state.windows.find((w) => w.id === id);
    if (!win || win.z === state.topZ) return;
    const z = state.topZ + 1;
    set({
      windows: state.windows.map((w) => (w.id === id ? { ...w, z } : w)),
      topZ: z,
    });
  },

  move(id, screen) {
    set({
      windows: get().windows.map((w) =>
        w.id === id && (w.screen.x !== screen.x || w.screen.y !== screen.y)
          ? { ...w, screen }
          : w,
      ),
    });
  },

  resize(id, size) {
    set({
      windows: get().windows.map((w) =>
        w.id === id && (w.size.w !== size.w || w.size.h !== size.h) ? { ...w, size } : w,
      ),
    });
  },

  setView(id, view) {
    const windows = get().windows;
    const win = windows.find((w) => w.id === id);
    if (!win) return;
    if (
      isWebglView(view) &&
      !isWebglView(win.view) &&
      countWebgl(windows, id) >= PEEK_MAX_WEBGL
    ) {
      useUiStore.getState().showToast("3D 窗口太多了，先关掉几个", "warn");
      return;
    }
    if (win.view === view && !win.viewAuto) return;
    set({
      windows: windows.map((w) => (w.id === id ? { ...w, view, viewAuto: false } : w)),
    });
  },

  syncView(id, view) {
    const windows = get().windows;
    const win = windows.find((w) => w.id === id);
    if (!win || win.view === view) return;
    if (
      isWebglView(view) &&
      !isWebglView(win.view) &&
      countWebgl(windows, id) >= PEEK_MAX_WEBGL
    ) {
      return;
    }
    set({ windows: windows.map((w) => (w.id === id ? { ...w, view } : w)) });
  },

  setOpts(id, partial) {
    set({
      windows: get().windows.map((w) =>
        w.id === id ? { ...w, opts: { ...w.opts, ...partial } } : w,
      ),
    });
  },

  setLocked(id, locked) {
    set({
      windows: get().windows.map((w) => (w.id === id ? { ...w, locked } : w)),
    });
  },

  setField(id, field) {
    set({
      windows: get().windows.map((w) =>
        w.id === id && w.field !== field ? { ...w, field, viewAuto: true } : w,
      ),
    });
  },

  prune(doc, path) {
    const windows = get().windows;
    if (windows.length === 0) return;
    const kept = windows.filter((w) => {
      if (!samePath(w.path, path)) return false;
      const level = levelOf(doc, w.path);
      return level.edges.some(
        (e) => e.id === w.edgeId && e.from.node === w.from.node && e.from.port === w.from.port,
      );
    });
    if (kept.length === windows.length) return;
    set({ windows: kept });
  },
}));
