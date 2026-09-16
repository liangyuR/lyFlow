import { useState } from "react";

import { num } from "../Inspector";
import type { OutputStat, OutputValue } from "../../types/execution";

interface Row {
  field: string;
  text: string;
  verdict?: string;
}

const MAX_DEPTH = 2;

function isNumberPair(v: unknown): v is [number, number] {
  return Array.isArray(v) && v.length === 2 && v.every((n) => typeof n === "number");
}

function scalar(v: unknown): string {
  if (v === null || v === undefined) return "—";
  if (typeof v === "number") return num(v);
  if (typeof v === "boolean") return v ? "是" : "否";
  if (typeof v === "string") return v;
  if (isNumberPair(v)) return `(${num(v[0])}, ${num(v[1])})`;
  if (Array.isArray(v)) {
    if (v.every((n) => typeof n === "number")) {
      return `(${(v as number[]).map(num).join(", ")})`;
    }
    return JSON.stringify(v);
  }
  return "{…}";
}

function flatten(value: unknown, prefix: string, depth: number, out: Row[]): void {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    out.push({ field: prefix, text: scalar(value) });
    return;
  }
  if (depth >= MAX_DEPTH) {
    out.push({ field: prefix, text: "{…}" });
    return;
  }
  const entries = Object.entries(value as Record<string, unknown>);
  if (entries.length === 0) {
    out.push({ field: prefix, text: "{}" });
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
      { field: "type", text: stat.type },
      { field: "elementCount", text: String(stat.elementCount) },
    ];
  }
  const rows: Row[] = [];
  for (const [key, raw] of Object.entries(value)) {
    if (raw === undefined) continue;
    if (key === "data") {
      flatten(raw, "data", 0, rows);
      continue;
    }
    if (key === "verdict" && typeof raw === "string") {
      rows.push({ field: key, text: raw, verdict: raw });
      continue;
    }
    rows.push({ field: key, text: scalar(raw) });
  }
  if (rows.length === 0) rows.push({ field: "elementCount", text: String(stat.elementCount) });
  return rows;
}

export function ValueView({ stat }: { stat: OutputStat }) {
  const [rawOpen, setRawOpen] = useState(false);
  const rows = rowsOf(stat);
  const raw = JSON.stringify(stat.value ?? stat, null, 2);

  return (
    <div className="peek-value">
      <div className="peek-value__scroll">
        <table className="peek-value__table" data-testid="peek-value-table">
          <tbody>
            {rows.map((r) => (
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
      <div className="peek-value__raw" data-testid="peek-raw" data-open={rawOpen ? "1" : "0"}>
        <button
          type="button"
          className="peek-value__rawtoggle"
          data-testid="peek-raw-toggle"
          onClick={() => setRawOpen((v) => !v)}
        >
          {rawOpen ? "▾" : "▸"} 原始 JSON
        </button>
        {rawOpen && <pre>{raw}</pre>}
      </div>
    </div>
  );
}
