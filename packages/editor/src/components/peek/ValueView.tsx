import { useState } from 'react';
import { resultSummary } from '@lyflow/visualization';

import { num } from '../Inspector';
import type { OutputStat, OutputValue } from '../../types/execution';
import { ResultScene } from '../ResultScene';

interface Row {
  field: string;
  text: string;
  verdict?: string;
}

const MAX_DEPTH = 2;

function isNumberPair(v: unknown): v is [number, number] {
  return Array.isArray(v) && v.length === 2 && v.every((n) => typeof n === 'number');
}

function scalar(v: unknown): string {
  if (v === null || v === undefined) return '—';
  if (typeof v === 'number') return num(v);
  if (typeof v === 'boolean') return v ? '是' : '否';
  if (typeof v === 'string') return v;
  if (isNumberPair(v)) return `(${num(v[0])}, ${num(v[1])})`;
  if (Array.isArray(v)) {
    if (v.every((n) => typeof n === 'number')) {
      return `(${(v as number[]).map(num).join(', ')})`;
    }
    return JSON.stringify(v);
  }
  return '{…}';
}

function flatten(value: unknown, prefix: string, depth: number, out: Row[]): void {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    out.push({ field: prefix, text: scalar(value) });
    return;
  }
  if (depth >= MAX_DEPTH) {
    out.push({ field: prefix, text: '{…}' });
    return;
  }
  const entries = Object.entries(value as Record<string, unknown>);
  if (entries.length === 0) {
    out.push({ field: prefix, text: '{}' });
    return;
  }
  for (const [key, child] of entries) {
    flatten(child, `${prefix}.${key}`, depth + 1, out);
  }
}

function rowsOf(stat: OutputStat): Row[] {
  const value: OutputValue | undefined = stat.value;
  if (!value) {
    return [
      { field: 'type', text: stat.type },
      { field: 'elementCount', text: String(stat.elementCount) },
    ];
  }
  const rows: Row[] = [];
  try {
    const summary = resultSummary(value);
    if (summary) {
      for (const [field, raw] of Object.entries(summary)) {
        if (field === 'items') {
          for (const item of raw as Record<string, unknown>[])
            for (const [key, child] of Object.entries(item))
              if (child !== undefined)
                rows.push({ field: `${item.id}.${key}`, text: scalar(child) });
        } else if (raw !== undefined) rows.push({ field, text: scalar(raw) });
      }
      return rows;
    }
  } catch (error) {
    rows.push({ field: 'diagnostic', text: String(error) });
  }
  for (const [key, raw] of Object.entries(value)) {
    if (raw === undefined) continue;
    if (key === 'data') {
      flatten(raw, 'data', 0, rows);
      continue;
    }
    if (key === 'verdict' && typeof raw === 'string') {
      rows.push({ field: key, text: raw, verdict: raw });
      continue;
    }
    rows.push({ field: key, text: scalar(raw) });
  }
  if (rows.length === 0) rows.push({ field: 'elementCount', text: String(stat.elementCount) });
  return rows;
}

export function ValueView({ stat }: { stat: OutputStat }) {
  const [rawOpen, setRawOpen] = useState(false);
  const [page, setPage] = useState(0);
  const rows = rowsOf(stat);
  const pageCount = Math.max(1, Math.ceil(rows.length / 100));
  const currentPage = Math.min(page, pageCount - 1);
  const raw = JSON.stringify(stat.value ?? stat, null, 2);

  return (
    <div className="peek-value">
      {stat.value && <ResultScene value={stat.value} />}
      <div className="peek-value__scroll">
        <table className="peek-value__table" data-testid="peek-value-table">
          <tbody>
            {rows.slice(currentPage * 100, (currentPage + 1) * 100).map((r) => (
              <tr key={r.field} data-field={r.field}>
                <th scope="row">{r.field}</th>
                <td>
                  {r.verdict ? (
                    <span className={`insp-out__verdict is-${r.verdict}`}>{r.verdict}</span>
                  ) : (
                    r.text
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {pageCount > 1 && (
        <div className="viewer__bar">
          <button
            type="button"
            disabled={currentPage === 0}
            onClick={() => setPage(currentPage - 1)}
          >
            上一页
          </button>
          <span>
            {currentPage + 1}/{pageCount} · {rows.length} 项
          </span>
          <button
            type="button"
            disabled={currentPage === pageCount - 1}
            onClick={() => setPage(currentPage + 1)}
          >
            下一页
          </button>
        </div>
      )}
      <div className="peek-value__raw" data-testid="peek-raw" data-open={rawOpen ? '1' : '0'}>
        <button
          type="button"
          className="peek-value__rawtoggle"
          data-testid="peek-raw-toggle"
          onClick={() => setRawOpen((v) => !v)}
        >
          {rawOpen ? '▾' : '▸'} 原始 JSON
        </button>
        {rawOpen && <pre>{raw}</pre>}
      </div>
    </div>
  );
}
