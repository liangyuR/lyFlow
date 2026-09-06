// 当前生效的传输。`<LyFlowEditor transport>` 在渲染前装上它；stores 与 lib 里的
// 调用点仍然写 `transport.xxx()`，由下面这个转发代理落到宿主给的那一个实现上。

import { StaticTransport } from "./static";
import type { Transport } from "./types";

export * from "./types";
export { TauriTransport } from "./tauri";
export { StaticTransport } from "./static";
export { HttpTransport } from "./http";

let current: Transport | null = null;

/** 宿主没给传输时的兜底：只读静态快照，比抛异常更容易看出配置漏了什么。 */
function fallback(): Transport {
  current ??= new StaticTransport();
  return current;
}

export function setTransport(next: Transport): void {
  current = next;
}

export function activeTransport(): Transport {
  return current ?? fallback();
}

export const transport: Transport = new Proxy({} as Transport, {
  get(_target, prop) {
    const active = activeTransport() as unknown as Record<string | symbol, unknown>;
    const value = active[prop];
    return typeof value === "function" ? value.bind(active) : value;
  },
  has(_target, prop) {
    return prop in (activeTransport() as unknown as object);
  },
});
