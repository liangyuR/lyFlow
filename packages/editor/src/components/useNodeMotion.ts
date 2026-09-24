// 节点自己的一次性动效：进场（N2）与状态切换的闪光/抖动（S1/S2）。用 motion 的命令式
// animateMini 而不是 motion.div：`.node` 的 opacity 与 box-shadow 由 stale、静音、选中、running
// 这些类决定，声明式动画播完会把终值留在 inline style 上，把这些类全压住。
//
// 也不能用完整版的 animate(element)：它给元素建一个 VisualElement，终值在下一帧的渲染批次里
// 才写回 —— 我们 then 里刚擦掉的 inline opacity 又被它写成 1，静音/未被需要的半透明永久失效。
// animateMini 是纯 WAAPI，先写终值再 resolve，then 里擦得干净。

import {
  animateMini as animate,
  type AnimationOptions,
  type AnimationPlaybackControls,
  type AnimationPlaybackControlsWithThen,
} from "motion/react";
import { useEffect, useLayoutEffect, useRef, useState, type RefObject } from "react";

import {
  GLOW_FADE,
  GLOW_FLASH,
  GLOW_FLASH_TIMES,
  HEAD_SHAKE,
  HEAD_SHAKE_MS,
  isEntering,
  NODE_ENTER,
  onNodeLocateFlash,
  TRANSITION,
  useMotionEnabled,
} from "../lib/motion";
import { useExecutionStore } from "../store/execution";
import type { NodeState } from "../types/execution";

export interface NodeMotionRefs {
  /** `.node` 本身。只许动 opacity（A5）。 */
  root: RefObject<HTMLDivElement | null>;
  /** 盖在节点上的光晕层。颜色由 data-kind 选（styles.motion.css），这里只动它的 opacity。 */
  fx: RefObject<HTMLSpanElement | null>;
  /** 标题栏：不含端口，是节点上唯一允许位移的部分（A6）。 */
  head: RefObject<HTMLDivElement | null>;
}

export function useNodeMotion(id: string, state: NodeState, refs: NodeMotionRefs): void {
  const motionOn = useMotionEnabled();
  // 进不进场只在挂载那一刻问一次：之后就算短命集合里还有它，也不该再播
  const [entering] = useState(() => motionOn && isEntering("node", id));
  const running = useRef<AnimationPlaybackControls[]>([]);

  // 进场放在 layout effect 里：在首帧绘制之前就把 opacity 压到 0，否则会先闪一下全亮
  useLayoutEffect(() => {
    const root = refs.root.current;
    const fx = refs.fx.current;
    if (!entering || !root) return;
    root.dataset.entering = "1";
    const fade = animate(root, NODE_ENTER, TRANSITION.base);
    const glow = fx ? glowOnce(fx, "enter", GLOW_FADE, TRANSITION.slow) : null;
    const done = () => {
      // 终值会被 motion 写成 inline opacity: 1，得擦掉，否则 stale/静音的半透明失效
      root.style.opacity = "";
      delete root.dataset.entering;
    };
    void fade.then(done);
    return () => {
      // StrictMode 的卸载-重挂与虚拟化卸载都会走到这里：停在半路，下一次挂载再从头来
      fade.stop();
      glow?.stop();
      done();
      if (fx) resetGlow(fx);
    };
    // 只在挂载时跑一次
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // 状态切换（S1）：只对本次挂载期间发生的迁移播放。初值就是挂载时的状态，
  // 所以进出子图、虚拟化重挂的已完成节点不会闪。
  const previous = useRef(state);
  useEffect(() => {
    const before = previous.current;
    previous.current = state;
    if (!motionOn || before === state) return;
    if (state !== "done" && state !== "error") return;
    // 实时预览（拖参数触发的 preview run）一秒能跑好几轮，每轮都闪一次绿只是噪音（S2）。
    // 出错照闪照抖：那是要人看的。读当下的值就够，不必订阅
    if (state === "done" && useExecutionStore.getState().preview) return;
    const root = refs.root.current;
    const fx = refs.fx.current;
    if (!root || !fx) return;
    stopAll(running.current, refs);
    root.dataset.flash = state;
    const flash = glowOnce(fx, state, GLOW_FLASH, { ...TRANSITION.slow, times: GLOW_FLASH_TIMES });
    running.current.push(flash);
    void flash.then(() => {
      if (root.dataset.flash === state) delete root.dataset.flash;
    });
    const head = refs.head.current;
    if (state === "error" && head) {
      const shake = animate(head, HEAD_SHAKE, { duration: HEAD_SHAKE_MS / 1000, ease: "easeOut" });
      running.current.push(shake);
      void shake.then(() => {
        head.style.transform = "";
      });
    }
    // refs 是三个 useRef，身份不变；只跟着状态走
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state, motionOn]);

  // 定位闪光（node-run U5）：复用 error 的红光，但不抖 —— 这个节点自己没有失败，只是被指认出来。
  // 关动效时不播：toast 里已经写明是哪几个上游，信息不丢
  useEffect(() => {
    if (!motionOn) return;
    return onNodeLocateFlash(id, () => {
      const root = refs.root.current;
      const fx = refs.fx.current;
      if (!root || !fx) return;
      stopAll(running.current, refs);
      root.dataset.flash = "locate";
      const flash = glowOnce(fx, "error", GLOW_FLASH, { ...TRANSITION.slow, times: GLOW_FLASH_TIMES });
      running.current.push(flash);
      void flash.then(() => {
        if (root.dataset.flash === "locate") delete root.dataset.flash;
      });
    });
    // refs 身份不变
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [id, motionOn]);

  useEffect(
    () => () => stopAll(running.current, refs),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );
}

/** 打断正在播的闪光/抖动。stop() 停在半路，残留的 inline 值得自己擦：标题栏停在 +3px 上
 *  就再也回不来了。 */
function stopAll(list: AnimationPlaybackControls[], refs: NodeMotionRefs): void {
  for (const a of list) a.stop();
  list.length = 0;
  if (refs.head.current) refs.head.current.style.transform = "";
  if (refs.fx.current) resetGlow(refs.fx.current);
  if (refs.root.current) delete refs.root.current.dataset.flash;
}

/** 光晕层播一次。结束后恢复成透明，data-kind 留着也无妨（opacity 为 0 看不见）。 */
function glowOnce(
  fx: HTMLSpanElement,
  kind: string,
  keyframes: { opacity: number[] },
  options: AnimationOptions,
): AnimationPlaybackControlsWithThen {
  fx.dataset.kind = kind;
  const a = animate(fx, keyframes, options);
  void a.then(() => resetGlow(fx));
  return a;
}

function resetGlow(fx: HTMLSpanElement): void {
  fx.style.opacity = "";
}
