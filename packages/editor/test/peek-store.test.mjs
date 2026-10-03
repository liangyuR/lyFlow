// 连线查看器的窗口上限（docs/edge-peek-plan.md §4.6）：超了自动关掉最早的未锁定窗口，并提示用户。
// 纯 store 逻辑；窗口真的开合、画面在 scripts/e2e/peek.mjs 里走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

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
