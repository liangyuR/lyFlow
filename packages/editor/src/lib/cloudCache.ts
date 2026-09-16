import type { CloudPayload } from "../types/execution";

/** 已取回的点云缓存。键是 runId+node+port+maxPoints，少任何一段都会串味：
 *  少 runId 会在重跑后拿到上次结果，少 maxPoints 会让滑块拖了没反应。 */
export const cloudCache = new Map<string, CloudPayload>();

/** 缓存的**字节**预算，不是条数预算：8M 点一条就是 96MB 坐标 + 32MB 强度，
 *  按条数封顶的话 8 条能攒到 1GB。 */
const CACHE_BYTES = 256 * 1024 * 1024;

function payloadBytes(p: CloudPayload) {
  return p.xyz.byteLength + (p.intensity?.byteLength ?? 0);
}

export function cacheKey(runId: string, nodeId: string, port: string, maxPoints: number) {
  return `${runId}|${nodeId}|${port}|${maxPoints}`;
}

const pinnedRuns = new Map<string, number>();

export function pinRun(runId: string): void {
  pinnedRuns.set(runId, (pinnedRuns.get(runId) ?? 0) + 1);
}

export function unpinRun(runId: string): void {
  const n = pinnedRuns.get(runId);
  if (n === undefined) return;
  if (n > 1) pinnedRuns.set(runId, n - 1);
  else pinnedRuns.delete(runId);
}

function runIdOf(key: string): string {
  const bar = key.indexOf("|");
  return bar < 0 ? key : key.slice(0, bar);
}

/** 换了一次运行就把旧运行的条目全丢掉 —— 它们再也不会被命中。 */
export function dropOtherRuns(runId: string) {
  for (const key of [...cloudCache.keys()]) {
    if (key.startsWith(`${runId}|`) || pinnedRuns.has(runIdOf(key))) continue;
    cloudCache.delete(key);
  }
}

export function putCache(key: string, payload: CloudPayload) {
  // delete + set 让它变成真正的 LRU：Map.set 命中已有键时不会调整顺序，
  // 少了这一行，你来回切着看的那片云恰恰是最先被淘汰的那个。
  cloudCache.delete(key);
  cloudCache.set(key, payload);
  let total = 0;
  for (const p of cloudCache.values()) total += payloadBytes(p);
  while (total > CACHE_BYTES && cloudCache.size > 1) {
    let victim: string | undefined;
    for (const k of cloudCache.keys()) {
      if (k === key || pinnedRuns.has(runIdOf(k))) continue;
      victim = k;
      break;
    }
    if (victim === undefined) break;
    total -= payloadBytes(cloudCache.get(victim)!);
    cloudCache.delete(victim);
  }
}
