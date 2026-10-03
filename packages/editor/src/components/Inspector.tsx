// 右侧检查器：选中节点的参数表单。字段、控件、范围、单位、分组、联动条件
// 全部由 manifest 生成（ADR-0003），这个文件里没有任何算子的名字。

import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";

import {
  graphParamSpecOf,
  graphParamValue,
  resolveGraphBinding,
  withBoundValues,
  type GraphBinding,
} from "../lib/graphParams";
import { groupParams, effectiveParams, isEnabled, isVisible, valueEquals } from "../lib/params";
import { readStoredBool, writeStoredBool } from "../lib/prefs";
import { frameKeyOfGroup, pickFrame, roiFramesOf } from "../lib/roiFrames";
import { augmentOperators, describeEventNode, fullId, levelOf, locateEventNode, nodeIndex, promotedBy } from "../lib/subgraph";
import { useExecutionStore, useNodeExecution, useParamErrors } from "../store/execution";
import { currentSubgraph, useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useGraphParamOverrides, useRecipeStore } from "../store/recipe";
import { formatValue } from "../lib/recipes";
import { useUiStore } from "../store/ui";
import { useGraphParamValidation, useNodeValidation, useValidationStore } from "../store/validation";
import type { OutputStat } from "../types/execution";
import type { OperatorDesc, Param } from "../types/manifest";
import type { GraphNode, SubgraphDef } from "../types/graph";

import { CommitText } from "./CommitText";
import { OperatorDetail, PortRow } from "./OperatorDetail";
import { ParamControl } from "./ParamControls";
import { MultiEditContext } from "./NumberInput";
import { copyText } from "../lib/clipboard";
import { formatOutputValue } from "../lib/outputs";

/** 六位有效数字。2D 几何的坐标是米，原样打印会拖一串浮点噪声。 */
// num 搬到了 lib/format（对比的差异表要在纯函数里用）；从这里转出，老的 import 不用改
export { num } from "../lib/format";
// formatOutputValue 搬到了 lib/outputs（节点底栏与运行收尾也要用）
export { formatOutputValue } from "../lib/outputs";

/** 该节点这次运行的输出：能读的值逐个列出；点云只列点数，点一下预览改看它（多个点云输出时，
 *  提取下标的 rest 这类以前在预览里怎么都看不到）。 */
function OutputValues({ outputs, nodeKey }: { outputs: OutputStat[]; nodeKey: string }) {
  const shown = outputs.filter((o) => o.value !== undefined);
  const clouds = outputs.filter((o) => o.type === "PointCloud");
  const pick = useUiStore((s) => s.viewerPortPick.get(nodeKey) ?? null);
  if (shown.length === 0 && clouds.length < 2) return null;
  return (
    <section className="insp__group" data-testid="inspector-outputs">
      <h4 className="insp__group-title">输出</h4>
      {clouds.length > 1 &&
        clouds.map((o) => (
          <button
            type="button"
            className={`insp-cloud${pick === o.port ? " is-picked" : ""}`}
            key={o.port}
            data-testid={`output-cloud-${o.port}`}
            title="在预览里看这个输出"
            onClick={() => useUiStore.getState().setViewerPortPick(nodeKey, o.port)}
          >
            <span className="insp-out__port">{o.port}</span>
            <span className="insp-out__value">{o.elementCount.toLocaleString()} 点</span>
          </button>
        ))}
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
  const doc = useGraphStore((s) => s.doc);
  const nodes = useExecutionStore((s) => s.nodes);
  const names = Object.keys(outputs ?? {});
  if (names.length === 0) return null;

  return (
    <section className="insp__group insp__outputs" data-testid="graph-outputs">
      <h4 className="insp__group-title">图级输出</h4>
      {names.map((name) => {
        const ref = outputs![name]!;
        const stat = nodes.get(ref.node)?.stats?.outputs?.find((o) => o.port === ref.port);
        // 指着的节点不在了（老图、手改过的文件）：存盘、运行都会被拒，标出来、✕ 照样能删
        const missing = locateEventNode(doc, ref.node) === null;
        return (
          <div
            className={`insp-out${missing ? " is-missing" : ""}`}
            data-missing={missing ? "1" : undefined}
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
              {missing ? `节点已不在图里（${ref.node}）` : stat ? formatOutputValue(stat) : `${ref.node}.${ref.port}`}
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

/** 顶层图参数的简表（param-recipe P1）。它不属于任何一个选中节点，所以和图级输出一样钉在上面。
 *  完整的「图参数」分组（改规格、搜索过滤）在参数面板里（P2.3）；面板开着时 Inspector 整个不显示，
 *  面板关着时这张简表留着 —— 不开面板也看得见、改得动、删得掉。 */
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
  const recipe = useRecipeStore((s) => s.current);
  // 图参数自己的诊断（P1.2：nodeId 为空、paramPath 是名字）贴在这一行下
  const diags = useGraphParamValidation(name);
  const gp = doc.params?.[name];
  if (!gp) return null;
  const ops = augmentOperators(base, doc.subgraphs);
  const g = useGraphStore.getState();
  const spec = graphParamSpecOf(doc, name, gp, ops);
  const value = graphParamValue(doc, name, overrides);
  const error = diags.find((d) => d.severity === "error")?.message;
  // 被当前配方覆盖（param-recipe P3.4）：与参数面板同一个橙色标记；恢复 / 写回基础在面板里
  const overridden = recipe !== null && Object.prototype.hasOwnProperty.call(overrides, name);

  return (
    <div
      className={`insp-param${error ? " has-error" : ""}${overridden ? " is-recipe-override" : ""}`}
      data-testid={`graph-param-${name}`}
      data-graph-param={name}
      data-param-error={error ? "1" : undefined}
      data-recipe-override={overridden ? "1" : undefined}
    >
      <div className="insp-param__label insp-gparam__head">
        <span title={gp.doc}>{gp.label || name}</span>
        <span className="insp-gparam__name">{name}</span>
        {overridden && (
          <span className="prow__tag prow__tag--recipe" title={`值来自配方「${recipe}」；基础是后面那个`}>
            配方 · 基础 {formatValue(gp.default, spec ?? undefined)}
          </span>
        )}
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
            previewGraphParam={name}
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

/** 端口小节开着还是收着，所有节点共用一份，记在 localStorage。 */
const PORTS_OPEN_KEY = "lyflow.inspector.portsOpen";

/** 节点的端口小节（M6 §3）：类型、契约、样例。排在参数后面（以前排在前面，gap 类算子在 900 高的窗口里
 *  第一屏常常看不到一个参数）；收起来时只剩一行「端口 2 入 · 1 出」，开合记住。 */
function NodePorts({ op }: { op: OperatorDesc }) {
  const [open, setOpen] = useState(() => readStoredBool(PORTS_OPEN_KEY) ?? true);
  return (
    <details
      className="insp__group insp__ports"
      data-testid="inspector-ports"
      open={open}
      // 读 currentTarget.open，不自己取反：浏览器已经切过了，取反会和它打架
      onToggle={(e) => {
        const next = e.currentTarget.open;
        if (next === open) return;
        setOpen(next);
        writeStoredBool(PORTS_OPEN_KEY, next);
      }}
    >
      <summary className="insp__group-title" data-testid="inspector-ports-toggle">
        {open ? "▾ " : "▸ "}端口
        <span className="insp__group-count">
          {op.inputs.length} 入 · {op.outputs.length} 出
        </span>
      </summary>
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

/** 节点 id，点一下复制：`lyflow run --to`、`--set <节点>.<参数>`、诊断里认的都是它（子图里是路径 id）。 */
function NodeIdChip({ id }: { id: string }) {
  return (
    <button
      type="button"
      className="insp__nodeid"
      data-testid="inspector-node-id"
      title="节点 id，点一下复制：lyflow run --to、--set <节点>.<参数> 与诊断里认的都是它（子图里是路径 id）"
      onClick={() => {
        void copyText(id).then((ok) => {
          useUiStore.getState().showToast(ok ? `已复制节点 id ${id}` : "剪贴板不可用，复制失败", ok ? "info" : "warn");
        });
      }}
    >
      #{id}
    </button>
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
  const operatorsById = useManifestStore((s) => s.operatorsById);
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

  // 高级组默认收起，与参数面板同一规则（param-recipe P2）。组里有参数报错、或抽屉里的诊断指到
  // 组里的参数时自动展开 —— 错误不能藏在收起的组里。换了节点就回到默认。
  const [openAdvanced, setOpenAdvanced] = useState<ReadonlySet<string>>(() => new Set());
  useEffect(() => setOpenAdvanced(new Set()), [node.id]);
  const toggleAdvanced = (name: string) =>
    setOpenAdvanced((cur) => {
      const next = new Set(cur);
      if (next.has(name)) next.delete(name);
      else next.add(name);
      return next;
    });

  const staleVersion = node.opVersion && node.opVersion !== op.version;

  return (
    <div className="insp">
      <header className="insp__head">
        <CommitText
          className="insp__title"
          data-testid="inspector-title"
          value={node.ui?.title ?? ""}
          placeholder={op.label}
          spellCheck={false}
          onCommit={(text) => setNodeUi(node.id, { title: text || null })}
        />
        <div className="insp__meta">
          <code>{op.id}</code>
          <span className="tag tag--version">v{op.version}</span>
          <NodeIdChip id={fullId(path, node.id)} />
        </div>
        {staleVersion && (
          <p className="insp__warn">
            此节点保存于 v{node.opVersion}，当前算子是 v{op.version}。
            默认值或参数含义可能已变更。
          </p>
        )}
        {op.doc && <ClampedDoc key={op.id} text={op.doc} />}
      </header>

      {/* 该节点这次运行的全部诊断（D5）。带 paramPath 的会同时在下面标红框，
          不带的（端口、IO 问题）只有这里看得到，所以一条都不能省。 */}
      {exec && exec.errors.length > 0 && (
        <section className="insp__errors" data-testid="inspector-errors">
          <h4 className="insp__errors-title">
            {exec.state === "cancelled" ? "未执行" : "执行出错"}
          </h4>
          <ul>
            {exec.errors.map((e, i) => {
              // 子图 / 库算子：这一条来自哪个内部节点（事件 id 是路径，ADR-0010）—— 写明、点了打开到它
              const source = exec.errorSources?.[i];
              const where = source ? describeEventNode(doc, operatorsById, source) : null;
              return (
                <li key={i}>
                  <code className="insp__errcode">{e.code}</code>
                  {where?.reveal && (
                    <button
                      type="button"
                      className="insp__errsrc"
                      data-testid="inspector-error-source"
                      title="打开到这个内部节点"
                      onClick={() => {
                        const r = where.reveal!;
                        useUiStore.getState().revealNode(r.path, r.localId, r.exact ? e.paramPath : undefined);
                      }}
                    >
                      {where.names[where.names.length - 1]}
                    </button>
                  )}
                  <span>{e.message}</span>
                </li>
              );
            })}
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

      {exec?.stats?.outputs && <OutputValues outputs={exec.stats.outputs} nodeKey={fullId(path, node.id)} />}

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
          if (!frame && g.advanced) {
            const invalid = visible.some((p) => errors.has(p.name));
            const pointed =
              focused?.nodeId === node.id && visible.some((p) => p.name === focused.paramPath);
            const open = openAdvanced.has(g.name) || invalid || pointed;
            return (
              <section
                key={`${g.name}-${g.advanced}`}
                className="insp__group insp__group--advanced"
                data-testid={`inspector-advanced-${g.name}`}
                data-open={open ? "1" : "0"}
              >
                <button
                  type="button"
                  className={`insp__group-title insp__group-toggle${invalid ? " is-invalid" : ""}`}
                  aria-expanded={open}
                  onClick={() => toggleAdvanced(g.name)}
                  title={invalid ? "组里有参数报错，保持展开" : undefined}
                >
                  {open ? "▾ " : "▸ "}
                  {g.name}
                  {g.name !== "高级" && <span className="insp__group-adv">高级</span>}
                  <span className="insp__group-count">{visible.length}</span>
                </button>
                {open && rows}
              </section>
            );
          }
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

      <NodePorts op={op} />
    </div>
  );
}

/** 算子说明截成两行（整段写着契约、算法细节，以前不截，把参数挤到第一屏外）；放不下才给「展开」。 */
function ClampedDoc({ text }: { text: string }) {
  const ref = useRef<HTMLParagraphElement>(null);
  const [open, setOpen] = useState(false);
  const [overflow, setOverflow] = useState(false);
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el || open) return;
    const measure = () => setOverflow(el.scrollHeight > el.clientHeight + 1);
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(el);
    return () => observer.disconnect();
  }, [text, open]);
  return (
    <>
      <p ref={ref} className={`insp__doc${open ? " is-open" : ""}`} data-testid="inspector-doc">
        {text}
      </p>
      {(overflow || open) && (
        <button type="button" className="insp__doc-more" data-testid="inspector-doc-more" onClick={() => setOpen(!open)}>
          {open ? "收起" : "展开说明"}
        </button>
      )}
    </>
  );
}

/** 子图本身的说明：名字、提升出来的参数。没选中节点时占着右侧那块地方。 */
function SubgraphInspector({ subgraphId, def }: { subgraphId: string; def: SubgraphDef }) {
  const rename = useGraphStore((s) => s.renameSubgraph);
  const unpromote = useGraphStore((s) => s.unpromoteParam);
  return (
    <div className="insp" data-testid="subgraph-inspector">
      <header className="insp__head">
        <CommitText
          className="insp__title"
          data-testid="subgraph-name"
          value={def.name ?? ""}
          placeholder={subgraphId}
          spellCheck={false}
          onCommit={(text) => rename(subgraphId, text)}
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

/** 多选时的检查器：选中的是同一种算子时一起改参数 —— 改一次写进每个节点，一条撤销（setParamMany）。
 *  各节点值相同的参数显示那个值；不同的标「不同」，控件里先放第一个节点的值，改了就统一成新值。
 *  不同算子混选时只列各有几个。 */
function MultiInspector({ ids }: { ids: string[] }) {
  const fullDoc = useGraphStore((s) => s.doc);
  const path = useUiStore((s) => s.path);
  const base = useManifestStore((s) => s.operatorsById);
  const operatorsById = augmentOperators(base, fullDoc.subgraphs);
  const level = levelOf(fullDoc, path);
  // 查表不逐个 find：全选几百个节点再拖动时每帧都重渲，原来是平方级
  const index = nodeIndex(level.nodes);
  const nodes = ids.flatMap((id) => {
    const n = index.get(id);
    return n ? [n] : [];
  });
  const counts = new Map<string, number>();
  for (const n of nodes) counts.set(n.op, (counts.get(n.op) ?? 0) + 1);
  const op = counts.size === 1 && nodes[0] ? operatorsById.get(nodes[0].op) : undefined;
  return (
    <div className="insp insp--multi" data-testid="inspector-multi">
      <header className="insp__head">
        <p className="insp__multi-count">已选中 {ids.length} 个节点</p>
        <ul className="insp__multi-ops">
          {[...counts].map(([opId, n]) => (
            <li key={opId}>
              {counts.size > 1 ? (
                // 混选了几种算子：点一种就把选区收窄成它，一起改参数的表单就出来了
                <button
                  type="button"
                  className="insp__multi-pick"
                  data-testid={`multi-pick-${opId}`}
                  title="只留这一种算子，一起改参数"
                  onClick={() => useUiStore.getState().setSelection(nodes.filter((x) => x.op === opId).map((x) => x.id), [])}
                >
                  {n} × {operatorsById.get(opId)?.label ?? opId}
                </button>
              ) : (
                <>
                  {n} × {operatorsById.get(opId)?.label ?? opId}
                </>
              )}
            </li>
          ))}
        </ul>
      </header>
      {op && op.params.length > 0 ? (
        <MultiParams nodes={nodes} op={op} />
      ) : (
        <p className="insp__hint">
          {op ? "此算子没有参数" : "选中的是同一种算子时，可以在这里一起改参数：点上面的一种，只留它。"}
        </p>
      )}
    </div>
  );
}

function MultiParams({ nodes, op }: { nodes: GraphNode[]; op: OperatorDesc }) {
  const doc = useGraphStore((s) => s.doc);
  const path = useUiStore((s) => s.path);
  const overrides = useGraphParamOverrides();
  const def = currentSubgraph(doc, path);
  const names = useMemo(() => op.params.map((p) => p.name), [op.params]);
  // 每个节点各自的有效值（被图参数绑定的取图参数的值，与单个节点的检查器同一份）
  const each = useMemo(
    () => nodes.map((n) => effectiveParams(op, withBoundValues(doc, path, n, names, overrides))),
    [nodes, op, doc, path, names, overrides],
  );
  const groups = useMemo(() => groupParams(op.params), [op.params]);
  const ids = nodes.map((n) => n.id);
  return (
    <>
      {groups.map((g) => {
        // 联动条件按各节点自己的值判：有一个节点上看得见就列出来
        const visible = g.params.filter((p) => each.some((e) => isVisible(p, e)));
        if (visible.length === 0) return null;
        return (
          <section key={`${g.name}-${g.advanced}`} className="insp__group">
            {(g.name || g.advanced) && (
              <h4 className="insp__group-title">
                {g.name || "高级"}
                {g.advanced && g.name !== "高级" && <span className="insp__group-adv">高级</span>}
              </h4>
            )}
            {visible.map((p) => {
              const values = each.map((e) => e[p.name]);
              // 已提升的内参只读、联动条件在哪个节点上不满足就禁用 —— 与单个节点的检查器同一规则
              const locked = nodes.some(
                (n) => promotedBy(def, n.id, p.name) !== undefined && !resolveGraphBinding(doc, path, n.id, p.name),
              );
              return (
                <MultiParamRow
                  key={p.name}
                  param={p}
                  ids={ids}
                  values={values}
                  disabled={locked || !each.every((e) => isEnabled(p, e))}
                />
              );
            })}
          </section>
        );
      })}
    </>
  );
}

function MultiParamRow({
  param,
  ids,
  values,
  disabled,
}: {
  param: Param;
  ids: string[];
  values: unknown[];
  disabled: boolean;
}) {
  const setParamMany = useGraphStore((s) => s.setParamMany);
  const first = values[0];
  const same = values.every((v) => valueEquals(v, first));
  return (
    <div
      className={`insp-param${disabled ? " is-disabled" : ""}`}
      data-testid={`multi-param-${param.name}`}
      data-mixed={same ? undefined : "1"}
    >
      <div className="insp-param__label">
        <span title={param.doc}>{param.label || param.name}</span>
        {!same && (
          <span className="insp-param__mixed" title="选中的节点里这个参数的值不一样；改了就统一成新值">
            不同
          </span>
        )}
      </div>
      <div className="insp-param__control">
        <MultiEditContext.Provider value={true}>
          <ParamControl
            param={param}
            value={first}
            disabled={disabled}
            onChange={(v) => {
              if (!same || !valueEquals(v, first)) setParamMany(ids, param.name, v);
            }}
            // 相对改法（*2、+=5）每个节点按自己的值改，不是都改成第一个的
            onChangeEach={(update) => setParamMany(ids, param.name, (cur: unknown) => update(cur))}
          />
        </MultiEditContext.Provider>
      </div>
    </div>
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

  if (selectedNodes.size > 1) return <MultiInspector ids={[...selectedNodes]} />;

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
