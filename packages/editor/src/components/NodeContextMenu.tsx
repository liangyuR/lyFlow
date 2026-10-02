// 节点的右键菜单（交互清单 P1 #24 #25 #27 + P2 #31）。开、收、Esc / 点外面收起在 GraphCanvas，
// 这里是菜单本身：每一项与它要的派生值（作用于谁、能不能「仅此节点」、哪些端口能标成图级输出）。

import { useReactFlow } from "@xyflow/react";
import { useCallback, useMemo } from "react";

import { keyHint } from "../lib/keymap";
import { layoutGraph } from "../lib/layout";
import { useMotionEnabled, viewportMs, withLayoutTransition } from "../lib/motion";
import {
  closureOf,
  isolateUnavailableTitle,
  nodeRunAvailability,
  runNodeOnly,
  runNodeSmart,
} from "../lib/nodeRun";
import { fullId } from "../lib/subgraph";
import { evictNodeCache } from "../store/cache";
import { useCompareStore } from "../store/compare";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";
import { transport } from "../transport";
import { subgraphIdOf, type GraphDoc } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

export interface ContextMenuState {
  nodeId: string;
  x: number;
  y: number;
  /** 右键时顺手改了选区的话，改之前的选区。「设为对比基准」要还原它：预览的 A 跟随选中，
   *  右键把 B 设好却把 A 也换成了同一个节点，两栏就比了个寂寞。 */
  before: { nodes: string[]; edges: string[] } | null;
}

export function NodeContextMenu({
  menu,
  view,
  operatorsById,
  measured,
  onClose,
  onRunToNode,
  onEnterSubgraph,
  onSaveLibrary,
}: {
  menu: ContextMenuState;
  /** 当前这一层（GraphCanvas 的 view）：选中上下游、整理布局都在这一层里算。 */
  view: GraphDoc;
  operatorsById: ReadonlyMap<string, OperatorDesc>;
  measured: ReadonlyMap<string, { width: number; height: number }>;
  onClose: () => void;
  onRunToNode: (nodeId: string) => void;
  onEnterSubgraph: (nodeId: string) => void;
  onSaveLibrary: (subgraphId: string) => void;
}) {
  const path = useUiStore((s) => s.path);
  const running = useExecutionStore((s) => s.runStatus === "running");
  const declaredOutputs = useGraphStore((s) => s.doc.outputs);
  const { fitView } = useReactFlow();
  const motionOn = useMotionEnabled();

  /** 菜单的动作作用于谁：有选区就是选区（右键时没选中的那个已经被选上了），否则就是右键的那个。 */
  const menuTargets = useCallback((): string[] => {
    const ui = useUiStore.getState();
    return ui.selectedNodes.size > 0 ? [...ui.selectedNodes] : [menu.nodeId];
  }, [menu]);

  const selectClosure = useCallback(
    (dir: "up" | "down") => {
      const ids = closureOf(view, menuTargets(), dir);
      const ui = useUiStore.getState();
      ui.setSelection(ids, []);
      ui.showToast(`已选中${dir === "up" ? "上游" : "下游"}（含自己）${ids.length} 个节点`);
      onClose();
    },
    [view, menuTargets, onClose],
  );

  const menuNode = view.nodes.find((n) => n.id === menu.nodeId);
  // 「仅此节点」的可用性（修订一 V6：上游不齐时置灰、写明缺谁）：菜单打开时判一次
  const menuRunOnly = nodeRunAvailability(menu.nodeId);
  const menuSubgraphId = menuNode ? subgraphIdOf(menuNode.op) : null;
  const menuIsLibrary = menuNode?.op.startsWith("lib.") === true;

  // 图级输出（ADR-0017）：outputs 里存的是**展开后**的路径 id，
  // 所以在子图里标输出也说得清是哪一个端口。
  const menuOutputs = useMemo(() => {
    if (!menuNode) return [];
    const op = operatorsById.get(menuNode.op);
    if (!op) return [];
    const declared = declaredOutputs ?? {};
    const full = fullId(path, menuNode.id);
    return op.outputs.map((port) => {
      const hit = Object.entries(declared).find(([, o]) => o.node === full && o.port === port.name);
      return { port: port.name, name: hit?.[0] };
    });
  }, [menuNode, operatorsById, declaredOutputs, path]);

  const doCompose = useCallback(() => {
    const ids = menuTargets();
    const result = useGraphStore.getState().composeSubgraph(ids);
    if (result) {
      useUiStore.getState().setSelection([result.nodeId], []);
      useUiStore.getState().showToast(`已合成子图（${ids.length} 个节点）`);
    }
    onClose();
  }, [menuTargets, onClose]);

  const doDissolve = useCallback(() => {
    const inlined = useGraphStore.getState().dissolveSubgraph(menu.nodeId);
    if (inlined.length > 0) {
      useUiStore.getState().setSelection(inlined, []);
      useUiStore.getState().showToast(`已解散，内联了 ${inlined.length} 个节点`);
    } else {
      useUiStore.getState().showToast("这个节点不是子图", "warn");
    }
    onClose();
  }, [menu, onClose]);

  return (
    <div
      className="ctxmenu"
      style={{ left: menu.x, top: menu.y }}
      data-testid="node-context-menu"
      onClick={(e) => e.stopPropagation()}
    >
      <button
        type="button"
        data-testid="run-to-node"
        title={running ? "取消正在进行的运行，改跑到此节点（与 Shift+F5、节点上的运行按钮一样是抢占）" : undefined}
        onClick={() => {
          onRunToNode(menu.nodeId);
          onClose();
        }}
      >
        运行到此节点 <kbd>{keyHint("runToNode")}</kbd>
      </button>
      {/* 与标题栏按钮同一组动作（修订一 V6）：运行到此 = 单击，强制重算 = Shift+单击 */}
      <button
        type="button"
        data-testid="ctx-force-node"
        title="跳过缓存真跑一遍此节点；上游照常只补缺的（= Shift+点击运行按钮）"
        onClick={() => {
          void runNodeSmart(menu.nodeId, true);
          onClose();
        }}
      >
        强制重算此节点
      </button>
      <button
        type="button"
        data-testid="ctx-run-node-only"
        disabled={!menuRunOnly?.available}
        data-run-reason={menuRunOnly?.missing.join(",") || undefined}
        title={
          menuRunOnly?.available
            ? "只跑此节点，上游一律用已有结果（不补跑）"
            : menuRunOnly && menuRunOnly.missing.length > 0
              ? isolateUnavailableTitle(menuRunOnly.names)
              : undefined
        }
        onClick={() => {
          void runNodeOnly(menu.nodeId);
          onClose();
        }}
      >
        仅此节点（用现有上游）
      </button>
      <button
        type="button"
        data-testid="ctx-evict-node"
        disabled={running}
        title="把此节点与它全部下游的缓存结果删掉：下次运行这些节点要重算，内存也放出来（正在运行时不能清）"
        onClick={() => {
          const g = useGraphStore.getState();
          void evictNodeCache(g.doc, g.filePath, fullId(path, menu.nodeId))
            .then((r) =>
              useUiStore
                .getState()
                .showToast(`清掉 ${r.removed} 条缓存（${r.nodes.length} 个节点）`),
            )
            .catch((e: unknown) =>
              useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn"),
            );
          onClose();
        }}
      >
        清除此节点及下游的缓存
      </button>
      <button
        type="button"
        data-testid="ctx-compare-b"
        title="把这个节点设为对比的基准（B），跟随它的最新结果；A 照旧跟随选中。已在对比就换 B"
        onClick={() => {
          useCompareStore.getState().setB({ path, nodeId: menu.nodeId });
          // A 留在右键之前看着的那个节点上
          if (menu.before) useUiStore.getState().setSelection(menu.before.nodes, menu.before.edges);
          onClose();
        }}
      >
        设为对比基准（B）
      </button>
      <button
        type="button"
        data-testid="ctx-select-upstream"
        title="从这个节点（或选中的这些）沿连线往上游走，把整条链都选上"
        onClick={() => selectClosure("up")}
      >
        选中上游
      </button>
      <button
        type="button"
        data-testid="ctx-select-downstream"
        title="从这个节点（或选中的这些）沿连线往下游走，把整条链都选上"
        onClick={() => selectClosure("down")}
      >
        选中下游
      </button>
      <button
        type="button"
        data-testid="ctx-compose"
        onClick={doCompose}
      >
        合成子图 <kbd>{keyHint("compose")}</kbd>
      </button>
      {menuSubgraphId && (
        <>
          <button
            type="button"
            data-testid="ctx-enter"
            onClick={() => {
              onEnterSubgraph(menu.nodeId);
              onClose();
            }}
          >
            进入子图
          </button>
          <button type="button" data-testid="ctx-dissolve" onClick={doDissolve}>
            解散子图 <kbd>{keyHint("dissolve")}</kbd>
          </button>
          <button
            type="button"
            data-testid="ctx-save-library"
            onClick={() => {
              onSaveLibrary(menuSubgraphId);
              onClose();
            }}
          >
            保存到库…
          </button>
        </>
      )}
      {menuIsLibrary && (
        <button
          type="button"
          data-testid="ctx-inline-library"
          title="把库算子的定义拷进这张图、换成可编辑的子图；之后与库文件脱钩，改库文件不影响这张图"
          onClick={() => {
            const nodeId = menu.nodeId;
            const opId = menuNode?.op ?? "";
            onClose();
            void transport
              .getLibraryDefinition(opId)
              .then((def) => {
                const ui = useUiStore.getState();
                if (!def) {
                  ui.showToast(`取不到 ${opId} 的定义：库文件可能已经删了，刷新库后再试`, "warn");
                  return;
                }
                if (!useGraphStore.getState().inlineLibrary(nodeId, def)) return;
                ui.setSelection([nodeId], []);
                ui.showToast("已展开为子图，双击进入编辑；库文件不受影响");
              })
              .catch((e: unknown) =>
                useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn"),
              );
          }}
        >
          展开为内联子图
        </button>
      )}
      {menuOutputs.map((o) => (
        <button
          key={o.port}
          type="button"
          data-testid={`ctx-mark-output-${o.port}`}
          data-marked={o.name ? "1" : "0"}
          onClick={() => {
            const graph = useGraphStore.getState();
            if (o.name) {
              graph.removeGraphOutput(o.name);
              useUiStore.getState().showToast(`已取消图级输出 ${o.name}`);
            } else {
              const name = graph.markGraphOutput({
                node: fullId(path, menu.nodeId),
                port: o.port,
              });
              useUiStore.getState().showToast(`已标为图级输出 ${name}`);
            }
            onClose();
          }}
        >
          {o.name ? `取消输出 ${o.name}` : `标为输出：${o.port}`}
        </button>
      ))}
      <button
        type="button"
        data-testid="ctx-mute"
        onClick={() => {
          const ids = menuTargets();
          useGraphStore.getState().setBypass(ids, !(menuNode?.bypass === true));
          onClose();
        }}
      >
        {menuNode?.bypass ? "取消静音" : "静音"} <kbd>{keyHint("mute")}</kbd>
      </button>
      <button
        type="button"
        data-testid="ctx-collapse"
        onClick={() => {
          const ids = menuTargets();
          useGraphStore.getState().setCollapsed(ids, !(menuNode?.ui?.collapsed === true));
          onClose();
        }}
      >
        {menuNode?.ui?.collapsed ? "展开" : "折叠"} <kbd>{keyHint("collapse")}</kbd>
      </button>
      <button
        type="button"
        data-testid="ctx-layout"
        onClick={() => {
          const ids = new Set(menuTargets());
          const moves = layoutGraph(view, {
            only: ids,
            measured,
          });
          // 用户触发的整理才过渡（N4），见 lib/motion.ts 的 withLayoutTransition
          withLayoutTransition(() => useGraphStore.getState().applyLayout(moves));
          onClose();
        }}
      >
        整理选中的布局 <kbd>{keyHint("layout")}</kbd>
      </button>
      <button
        type="button"
        data-testid="ctx-fit-selection"
        onClick={() => {
          // 与 F 键同一档：只对准一个时不放大到糊满屏幕
          void fitView({
            nodes: menuTargets().map((id) => ({ id })),
            duration: viewportMs(motionOn),
            maxZoom: 1,
            padding: 0.4,
          });
          onClose();
        }}
      >
        对准选中的节点 <kbd>{keyHint("fitSelection")}</kbd>
      </button>
      <button
        type="button"
        data-testid="ctx-fit"
        onClick={() => {
          void fitView({ duration: viewportMs(motionOn) });
          onClose();
        }}
      >
        适配视图 <kbd>{keyHint("fitView")}</kbd>
      </button>
    </div>
  );
}
