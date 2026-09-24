// 右侧检查器：选中节点的参数表单。字段、控件、范围、单位、分组、联动条件
// 全部由 manifest 生成（ADR-0003），这个文件里没有任何算子的名字。

import { useEffect, useMemo, useState } from "react";

import {
  graphParamValue,
  resolveGraphBinding,
  splitBind,
  withBoundValues,
  type GraphBinding,
} from "../lib/graphParams";
import { groupParams, effectiveParams, isEnabled, isVisible, valueEquals } from "../lib/params";
import { frameKeyOfGroup, pickFrame, roiFramesOf } from "../lib/roiFrames";
import { augmentOperators, levelOf, promotedBy } from "../lib/subgraph";
import { useExecutionStore, useNodeExecution, useParamErrors } from "../store/execution";
import { currentSubgraph, useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useGraphParamOverrides } from "../store/recipe";
import { useUiStore } from "../store/ui";
import { useGraphParamValidation, useNodeValidation, useValidationStore } from "../store/validation";
import type { OutputStat, OutputValue } from "../types/execution";
import type { OperatorDesc, Param } from "../types/manifest";
import type { GraphDoc, GraphNode, GraphParam, SubgraphDef } from "../types/graph";

import { OperatorDetail, PortRow } from "./OperatorDetail";
import { ParamControl } from "./ParamControls";

/** 六位有效数字。2D 几何的坐标是米，原样打印会拖一串浮点噪声。 */
export function num(v: number | undefined): string {
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

/** 图级命名输出（ADR-0017）。宿主按名字取值，所以这张表必须看得见、删得掉。 */
function GraphOutputs() {
  const outputs = useGraphStore((s) => s.doc.outputs);
  const remove = useGraphStore((s) => s.removeGraphOutput);
  const nodes = useExecutionStore((s) => s.nodes);
  const names = Object.keys(outputs ?? {});
  if (names.length === 0) return null;

  return (
    <section className="insp__group insp__outputs" data-testid="graph-outputs">
      <h4 className="insp__group-title">图级输出</h4>
      {names.map((name) => {
        const ref = outputs![name]!;
        const stat = nodes.get(ref.node)?.stats?.outputs?.find((o) => o.port === ref.port);
        return (
          <div
            className="insp-out"
            key={name}
            data-testid={`graph-output-${name}`}
            data-node={ref.node}
            data-port={ref.port}
            data-type={stat?.type}
          >
            <span className="insp-out__port" title={`${ref.node}.${ref.port}`}>
              {name}
            </span>
            <span className="insp-out__value">
              {stat ? formatOutputValue(stat) : `${ref.node}.${ref.port}`}
            </span>
            <button
              type="button"
              className="ctl-btn"
              data-testid={`remove-output-${name}`}
              title="取消这个图级输出"
              onClick={() => remove(name)}
            >
              ✕
            </button>
          </div>
        );
      })}
    </section>
  );
}

/** 一个图参数用什么控件画。给了 type 的用它自己的规格（P1.1）；老格式没有 type，
 *  就借第一个被绑定目标的声明（规格本来就是它），label 换成图参数自己的。 */
function graphParamControlSpec(
  doc: GraphDoc,
  name: string,
  gp: GraphParam,
  ops: ReadonlyMap<string, OperatorDesc>,
): Param | null {
  if (gp.type) {
    const { binds: _binds, ...spec } = gp;
    return { ...spec, name, type: gp.type } as Param;
  }
  for (const bind of gp.binds) {
    const t = splitBind(bind);
    const node = t ? doc.nodes.find((n) => n.id === t.node) : undefined;
    const decl = t && node ? ops.get(node.op)?.params.find((p) => p.name === t.param) : undefined;
    if (decl) return { ...decl, name, label: gp.label ?? decl.label ?? name, default: gp.default };
  }
  return null;
}

/** 顶层图参数的简表（param-recipe P1）。它不属于任何一个选中节点，所以和图级输出一样钉在上面。
 *  完整的「图参数」分组（改规格、搜索过滤）是 P2 的参数面板；这里只让它看得见、改得动、删得掉。 */
/** 多于这么多个图参数时简表默认收起：导入器生成的图可能带一长串，不该把选中节点的表单挤到屏幕外。 */
const GRAPH_PARAMS_OPEN_MAX = 4;

function GraphParams() {
  const doc = useGraphStore((s) => s.doc);
  const invalid = useValidationStore((s) => s.graphLevel.filter((d) => d.severity === "error").length);
  const names = Object.keys(doc.params ?? {});
  if (names.length === 0) return null;
  return (
    <details
      className="insp__group insp__graph-params"
      data-testid="graph-params"
      open={names.length <= GRAPH_PARAMS_OPEN_MAX}
    >
      <summary className={`insp__group-title${invalid > 0 ? " is-invalid" : ""}`}>
        图参数 {names.length}
        {invalid > 0 && ` · ${invalid} 处有错`}
      </summary>
      {names.map((name) => (
        <GraphParamRow key={name} name={name} />
      ))}
    </details>
  );
}

function GraphParamRow({ name }: { name: string }) {
  const doc = useGraphStore((s) => s.doc);
  const base = useManifestStore((s) => s.operatorsById);
  const overrides = useGraphParamOverrides();
  // 图参数自己的诊断（P1.2：nodeId 为空、paramPath 是名字）贴在这一行下
  const diags = useGraphParamValidation(name);
  const gp = doc.params?.[name];
  if (!gp) return null;
  const ops = augmentOperators(base, doc.subgraphs);
  const g = useGraphStore.getState();
  const spec = graphParamControlSpec(doc, name, gp, ops);
  const value = graphParamValue(doc, name, overrides);
  const error = diags.find((d) => d.severity === "error")?.message;

  return (
    <div
      className={`insp-param${error ? " has-error" : ""}`}
      data-testid={`graph-param-${name}`}
      data-graph-param={name}
      data-param-error={error ? "1" : undefined}
    >
      <div className="insp-param__label insp-gparam__head">
        <span title={gp.doc}>{gp.label || name}</span>
        <span className="insp-gparam__name">{name}</span>
        <button
          type="button"
          className="ctl-btn"
          data-testid={`remove-graph-param-${name}`}
          title="删除这个图参数：当前值写回它绑定的每一个参数，行为不变"
          onClick={() => g.removeGraphParam(name)}
        >
          ✕
        </button>
      </div>
      <div className="insp-param__control">
        {spec ? (
          <ParamControl
            param={spec}
            value={value}
            disabled={false}
            onChange={(v) => {
              if (!valueEquals(v, value)) useGraphStore.getState().editGraphParamValue(name, v);
            }}
          />
        ) : (
          <code>{JSON.stringify(value)}</code>
        )}
        {error && <p className="insp-param__error">{error}</p>}
        <div className="insp-gparam__binds">
          {gp.binds.length === 0 && <span>（没有绑定任何参数）</span>}
          {gp.binds.map((b) => (
            <span className="insp-gparam__bind" key={b} data-bind={b}>
              {b}
              <button
                type="button"
                className="ctl-btn"
                data-testid={`unbind-graph-param-${name}-${b}`}
                title="解除这一条绑定：当前值写回这个参数，行为不变"
                onClick={() => g.unbindFromGraphParam(name, b)}
              >
                ✕
              </button>
            </span>
          ))}
        </div>
      </div>
    </div>
  );
}

/** 节点的端口小节（M6 §3）：类型、契约、样例。折叠成 <details>，默认展开 ——
 *  没声明契约的算子照样列出来，type 和 doc 本来就有用，但收起来时不占地方。 */
function NodePorts({ op }: { op: OperatorDesc }) {
  return (
    <details className="insp__group insp__ports" data-testid="inspector-ports" open>
      <summary className="insp__group-title">端口</summary>
      <div className="insp__ports-body">
        <div>
          <h5 className="insp__ports-label">输入</h5>
          {op.inputs.length === 0 ? (
            <p className="insp__none">无（源节点）</p>
          ) : (
            <ul className="port-list">
              {op.inputs.map((p) => (
                <PortRow key={p.name} port={p} isInput compact />
              ))}
            </ul>
          )}
        </div>
        <div>
          <h5 className="insp__ports-label">输出</h5>
          {op.outputs.length === 0 ? (
            <p className="insp__none">无（终端节点）</p>
          ) : (
            <ul className="port-list">
              {op.outputs.map((p) => (
                <PortRow key={p.name} port={p} isInput={false} compact />
              ))}
            </ul>
          )}
        </div>
      </div>
    </details>
  );
}

function ParamRow({
  param,
  node,
  effective,
  error,
  def,
  binding,
}: {
  param: Param;
  node: GraphNode;
  effective: Record<string, unknown>;
  error?: string | undefined;
  def?: SubgraphDef | undefined;
  /** 这个参数最终由哪个图参数提供（P1.4）。给了就显示图参数的有效值，编辑路由到图参数。 */
  binding: GraphBinding | null;
}) {
  const setParam = useGraphStore((s) => s.setParam);
  const value = effective[param.name];
  // 已提升的内参在内部只读：真正的值来自外层表单，两处都能改就没人知道谁赢（F4）。
  // 例外是整条链一直通到图参数的（纳入配方）：那时「外层」就是图参数，改这一行 = 改它，
  // 与顶层被绑定的行同一个语义（setParam 在 store 里路由），没有第二个写入处
  const promoted = promotedBy(def, node.id, param.name);
  const disabled = !isEnabled(param, effective) || (promoted !== undefined && !binding);
  // 稀疏存储的直接可视化：params 里有这个键 = 用户改过它。被图参数提供的行看值本身。
  const overridden = binding
    ? !valueEquals(value, param.default)
    : node.params?.[param.name] !== undefined;

  return (
    <div
      className={`insp-param${disabled ? " is-disabled" : ""}${error ? " has-error" : ""}`}
      data-testid={`param-${param.name}`}
      data-param-error={error ? "1" : undefined}
      data-promoted={promoted ? promoted.name : undefined}
      data-graph-param={binding ? binding.graphParam : undefined}
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
        {binding && (
          <span
            className="insp-param__graph"
            data-testid={`param-graph-${param.name}`}
            title={`值来自顶层图参数 ${binding.graphParam}；在这一行改的是它（选着「基础」时改它的默认值）`}
          >
            由图参数 {binding.graphParam} 提供
          </span>
        )}
        {overridden && (!promoted || binding) && (
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
          graphBinding={binding}
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
  const runErrors = useParamErrors(node.id);
  // 编辑期的校验诊断（m8-plan L16）与上次运行的错误一起标到参数上；同一个参数两边都有时
  // 取校验的那一条 —— 它是对着当前的值说的，运行的那条可能已经过时了。
  const validation = useNodeValidation(node.id);
  const errors = useMemo(() => {
    const merged = new Map(runErrors);
    for (const d of validation) {
      if (d.severity === "error" && d.paramPath) merged.set(d.paramPath, d.message);
    }
    return merged;
  }, [runErrors, validation]);
  const exec = useNodeExecution(node.id);
  const doc = useGraphStore((s) => s.doc);
  const path = useUiStore((s) => s.path);
  const overrides = useGraphParamOverrides();
  const def = currentSubgraph(doc, path);
  // 被图参数绑定的参数显示图参数的有效值（P1.4）：显示、联动条件、2D 拖框的分组都看这一份
  const shown = useMemo(
    () => withBoundValues(doc, path, node, op.params.map((p) => p.name), overrides),
    [doc, path, node, op.params, overrides],
  );
  const effective = effectiveParams(op, shown);
  const groups = useMemo(() => groupParams(op.params), [op.params]);

  // 一组框一节（m8-plan L20）：带底图的 roi 参数分属几节时（locate_template 的四个模板槽），
  // 这几节是手风琴 —— 2D 视图切换条选中的那一组所在的一节展开，其余收起；展开另一节也就切了
  // 视图里的组。没启用的槽那一节照样能展开（去勾 Enabled），视图这时仍画第一组。
  const frameOfGroup = useMemo(() => groups.map((g) => frameKeyOfGroup(g.params)), [groups]);
  const accordion = frameOfGroup.filter(Boolean).length >= 2;
  const selectedFrame = useUiStore((s) => s.roiFrame[node.id]);
  const setRoiFrame = useUiStore((s) => s.setRoiFrame);
  const openFrame = selectedFrame ?? pickFrame(roiFramesOf(op, shown), undefined)?.key ?? null;
  // 手动收起的那一节（再点一下标题）。换了组就作废。
  const [collapsed, setCollapsed] = useState<string | null>(null);
  useEffect(() => setCollapsed(null), [openFrame]);
  // 抽屉里点了指向某个槽参数的诊断：切到那个槽，参数行才看得见
  const focused = useUiStore((s) => s.focusedDiagnostic);
  useEffect(() => {
    if (!accordion || focused?.nodeId !== node.id || !focused.paramPath) return;
    const i = groups.findIndex((g) => g.params.some((p) => p.name === focused.paramPath));
    const key = i >= 0 ? frameOfGroup[i] : null;
    if (key) setRoiFrame(node.id, key);
  }, [accordion, focused, node.id, groups, frameOfGroup, setRoiFrame]);

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

      {validation.length > 0 && (
        <section className="insp__errors insp__errors--validate" data-testid="inspector-validation">
          <h4 className="insp__errors-title">校验</h4>
          <ul>
            {validation.map((d, i) => (
              <li key={i} data-severity={d.severity} data-param={d.paramPath ?? undefined}>
                <code className="insp__errcode">{d.code}</code>
                <span>{d.message}</span>
              </li>
            ))}
          </ul>
        </section>
      )}

      {exec?.stats?.outputs && <OutputValues outputs={exec.stats.outputs} />}

      <NodePorts op={op} />

      {op.params.length === 0 ? (
        <p className="insp__none">此算子没有参数</p>
      ) : (
        groups.map((g, gi) => {
          const visible = g.params.filter((p) => isVisible(p, effective));
          if (visible.length === 0) return null;
          const rows = visible.map((p) => (
            <ParamRow
              key={p.name}
              param={p}
              node={node}
              effective={effective}
              error={errors.get(p.name)}
              def={def}
              binding={resolveGraphBinding(doc, path, node.id, p.name)}
            />
          ));
          const frame = accordion ? frameOfGroup[gi] : null;
          if (!frame) {
            return (
              <section key={`${g.name}-${g.advanced}`} className="insp__group">
                {g.name && <h4 className="insp__group-title">{g.name}</h4>}
                {rows}
              </section>
            );
          }
          const open = frame === openFrame && collapsed !== frame;
          const invalid = visible.some((p) => errors.has(p.name));
          return (
            <details
              key={`${g.name}-${g.advanced}`}
              className="insp__group insp__group--frame"
              data-testid={`inspector-frame-${g.name}`}
              data-frame={frame}
              data-open={open ? "1" : "0"}
              open={open}
            >
              <summary
                className={`insp__group-title${invalid ? " is-invalid" : ""}`}
                onClick={(e) => {
                  // 开合由 roiFrame 决定，不让 <details> 自己切
                  e.preventDefault();
                  if (open) {
                    setCollapsed(frame);
                  } else {
                    setCollapsed(null);
                    setRoiFrame(node.id, frame);
                  }
                }}
              >
                {open ? "▾ " : "▸ "}
                {g.name}
              </summary>
              {rows}
            </details>
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
  return (
    <>
      <GraphOutputs />
      <GraphParams />
      <InspectorBody />
    </>
  );
}

function InspectorBody() {
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
