import { isRestoringFlow, useFlowTabsStore } from '../store/flowTabs';
import { useGraphStore } from '../store/graph';
import { recipesDirty, useRecipeStore } from '../store/recipe';
import type { GraphDoc } from '../types/graph';

export function subscribeDocChanges(onChange: (doc: GraphDoc, dirty: boolean) => void): () => void {
  let lastDoc = useGraphStore.getState().doc;
  let lastDirty = useGraphStore.getState().dirty || recipesDirty();
  let lastTab = useFlowTabsStore.getState().activeId;
  const notify = () => {
    const tabs = useFlowTabsStore.getState();
    if (isRestoringFlow() || tabs.switching) return;
    const graph = useGraphStore.getState();
    const dirty = graph.dirty || recipesDirty();
    if (graph.doc === lastDoc && dirty === lastDirty && tabs.activeId === lastTab) return;
    lastDoc = graph.doc;
    lastDirty = dirty;
    lastTab = tabs.activeId;
    onChange(graph.doc, dirty);
  };
  const stops = [
    useGraphStore.subscribe(notify),
    useRecipeStore.subscribe(notify),
    useFlowTabsStore.subscribe(notify),
  ];
  return () => stops.forEach((stop) => stop());
}

export function watchInitialWorkspace(): { unchanged(): boolean; stop(): void } {
  const initial = useFlowTabsStore.getState();
  const initialDoc = useGraphStore.getState().doc;
  const blank = () => {
    const graph = useGraphStore.getState();
    const recipe = useRecipeStore.getState();
    const tabs = useFlowTabsStore.getState();
    return (
      tabs.tabs.length === 1 &&
      tabs.activeId === initial.activeId &&
      tabs.tabs[0]?.id === initial.activeId &&
      !tabs.switching &&
      graph.doc === initialDoc &&
      !graph.filePath &&
      !graph.dirty &&
      graph.doc.nodes.length === 0 &&
      recipe.set === recipe.saved &&
      recipe.set.recipes.length === 0 &&
      recipe.current === null
    );
  };
  let changed = !blank();
  const stops = [
    useGraphStore.subscribe(() => {
      changed ||= !blank();
    }),
    useRecipeStore.subscribe(() => {
      changed ||= !blank();
    }),
    useFlowTabsStore.subscribe(() => {
      changed ||= !blank();
    }),
  ];
  return {
    unchanged: () => !changed && blank(),
    stop: () => {
      changed = true;
      stops.forEach((stop) => stop());
    },
  };
}
