import { useEffect, useState } from 'react';
import { transport } from '../transport';
import { useExecutionStore } from '../store/execution';
import { resultBindingsChanged } from '../lib/resultBindings';
import type { OutputInfo } from '../types/execution';
import { useRuntimeCapabilities } from './useRuntimeCapabilities';

export interface RelatedOutput extends OutputInfo {
  nodeId: string;
}

export function useResultBindings(runId: string | null, enabled = true) {
  const [state, setState] = useState<{
    runId: string | null;
    outputs: RelatedOutput[];
    error: string;
  }>({ runId: null, outputs: [], error: '' });
  const [revision, setRevision] = useState(0);
  const capabilities = useRuntimeCapabilities();
  useEffect(() => {
    if (!enabled || !runId) return;
    return useExecutionStore.subscribe((current, previous) => {
      if (resultBindingsChanged(runId, current, previous)) setRevision((value) => value + 1);
    });
  }, [runId, enabled]);
  useEffect(() => {
    if (!enabled || !runId || !transport.getRunBindings || capabilities.resultBindings !== true) {
      setState({ runId, outputs: [], error: '' });
      return;
    }
    let cancelled = false;
    void (async () => {
      try {
        const bindings = await transport.getRunBindings!(runId);
        if (cancelled) return;
        if (bindings.entries.length > 16_384) throw new Error('可视化结果端口数量超过 16384');
        if (
          bindings.entries
            .filter((entry) => entry.type === 'Record')
            .reduce((sum, entry) => sum + entry.byteSize, 0) > 16_777_216
        )
          throw new Error('关联可视化元数据超过 16 MiB 预算');
        const nodes = [
          ...new Set(
            bindings.entries
              .filter(
                (entry) =>
                  entry.recordType || ['Indices', 'Tensor', 'PointCloud'].includes(entry.type),
              )
              .map((entry) => entry.nodeId),
          ),
        ];
        const outputs: RelatedOutput[] = [];
        if (nodes.length > 256) throw new Error('关联可视化节点超过 256 个预算');
        for (let i = 0; i < nodes.length; i += 8) {
          const batch = await Promise.all(
            nodes.slice(i, i + 8).map(async (nodeId) =>
              (await transport.getOutputInfo(runId, nodeId)).map((output) => ({
                ...output,
                nodeId,
              })),
            ),
          );
          if (cancelled) return;
          outputs.push(...batch.flat());
        }
        if (!cancelled) setState({ runId, outputs, error: '' });
      } catch (error) {
        if (!cancelled) setState({ runId, outputs: [], error: String(error) });
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [runId, enabled, capabilities.resultBindings, revision]);
  return state.runId === runId ? state : { runId, outputs: [], error: '' };
}
