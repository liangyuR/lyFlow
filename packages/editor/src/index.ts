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
export {
  useManifestStore,
  missingOperators,
  useOperator,
  allSnippets,
  findSnippet,
} from "./store/manifest";
export { useValidationStore, requestValidate, scheduleValidate } from "./store/validation";
export {
  useRecipeStore,
  runParamsOf,
  currentOverrides,
  selectRecipe,
  recipeSet,
  recipesDirty,
  useRecipesDirty,
} from "./store/recipe";
export {
  loadRecipesFor,
  recipesLoaded,
  importRecipeFrom,
  exportRecipeTo,
  writeRecipeAutosave,
  restoreRecipeAutosave,
} from "./store/recipeFiles";
// 配方（param-recipe P3）的纯函数：文件格式、目录约定、规格摘要、四类失配。P4 的 CLI 在 Rust 里照同一份规则再实现
export {
  recipeDirOf,
  recipeFileName,
  specDigest,
  specCanonical,
  recipeReport,
  serializeRecipe,
  parseRecipeText,
  type RecipeEntry,
  type RecipeSet,
  type RecipeReport,
  type Mismatch,
  type MismatchKind,
} from "./lib/recipes";
export {
  effectiveGraphValues,
  graphParamBoundTo,
  joinBind,
  resolveGraphBinding,
  splitBind,
  type GraphBinding,
} from "./lib/graphParams";
export { planAutoConnect, type AutoConnectPlan } from "./lib/autoconnect";
export { addNodeWithAutoConnect, insertSnippet, insertSnippetById } from "./lib/insert";
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
