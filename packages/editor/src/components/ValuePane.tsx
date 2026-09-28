// 主预览的「值」内容（lib/viewRule）。普通模式一张，对比模式两侧都只有值时并排两张（compare-plan §1.6）。

import { ValueView } from "./peek/ValueView";
import type { OutputStat } from "../types/execution";

/** 主预览的「值」内容：每个输出端口一张表，与连线查看器的值视图是同一个组件。
 *  Bundle 字段展开出来的 `<port>.<field>` 不单列 —— 整个端口的值里已经有 fields。 */
export function ValuePane({ outputs }: { outputs: readonly OutputStat[] | undefined }) {
  const shown = (outputs ?? []).filter((o) => !o.port.includes("."));
  return (
    <div className="viewer__values" data-testid="viewer-values">
      {shown.map((o) => (
        <section key={o.port} className="viewer__value" data-port={o.port}>
          <header className="viewer__value-head">
            <span>{o.port}</span>
            <span className="viewer__value-type">{o.type}</span>
          </header>
          <ValueView stat={o} />
        </section>
      ))}
    </div>
  );
}
