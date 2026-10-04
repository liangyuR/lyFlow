// 连线查看器的窗口上限（docs/edge-peek-plan.md §4.6）：超了自动关掉最早的未锁定窗口，并提示用户。
// 纯 store 逻辑；窗口真的开合、画面在 scripts/e2e/peek.mjs 里走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { keepLastFrame } from "../src/lib/peekSource.ts";
import { PEEK_MAX_WEBGL, PEEK_MAX_WINDOWS, usePeekStore } from "../src/store/peek.ts";
import { useUiStore } from "../src/store/ui.ts";

const request = (i, view, locked = null) => ({
  edgeId: `e${i}`,
  path: [],
  from: { node: `n${i}`, port: "out" },
  screen: { x: 0, y: 0 },
  view,
  locked,
});

function openAll(views) {
  usePeekStore.getState().closeAll();
  useUiStore.setState({ toast: null });
  return views.map((v, i) => usePeekStore.getState().open(request(i, ...v)));
}

test("keepLastFrame：正在重算同一个端口时接着显示上一帧（原样那一个对象），别的情况照旧显示 live", () => {
  const src = (over = {}) => ({
    resolved: { nodeId: "n1", port: "cloud" }, runId: "r1", type: "PointCloud", stat: undefined, status: null,
    label: "n1", field: null, bundle: null, outputs: undefined, busy: false, ...over,
  });
  const last = src();
  const running = src({ runId: "r2", status: "正在计算…", busy: true });
  const cases = [
    ["正在重算同一个端口", running, last, last],
    ["没在算（出错了）", src({ status: "该节点运行出错" }), last, null],
    ["没在算（跑完了）", src({ runId: "r2" }), last, null],
    ["还没有过好的一帧", running, null, null],
    ["上一帧本身不是好的", running, src({ status: "未运行" }), null],
    ["换了端口", src({ resolved: { nodeId: "n1", port: "other" }, status: "正在计算…", busy: true }), last, null],
    ["换了节点", src({ resolved: { nodeId: "n2", port: "cloud" }, status: "正在计算…", busy: true }), last, null],
    ["换了字段", src({ field: "merged", status: "正在计算…", busy: true }), last, null],
    ["内部结果查不到", src({ resolved: null, status: "这个算子的内部结果查不到", busy: true }), last, null],
  ];
  for (const [name, live, prev, want] of cases) assert.equal(keepLastFrame(live, prev), want, name);
});

test("超过上限时关掉最早的未锁定窗口并提示；锁定的窗口留着", () => {
  const cases = [
    // [说明, 依次打开的 [view, locked], 期望关掉的是第几个, 期望提示里的上限]
    [
      "第 5 个 3D 窗口",
      Array.from({ length: PEEK_MAX_WEBGL + 1 }, () => ["cloud3d"]),
      0,
      PEEK_MAX_WEBGL,
    ],
    [
      "最早的那个 3D 窗口锁定了，就关第二早的",
      [["cloud3d", { runId: "r" }], ...Array.from({ length: PEEK_MAX_WEBGL }, () => ["cloud3d"])],
      1,
      PEEK_MAX_WEBGL,
    ],
    [
      "第 7 个窗口（都不是 3D）",
      Array.from({ length: PEEK_MAX_WINDOWS + 1 }, () => ["value"]),
      0,
      PEEK_MAX_WINDOWS,
    ],
  ];
  for (const [name, views, evicted, limit] of cases) {
    const ids = openAll(views);
    const open = new Set(usePeekStore.getState().windows.map((w) => w.id));
    const toast = useUiStore.getState().toast;
    assert.deepEqual(
      {
        closed: ids.filter((id) => !open.has(id)),
        toastMentionsLimit: toast?.text.includes(`最多 ${limit} 个`) ?? false,
      },
      { closed: [ids[evicted]], toastMentionsLimit: true },
      `${name}：toast=${toast?.text}`,
    );
  }

  // 新窗口沿用主预览的着色、色带、点大小；显示点数仍是查看器自己的 200k
  const prefs = useUiStore.getState().viewerPrefs;
  useUiStore.setState({ viewerPrefs: { shading: "height", ramp: "jet", pointSize: 3, maxPoints: 8_000_000 } });
  const [id] = openAll([["cloud3d"]]);
  const { shading, ramp, pointSize, maxPoints } = usePeekStore.getState().windows.find((w) => w.id === id).opts;
  assert.deepEqual({ shading, ramp, pointSize, maxPoints }, { shading: "height", ramp: "jet", pointSize: 3, maxPoints: 200_000 });
  useUiStore.setState({ viewerPrefs: prefs });
});
