export { LyFlowEditor, type LyFlowEditorProps } from "./LyFlowEditor";

export * from "./transport";
export { browserDialogs, NoDialogError, type EditorDialogs } from "./lib/dialogs";

export type * from "./types/graph";
export type * from "./types/manifest";
export type * from "./types/execution";
export { isMigration, decodeCloud, CLOUD_MAGIC } from "./types/execution";
export { decodeTensor, decodeIndices, TENSOR_MAGIC, INDICES_MAGIC } from "./types/execution";
export {
  GRAPH_SCHEMA_VERSION,
  LIBRARY_OP_PREFIX,
  SUBGRAPH_OP_PREFIX,
  subgraphIdOf,
} from "./types/graph";

// 宿主要自己搭工具栏或验收桥时用得到的内部件。语义化动作永远走 store。
export { useGraphStore, currentSubgraph, emptyDoc } from "./store/graph";
export { useUiStore } from "./store/ui";
export { usePeekStore, type PeekWindow, type PeekView } from "./store/peek";
export { peekSourceOf, type PeekSource } from "./lib/peekSource";
export { useManifestStore, missingOperators, useOperator } from "./store/manifest";
export {
  useExecutionStore,
  onNodeTransition,
  startRun,
  cancelCurrentRun,
  subscribeExecutionEvents,
  setRunSceneId,
  type RunRequest,
  type StateTransition,
} from "./store/execution";
export { useCacheStore, requestPlan, schedulePlan, refreshCacheStats, formatBytes } from "./store/cache";
export { levelOf, pathPrefix, fullId } from "./lib/subgraph";
export { layoutGraph, needsInitialLayout } from "./lib/layout";
