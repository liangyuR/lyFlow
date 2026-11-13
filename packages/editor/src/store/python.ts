import { create } from 'zustand';

import {
  defaultPythonConfig,
  normalizePythonConfig,
  pythonConfigProblem,
  unsupportedPythonSettings,
} from '../lib/pythonConfig';
import {
  activeTransport,
  type PythonRefresh,
  type PythonRuntimeConfig,
  type PythonSettings,
  type Transport,
} from '../transport';
import { useManifestStore } from './manifest';
import { useUiStore } from './ui';
import { useGraphStore } from './graph';
import { schedulePlan } from './cache';
import { scheduleValidate } from './validation';

interface PythonState {
  settings: PythonSettings | null;
  draft: PythonRuntimeConfig;
  dirty: boolean;
  loading: boolean;
  busy: boolean;
  error: string | null;
  openRequest: number;
  open(): void;
  edit(change: Partial<PythonRuntimeConfig>): void;
  load(): Promise<void>;
  save(): Promise<boolean>;
  refresh(): Promise<boolean>;
}

let owner: Transport | null = null;
let generation = 0;
const messageOf = (error: unknown) => (error instanceof Error ? error.message : String(error));

function applyRefresh(result: PythonRefresh): void {
  useManifestStore.getState().replaceBundle(result.manifest, 0);
  const ui = useUiStore.getState();
  if (
    ui.inspectedOperator &&
    !useManifestStore.getState().operatorsById.has(ui.inspectedOperator)
  ) {
    ui.setInspectedOperator(null);
  }
  const graph = useGraphStore.getState();
  void schedulePlan(graph.doc, graph.filePath);
  scheduleValidate(graph.doc, graph.filePath);
}

export const usePythonStore = create<PythonState>((set, get) => ({
  settings: null,
  draft: defaultPythonConfig(),
  dirty: false,
  loading: false,
  busy: false,
  error: null,
  openRequest: 0,
  open: () => set((state) => ({ openRequest: state.openRequest + 1 })),
  edit: (change) => {
    if (get().busy || get().loading || !get().settings?.editable) return;
    set((state) => ({ draft: { ...state.draft, ...change }, dirty: true, error: null }));
  },
  load: async () => {
    const host = activeTransport();
    if (host === owner && (get().busy || get().loading)) return;
    const changed = owner !== host;
    owner = host;
    const request = ++generation;
    set({
      loading: true,
      busy: false,
      error: null,
      ...(changed ? { settings: null, draft: defaultPythonConfig(), dirty: false } : {}),
    });
    try {
      const settings = host.getPythonSettings
        ? await host.getPythonSettings()
        : unsupportedPythonSettings();
      if (request !== generation || activeTransport() !== host) return;
      set({ settings, ...(!get().dirty ? { draft: structuredClone(settings.config) } : {}) });
    } catch (error) {
      if (request === generation && activeTransport() === host) {
        set({ settings: unsupportedPythonSettings(), error: messageOf(error) });
      }
    } finally {
      if (request === generation) set({ loading: false });
    }
  },
  save: () => changeRuntime('save'),
  refresh: () => changeRuntime('refresh'),
}));

async function changeRuntime(action: 'save' | 'refresh'): Promise<boolean> {
  const state = usePythonStore.getState();
  const host = activeTransport();
  if (
    state.busy ||
    state.loading ||
    !state.settings?.supported ||
    !state.settings.editable ||
    owner !== host
  )
    return false;
  const config = normalizePythonConfig(state.draft);
  const problem =
    action === 'save'
      ? pythonConfigProblem(config)
      : state.dirty
        ? '配置已修改，请先保存并检查环境。'
        : null;
  if (problem) {
    usePythonStore.setState({ error: problem });
    return false;
  }
  const operation = action === 'save' ? host.setPythonSettings : host.refreshPythonNodes;
  if (!operation) {
    usePythonStore.setState({ error: '当前宿主没有提供此 Python 管理操作。' });
    return false;
  }
  const request = ++generation;
  usePythonStore.setState({ busy: true, error: null });
  try {
    const result =
      action === 'save' ? await host.setPythonSettings!(config) : await host.refreshPythonNodes!();
    if (request !== generation || activeTransport() !== host) return false;
    applyRefresh(result);
    const saved = {
      executable: result.status.executable,
      packagePaths: [...result.status.packagePaths],
      timeoutMs: result.status.timeoutMs,
    };
    usePythonStore.setState({
      settings: {
        supported: true,
        editable: state.settings.editable,
        config: saved,
        status: result.status,
      },
      draft: structuredClone(saved),
      dirty: false,
    });
    return true;
  } catch (error) {
    if (request === generation && activeTransport() === host)
      usePythonStore.setState({ error: messageOf(error) });
    return false;
  } finally {
    if (request === generation) usePythonStore.setState({ busy: false });
  }
}
