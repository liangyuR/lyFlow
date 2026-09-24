// 编辑器的动效库（docs/motion-plan.md A2）：时长、缓动、motion 的 transition 预设、
// 「现在要不要动」的判定，以及画布差分与节点/连线组件之间传话的两条小通道。
// 别处不散写时长 —— 调手感、换库、关动效都只改这里和 styles.motion.css。

import { createContext, useContext, useSyncExternalStore } from "react";

/** 三档时长（毫秒）。CSS 那边的 `--lyflow-motion-*` 必须与它一致（styles.motion.css）。 */
export const MOTION_MS = { fast: 120, base: 200, slow: 320 } as const;

/** 缓出：起步快、落地轻。进出场与闪光都用它，线性只留给循环（CSS 里的流动）。 */
export const EASE_OUT = [0.22, 1, 0.36, 1] as const;
/** 缓入缓出：布局过渡用。两头慢，节点不会「弹」到新位置上。 */
export const EASE_IN_OUT = [0.65, 0, 0.35, 1] as const;

/** motion 的 transition 预设。motion 的 duration 单位是秒。 */
export const TRANSITION = {
  fast: { duration: MOTION_MS.fast / 1000, ease: EASE_OUT },
  base: { duration: MOTION_MS.base / 1000, ease: EASE_OUT },
  slow: { duration: MOTION_MS.slow / 1000, ease: EASE_OUT },
  layout: { duration: MOTION_MS.slow / 1000, ease: EASE_IN_OUT },
} as const;

/** 节点进场：自身 opacity 0→1（base），光晕在 slow 内淡出。只动 opacity —— 节点里有端口，
 *  位移/缩放一旦赶上 React Flow 的端口量测，连线端点就永久错位（A5）。 */
export const NODE_ENTER = { opacity: [0, 1] };
export const GLOW_FADE = { opacity: [1, 0] };
/** 状态闪光：先亮后灭。times 让亮起只占前四分之一，余下全是淡出。 */
export const GLOW_FLASH = { opacity: [0, 1, 0] };
export const GLOW_FLASH_TIMES = [0, 0.25, 1];
/** 错误抖动：±4px 衰减，约 300 ms。只作用在不含端口的 `.node__head` 上（A6）。
 *  写成完整的 transform 串：元素动画走 animateMini（纯 WAAPI），它不认 x 这类独立变换。 */
export const HEAD_SHAKE = {
  transform: [0, -4, 4, -3, 3, -1.5, 1.5, 0].map((x) => `translateX(${x}px)`),
};
export const HEAD_SHAKE_MS = 300;
/** 删除残影：淡出 + 轻微缩小。残影不是 React Flow 节点，缩放不碍量测（A6）。 */
export const GHOST_EXIT = { opacity: [1, 0], transform: ["scale(1)", "scale(0.96)"] };

/** 一次删掉超过这么多节点就不出残影（N3）：大批删除时残影只是噪音。 */
export const GHOST_MAX = 30;
/** 当前层超过这个节点数不做布局过渡（N4），一次冒出来超过这么多节点也不播进场。
 *  和画布的虚拟化阈值是同一个数：那之后本来就只渲染视野里的节点，逐帧插值几百个节点的
 *  位置也不划算。 */
export const MOTION_NODE_LIMIT = 80;

// ------------------------------------------------------------ 启用判定（A4）

const REDUCE_QUERY = "(prefers-reduced-motion: reduce)";

function reduceMedia(): MediaQueryList | null {
  return typeof window !== "undefined" && typeof window.matchMedia === "function"
    ? window.matchMedia(REDUCE_QUERY)
    : null;
}

function subscribeReduce(onChange: () => void): () => void {
  const mq = reduceMedia();
  if (!mq) return () => {};
  mq.addEventListener("change", onChange);
  return () => mq.removeEventListener("change", onChange);
}

/** 系统是不是要求减少动效。CDP 的 Emulation.setEmulatedMedia 也走这条（会触发 change）。 */
export function usePrefersReducedMotion(): boolean {
  return useSyncExternalStore(
    subscribeReduce,
    () => reduceMedia()?.matches === true,
    () => false,
  );
}

/** 当前是否启用动效：`animations` prop 且系统没要求减少。由编辑器根组件算好往下传 ——
 *  用 context 而不是模块变量：prop 变了要让所有节点跟着重渲，而且同一页面上可以有两个编辑器。 */
export const MotionEnabledContext = createContext(true);

export function useMotionEnabled(): boolean {
  return useContext(MotionEnabledContext);
}

/** 编辑器自己发起的视口动画（fitView 之类）的时长，毫秒。关动效时是 0：一步到位（A4）。 */
export function viewportMs(motionOn: boolean): number {
  return motionOn ? MOTION_MS.base : 0;
}

// ------------------------------------------------ 进场的短命集合（N1）

/** 「谁算新出现」由画布对 doc 的差分决定（N1），节点/连线组件挂载时来这里问一句。
 *  存的是到期时刻而不是「播完即删」的布尔：StrictMode 的双挂载要问两次都得到 true，
 *  而虚拟化下晚了很久才平移进视口的节点必须得到 false —— 按挂载判定会满屏乱闪。 */
const entering = new Map<string, number>();
/** 标记之后多久内挂载才算进场。比 base 宽一点，给一次较重的首帧渲染留余量。 */
const ENTER_WINDOW_MS = MOTION_MS.base + 250;

const enterKey = (kind: "node" | "edge", id: string) => `${kind}:${id}`;

export function markEntering(kind: "node" | "edge", ids: readonly string[]): void {
  const now = performance.now();
  for (const [key, until] of entering) if (until <= now) entering.delete(key);
  for (const id of ids) entering.set(enterKey(kind, id), now + ENTER_WINDOW_MS);
}

export function isEntering(kind: "node" | "edge", id: string): boolean {
  const until = entering.get(enterKey(kind, id));
  return until !== undefined && until > performance.now();
}

/** 换了一整张图（loadDoc / newDoc / 进出子图）：之前标的一律作废。 */
export function clearEntering(): void {
  entering.clear();
}

// ------------------------------------------------ 布局过渡的意图（N4）

/** 只有用户触发的「自动布局」才过渡。applyLayout 本身还被打开文件时的初始布局、
 *  验收脚本的摆位用着，那些都该一步到位：前者是「打开文件不该动」，后者要紧接着
 *  按屏幕坐标拖拽，位置还在飞就会拖空。所以由调用方显式声明，画布的差分只认这一次。 */
let layoutIntent = false;

export function withLayoutTransition(apply: () => void): void {
  layoutIntent = true;
  try {
    apply(); // store 的订阅是同步回调，画布在这一行里面就已经看到了意图
  } finally {
    layoutIntent = false;
  }
}

export function layoutTransitionRequested(): boolean {
  return layoutIntent;
}

// ------------------------------------------------ 定位闪光（node-run U5）

/** 「这几个节点有问题，看这里」：单节点运行撞上 upstream_not_ready 时，缺结果的上游各闪一下
 *  红光（S2 的 error 闪光，不抖动 —— 它们自己并没有失败）。执行 store 喊，节点组件听；
 *  键是**本层**的节点 id，不在当前层的喊了也没人应，正好。 */
const flashListeners = new Map<string, Set<() => void>>();

export function onNodeLocateFlash(id: string, fn: () => void): () => void {
  let set = flashListeners.get(id);
  if (!set) {
    set = new Set();
    flashListeners.set(id, set);
  }
  set.add(fn);
  return () => {
    set.delete(fn);
    if (set.size === 0) flashListeners.delete(id);
  };
}

export function flashNodesLocate(ids: readonly string[]): void {
  for (const id of ids) for (const fn of flashListeners.get(id) ?? []) fn();
}
