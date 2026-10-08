/** Presentation of core values. Detection decisions stay in the original records. */
export function object(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : {};
}

export function valueAt(value: unknown, path: string): unknown {
  let current = value;
  for (const key of path ? path.split(".") : []) {
    if (Array.isArray(current)) current = current[Number(key)];
    else {
      const o = object(current);
      const bundle = Array.isArray(o["fields"]) ? o["fields"].map(object).find((f) => f["name"] === key) : undefined;
      current = Object.hasOwn(o, key) ? o[key] : object(o["data"])[key] ?? bundle?.["value"];
    }
  }
  return current;
}

export function statistics(values: unknown[]): Record<string, unknown> {
  const finite = values.filter((v): v is number => typeof v === "number" && Number.isFinite(v)).sort((a, b) => a - b);
  const n = finite.length;
  const mean = n ? finite.reduce((a, b) => a + b, 0) / n : null;
  const q = (p: number) => {
    if (!n) return null;
    const at = (n - 1) * p;
    const lo = Math.floor(at);
    return finite[lo]! + (finite[Math.ceil(at)]! - finite[lo]!) * (at - lo);
  };
  return {
    count: values.length, valid: n, missing: values.length - n,
    min: finite[0] ?? null, max: finite[n - 1] ?? null, mean,
    p50: q(0.5), p95: q(0.95),
    std: n > 1 ? Math.sqrt(finite.reduce((a, b) => a + (b - mean!) ** 2, 0) / (n - 1)) : null,
  };
}

export interface Projection {
  fields?: string[] | undefined;
  offset?: number | undefined;
  limit?: number | undefined;
}

/** Bounds every nested array; numeric and categorical distributions retain the full population. */
export function projectValue(value: unknown, options: Projection = {}): Record<string, unknown> {
  const offset = options.offset ?? 0;
  const limit = options.limit ?? 8;
  const arrays: Record<string, unknown> = {};
  let truncated = false;
  const walk = (v: unknown, path: string, depth: number, nestedArray = false): unknown => {
    if (depth > 12) { truncated = true; return { omitted: "depth_limit" }; }
    if (Array.isArray(v)) {
      // An outer page must not trim each coordinate vector or nested list again.
      const start = nestedArray ? 0 : offset;
      const size = nestedArray && v.length <= 4 && v.every((x) => typeof x === "number") ? v.length : limit;
      const details: Record<string, unknown> = { count: v.length, offset: start, shown: Math.max(0, Math.min(size, v.length - start)) };
      if (v.every((x) => x === null || typeof x === "number")) details["statistics"] = statistics(v);
      if (v.every((x) => x === null || typeof x === "string" || typeof x === "boolean")) {
        const counts = new Map<string, number>();
        for (const x of v) counts.set(String(x), (counts.get(String(x)) ?? 0) + 1);
        details["counts"] = Object.fromEntries([...counts].sort((a,b) => b[1]-a[1] || a[0].localeCompare(b[0])).slice(0,limit));
        details["distinct"] = counts.size;
        details["countsTruncated"] = counts.size > limit;
      }
      arrays[path] = details;
      if (start > 0 || start + size < v.length) truncated = true;
      return v.slice(start, start + size).map((x, i) => walk(x, `${path}.${start + i}`, depth + 1, true));
    }
    if (v !== null && typeof v === "object") {
      return Object.fromEntries(Object.entries(object(v)).map(([k, x]) => [k, walk(x, path ? `${path}.${k}` : k, depth + 1, nestedArray)]));
    }
    return v;
  };
  const chosen = options.fields
    ? Object.fromEntries(options.fields.map((p) => [p, valueAt(value, p) ?? null])) : value;
  return { value: walk(chosen, "", 0), arrays, truncated };
}

/** Explicit domain facts, independent of the engine's execution status. */
export function assessment(value: unknown): Record<string, unknown> {
  const o = object(value);
  const d = object(o["data"]);
  const type = o["type"];
  if (type === "glue.Pose2D" || type === "glue.PathInfo") {
    return { detectionStatus: typeof d["ok"] !== "boolean" ? "unknown" : d["ok"] ? "valid" : "invalid", reason: d["message"] ?? null, score: d["score"] ?? null };
  }
  if (type === "glue.StationMeasure") {
    const counts = object(d["counts"]);
    return { measurementStatus: typeof counts["ok"] !== "number" || typeof d["count"] !== "number" ? "unknown" : counts["ok"] === d["count"] ? "complete" : "incomplete", counts, unit: d["unit"] ?? null };
  }
  if (type === "glue.BeadInfo" || type === "glue.Breaks") {
    return { detectionStatus: typeof d["pathOk"] !== "boolean" ? "unknown" : d["pathOk"] ? "valid" : "invalid", reason:d["message"] ?? null,
      unit:d["unit"] ?? null, ...(type === "glue.BeadInfo" ? {coverage:d["coverage"] ?? null} : {breakCount:d["count"] ?? null}) };
  }
  if (type === "glue.Verdict") {
    return { productVerdict: typeof d["ok"] !== "boolean" ? "unknown" : d["ok"] ? "ok" : "ng", counts: d["counts"] ?? null, reason: d["message"] ?? null, unit: d["unit"] ?? null };
  }
  if (o["kind"] === "Bundle") {
    if (Array.isArray(o["fields"])) return Object.fromEntries(o["fields"].map(object).map((f) => [String(f["name"]), assessment(f["value"])]));
    return Object.fromEntries(Object.entries(object(o["fields"] ?? o["data"])).map(([k, x]) => [k, assessment(x)]));
  }
  return {};
}
