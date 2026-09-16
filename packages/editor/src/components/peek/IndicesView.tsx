import { useEffect, useRef, useState, type ReactNode } from "react";

import { PEEK_FROZEN } from "../../store/peek";
import { transport } from "../../transport";
import { decodeIndices } from "../../types/execution";
import type { PeekViewProps } from "./types";

const PAGE = 10_000;
const ROW_H = 18;
const OVERSCAN = 8;
const FROZEN = PEEK_FROZEN;

interface Loaded {
  values: Int32Array;
  total: number;
  sourceCloudId: number;
}

function concat(a: Int32Array, b: Int32Array): Int32Array {
  const out = new Int32Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

export function IndicesView({ win, src }: PeekViewProps) {
  const scrollRef = useRef<HTMLDivElement>(null);
  const [loaded, setLoaded] = useState<Loaded | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewH, setViewH] = useState(160);

  const lockedRun = win.locked?.runId ?? null;
  const runId = lockedRun ?? src.runId;
  const resolved = src.resolved;

  useEffect(() => {
    let cancelled = false;
    setLoaded(null);
    setError(null);
    setScrollTop(0);
    if (scrollRef.current) scrollRef.current.scrollTop = 0;
    if (src.status !== null || !runId || !resolved) {
      setLoading(false);
      return;
    }
    setLoading(true);
    void (async () => {
      try {
        const buffer = await transport.getOutputIndices(
          runId,
          resolved.nodeId,
          resolved.port,
          0,
          PAGE,
        );
        if (cancelled) return;
        const payload = decodeIndices(buffer);
        setLoaded({
          values: payload.values,
          total: payload.total,
          sourceCloudId: payload.sourceCloudId,
        });
      } catch (e) {
        if (cancelled) return;
        setError(lockedRun ? FROZEN : e instanceof Error ? e.message : String(e));
      } finally {
        setLoading(false);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [runId, lockedRun, resolved, src.status]);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    const measure = () => setViewH(el.clientHeight || 1);
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(el);
    return () => observer.disconnect();
  }, []);

  const more = async () => {
    if (!loaded || !runId || !resolved || loading) return;
    setLoading(true);
    try {
      const buffer = await transport.getOutputIndices(
        runId,
        resolved.nodeId,
        resolved.port,
        loaded.values.length,
        PAGE,
      );
      const payload = decodeIndices(buffer);
      if (payload.values.length === 0) {
        setError("后端没有再返回下标，这一段到此为止");
        return;
      }
      setLoaded((prev) =>
        prev === null
          ? null
          : {
              values: concat(prev.values, payload.values),
              total: payload.total,
              sourceCloudId: payload.sourceCloudId,
            },
      );
    } catch (e) {
      setError(lockedRun ? FROZEN : e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  };

  const values = loaded?.values ?? new Int32Array(0);
  const first = Math.max(0, Math.floor(scrollTop / ROW_H) - OVERSCAN);
  const last = Math.min(values.length, Math.ceil((scrollTop + viewH) / ROW_H) + OVERSCAN);
  const rows: ReactNode[] = [];
  for (let i = first; i < last; i += 1) {
    rows.push(
      <div className="peek-indices__row" key={i} style={{ top: i * ROW_H, height: ROW_H }}>
        <span className="peek-indices__ord">{i}</span>
        <span className="peek-indices__val">{values[i]}</span>
      </div>,
    );
  }

  const total = loaded?.total ?? 0;
  const rest = total - values.length;

  return (
    <div className="peek-indices" data-testid="peek-indices">
      <div
        className="peek-indices__head"
        data-total={total}
        data-source-cloud={loaded?.sourceCloudId ?? ""}
      >
        {loaded ? (
          <>
            <span>{total.toLocaleString()} 个下标</span>
            <span className="peek-indices__sep">·</span>
            <span className="peek-indices__src" title="这些下标指向哪片云">
              来源云 {loaded.sourceCloudId}
            </span>
            <span className="peek-indices__spacer" />
            <span className="peek-indices__shown">已取 {values.length.toLocaleString()}</span>
          </>
        ) : (
          <span>{loading ? "正在取下标…" : (error ?? "无下标数据")}</span>
        )}
      </div>

      <div
        className="peek-indices__scroll"
        ref={scrollRef}
        onScroll={(e) => setScrollTop(e.currentTarget.scrollTop)}
      >
        <div className="peek-indices__spacerbox" style={{ height: values.length * ROW_H }}>
          {rows}
        </div>
      </div>

      {loaded && rest > 0 && (
        <div className="peek-indices__foot">
          <button
            type="button"
            className="peek-indices__more"
            data-testid="peek-indices-more"
            onClick={() => void more()}
            disabled={loading || lockedRun !== null}
            title={lockedRun ? FROZEN : `还有 ${rest.toLocaleString()} 个没取`}
          >
            再取 {Math.min(PAGE, rest).toLocaleString()} 个
          </button>
          <span className="peek-indices__rest">剩 {rest.toLocaleString()}</span>
        </div>
      )}
      {loaded && error !== null && (
        <div className="peek-indices__foot peek-indices__foot--err">{error}</div>
      )}
    </div>
  );
}
