//
// 算子详情。
//
// M0 里这是**只读**的：它证明 manifest 里的每一个字段都真的走通了三层，
// 包括端口类型着色、参数范围、enum 选项、参数联动条件。
//
// M1 会把「参数」那一段换成真正可编辑的表单（同样由 manifest 驱动），
// 端口那一段搬到画布上的节点里去。这里的渲染逻辑到时候可以直接搬走。
//

import { usePortColor } from "../store/manifest";
import type { OperatorDesc, Param, Port } from "../types/manifest";

function formatDefault(value: unknown): string {
  if (value === null || value === undefined) return "—";
  if (Array.isArray(value)) return `[${value.join(", ")}]`;
  if (typeof value === "string") return value === "" ? '""' : value;
  return String(value);
}

function formatRange(p: Param): string | null {
  const parts: string[] = [];
  const hard =
    p.min !== undefined || p.max !== undefined
      ? `${p.min ?? "-∞"} … ${p.max ?? "+∞"}`
      : null;
  const soft =
    p.softMin !== undefined || p.softMax !== undefined
      ? `滑块 ${p.softMin ?? "-∞"} … ${p.softMax ?? "+∞"}`
      : null;
  if (hard) parts.push(hard);
  if (soft) parts.push(soft);
  if (p.step !== undefined) parts.push(`步长 ${p.step}`);
  return parts.length > 0 ? parts.join("　") : null;
}

function conditionText(p: Param): string | null {
  const c = p.visibleWhen ?? p.enabledWhen;
  if (!c) return null;
  const verb = p.visibleWhen ? "显示" : "启用";
  if (c.eq !== undefined) return `仅当 ${c.param} = ${formatDefault(c.eq)} 时${verb}`;
  if (c.in !== undefined) return `仅当 ${c.param} ∈ [${c.in.map(formatDefault).join(", ")}] 时${verb}`;
  if (c.ne !== undefined) return `仅当 ${c.param} ≠ ${formatDefault(c.ne)} 时${verb}`;
  return null;
}

function PortRow({ port, isInput }: { port: Port; isInput: boolean }) {
  const color = usePortColor(port.type);
  const optional = isInput && port.required === false;

  return (
    <li className="port">
      <span className="port__dot" style={{ background: color }} aria-hidden />
      <span className="port__name">{port.label || port.name}</span>
      <code className="port__type" style={{ color }}>
        {port.type}
      </code>
      {optional && <span className="port__optional">可选</span>}
      {port.doc && <span className="port__doc">{port.doc}</span>}
    </li>
  );
}

function ParamRow({ param }: { param: Param }) {
  const range = formatRange(param);
  const condition = conditionText(param);

  return (
    <li className="param">
      <div className="param__head">
        <span className="param__name">{param.label || param.name}</span>
        <code className="param__type">{param.type}</code>
        {param.advanced && <span className="tag tag--muted">高级</span>}
      </div>

      {param.doc && <p className="param__doc">{param.doc}</p>}

      <dl className="param__facts">
        <dt>默认</dt>
        <dd>
          <code>{formatDefault(param.default)}</code>
          {param.unit && <span className="param__unit">{param.unit}</span>}
        </dd>

        {range && (
          <>
            <dt>范围</dt>
            <dd>{range}</dd>
          </>
        )}

        {param.options && (
          <>
            <dt>选项</dt>
            <dd className="param__options">
              {param.options.map((o) => (
                <span key={String(o.value)} className="tag" title={o.doc}>
                  {o.label}
                </span>
              ))}
            </dd>
          </>
        )}

        {param.filters && (
          <>
            <dt>文件类型</dt>
            <dd>
              {param.filters
                .map((f) => `${f.name} (${f.extensions.join(", ")})`)
                .join("　")}
            </dd>
          </>
        )}

        {condition && (
          <>
            <dt>联动</dt>
            <dd className="param__condition">{condition}</dd>
          </>
        )}
      </dl>
    </li>
  );
}

export function OperatorDetail({ op }: { op: OperatorDesc }) {
  const caps = op.capabilities ?? {};

  return (
    <article className="detail">
      <header className="detail__head">
        <h2>{op.label}</h2>
        <div className="detail__meta">
          <code>{op.id}</code>
          <span className="tag tag--version">v{op.version}</span>
          <span className="detail__category">{op.category}</span>
        </div>
        {op.doc && <p className="detail__doc">{op.doc}</p>}
        <div className="detail__caps">
          {caps.cancellable && <span className="tag" title="支持中途取消，可用于 live preview">可取消</span>}
          {caps.previewable && <span className="tag" title="支持降级质量的快速预览">可预览</span>}
          {caps.deterministic ? (
            <span className="tag" title="同输入同输出，可参与缓存复用">确定性</span>
          ) : (
            <span className="tag tag--warn" title="结果不可复现，不能参与缓存">非确定性</span>
          )}
        </div>
      </header>

      <section className="detail__section">
        <h3>端口</h3>
        <div className="detail__ports">
          <div>
            <h4>输入</h4>
            {op.inputs.length === 0 ? (
              <p className="detail__none">无（源节点）</p>
            ) : (
              <ul className="port-list">
                {op.inputs.map((p) => (
                  <PortRow key={p.name} port={p} isInput />
                ))}
              </ul>
            )}
          </div>
          <div>
            <h4>输出</h4>
            {op.outputs.length === 0 ? (
              <p className="detail__none">无（终端节点）</p>
            ) : (
              <ul className="port-list">
                {op.outputs.map((p) => (
                  <PortRow key={p.name} port={p} isInput={false} />
                ))}
              </ul>
            )}
          </div>
        </div>
      </section>

      <section className="detail__section">
        <h3>
          参数
          <span className="detail__hint">M1 会把这里换成 manifest 驱动的可编辑表单</span>
        </h3>
        {op.params.length === 0 ? (
          <p className="detail__none">无参数</p>
        ) : (
          <ul className="param-list">
            {op.params.map((p) => (
              <ParamRow key={p.name} param={p} />
            ))}
          </ul>
        )}
      </section>
    </article>
  );
}
