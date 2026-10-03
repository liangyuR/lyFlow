// 抽屉的「调参」页：每次正式运行一行，新的在上。写着这次比上一次改了什么、量测读数是多少、比上一次变了多少，
// 点读数打开到那个节点；「恢复这组参数」把参数改回那一次的（一条撤销）。调参的循环（改阈值 → 跑 → 看 gap 变了多少）
// 以前上一次是多少全靠脑子记，回到读数最好的那一组只能一个个参数凭记忆改回去。

import { useMemo } from "react";

import { num } from "../lib/format";
import { verdictTone } from "../lib/outputs";
import { diffRuns, diffText, previousChain, readingBefore, type RunRecord } from "../lib/runHistory";
import { augmentOperators, describeEventNode } from "../lib/subgraph";
import { findRecipe } from "../lib/recipes";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { runParamsOf, useRecipeStore } from "../store/recipe";
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
  const baselineId = useRunHistoryStore((s) => s.baseline);
  const doc = useGraphStore((s) => s.doc);
  const baseOps = useManifestStore((s) => s.operatorsById);
  const baseline = baselineId ? records.find((r) => r.runId === baselineId) : undefined;
  // 「上一次」的链：被挤出 50 条、接在最后的基准不算谁的上一次
  const chain = useMemo(() => previousChain(records), [records]);

  // 每一行与它前一次（更早的那一条）比；定了基准的话再与基准比一行。图参数、子图定义都按各自那一份
  const rows = useMemo(
    () =>
      records.map((r, i) => {
        const prev = chain[i + 1];
        const ops = augmentOperators(baseOps, { ...prev?.doc.subgraphs, ...baseline?.doc.subgraphs, ...r.doc.subgraphs });
        const diff = prev ? diffRuns(prev, r, ops) : null;
        const vsBase = baseline && baseline !== r ? diffRuns(baseline, r, ops) : null;
        return { r, i, diff, vsBase };
      }),
    [records, chain, baseOps, baseline],
  );
  const ops = useMemo(() => augmentOperators(baseOps, doc.subgraphs), [baseOps, doc.subgraphs]);

  // 基准那一条的读数：与每一行同一个写法（带节点名、单位），同名端口（gap / flush）才分得清
  const baseText = (b: RunRecord) =>
    b.readings.map(
      (rd) =>
        `${describeEventNode(doc, ops, rd.id).names.join(" › ")}.${rd.port} ${rd.value === null ? "未测出" : `${num(rd.value)}${rd.unit ? ` ${rd.unit}` : ""}`}`,
    );

  if (records.length === 0) {
    return (
      <p className="drawer__empty">
        还没有运行记录。改参数、按 F5 跑一次：这里一行一次，写着改了什么、量测读数变成多少。
      </p>
    );
  }

  return (
    <>
      {baseline && (
        // 基准钉在最上面：调了几次之后要回头看的就是它
        <div className="runs__base" data-testid="run-baseline-bar">
          <span className="runs__base-text" title={baseText(baseline).join("\n")}>
            基准 #{baseline.seq} · {clock(baseline.at)}
            {baseline.readings.length > 0 && ` · ${baseText(baseline).join("，")}`}
          </span>
          <button type="button" className="runs__restore" onClick={() => useRunHistoryStore.getState().setBaseline(null)}>
            取消基准
          </button>
        </div>
      )}
      <ol className="runs" data-testid="run-history">
        {rows.map(({ r, i, diff, vsBase }) => (
          <RunRow
            key={r.runId}
            record={r}
            // 更早的记录被 50 条的上限挤掉了时说清楚，不冒充「第一次」
            diffLine={diff ? diffText(diff) : r.seq === 1 ? "这张图打开以来的第一次" : "更早的记录已不保留，比不出改了什么"}
            diffTitle={diff ? diffText(diff, Infinity) : ""}
            baseLine={vsBase && baseline ? `比基准 #${baseline.seq}：${diffText(vsBase)}` : null}
            baseTitle={vsBase ? diffText(vsBase, Infinity) : ""}
            isBaseline={baseline === r}
            // 读数的变化：定了基准就与基准比，不然与上一次比
            before={(rd) => readingBefore(chain, i, r, baseline, rd).reading}
            beforeLabel={baseline && baseline !== r ? `基准 #${baseline.seq}` : "上一次"}
            describe={(id) => describeEventNode(doc, ops, id)}
            onRestore={() => restoreRun(r, baseOps)}
            onBaseline={() => useRunHistoryStore.getState().setBaseline(baseline === r ? null : r.runId)}
          />
        ))}
      </ol>
    </>
  );
}

/** 「恢复这组参数」：改回那一次的参数，说清楚哪些没能恢复（后来删掉的节点、那时还没有的、配方里的值）。 */
function restoreRun(r: RunRecord, baseOps: Parameters<typeof augmentOperators>[0]): void {
  const { changed, skipped } = useGraphStore.getState().restoreParams(r.doc, `恢复第 ${r.seq} 次运行的参数`, {
    params: r.params,
    recipe: r.recipe,
    recipeId: r.recipeId,
  });
  const rs = useRecipeStore.getState();
  const current = rs.current;
  const where = (name: string | null) => (name ? `配方「${name}」` : "基础");
  // 那次的配方现在叫什么（改过名的按 id 找到新名字）；找不到就是删掉了
  const thenEntry = r.recipeId ? rs.set.recipes.find((e) => e.id === r.recipeId) : r.recipe ? findRecipe(rs.set, r.recipe) : undefined;
  const thenName = thenEntry?.name ?? null;
  const sameRecipe = r.recipeId != null ? r.recipeId === rs.currentId : r.recipe === current;
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
    recipeDiffs > 0
      ? sameRecipe
        ? `${recipeDiffs} 处对不上`
        : r.recipe && !thenEntry
          ? `那一次是在配方「${r.recipe}」下跑的，这个配方后来删掉了：配方里的 ${recipeDiffs} 处没动`
          : `那一次是在${where(thenName)}下跑的、现在是${where(current)}：配方里的 ${recipeDiffs} 处没动（切到${where(thenName)}再恢复）`
      : null,
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
  baseLine,
  baseTitle,
  isBaseline,
  before: beforeOf,
  beforeLabel,
  describe,
  onRestore,
  onBaseline,
}: {
  record: RunRecord;
  diffLine: string;
  diffTitle: string;
  /** 「比基准 #7：…」（定了基准、这一行不是基准时）。 */
  baseLine: string | null;
  baseTitle: string;
  isBaseline: boolean;
  /** 读数比的那一次的同一个读数：基准，或上一次（没被取消、有这个读数的最近一次）。 */
  before: (rd: RunRecord["readings"][number]) => RunRecord["readings"][number] | null;
  beforeLabel: string;
  describe: (id: string) => ReturnType<typeof describeEventNode>;
  onRestore: () => void;
  onBaseline: () => void;
}) {
  // 写全路径：同一个子图用了两次时，只写节点自己的名字分不出是哪一个
  const scope = r.targets.length > 0 ? `运行到 ${r.targets.map((t) => describe(t).names.join(" › ")).join("、")}` : "整张图";
  return (
    <li className={`runs__row${isBaseline ? " is-baseline" : ""}`} data-testid="run-record" data-seq={r.seq} data-status={r.status}
      data-baseline={isBaseline ? "1" : undefined}>
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
            className={`runs__restore${isBaseline ? " is-on" : ""}`}
            data-testid="run-baseline"
            title={isBaseline ? "取消基准" : "设为基准：之后每一次都与它比（改了什么、读数差多少）"}
            onClick={onBaseline}
          >
            {isBaseline ? "基准 ✓" : "设为基准"}
          </button>
        )}
        {r.status !== "running" && (
          <button
            type="button"
            className="runs__restore"
            data-testid="run-restore"
            title="把参数改回这一次运行时的（节点的参数与静音、图参数；现在选着的就是那次的配方时配方里的值也改回去；节点不增不删）。一条撤销"
            onClick={onRestore}
          >
            恢复这组参数
          </button>
        )}
      </div>
      <div className="runs__diff" data-testid="run-diff" title={diffTitle}>
        {diffLine}
      </div>
      {baseLine && (
        <div className="runs__diff runs__diff--base" data-testid="run-diff-base" title={baseTitle}>
          {baseLine}
        </div>
      )}
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
                title={`${d.names.join(" › ")}.${rd.port}${before ? `；${beforeLabel} ${before.value === null ? "未测出" : num(before.value)}` : ""}${d.reveal ? " —— 点此打开到它" : ""}`}
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
