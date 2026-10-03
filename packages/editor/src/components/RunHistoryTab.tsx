// 抽屉的「调参」页：每次正式运行一行，新的在上。写着这次比上一次改了什么、量测读数是多少、比上一次变了多少，
// 点读数打开到那个节点；「恢复这组参数」把参数改回那一次的（一条撤销）。调参的循环（改阈值 → 跑 → 看 gap 变了多少）
// 以前上一次是多少全靠脑子记，回到读数最好的那一组只能一个个参数凭记忆改回去。

import { useMemo } from "react";

import { num } from "../lib/format";
import { verdictTone } from "../lib/outputs";
import { diffRuns, diffText, previousReadingIn, type RunRecord } from "../lib/runHistory";
import { augmentOperators, describeEventNode } from "../lib/subgraph";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { runParamsOf } from "../store/recipe";
import { useRunHistoryStore } from "../store/runHistory";
import { useUiStore } from "../store/ui";

const STATUS_LABEL: Record<string, string> = {
  running: "运行中",
  ok: "完成",
  error: "出错",
  cancelled: "取消",
};

function clock(at: number): string {
  const d = new Date(at);
  const two = (n: number) => String(n).padStart(2, "0");
  return `${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())}`;
}

function duration(ms: number): string {
  return ms >= 1000 ? `${(ms / 1000).toFixed(2)} s` : `${Math.round(ms)} ms`;
}

/** 读数比上一次变了多少：「+0.012」。差得太小（六位有效数字下看不出）不写。 */
function deltaText(now: number | null, before: number | null): string | null {
  if (now === null || before === null) return null;
  const d = now - before;
  if (Math.abs(d) < 1e-9 * Math.max(1, Math.abs(now))) return null;
  return `${d > 0 ? "+" : ""}${num(d)}`;
}

export function RunHistoryTab() {
  const records = useRunHistoryStore((s) => s.records);
  const doc = useGraphStore((s) => s.doc);
  const baseOps = useManifestStore((s) => s.operatorsById);

  // 每一行与它前一次（更早的那一条）比；图参数、子图定义都按各自那一份
  const rows = useMemo(
    () =>
      records.map((r, i) => {
        const prev = records[i + 1];
        const ops = augmentOperators(baseOps, { ...prev?.doc.subgraphs, ...r.doc.subgraphs });
        const diff = prev ? diffRuns(prev, r, ops) : null;
        return { r, i, diff };
      }),
    [records, baseOps],
  );
  const ops = useMemo(() => augmentOperators(baseOps, doc.subgraphs), [baseOps, doc.subgraphs]);

  if (records.length === 0) {
    return (
      <p className="drawer__empty">
        还没有运行记录。改参数、按 F5 跑一次：这里一行一次，写着改了什么、量测读数变成多少。
      </p>
    );
  }

  return (
    <ol className="runs" data-testid="run-history">
      {rows.map(({ r, i, diff }) => (
        <RunRow
          key={r.runId}
          record={r}
          // 更早的记录被 50 条的上限挤掉了时说清楚，不冒充「第一次」
          diffLine={diff ? diffText(diff) : r.seq === 1 ? "这张图打开以来的第一次" : "更早的记录已不保留，比不出改了什么"}
          diffTitle={diff ? diffText(diff, Infinity) : ""}
          before={(rd) => previousReadingIn(records, i, rd)}
          describe={(id) => describeEventNode(doc, ops, id)}
          onRestore={() => restoreRun(r, baseOps)}
        />
      ))}
    </ol>
  );
}

/** 「恢复这组参数」：改回那一次的参数，说清楚哪些没能恢复（后来删掉的节点、那时还没有的、配方里的值）。 */
function restoreRun(r: RunRecord, baseOps: Parameters<typeof augmentOperators>[0]): void {
  const { changed, skipped } = useGraphStore.getState().restoreParams(r.doc, `恢复第 ${r.seq} 次运行的参数`);
  const after = useGraphStore.getState().doc;
  const left = diffRuns(
    { doc: r.doc, params: r.params },
    { doc: after, params: runParamsOf(after) },
    augmentOperators(baseOps, { ...r.doc.subgraphs, ...after.subgraphs }),
  );
  // 图参数只在一边有（后来加的、删掉的）：不怪配方
  const onBoth = (id: string) => !!r.doc.params?.[id.slice(3)] && !!after.params?.[id.slice(3)];
  const gpOneSide = left.changes.filter((c) => c.id.startsWith("gp:") && !onBoth(c.id)).length;
  const recipeDiffs = left.changes.filter((c) => c.id.startsWith("gp:") && onBoth(c.id)).length;
  const notes = [
    left.removed.length > 0 ? `${left.removed.length} 个节点后来删掉了` : null,
    left.added.length > 0 ? `${left.added.length} 个节点那时还没有` : null,
    skipped > 0 ? `${skipped} 处参数现在（或那时）由图参数 / 子图参数提供、或已不存在，没动` : null,
    gpOneSide > 0 ? `${gpOneSide} 个图参数是后来加的或删掉的` : null,
    recipeDiffs > 0 ? `${recipeDiffs} 处对不上（配方里的值没动）` : null,
  ].filter(Boolean);
  const tail = notes.length > 0 ? `；${notes.join("，")}` : "";
  const ui = useUiStore.getState();
  // 什么都没改就没有撤销可撤：不说「Ctrl+Z 撤回」（按了会撤掉别的一步）
  if (changed === 0) {
    ui.showToast(notes.length === 0 ? `参数与第 ${r.seq} 次运行时一样` : `没有可恢复的参数${tail}`, notes.length > 0 ? "warn" : "info");
    return;
  }
  ui.showToast(`已恢复第 ${r.seq} 次运行时的参数（${changed} 处，Ctrl+Z 撤回）${tail}`, notes.length > 0 ? "warn" : "info");
}

function RunRow({
  record: r,
  diffLine,
  diffTitle,
  before: beforeOf,
  describe,
  onRestore,
}: {
  record: RunRecord;
  diffLine: string;
  diffTitle: string;
  /** 「比上一次」那一次的同一个读数（没被取消、有这个读数的最近一次）。 */
  before: (rd: RunRecord["readings"][number]) => RunRecord["readings"][number] | null;
  describe: (id: string) => ReturnType<typeof describeEventNode>;
  onRestore: () => void;
}) {
  // 写全路径：同一个子图用了两次时，只写节点自己的名字分不出是哪一个
  const scope = r.targets.length > 0 ? `运行到 ${r.targets.map((t) => describe(t).names.join(" › ")).join("、")}` : "整张图";
  return (
    <li className="runs__row" data-testid="run-record" data-seq={r.seq} data-status={r.status}>
      <div className="runs__head">
        <span className="runs__seq">#{r.seq}</span>
        <span className="runs__time">{clock(r.at)}</span>
        <span className={`runs__status runs__status--${r.status}`}>{STATUS_LABEL[r.status] ?? r.status}</span>
        {r.durationMs !== null && <span className="runs__time">{duration(r.durationMs)}</span>}
        <span className="runs__scope">{scope}</span>
        {r.recipe && <span className="runs__scope">配方 {r.recipe}</span>}
        <span className="runs__spacer" />
        {r.status !== "running" && (
          <button
            type="button"
            className="runs__restore"
            data-testid="run-restore"
            title="把参数改回这一次运行时的（节点的参数与静音、图参数的基础值；节点不增不删、配方不动）。一条撤销"
            onClick={onRestore}
          >
            恢复这组参数
          </button>
        )}
      </div>
      <div className="runs__diff" data-testid="run-diff" title={diffTitle}>
        {diffLine}
      </div>
      {r.readings.length > 0 && (
        <div className="runs__readings">
          {r.readings.map((rd) => {
            const d = describe(rd.id);
            const before = beforeOf(rd);
            const delta = deltaText(rd.value, before?.value ?? null);
            return (
              <button
                key={`${rd.id}.${rd.port}`}
                type="button"
                className="runs__reading"
                data-testid="run-reading"
                data-tone={verdictTone(rd.verdict) ?? undefined}
                title={`${d.names.join(" › ")}.${rd.port}${before ? `；上一次 ${before.value === null ? "未测出" : num(before.value)}` : ""}${d.reveal ? " —— 点此打开到它" : ""}`}
                disabled={!d.reveal}
                onClick={() => {
                  if (d.reveal) useUiStore.getState().revealNode(d.reveal.path, d.reveal.localId);
                }}
              >
                <span className="runs__reading-name">{d.names.join(" › ")}.{rd.port}</span>
                <span className="runs__reading-value">
                  {rd.value === null ? "未测出" : `${num(rd.value)}${rd.unit ? ` ${rd.unit}` : ""}`}
                </span>
                {rd.verdict && <span className={`insp-out__verdict is-${rd.verdict}`}>{rd.verdict}</span>}
                {delta && <span className="runs__delta">{delta}</span>}
              </button>
            );
          })}
        </div>
      )}
    </li>
  );
}
