// lib/nodeRun 的图结构部分：智能运行的上游闭包（ancestorsOf）与右键「选中上游 / 下游」（closureOf）。
// 按钮本身的手感、预告与抢占在 e2e scripts/e2e/noderun.mjs 里走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { ancestorsOf, closureOf } from "../src/lib/nodeRun.ts";

//   a → b → d → e
//        ↘     ↗
//   c ───→ x
//   f（孤立）
const edge = (from, to) => ({ id: `${from}-${to}`, from: { node: from, port: "out" }, to: { node: to, port: "in" } });
const level = {
  nodes: ["a", "b", "c", "d", "e", "f", "x"].map((id) => ({ id, op: "t.op" })),
  edges: [edge("a", "b"), edge("b", "d"), edge("d", "e"), edge("b", "x"), edge("c", "x"), edge("x", "e")],
};

test("closureOf：沿连线往上 / 往下走到头，含起点，按文档顺序；多个起点合在一起", () => {
  const cases = [
    // [起点, 方向, 期望]
    [["d"], "up", ["a", "b", "d"]],
    [["d"], "down", ["d", "e"]],
    [["x"], "up", ["a", "b", "c", "x"]],
    [["b"], "down", ["b", "d", "e", "x"]],
    [["a", "c"], "down", ["a", "b", "c", "d", "e", "x"]],
    [["f"], "up", ["f"]],
    [["gone"], "down", []],
  ];
  for (const [start, dir, want] of cases) {
    assert.deepEqual(closureOf(level, start, dir), want, `${start.join("+")} ${dir}`);
  }
});

test("ancestorsOf：不含自己，离源头近的排前面", () => {
  // 从 e 往上数最长的那条路：a 三步，b、c 两步，d、x 一步
  assert.deepEqual(ancestorsOf(level, "e"), ["a", "b", "c", "d", "x"]);
  assert.deepEqual(ancestorsOf(level, "a"), []);
});
