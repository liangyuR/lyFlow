// 参数面板的行列表虚拟化（param-recipe P2.9）：只挂视口里（加上下各一截余量）的那些行。
// 行高不一（curve 一百多像素、bool 一行），先按类型估一个，挂上之后 ResizeObserver 量出真值再修正；
// 视口上方的行高变了就把 scrollTop 补上同样的差，看着的那一行不跳。不引虚拟化库：要的只有这么多。

import { useCallback, useEffect, useImperativeHandle, useLayoutEffect, useMemo, useRef, useState, type Ref } from "react";

export interface VirtualListHandle {
  /** 把 key 那一项滚到视口顶上（留 8px）。找不到返回 false。 */
  scrollToKey(key: string, align?: "start" | "nearest"): boolean;
  /** 当前挂着的键（验收脚本断言虚拟化用）。 */
  mountedKeys(): string[];
  element(): HTMLDivElement | null;
}

interface Props<T> {
  items: readonly T[];
  keyOf: (item: T) => string;
  estimate: (item: T) => number;
  render: (item: T) => React.ReactNode;
  /** 视口上下各多挂这么多像素。 */
  overscan?: number;
  className?: string;
  testId?: string;
  handle?: Ref<VirtualListHandle>;
}

export function VirtualList<T>({ items, keyOf, estimate, render, overscan = 400, className, testId, handle }: Props<T>) {
  const scroller = useRef<HTMLDivElement>(null);
  const heights = useRef(new Map<string, number>());
  const [scrollTop, setScrollTop] = useState(0);
  const [viewport, setViewport] = useState(600);
  const [version, setVersion] = useState(0);
  const pending = useRef(0);

  const keys = useMemo(() => items.map(keyOf), [items, keyOf]);
  // 每项的顶边。一千来项的一次前缀和，比任何缓存失效逻辑都便宜
  const offsets = useMemo(() => {
    const out = new Float64Array(items.length + 1);
    for (let i = 0; i < items.length; i += 1) {
      out[i + 1] = out[i]! + (heights.current.get(keys[i]!) ?? estimate(items[i]!));
    }
    return out;
    // version：量到了新的行高，heights 这个 ref 里的数变了
  }, [items, keys, estimate, version]);
  const total = offsets[items.length] ?? 0;

  const first = Math.max(0, upperBound(offsets, scrollTop - overscan) - 1);
  const lastEdge = scrollTop + viewport + overscan;
  let last = first;
  while (last < items.length && offsets[last]! < lastEdge) last += 1;

  useLayoutEffect(() => {
    const el = scroller.current;
    if (!el) return;
    setViewport(el.clientHeight);
    const ro = new ResizeObserver(() => setViewport(el.clientHeight));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // 量行高：一个 ResizeObserver 看所有挂着的行，改动攒到下一帧一起提交
  const offsetsRef = useRef(offsets);
  offsetsRef.current = offsets;
  const keysRef = useRef(keys);
  keysRef.current = keys;
  const observer = useMemo(
    () =>
      typeof ResizeObserver === "undefined"
        ? null
        : new ResizeObserver((entries) => {
            let changed = false;
            let above = 0;
            const el = scroller.current;
            const top = el?.scrollTop ?? 0;
            for (const e of entries) {
              const node = e.target as HTMLElement;
              const key = node.dataset.vkey;
              if (!key) continue;
              const h = node.offsetHeight;
              const old = heights.current.get(key);
              if (old === h) continue;
              const i = keysRef.current.indexOf(key);
              const before = old ?? (i >= 0 ? offsetsRef.current[i + 1]! - offsetsRef.current[i]! : h);
              heights.current.set(key, h);
              changed = true;
              if (i >= 0 && offsetsRef.current[i + 1]! <= top) above += h - before;
            }
            if (!changed) return;
            if (el && above !== 0) el.scrollTop = top + above;
            cancelAnimationFrame(pending.current);
            pending.current = requestAnimationFrame(() => setVersion((v) => v + 1));
          }),
    [],
  );
  // 滚出视口卸掉的行要从观察器里摘掉，不然它一直攥着那些 DOM
  const observed = useRef(new Set<HTMLDivElement>());
  useEffect(() => {
    // StrictMode 的「卸载再挂载」会先 disconnect 一次：重新挂上时把已经在的行再观察一遍
    for (const node of observed.current) observer?.observe(node);
    return () => {
      observer?.disconnect();
      cancelAnimationFrame(pending.current);
    };
  }, [observer]);
  const measure = useCallback(
    (node: HTMLDivElement | null) => {
      if (!node || !observer) return;
      observer.observe(node);
      observed.current.add(node);
    },
    [observer],
  );
  useEffect(() => {
    for (const node of observed.current) {
      if (node.isConnected) continue;
      observer?.unobserve(node);
      observed.current.delete(node);
    }
  });

  useImperativeHandle(
    handle,
    () => ({
      scrollToKey(key, align = "start") {
        const el = scroller.current;
        const i = keysRef.current.indexOf(key);
        if (!el || i < 0) return false;
        const top = offsetsRef.current[i]!;
        const bottom = offsetsRef.current[i + 1]!;
        if (align === "nearest" && top >= el.scrollTop && bottom <= el.scrollTop + el.clientHeight) return true;
        el.scrollTop = Math.max(0, top - 8);
        setScrollTop(el.scrollTop);
        return true;
      },
      mountedKeys() {
        return keysRef.current.slice(first, last);
      },
      element() {
        return scroller.current;
      },
    }),
    [first, last],
  );

  return (
    <div
      ref={scroller}
      className={className}
      data-testid={testId}
      data-total={items.length}
      data-mounted={last - first}
      onScroll={(e) => setScrollTop(e.currentTarget.scrollTop)}
    >
      <div className="vlist__spacer" style={{ height: total, position: "relative" }}>
        {items.slice(first, last).map((item, k) => {
          const i = first + k;
          const key = keys[i]!;
          return (
            <div
              key={key}
              ref={measure}
              data-vkey={key}
              className="vlist__item"
              style={{ position: "absolute", top: offsets[i], left: 0, right: 0 }}
            >
              {render(item)}
            </div>
          );
        })}
      </div>
    </div>
  );
}

/** 第一个 > v 的下标。offsets 单调不减。 */
function upperBound(offsets: Float64Array, v: number): number {
  let lo = 0;
  let hi = offsets.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (offsets[mid]! <= v) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
