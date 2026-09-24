// 画布级的动效（docs/motion-plan.md N1 / N3 / N4）：谁是新出现的、删掉的节点留一个残影、
// 自动布局的位置过渡。三件事都从同一个地方来 —— 对 graph store 的**同步**订阅：
// 在那一刻 React 还没重渲，被删节点的 DOM 还在，新节点也还没挂载。

import { animate, animateMini, type AnimationPlaybackControls } from "motion/react";
import { useCallback, useEffect, useRef, useSyncExternalStore, type RefObject } from "react";

import {
  clearEntering,
  GHOST_EXIT,
  GHOST_MAX,
  layoutTransitionRequested,
  markEntering,
  MOTION_MS,
  MOTION_NODE_LIMIT,
  TRANSITION,
} from "../lib/motion";
import { levelOf, pathPrefix } from "../lib/subgraph";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";

type Point = { x: number; y: number };
/** 布局过渡期间喂给 React Flow 的临时位置。null = 没有过渡，一切以 doc 为准。 */
export type PositionOverride = ReadonlyMap<string, Point> | null;

/** 当前层级的「身份证」：差分只在同一张图（epoch）、同一层（path）里做。 */
interface LevelSnapshot {
  epoch: number;
  pathKey: string;
  nodes: Map<string, Point>;
  edges: Set<string>;
}

function snapshot(): LevelSnapshot {
  const g = useGraphStore.getState();
  const path = useUiStore.getState().path;
  const lvl = levelOf(g.doc, path);
  const nodes = new Map<string, Point>();
  for (const n of lvl.nodes) nodes.set(n.id, n.ui?.position ?? { x: 0, y: 0 });
  return { epoch: g.epoch, pathKey: pathPrefix(path), nodes, edges: new Set(lvl.edges.map((e) => e.id)) };
}

/** 位置覆盖放在一个外部 store 里，经 useSyncExternalStore 读：它和 graph store 的更新
 *  必须落进**同一次**渲染。用 useState 的话两者车道不同（订阅回调里的 setState 是
 *  DefaultLane，zustand 的是 SyncLane），会先画出一帧「已经在终点」再跳回起点。 */
function createOverrideStore() {
  let value: PositionOverride = null;
  const listeners = new Set<() => void>();
  return {
    get: () => value,
    set(next: PositionOverride) {
      if (next === value) return;
      value = next;
      for (const fn of listeners) fn();
    },
    subscribe(fn: () => void) {
      listeners.add(fn);
      return () => listeners.delete(fn);
    },
  };
}

/** 残影不能被 e2e 或无障碍树当成真节点（N3）：id 与全部 data-*（data-testid、React Flow 的
 *  data-handleid / data-nodeid……）一律剥掉。 */
function scrub(root: Element): void {
  for (const el of [root, ...root.querySelectorAll("*")]) {
    el.removeAttribute("id");
    for (const attr of [...el.attributes]) {
      if (attr.name.startsWith("data-")) el.removeAttribute(attr.name);
    }
  }
}

export interface CanvasMotion {
  /** 这一帧的临时位置（布局过渡中），没有就是 null。 */
  override: PositionOverride;
  /** 用户开始拖节点时掐掉正在进行的布局过渡：拖动以 doc 的位置为准。 */
  cancelLayout(): void;
}

export function useCanvasMotion(
  wrapper: RefObject<HTMLDivElement | null>,
  ghostLayer: RefObject<HTMLDivElement | null>,
  enabled: boolean,
): CanvasMotion {
  const store = useRef<ReturnType<typeof createOverrideStore> | null>(null);
  store.current ??= createOverrideStore();
  const overrides = store.current;
  const override = useSyncExternalStore(overrides.subscribe, overrides.get, overrides.get);

  // 订阅只建一次，开关经 ref 读：prop 变化不该让差分的基线重来
  const enabledRef = useRef(enabled);
  enabledRef.current = enabled;
  const layout = useRef<{ ctrl: AnimationPlaybackControls | null; frame: Map<string, Point> } | null>(null);

  const cancelLayout = useCallback(() => {
    layout.current?.ctrl?.stop();
    layout.current = null;
    overrides.set(null);
  }, [overrides]);

  useEffect(() => {
    let base = snapshot();

    /** N3：从还在的 DOM 里克隆被删的节点，放进残影层淡出。 */
    const spawnGhosts = (ids: readonly string[]) => {
      const layer = ghostLayer.current;
      const root = wrapper.current;
      if (!layer || !root) return;
      for (const id of ids) {
        const host = root.querySelector<HTMLElement>(`.react-flow__node[data-id="${CSS.escape(id)}"]`);
        const node = host?.querySelector(".node");
        if (!host || !node) continue; // 虚拟化下不在视野里的节点本来就没有 DOM
        const ghost = document.createElement("div");
        ghost.className = "canvas__ghost";
        ghost.setAttribute("aria-hidden", "true");
        // React Flow 把节点位置写在包装层的 translate 上，残影层同在视口坐标系里，照抄即可
        ghost.style.transform = host.style.transform;
        ghost.style.width = `${host.offsetWidth}px`;
        const clone = node.cloneNode(true) as HTMLElement;
        scrub(clone);
        ghost.appendChild(clone);
        layer.appendChild(ghost);
        const remove = () => ghost.remove();
        // 纯 WAAPI 就够：残影播完就摘掉，不需要 motion value
        void animateMini(clone, GHOST_EXIT, TRANSITION.fast).then(remove);
        // 兜底：动画被节流（窗口在后台）时 then 可能迟迟不来，残影不能一直挂着
        setTimeout(remove, MOTION_MS.fast + 400);
      }
    };

    /** N4：从旧位置插值到新位置。doc 已经一步到位（一个撤销步），这里只改画面。 */
    const startLayout = (before: LevelSnapshot, next: LevelSnapshot) => {
      const current = layout.current?.frame;
      layout.current?.ctrl?.stop();
      layout.current = null;
      if (next.nodes.size > MOTION_NODE_LIMIT) {
        overrides.set(null);
        return;
      }
      const from = new Map<string, Point>();
      const to = new Map<string, Point>();
      for (const [id, p] of next.nodes) {
        // 上一段过渡还没走完就又整理了一次：从画面上此刻的位置接着走，不跳回去
        const q = current?.get(id) ?? before.nodes.get(id);
        if (q && (q.x !== p.x || q.y !== p.y)) {
          from.set(id, q);
          to.set(id, p);
        }
      }
      if (from.size === 0) {
        overrides.set(null);
        return;
      }
      const state: { ctrl: AnimationPlaybackControls | null; frame: Map<string, Point> } = {
        ctrl: null,
        frame: from,
      };
      layout.current = state;
      // 同步写入首帧：这次 doc 更新的那一遍渲染里，节点还停在原位
      overrides.set(from);
      state.ctrl = animate(0, 1, {
        ...TRANSITION.layout,
        onUpdate: (t) => {
          const frame = new Map<string, Point>();
          for (const [id, a] of from) {
            const b = to.get(id)!;
            frame.set(id, { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t });
          }
          state.frame = frame;
          overrides.set(frame);
        },
        onComplete: () => {
          if (layout.current !== state) return;
          layout.current = null;
          overrides.set(null);
        },
      });
    };

    const stopGraph = useGraphStore.subscribe((state, prev) => {
      if (state.doc === prev.doc && state.epoch === prev.epoch) return;
      const next = snapshot();
      const before = base;
      base = next;
      const ui = useUiStore.getState();
      // hover 指着的东西被删掉了：元素直接没了，mouseleave 不会来
      if (ui.hoverNodeId && !next.nodes.has(ui.hoverNodeId)) ui.setHoverNode(null);
      if (ui.hoverEdge && !next.edges.has(ui.hoverEdge.id)) ui.setHoverEdge(null);

      // loadDoc / newDoc：换了一整张图，不参与差分（N1）
      if (next.epoch !== before.epoch || next.pathKey !== before.pathKey) {
        clearEntering();
        cancelLayout();
        return;
      }
      const requested = layoutTransitionRequested();
      // 过渡途中 doc 又被别的动作改了（撤销、拖动）：以 doc 为准，掐掉
      if (!requested && layout.current) cancelLayout();
      if (!enabledRef.current) return;

      const addedNodes = [...next.nodes.keys()].filter((id) => !before.nodes.has(id));
      const addedEdges = [...next.edges].filter((id) => !before.edges.has(id));
      // 一次冒出来太多就不播了：满屏同时淡入没有信息量，还拖慢那一帧
      if (addedNodes.length > 0 && addedNodes.length <= MOTION_NODE_LIMIT) {
        markEntering("node", addedNodes);
      }
      if (addedEdges.length > 0 && addedEdges.length <= MOTION_NODE_LIMIT * 2) {
        markEntering("edge", addedEdges);
      }

      const removed = [...before.nodes.keys()].filter((id) => !next.nodes.has(id));
      if (removed.length > 0 && removed.length <= GHOST_MAX) spawnGhosts(removed);

      if (requested) startLayout(before, next);
    });

    // 进出子图也是「换一整张图」：基线换成新这一层，否则进去后的第一下编辑会被当成全图新增
    const stopUi = useUiStore.subscribe((s, p) => {
      if (s.path === p.path) return;
      base = snapshot();
      clearEntering();
      cancelLayout();
    });

    const layer = ghostLayer.current;
    return () => {
      stopGraph();
      stopUi();
      layout.current?.ctrl?.stop();
      layout.current = null;
      layer?.replaceChildren();
    };
  }, [wrapper, ghostLayer, overrides, cancelLayout]);

  return { override, cancelLayout };
}
