// 点云缓存（lib/cloudCache）的并发请求合并：同一个键只取一次。取数 effect 因依赖变化重跑时
// （节点刚 done、运行紧接着 finished）不合并会并发取两三遍 —— m4「事件到渲染」偶发超时的根因。
import assert from "node:assert/strict";
import { test } from "node:test";

import { cloudCache, fetchCloud } from "../src/lib/cloudCache.ts";

const payload = (n) => ({
  pointCount: n,
  totalPoints: n,
  bounds: new Float32Array(6),
  xyz: new Float32Array(n * 3),
  intensity: null,
  normals: null,
  rgb: null,
});

test("同一个键并发只取一次、取完进缓存；取失败不留占位，下一次照常再取", async () => {
  let calls = 0;
  let release;
  const gate = new Promise((r) => (release = r));
  const load = async () => {
    calls += 1;
    await gate;
    return payload(3);
  };
  const a = fetchCloud("run1|n|cloud|100", load);
  const b = fetchCloud("run1|n|cloud|100", load);
  const other = fetchCloud("run1|m|cloud|100", async () => payload(1));
  release();
  const [pa, pb] = await Promise.all([a, b, other]);
  assert.equal(calls, 1, "第二次等的是第一次那一份");
  assert.equal(pa, pb);
  assert.equal(cloudCache.get("run1|n|cloud|100"), pa, "取完进缓存");
  assert.ok(cloudCache.has("run1|m|cloud|100"), "别的键各取各的");

  let fails = 0;
  const bad = () => {
    fails += 1;
    return Promise.reject(new Error("IPC 断了"));
  };
  await assert.rejects(fetchCloud("run2|n|cloud|100", bad));
  await assert.rejects(fetchCloud("run2|n|cloud|100", bad));
  assert.equal(fails, 2, "失败的那次不留在进行中表里");
});
