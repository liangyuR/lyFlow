//
// 右侧检查器：选中节点的参数表单。
//
// 表单完全由 manifest 生成 —— 字段、控件、范围、单位、分组、联动条件全部来自
// C++ 侧（ADR-0003）。这个文件里没有任何算子的名字。
//

import { groupParams, effectiveParams, isEnabled, isVisible, valueEquals } from "../lib/params";
import { useNodeExecution, useParamErrors } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { OperatorDesc, Param } from "../types/manifest";
import type { GraphNode } from "../types/graph";

import { OperatorDetail } from "./OperatorDetail";
import { ParamControl } from "./ParamControls";

function ParamRow({
  param,
  node,
  effective,
  error,
}: {
  param: Param;
  node: GraphNode;
  effective: Record<string, unknown>;
  error?: string | undefined;
}) {
  const setParam = useGraphStore((s) => s.setParam);
  const value = effective[param.name];
  const disabled = !isEnabled(param, effective);
  // 稀疏存储的直接可视化：params 里有这个键 = 用户改过它。
  const overridden = node.params?.[param.name] !== undefined;

  return (
    <div
      className={`insp-param${disabled ? " is-disabled" : ""}${error ? " has-error" : ""}`}
      data-testid={`param-${param.name}`}
      data-param-error={error ? "1" : undefined}
    >
      <div className="insp-param__label">
        <span className={overridden ? "is-overridden" : ""} title={param.doc}>
          {param.label || param.name}
        </span>
        {overridden && (
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
                />
              ))}
            </section>
          );
        })
      )}
    </div>
  );
}

export function Inspector() {
  const selectedNodes = useUiStore((s) => s.selectedNodes);
  const inspectedOperator = useUiStore((s) => s.inspectedOperator);
  const doc = useGraphStore((s) => s.doc);
  const operatorsById = useManifestStore((s) => s.operatorsById);

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
