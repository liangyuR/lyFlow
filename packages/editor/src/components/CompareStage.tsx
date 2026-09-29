// 对比模式的两栏（交互清单 #35，docs/compare-plan.md §1.2）。画面本身在 Viewer3D 那一块画布上 ——
// 同一个场景切成两个 scissor 视口（lib/cloudScene，C4）；这里只是盖在上面的 DOM：每栏的标签条、
// 状态文字、点数，两侧都只有值时的两张值表格。标签条以外不接鼠标，拖动落到画布上，两栏一起转。

import type { Display } from "../hooks/useViewerSource";
import { boundsAttr, type SplitMode } from "../lib/cloudScene";
import type { ViewerContent } from "../lib/viewRule";
import type { OutputStat } from "../types/execution";
import { ValuePane } from "./ValuePane";

export interface ComparePaneProps {
  display: Display;
  loading: boolean;
  /** 这一侧自己的内容（按它的输出类型）。 */
  content: ViewerContent;
  outputs: readonly OutputStat[] | undefined;
  /** 节点标题（没有就是 id）。 */
  label: string;
}

/** run id 的后 6 位（与 EdgePeek 的 shortRun 同）。 */
function shortRun(runId: string | null): string {
  return runId ? runId.slice(-6) : "";
}

function paneView(p: ComparePaneProps, stage: ViewerContent): "loading" | "cloud" | "value" | "empty" {
  if (p.loading) return "loading";
  if (stage === "value") return p.display.status ? "empty" : "value";
  return p.display.cloud && p.display.cloud.pointCount > 0 ? "cloud" : "empty";
}

function paneStatus(p: ComparePaneProps, stage: ViewerContent): string | null {
  if (p.loading) return "正在取点云…";
  if (p.display.status) return p.display.status;
  // 两栏是点云场景、这一侧只有值（§1.6）：值照样进下面的差异表
  // 图像那一侧同理：对比里不并排画图（compareContentFor）
  if (stage === "cloud" && p.content !== "cloud") return "该节点无点云输出，它的值见下方差异表";
  return null;
}

export function CompareStage({
  split,
  stage,
  a,
  b,
  frozen,
  canFreeze,
  frozenPoints,
  onFreeze,
  onUnfreeze,
  onExit,
}: {
  split: SplitMode;
  /** 两栏合起来的内容（compareContentFor）。 */
  stage: ViewerContent;
  a: ComparePaneProps;
  b: ComparePaneProps;
  frozen: boolean;
  canFreeze: boolean;
  /** B 冻结时快照的 maxPoints：B 的点数不再跟着下拉框变（§1.4）。 */
  frozenPoints: number | null;
  onFreeze(): void;
  onUnfreeze(): void;
  onExit(): void;
}) {
  const pane = (side: "A" | "B", p: ComparePaneProps) => {
    const view = paneView(p, stage);
    const status = paneStatus(p, stage);
    const cloud = p.display.cloud;
    return (
      <div
        className={`compare-pane compare-pane--${side.toLowerCase()}`}
        data-testid={`compare-pane-${side.toLowerCase()}`}
        data-view={view}
        data-node={p.display.nodeId ?? ""}
        data-run={p.display.runId ?? ""}
        data-base={p.display.base?.localId ?? ""}
        data-cloud-bounds={boundsAttr(cloud && cloud.pointCount > 0 ? cloud.bounds : null)}
      >
        <div className="compare-pane__bar">
          <span className="compare-pane__side">{side === "A" ? "A · 当前" : "B · 基准"}</span>
          <span className="compare-pane__label" title={p.label}>
            {p.label}
          </span>
          {p.display.base && (
            <span className="viewer__base" title={`底图取自上游最近的一片云（${p.display.base.localId}）`}>
              底图：{p.display.base.label}
            </span>
          )}
          <span className="viewer__spacer" />
          {side === "B" && (
            <>
              <button
                type="button"
                className={`viewer__btn compare-pane__freeze${frozen ? " is-on" : ""}`}
                data-testid="compare-freeze"
                data-frozen={frozen ? "1" : "0"}
                disabled={!frozen && !canFreeze}
                onClick={frozen ? onUnfreeze : onFreeze}
                title={
                  frozen
                    ? `已冻结在 run ·${shortRun(p.display.runId)}${frozenPoints ? `（快照固定在 ${frozenPoints.toLocaleString()} 点）` : ""}；点一下解冻，回到跟随最新`
                    : canFreeze
                      ? "冻结：把 B 定在这一次结果上，之后重跑只有 A 变"
                      : "先运行一次"
                }
              >
                ❄{frozen ? ` run ·${shortRun(p.display.runId)}` : ""}
              </button>
              <button
                type="button"
                className="viewer__btn"
                data-testid="compare-exit-b"
                onClick={onExit}
                title="退出对比"
              >
                ×
              </button>
            </>
          )}
        </div>
        {stage === "value" && view === "value" && <ValuePane outputs={p.outputs} />}
        {status && (
          <div className="compare-pane__status" data-testid={`compare-status-${side.toLowerCase()}`}>
            {status}
          </div>
        )}
        {cloud && cloud.pointCount > 0 && stage === "cloud" && (
          <span className="compare-pane__count" title="显示点数 / 总点数">
            {cloud.pointCount.toLocaleString()} / {cloud.totalPoints.toLocaleString()} 点
          </span>
        )}
      </div>
    );
  };
  return (
    <div className={`compare-stage compare-stage--${split}`} data-testid="compare-stage">
      {pane("A", a)}
      {pane("B", b)}
    </div>
  );
}
