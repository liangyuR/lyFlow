// 记在 localStorage 里的界面偏好：分栏宽度、预览与抽屉的高度。读不到、被禁用、存的不是数都当没有 ——
// 这只是个方便，不是状态；写不进（隐私模式、配额满）就只在这一次会话里有效。

export function readStoredNumber(key: string): number | null {
  try {
    const raw = globalThis.localStorage?.getItem(key);
    if (raw == null || raw === "") return null;
    const v = Number(raw);
    return Number.isFinite(v) ? v : null;
  } catch {
    return null;
  }
}

export function writeStoredNumber(key: string, value: number): void {
  try {
    globalThis.localStorage?.setItem(key, String(value));
  } catch {
    // 记不住就记不住，这一次照样生效
  }
}

export function removeStored(key: string): void {
  try {
    globalThis.localStorage?.removeItem(key);
  } catch {
    // 同上
  }
}
