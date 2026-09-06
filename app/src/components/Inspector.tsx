// 右侧检查器：选中节点的参数表单。字段、控件、范围、单位、分组、联动条件
// 全部由 manifest 生成（ADR-0003），这个文件里没有任何算子的名字。

import { groupParams, effectiveParams, isEnabled, isVisible, valueEquals } from "../lib/params";
import { augmentOperators, levelOf, promotedBy } from "../lib/subgraph";
import { useNodeExecution, useParamErrors } from "../store/execution";
import { currentSubgraph, useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { OutputStat, OutputValue } from "../types/execution";
import type { OperatorDesc, Param } from "../types/manifest";
import type { GraphNode, SubgraphDef } from "../types/graph";

import { OperatorDetail } from "./OperatorDetail";
import { ParamControl } from "./ParamControls";

/** 六位有效数字。2D 几何的坐标是米，原样打印会拖一串浮点噪声。 */
function num(v: number | undefined): string {
  if (v === undefined || !Number.isFinite(v)) return "—";
  return String(Number(v.toPrecision(6)));
}

function pair(v: [number, number] | number | null | undefined): string {
  return Array.isArray(v) ? `(${num(v[0])}, ${num(v[1])})` : "—";
}

/** 非点云输出的一行文本。类型未知时退回类型名，永远不抛。 */
export function formatOutputValue(o: OutputStat): string {
  const v: OutputValue | undefined = o.value;
  if (!v) return `${o.elementCount} 个元素`;
  switch (v.kind) {
    case "Measurement":
      if (v.value === null || v.value === undefined) return v.message || "未测出";
      return `${num(v.value)} ${v.unit ?? ""}`.trim();
    case "Box2D":
      return `${pair(v.min)} → ${pair(v.max)}`;
    case "Line2D":
      return v.hasSegment
        ? `${pair(v.start)} → ${pair(v.end)}`
        : `过 ${pair(v.point)} 方向 ${pair(v.dir)}`;
    case "Circle2D":
      return `圆心 ${pair(v.center)} 半径 ${num(v.radius)}`;
    case "Point2D":
      return pair(v.p);
    case "Record":
      return `${v.type ?? ""} ${JSON.stringify(v.data ?? {})}`.trim();
    case "Plane":
      return `n=(${(v.normal ?? []).map(num).join(", ")}) d=${num(v.d)}`;
    case "Tensor":
      return `[${(v.shape ?? []).join(", ")}] 均值 ${num(v.mean ?? undefined)}`;
    default:
      return `${o.elementCount} 个元素`;
  }
}

/** 该节点这次运行的非点云输出。点云走 3D 视图，这里只显示能读的值。 */
function OutputValues({ outputs }: { outputs: OutputStat[] }) {
  const shown = outputs.filter((o) => o.value !== undefined);
  if (shown.length === 0) return null;
  return (
    <section className="insp__group" data-testid="inspector-outputs">
      <h4 className="insp__group-title">输出</h4>
      {shown.map((o) => {
        const verdict = o.value?.kind === "Measurement" ? o.value.verdict : undefined;
        return (
          <div
            className="insp-out"
            key={o.port}
            data-testid={`output-${o.port}`}
            data-type={o.type}
            data-verdict={verdict || undefined}
          >
            <span className="insp-out__port" title={o.type}>
              {o.port}
            </span>
            <span className="insp-out__value">{formatOutputValue(o)}</span>
            {verdict && <span className={`insp-out__verdict is-${verdict}`}>{verdict}</span>}
          </div>
        );
      })}
    </section>
  );
}

function ParamRow({
  param,
  node,
  effective,
  error,
  def,
}: {
  param: Param;
  node: GraphNode;
  effective: Record<string, unknown>;
  error?: string | undefined;
  def?: SubgraphDef | undefined;
}) {
  const setParam = useGraphStore((s) => s.setParam);
  const value = effective[param.name];
  // 已提升的内参在内部只读：真正的值来自外层表单，两处都能改就没人知道谁赢（F4）
  const promoted = promotedBy(def, node.id, param.name);
  const disabled = !isEnabled(param, effective) || promoted !== undefined;
  // 稀疏存储的直接可视化：params 里有这个键 = 用户改过它。
  const overridden = node.params?.[param.name] !== undefined;

  return (
    <div
      className={`insp-param${disabled ? " is-disabled" : ""}${error ? " has-error" : ""}`}
      data-testid={`param-${param.name}`}
      data-param-error={error ? "1" : undefined}
      data-promoted={promoted ? promoted.name : undefined}
    >
      <div className="insp-param__label">
        <span className={overridden ? "is-overridden" : ""} title={param.doc}>
          {param.label || param.name}
        </span>
        {promoted && (
          <span className="insp-param__promoted" title={`已提升为子图参数 ${promoted.name}`}>
            ↑{promoted.name}
          </span>
        )}
        {overridden && !promoted && (
          <button
            type="button"
            className="insp-param__reset"
            title={`重置为默认值 ${JSON.stringify(param.default)}`}
            onClick={() => setParam(node.id, param.name, param.default)}
          >
            ↺
          </button>
        )}
      </div>
      <div className="insp-param__control">
        <ParamControl
          param={param}
          value={value}
          disabled={disabled}
          nodeId={node.id}
          promotedAs={promoted?.name}
          onChange={(v) => {
            if (!valueEquals(v, value)) setParam(node.id, param.name, v);
          }}
        />
        {/* 错误消息直接贴在控件下面，而不是只做个红框 ——
            红框只说明「这里错了」，用户还得自己猜错在哪（P0 #15）。 */}
        {error && <p className="insp-param__error">{error}</p>}
      </div>
    </div>
  );
}

function NodeInspector({ node, op }: { node: GraphNode; op: OperatorDesc }) {
  const setNodeUi = useGraphStore((s) => s.setNodeUi);
  const errors = useParamErrors(node.id);
  const exec = useNodeExecution(node.id);
  const doc = useGraphStore((s) => s.doc);
  const path = useUiStore((s) => s.path);
  const def = currentSubgraph(doc, path);
  const effective = effectiveParams(op, node);
  const groups = groupParams(op.params);

  const staleVersion = node.opVersion && node.opVersion !== op.version;

  return (
    <div className="insp">
      <header className="insp__head">
        <input
          className="insp__title"
          value={node.ui?.title ?? ""}
          placeholder={op.label}
          spellCheck={false}
          onChange={(e) => setNodeUi(node.id, { title: e.target.value || null })}
        />
        <div className="insp__meta">
          <code>{op.id}</code>
          <span className="tag tag--version">v{op.version}</span>
        </div>
        {staleVersion && (
          <p className="insp__warn">
            此节点保存于 v{node.opVersion}，当前算子是 v{op.version}。
            默认值或参数含义可能已变更。
          </p>
        )}
        {op.doc && <p className="insp__doc">{op.doc}</p>}
      </header>

      {/* 该节点这次运行的全部诊断（D5）。带 paramPath 的会同时在下面标红框，
          不带的（端口、IO 问题）只有这里看得到，所以一条都不能省。 */}
      {exec && exec.errors.length > 0 && (
        <section className="insp__errors" data-testid="inspector-errors">
          <h4 className="insp__errors-title">
            {exec.state === "cancelled" ? "未执行" : "执行出错"}
          </h4>
          <ul>
            {exec.errors.map((e, i) => (
              <li key={i}>
                <code className="insp__errcode">{e.code}</code>
                <span>{e.message}</span>
              </li>
            ))}
          </ul>
        </section>
      )}

      {exec?.stats?.outputs && <OutputValues outputs={exec.stats.outputs} />}

      {op.params.length === 0 ? (
        <p className="insp__none">此算子没有参数</p>
      ) : (
        groups.map((g) => {
          const visible = g.params.filter((p) => isVisible(p, effective));
          if (visible.length === 0) return null;
          return (
            <section key={`${g.name}-${g.advanced}`} className="insp__group">
              {g.name && <h4 className="insp__group-title">{g.name}</h4>}
              {visible.map((p) => (
                <ParamRow
                  key={p.name}
                  param={p}
                  node={node}
                  effective={effective}
                  error={errors.get(p.name)}
                  def={def}
                />
              ))}
            </section>
          );
        })
      )}
    </div>
  );
}

/** 子图本身的说明：名字、提升出来的参数。没选中节点时占着右侧那块地方。 */
function SubgraphInspector({ subgraphId, def }: { subgraphId: string; def: SubgraphDef }) {
  const rename = useGraphStore((s) => s.renameSubgraph);
  const unpromote = useGraphStore((s) => s.unpromoteParam);
  return (
    <div className="insp" data-testid="subgraph-inspector">
      <header className="insp__head">
        <input
          className="insp__title"
          data-testid="subgraph-name"
          value={def.name ?? ""}
          placeholder={subgraphId}
          spellCheck={false}
          onChange={(e) => rename(subgraphId, e.target.value)}
        />
        <div className="insp__meta">
          <code>sub:{subgraphId}</code>
          <span className="tag tag--version">
            {def.nodes.length} 节点 · {def.inputs.length} 入 · {def.outputs.length} 出
          </span>
        </div>
      </header>
      <section className="insp__group">
        <h4 className="insp__group-title">提升的参数</h4>
        {def.params.length === 0 ? (
          <p className="insp__none">还没有提升任何参数。在内部节点的参数上右键即可提升。</p>
        ) : (
          def.params.map((p) => (
            <div className="insp-param" key={p.name} data-testid={`promoted-${p.name}`}>
              <div className="insp-param__label">
                <span>{p.label || p.name}</span>
              </div>
              <div className="insp-param__control">
                <code>{p.binds.map((b) => `${b.node}.${b.param}`).join(", ")}</code>
                <button
                  type="button"
                  className="ctl-btn"
                  data-testid={`unpromote-${p.name}`}
                  onClick={() => unpromote(p.name)}
                >
                  取消提升
                </button>
              </div>
            </div>
          ))
        )}
      </section>
    </div>
  );
}

export function Inspector() {
  const selectedNodes = useUiStore((s) => s.selectedNodes);
  const inspectedOperator = useUiStore((s) => s.inspectedOperator);
  const path = useUiStore((s) => s.path);
  const fullDoc = useGraphStore((s) => s.doc);
  const base = useManifestStore((s) => s.operatorsById);
  const operatorsById = augmentOperators(base, fullDoc.subgraphs);
  const doc = levelOf(fullDoc, path);

  if (selectedNodes.size === 1) {
    const id = [...selectedNodes][0]!;
    const node = doc.nodes.find((n) => n.id === id);
    const op = node ? operatorsById.get(node.op) : undefined;
    if (node && op) return <NodeInspector node={node} op={op} />;
    if (node) {
      return (
        <div className="insp insp--missing">
          <h3>未知算子</h3>
          <code>{node.op}</code>
          <p>当前 core 里没有注册这个算子。可能是打开了用别的版本存的图。</p>
        </div>
      );
    }
  }

  if (selectedNodes.size > 1) {
    return (
      <div className="insp insp--multi">
        <p>已选中 {selectedNodes.size} 个节点</p>
        <p className="insp__hint">批量编辑参数是后续里程碑的事，M1 一次只编辑一个节点。</p>
      </div>
    );
  }

  // 在子图里且没选中节点：显示子图自己的说明与提升出来的参数
  const last = path[path.length - 1];
  const def = last ? fullDoc.subgraphs?.[last.subgraphId] : undefined;
  if (last && def) return <SubgraphInspector subgraphId={last.subgraphId} def={def} />;

  // 没选中节点时，展示面板里高亮的算子说明 —— 加进图之前先看清楚它是什么。
  const op = inspectedOperator ? operatorsById.get(inspectedOperator) : undefined;
  if (op) return <OperatorDetail op={op} />;

  return (
    <div className="insp insp--empty">
      <p>选中一个节点来编辑它的参数。</p>
      <p className="insp__hint">
        双击画布空白处搜索算子；从端口拖出连线来连接节点。
      </p>
    </div>
  );
}
