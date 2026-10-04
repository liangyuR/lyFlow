// 画布视口的一个把手：GraphCanvas 挂载时挂上（React Flow 的 getViewport / setViewport 只能在它的 provider 里拿），
// 外面要定死视口时用 —— 验收脚本的窗口桥（app/src/devbridge.ts）：上一组留下的缩放会落到下一组头上
// （scripts/e2e/README.md 记过的那处顺序依赖）。画布没挂载时是 null。

export interface Viewport {
  x: number;
  y: number;
  zoom: number;
}

export interface ViewportHandle {
  get(): Viewport;
  set(v: Viewport): void;
}

let current: ViewportHandle | null = null;

export function registerViewportHandle(handle: ViewportHandle | null): void {
  current = handle;
}

export function viewportHandle(): ViewportHandle | null {
  return current;
}
