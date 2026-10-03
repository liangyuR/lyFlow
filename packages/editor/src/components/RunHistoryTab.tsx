// 抽屉的「调参」页：每次正式运行一行，新的在上。写着这次比上一次改了什么、量测读数是多少、比上一次变了多少，
// 点读数打开到那个节点。调参的循环（改阈值 → 跑 → 看 gap 变了多少）以前上一次是多少全靠脑子记。

import { useMemo } from "react";

import { num } from "../lib/format";
import { verdictTone } from "../lib/outputs";
import { diffRuns, diffText, previousReading, type RunRecord } from "../lib/runHistory";
import { augmentOperators, describeEventNode } from "../lib/subgraph";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
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
        // 读数的「比上一次」只跟跑完了的比
        const lastDone = records.slice(i + 1).find((p) => p.status !== "running");
        return { r, diff, lastDone };
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
      {rows.map(({ r, diff, lastDone }) => (
        <RunRow key={r.runId} record={r} diffLine={diff ? diffText(diff) : "这张图打开以来的第一次"}
          diffTitle={diff ? diffText(diff, Infinity) : ""} lastDone={lastDone} describe={(id) => describeEventNode(doc, ops, id)} />
      ))}
    </ol>
  );
}

function RunRow({
  record: r,
  diffLine,
  diffTitle,
  lastDone,
  describe,
}: {
  record: RunRecord;
  diffLine: string;
  diffTitle: string;
  lastDone: RunRecord | undefined;
  describe: (id: string) => ReturnType<typeof describeEventNode>;
}) {
  const scope = r.targets.length > 0 ? `运行到 ${r.targets.map((t) => describe(t).names.at(-1) ?? t).join("、")}` : "整张图";
  return (
    <li className="runs__row" data-testid="run-record" data-seq={r.seq} data-status={r.status}>
      <div className="runs__head">
        <span className="runs__seq">#{r.seq}</span>
        <span className="runs__time">{clock(r.at)}</span>
        <span className={`runs__status runs__status--${r.status}`}>{STATUS_LABEL[r.status] ?? r.status}</span>
        {r.durationMs !== null && <span className="runs__time">{duration(r.durationMs)}</span>}
        <span className="runs__scope">{scope}</span>
        {r.recipe && <span className="runs__scope">配方 {r.recipe}</span>}
      </div>
      <div className="runs__diff" data-testid="run-diff" title={diffTitle}>
        {diffLine}
      </div>
      {r.readings.length > 0 && (
        <div className="runs__readings">
          {r.readings.map((rd) => {
            const d = describe(rd.id);
            const before = previousReading(lastDone, rd);
            const delta = deltaText(rd.value, before?.value ?? null);
            return (
              <button
                key={`${rd.id}.${rd.port}`}
                type="button"
                className="runs__reading"
                data-testid="run-reading"
                data-tone={verdictTone(rd.verdict) ?? undefined}
                title={`${d.names.join(" › ")}.${rd.port}${before ? `；上一次 ${before.value === null ? "未测出" : num(before.value)}` : ""} —— 点此打开到它`}
                disabled={!d.reveal}
                onClick={() => {
                  if (d.reveal) useUiStore.getState().revealNode(d.reveal.path, d.reveal.localId);
                }}
              >
                <span className="runs__reading-name">{d.names.at(-1) ?? rd.id}.{rd.port}</span>
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
