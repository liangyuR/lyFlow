import { create } from 'zustand';

import { newDocId } from '../lib/ids';
import { useModalStore } from '../lib/modal';
import { dropAutoRun } from '../lib/preview';
import { resolveUnsaved } from '../lib/unsaved';
import { viewportHandle, type Viewport } from '../lib/viewportHandle';
import { invalidatePlanRequests, schedulePlan, useCacheStore } from './cache';
import { useCompareStore } from './compare';
import {
  blockExecution,
  executionBusy,
  invalidateExecutionRequests,
  judgedNodesOf,
  restoreExecutionState,
  useExecutionStore,
  type NodeExecution,
} from './execution';
import { useGraphStore } from './graph';
import { usePeekStore } from './peek';
import { resetRecipes, useRecipeStore } from './recipe';
import { invalidateRecipeLoad } from './recipeFiles';
import { useRunHistoryStore } from './runHistory';
import { useUiStore } from './ui';
import { invalidateValidationRequests, scheduleValidate, useValidationStore } from './validation';
import type { GraphDoc } from '../types/graph';

type Data<T> = Pick<
  T,
  { [K in keyof T]: T[K] extends (...args: never[]) => unknown ? never : K }[keyof T]
>;
type GraphState = ReturnType<typeof useGraphStore.getState>;
type UiState = ReturnType<typeof useUiStore.getState>;
export type FlowRecipeSnapshot = ReturnType<typeof useRecipeStore.getState>;
type DocumentUi = Pick<
  UiState,
  | 'path'
  | 'selectedNodes'
  | 'selectedEdges'
  | 'inspectedOperator'
  | 'pinnedNode'
  | 'viewerContentPick'
  | 'viewerPortPick'
  | 'roiFrame'
>;

export interface FlowSnapshot {
  historyTopologyLocked: boolean;
  graph: Pick<
    GraphState,
    | 'doc'
    | 'filePath'
    | 'dirty'
    | 'savedDoc'
    | 'past'
    | 'future'
    | 'pendingSnapshot'
    | 'pendingRecipes'
    | 'lastRejection'
  >;
  recipe: FlowRecipeSnapshot;
  ui: DocumentUi;
  execution: Data<ReturnType<typeof useExecutionStore.getState>>;
  judgedNodes: ReadonlyMap<string, NodeExecution>;
  cache: Pick<ReturnType<typeof useCacheStore.getState>, 'plan' | 'ranWith' | 'unavailable'>;
  history: Data<ReturnType<typeof useRunHistoryStore.getState>>;
  validation: Data<ReturnType<typeof useValidationStore.getState>>;
  peek: Pick<ReturnType<typeof usePeekStore.getState>, 'windows' | 'topZ'>;
  compare: Pick<ReturnType<typeof useCompareStore.getState>, 'on' | 'b' | 'snapshot'>;
  viewport: Viewport | null;
}

export interface FlowTab {
  id: string;
  title: string;
  filePath: string | null;
  dirty: boolean;
}

interface FlowTabsState {
  tabs: readonly FlowTab[];
  activeId: string;
  switching: boolean;
}

export const useFlowTabsStore = create<FlowTabsState>(() => ({
  tabs: [],
  activeId: '',
  switching: false,
}));
const snapshots = new Map<string, FlowSnapshot>();
let restoring = false;
let locked = false;
let hosts = 0;
let unsubscribe: (() => void) | null = null;
let viewportTicket = 0;

function dataOnly<T extends object>(state: T): Data<T> {
  return Object.fromEntries(
    Object.entries(state).filter(([, value]) => typeof value !== 'function'),
  ) as Data<T>;
}

function documentUi(state: UiState): DocumentUi {
  const {
    path,
    selectedNodes,
    selectedEdges,
    inspectedOperator,
    pinnedNode,
    viewerContentPick,
    viewerPortPick,
    roiFrame,
  } = state;
  return {
    path,
    selectedNodes,
    selectedEdges,
    inspectedOperator,
    pinnedNode,
    viewerContentPick,
    viewerPortPick,
    roiFrame,
  };
}

function capture(): FlowSnapshot {
  const {
    doc,
    filePath,
    dirty,
    savedDoc,
    past,
    future,
    pendingSnapshot,
    pendingRecipes,
    lastRejection,
    topologyLocked,
  } = useGraphStore.getState();
  const { plan, ranWith, unavailable } = useCacheStore.getState();
  const { windows, topZ } = usePeekStore.getState();
  const { on, b, snapshot } = useCompareStore.getState();
  return {
    historyTopologyLocked: topologyLocked,
    graph: {
      doc,
      filePath,
      dirty,
      savedDoc,
      past,
      future,
      pendingSnapshot,
      pendingRecipes,
      lastRejection,
    },
    recipe: useRecipeStore.getState(),
    ui: documentUi(useUiStore.getState()),
    execution: dataOnly(useExecutionStore.getState()),
    judgedNodes: judgedNodesOf(useExecutionStore.getState()),
    cache: { plan, ranWith, unavailable },
    history: dataOnly(useRunHistoryStore.getState()),
    validation: dataOnly(useValidationStore.getState()),
    peek: { windows, topZ },
    compare: { on, b, snapshot },
    viewport: viewportHandle()?.get() ?? null,
  };
}

function meta(id: string, snapshot: FlowSnapshot): FlowTab {
  const { doc, filePath, dirty } = snapshot.graph;
  return {
    id,
    title: doc.name?.trim() || '未命名',
    filePath,
    dirty: dirty || snapshot.recipe.set !== snapshot.recipe.saved,
  };
}

function syncActive(): void {
  if (restoring) return;
  const state = useFlowTabsStore.getState();
  if (!state.activeId) return;
  const next = meta(state.activeId, capture());
  const old = state.tabs.find((tab) => tab.id === state.activeId);
  if (old && old.title === next.title && old.filePath === next.filePath && old.dirty === next.dirty)
    return;
  useFlowTabsStore.setState({ tabs: state.tabs.map((tab) => (tab.id === next.id ? next : tab)) });
}

function ensureSession(): void {
  if (useFlowTabsStore.getState().activeId) return;
  const id = newDocId();
  const snapshot = capture();
  snapshots.set(id, snapshot);
  useFlowTabsStore.setState({ activeId: id, tabs: [meta(id, snapshot)] });
}

export function startFlowTabs(): () => void {
  ensureSession();
  hosts += 1;
  if (!unsubscribe) {
    const offGraph = useGraphStore.subscribe(syncActive);
    const offRecipe = useRecipeStore.subscribe(syncActive);
    unsubscribe = () => {
      offGraph();
      offRecipe();
    };
    syncActive();
  }
  let stopped = false;
  return () => {
    if (stopped) return;
    stopped = true;
    hosts -= 1;
    if (hosts === 0) {
      unsubscribe?.();
      unsubscribe = null;
    }
  };
}

export function isRestoringFlow(): boolean {
  return restoring;
}

function refuse(message: string): false {
  useUiStore.getState().showToast(message, 'warn');
  return false;
}

export function canSwitchFlow(): boolean {
  if (locked) return refuse('正在处理工作流，请稍后再切换');
  if (executionBusy()) return refuse('工作流正在运行，请停止运行后再切换');
  if (useRecipeStore.getState().status === 'loading') return refuse('正在读取配方，请稍后再切换');
  if (useUiStore.getState().previewing) return refuse('请先结束参数拖动，再切换工作流');
  if (useModalStore.getState().current) return refuse('请先处理当前对话框');
  return true;
}

function acquire(idle: boolean): boolean {
  ensureSession();
  if (idle ? !canSwitchFlow() : locked)
    return idle ? false : refuse('正在处理工作流，请稍后再操作');
  locked = true;
  blockExecution(true);
  useFlowTabsStore.setState({ switching: true });
  return true;
}

function release(): void {
  restoring = false;
  locked = false;
  blockExecution(false);
  useFlowTabsStore.setState({ switching: false });
  syncActive();
}

export async function withFlowOperation<T>(work: () => Promise<T>): Promise<T | undefined> {
  if (!acquire(false)) return undefined;
  try {
    return await work();
  } finally {
    release();
  }
}

function invalidate(): void {
  viewportTicket += 1;
  dropAutoRun();
  invalidateExecutionRequests();
  invalidatePlanRequests();
  invalidateValidationRequests();
  invalidateRecipeLoad();
}

function stash(): void {
  if (typeof document !== 'undefined') {
    const active = document.activeElement;
    if (active instanceof HTMLElement && active.closest('[data-lyflow-editor]')) active.blur();
  }
  const graph = useGraphStore.getState();
  if (graph.pendingSnapshot) graph.commit();
  dropAutoRun();
  const id = useFlowTabsStore.getState().activeId;
  snapshots.set(id, capture());
  syncActive();
}

function resetTransient(): void {
  useUiStore.setState({
    previewing: false,
    pendingFrom: null,
    compatiblePorts: new Set(),
    replacePorts: new Set(),
    hoverNodeId: null,
    hoverEdge: null,
    hoverPaused: false,
    searchPopup: null,
    finderOpen: false,
    finderSeed: '',
    renamingNode: null,
    focusedDiagnostic: null,
    revealRequest: null,
    autoHint: null,
    viewerMeasuring: false,
    viewerMaximized: false,
  });
}

function refresh(): void {
  const graph = useGraphStore.getState();
  schedulePlan(graph.doc, graph.filePath);
  scheduleValidate(graph.doc, graph.filePath);
}

function restore(id: string): void {
  const snapshot = snapshots.get(id);
  if (!snapshot) return;
  restoring = true;
  try {
    invalidate();
    useExecutionStore.getState().reset();
    useRunHistoryStore.getState().clear();
    resetTransient();
    useRecipeStore.setState({ ...snapshot.recipe, switchedBy: 'auto' });
    const graph = useGraphStore.getState();
    const history =
      graph.topologyLocked && !snapshot.historyTopologyLocked
        ? { past: [], future: [], pendingSnapshot: null, pendingRecipes: null }
        : {};
    useGraphStore.setState({ ...snapshot.graph, ...history, epoch: graph.epoch + 1 });
    useUiStore.setState(snapshot.ui);
    useCacheStore.setState(snapshot.cache);
    restoreExecutionState(
      { ...snapshot.execution, orphans: [], cancelling: false },
      snapshot.judgedNodes,
    );
    useRunHistoryStore.setState(snapshot.history);
    useValidationStore.setState(snapshot.validation);
    usePeekStore.setState(snapshot.peek);
    useCompareStore.setState(snapshot.compare);
    useFlowTabsStore.setState({ activeId: id });
    const ticket = viewportTicket;
    const viewport = snapshot.viewport ?? { x: 0, y: 0, zoom: 1 };
    viewportHandle()?.set(viewport);
    setTimeout(() => {
      if (ticket === viewportTicket) viewportHandle()?.set(viewport);
    }, 0);
  } finally {
    restoring = false;
  }
  refresh();
}

export interface PreparedFlowTab {
  commit(): void;
  cancel(): void;
}

export function prepareNewTab(options: { reuseEmpty?: boolean } = {}): PreparedFlowTab | null {
  if (!acquire(true)) return null;
  stash();
  const previous = useFlowTabsStore.getState().activeId;
  const before = snapshots.get(previous)!;
  const reusable =
    options.reuseEmpty &&
    !before.graph.filePath &&
    !before.graph.dirty &&
    before.graph.doc.nodes.length === 0 &&
    before.recipe.set.recipes.length === 0 &&
    before.recipe.set === before.recipe.saved;
  const id = reusable ? previous : newDocId();
  restoring = true;
  invalidate();
  resetRecipes(null, 'none');
  resetTransient();
  useExecutionStore.getState().reset();
  useRunHistoryStore.getState().clear();
  useCacheStore.getState().reset();
  useValidationStore.getState().reset();
  usePeekStore.getState().closeAll();
  useCompareStore.getState().exit();
  useUiStore.setState(documentUi(useUiStore.getInitialState()));
  viewportHandle()?.set({ x: 0, y: 0, zoom: 1 });
  let settled = false;
  return {
    commit() {
      if (settled) return;
      settled = true;
      const snapshot = capture();
      snapshots.set(id, snapshot);
      const state = useFlowTabsStore.getState();
      const tab = meta(id, snapshot);
      useFlowTabsStore.setState({
        activeId: id,
        tabs: reusable
          ? state.tabs.map((old) => (old.id === id ? tab : old))
          : [...state.tabs, tab],
      });
      release();
      refresh();
    },
    cancel() {
      if (settled) return;
      settled = true;
      try {
        restore(previous);
      } finally {
        release();
      }
    },
  };
}

export async function switchTab(id: string): Promise<boolean> {
  ensureSession();
  if (!useFlowTabsStore.getState().tabs.some((tab) => tab.id === id)) return false;
  if (id === useFlowTabsStore.getState().activeId) return true;
  if (!acquire(true)) return false;
  try {
    stash();
    restore(id);
    return true;
  } finally {
    release();
  }
}

export function findFlowByPath(path: string): string | null {
  ensureSession();
  const key = (value: string) =>
    /^[a-z]:[\\/]|^\\\\|^\/\//i.test(value) ? value.replaceAll('\\', '/').toLowerCase() : value;
  return (
    useFlowTabsStore
      .getState()
      .tabs.find((tab) => tab.filePath !== null && key(tab.filePath) === key(path))?.id ?? null
  );
}

export function flowsDirty(): boolean {
  ensureSession();
  syncActive();
  return useFlowTabsStore.getState().tabs.some((tab) => tab.dirty);
}

export async function closeTab(id: string): Promise<boolean> {
  ensureSession();
  const initial = useFlowTabsStore.getState();
  if (!initial.tabs.some((tab) => tab.id === id)) return false;
  if (!acquire(true)) return false;
  const previous = initial.activeId;
  try {
    stash();
    if (id !== previous) restore(id);
    const accepted = await resolveUnsaved('关闭工作流');
    stash();
    if (!accepted) {
      if (id !== previous) restore(previous);
      return false;
    }
    const state = useFlowTabsStore.getState();
    const index = state.tabs.findIndex((tab) => tab.id === id);
    const remaining = state.tabs.filter((tab) => tab.id !== id);
    snapshots.delete(id);
    if (remaining.length > 0) {
      useFlowTabsStore.setState({ tabs: remaining });
      restore(id !== previous ? previous : remaining[Math.min(index, remaining.length - 1)]!.id);
    } else {
      restoring = true;
      invalidate();
      useGraphStore.getState().newDoc();
      resetRecipes(null, 'none');
      resetTransient();
      useUiStore.setState(documentUi(useUiStore.getInitialState()));
      useExecutionStore.getState().reset();
      useRunHistoryStore.getState().clear();
      useCacheStore.getState().reset();
      useValidationStore.getState().reset();
      usePeekStore.getState().closeAll();
      useCompareStore.getState().exit();
      viewportHandle()?.set({ x: 0, y: 0, zoom: 1 });
      const next = newDocId();
      const snapshot = capture();
      snapshots.set(next, snapshot);
      useFlowTabsStore.setState({ activeId: next, tabs: [meta(next, snapshot)] });
      restoring = false;
      refresh();
    }
    return true;
  } catch (error) {
    stash();
    if (snapshots.has(previous) && useFlowTabsStore.getState().activeId !== previous)
      restore(previous);
    useUiStore.getState().showToast(error instanceof Error ? error.message : String(error), 'warn');
    return false;
  } finally {
    release();
  }
}

export async function resolveAllUnsaved(action = '关闭窗口'): Promise<boolean> {
  ensureSession();
  if (!acquire(true)) return false;
  const previous = useFlowTabsStore.getState().activeId;
  try {
    stash();
    for (const tab of useFlowTabsStore.getState().tabs) {
      if (!tab.dirty) continue;
      if (useFlowTabsStore.getState().activeId !== tab.id) restore(tab.id);
      const accepted = await resolveUnsaved(`${action}（${tab.title}）`);
      stash();
      if (!accepted) return false;
    }
    return true;
  } finally {
    if (useFlowTabsStore.getState().activeId !== previous) restore(previous);
    release();
  }
}

export interface FlowWorkspaceSnapshot {
  activeId: string;
  tabs: { id: string; title: string; snapshot: FlowSnapshot }[];
}

export function getFlowSnapshots(): FlowWorkspaceSnapshot {
  ensureSession();
  if (!locked) {
    snapshots.set(useFlowTabsStore.getState().activeId, capture());
    syncActive();
  }
  const state = useFlowTabsStore.getState();
  return {
    activeId: state.activeId,
    tabs: state.tabs.map((tab) => ({
      id: tab.id,
      title: tab.title,
      snapshot: snapshots.get(tab.id)!,
    })),
  };
}

export function restoreFlowSnapshots(payload: FlowWorkspaceSnapshot): boolean {
  if (
    payload.tabs.length === 0 ||
    !payload.tabs.some((tab) => tab.id === payload.activeId) ||
    !acquire(true)
  )
    return false;
  try {
    snapshots.clear();
    for (const tab of payload.tabs) snapshots.set(tab.id, tab.snapshot);
    useFlowTabsStore.setState({ tabs: payload.tabs.map((tab) => meta(tab.id, tab.snapshot)) });
    restore(payload.activeId);
    return true;
  } finally {
    release();
  }
}

export interface FlowDocument {
  id: string;
  title?: string;
  doc: GraphDoc;
  filePath: string | null;
  dirty: boolean;
  savedDoc?: GraphDoc | null;
  recipe?: Partial<FlowRecipeSnapshot>;
  recipeDirty?: boolean;
}

export function restoreFlowDocuments(payload: { activeId: string; tabs: FlowDocument[] }): boolean {
  const defaults = capture();
  const tabs = payload.tabs.map((tab) => {
    const recipe = {
      ...useRecipeStore.getInitialState(),
      ...tab.recipe,
      switchedBy: 'auto' as const,
    };
    const current =
      recipe.set.recipes.find((entry) => entry.id === recipe.currentId) ??
      recipe.set.recipes.find((entry) => entry.name === recipe.current);
    recipe.current = current?.name ?? null;
    recipe.currentId = current?.id ?? null;
    recipe.overrides = current?.values ?? {};
    recipe.status = recipe.dir ? 'ready' : 'none';
    if (!tab.recipeDirty) recipe.saved = recipe.set;
    else if (recipe.saved === recipe.set) recipe.saved = { ...recipe.saved };
    const snapshot: FlowSnapshot = {
      ...defaults,
      graph: {
        doc: tab.doc,
        filePath: tab.filePath,
        dirty: tab.dirty,
        savedDoc: tab.dirty ? (tab.savedDoc ?? null) : tab.doc,
        past: [],
        future: [],
        pendingSnapshot: null,
        pendingRecipes: null,
        lastRejection: null,
      },
      recipe,
      ui: documentUi(useUiStore.getInitialState()),
      execution: dataOnly(useExecutionStore.getInitialState()),
      judgedNodes: new Map(),
      cache: { plan: new Map(), ranWith: new Map(), unavailable: false },
      history: dataOnly(useRunHistoryStore.getInitialState()),
      validation: dataOnly(useValidationStore.getInitialState()),
      peek: { windows: [], topZ: 0 },
      compare: { on: false, b: null, snapshot: null },
      viewport: null,
    };
    return { id: tab.id, title: tab.title ?? tab.doc.name ?? '未命名', snapshot };
  });
  return restoreFlowSnapshots({ activeId: payload.activeId, tabs });
}
