import type { NodeStats, NodeState } from '../types/execution';

interface ResultSnapshot {
  runId: string | null;
  resultRunId: string | null;
  runStatus: string;
  nodes: ReadonlyMap<string, { state: NodeState; stats?: NodeStats | undefined }>;
}

export function resultBindingsChanged(
  runId: string | null,
  current: ResultSnapshot,
  previous: ResultSnapshot,
): boolean {
  if (!runId || current.resultRunId !== runId) return false;
  if (previous.resultRunId !== runId) return true;
  if (
    current.runId === runId &&
    (current.runId !== previous.runId || current.runStatus !== previous.runStatus)
  )
    return true;
  if (current.nodes === previous.nodes) return false;
  for (const [id, node] of current.nodes) {
    if (
      (node.state !== 'done' && node.state !== 'skipped') ||
      node.stats?.outputsAvailable === false
    )
      continue;
    const before = previous.nodes.get(id);
    if (before?.state !== node.state || before.stats !== node.stats) return true;
  }
  return false;
}
