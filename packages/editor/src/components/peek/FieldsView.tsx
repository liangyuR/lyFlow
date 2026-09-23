// Bundle 的字段表（m8-plan L17）：先列字段，点进一个字段再按字段类型复用现有视图
// （点云 / 框 / 下标 …）。字段的值从 `<port>.<field>` 那一项输出统计里取，与取数同一套寻址。

import { formatOutputValue } from "../Inspector";
import type { PeekSource } from "../../lib/peekSource";
import { useManifestStore } from "../../store/manifest";

export function FieldsView({ src, onPick }: { src: PeekSource; onPick: (field: string) => void }) {
  const typesByName = useManifestStore((s) => s.typesByName);
  const bundle = src.bundle;
  if (!bundle || !src.resolved) {
    return (
      <p className="peek__status" data-testid="peek-status">
        这个端口不是 Bundle
      </p>
    );
  }
  const port = src.resolved.port;
  return (
    <div className="peek-fields" data-testid="peek-fields" data-bundle={bundle.kind}>
      <p className="peek-fields__head">
        {bundle.label ?? bundle.kind} · {bundle.fields.length} 个字段
      </p>
      <ul className="peek-fields__list">
        {bundle.fields.map((f) => {
          const stat = src.outputs?.find((o) => o.port === `${port}.${f.name}`);
          const color = typesByName.get(f.type)?.color ?? "#6b7280";
          return (
            <li key={f.name}>
              <button
                type="button"
                className="peek-fields__row"
                data-testid={`peek-field-${f.name}`}
                data-type={f.type}
                data-count={stat?.elementCount ?? undefined}
                title={f.doc ?? f.name}
                onClick={() => onPick(f.name)}
              >
                <span className="peek-fields__name">{f.name}</span>
                <span className="peek-fields__type" style={{ borderColor: color, color }}>
                  {f.type}
                </span>
                <span className="peek-fields__value">
                  {stat
                    ? stat.value
                      ? formatOutputValue(stat)
                      : `${stat.elementCount.toLocaleString()} ${f.type === "PointCloud" ? "点" : "个"}`
                    : "—"}
                </span>
                <span className="peek-fields__go" aria-hidden>
                  ›
                </span>
              </button>
            </li>
          );
        })}
      </ul>
    </div>
  );
}
