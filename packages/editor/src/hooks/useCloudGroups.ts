import { useEffect, useMemo, useState } from 'react';
import { cloudGroupPreview, RESULT_LIMITS } from '@lyflow/visualization';
import { transport } from '../transport';
import { decodeCloud, decodeIndices, type CloudPayload } from '../types/execution';
import { useResultBindings } from './useResultBindings';
import { useRuntimeCapabilities } from './useRuntimeCapabilities';

export function useCloudGroups(
  runId: string | null,
  cloud: CloudPayload | null,
  maxPoints: number,
  enabled: boolean,
) {
  const related = useResultBindings(runId, enabled);
  const capabilities = useRuntimeCapabilities();
  const groupOutputs = related.outputs.filter((output) => output.value?.type === 'cloud.Groups.v1');
  const [pick, setPick] = useState('');
  const [selectedId, setSelectedId] = useState('');
  const [indicesPick, setIndicesPick] = useState('');
  const [loaded, setLoaded] = useState<{
    key: string;
    cloud: CloudPayload;
    indices: ReturnType<typeof decodeIndices>;
  } | null>(null);
  const [error, setError] = useState('');
  const group = groupOutputs.find((output) => `${output.nodeId}:${output.port}` === pick);
  const cap = Math.min(maxPoints || RESULT_LIMITS.points, RESULT_LIMITS.points);
  const indexOutputs = related.outputs.filter(
    (output) => output.nodeId === group?.nodeId && output.type === 'Indices',
  );
  const key = runId && group ? `${runId}:${pick}:${indicesPick}:${cap}` : '';
  useEffect(() => {
    setPick('');
    setSelectedId('');
    setLoaded(null);
  }, [runId]);
  useEffect(() => {
    setError('');
    if (!key || !runId || !group) return;
    let cancelled = false;
    void (async () => {
      try {
        if (capabilities.cloudSampling !== 'ordered-stride-v1')
          throw new Error('宿主未声明固定有序抽样，无法可靠映射分组索引');
        const native = related.outputs.find(
          (output) =>
            output.type === 'PointCloud' &&
            output.sourceCloudId === group.value!.data!.sourceCloudId,
        );
        const candidates = indexOutputs.filter(
          (output) => indexOutputs.length === 1 || output.port === indicesPick,
        );
        if (!native || candidates.length === 0)
          throw new Error('分组的原始点云或配套 Indices 不可用');
        const cloudValue =
          cloud &&
          cloud.pointCount <= cap &&
          native.sourceCloudId &&
          cloud.sourceCloudId === native.sourceCloudId
            ? cloud
            : decodeCloud(await transport.getOutputCloud(runId, native.nodeId, native.port, cap));
        if (cloudValue.pointCount > RESULT_LIMITS.points) throw new Error('显示点预算超过 200 万');
        const source = {
          ...cloudValue,
          sourceCloudId: native.sourceCloudId!,
          sampleStride:
            cloudValue.sampleStride ?? Math.max(1, Math.ceil(cloudValue.totalPoints / cap)),
        };
        const metadata = group.value?.data?.groups;
        if (
          !Array.isArray(metadata) ||
          metadata.length > 4096 ||
          metadata.some((entry) => !entry || !Number.isSafeInteger(entry.count) || entry.count < 0)
        )
          throw new Error('分组元数据或成员计数非法');
        const total = (metadata as { count: number }[]).reduce(
          (sum, entry) => sum + entry.count,
          0,
        );
        if (total > RESULT_LIMITS.memberships) throw new Error('分组索引预算超过 16777216');
        let match: ReturnType<typeof decodeIndices> | null = null;
        for (const candidate of candidates) {
          if (candidate.elementCount !== total) continue;
          const indices = decodeIndices(
            await transport.getOutputIndices(runId, candidate.nodeId, candidate.port, 0, total),
          );
          if (indices.count !== indices.total || indices.total !== total) continue;
          try {
            cloudGroupPreview(group.value, indices, source);
          } catch {
            continue;
          }
          if (match) throw new Error('存在多个可匹配 Indices，请通过独立节点明确配套端口');
          match = indices;
        }
        if (!match) throw new Error('没有来源与范围一致的完整分组索引');
        if (!cancelled) setLoaded({ key, cloud: source, indices: match });
      } catch (error) {
        if (!cancelled) setError(String(error));
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [key, cloud, related.outputs, group, runId, maxPoints, capabilities.cloudSampling]);
  const preview = useMemo(() => {
    if (!group || loaded?.key !== key) return null;
    try {
      return cloudGroupPreview(group.value, loaded.indices, loaded.cloud, {
        selectedId: selectedId || null,
      });
    } catch {
      return null;
    }
  }, [group, loaded, key, selectedId]);
  const displayed = useMemo(() => {
    if (!enabled || !preview || loaded?.key !== key) return cloud;
    const original = loaded.cloud;
    const selected = new Uint32Array(preview.visible.reduce((sum, visible) => sum + visible, 0));
    let target = 0;
    for (let index = 0; index < preview.visible.length; index++)
      if (preview.visible[index]) selected[target++] = index;
    const select = <T extends Float32Array | Uint8Array>(
      array: T | null,
      count: number,
    ): T | null => {
      if (!array) return null;
      const output = new (array.constructor as { new (length: number): T })(
        selected.length * count,
      );
      selected.forEach((source, target) =>
        output.set(array.subarray(source * count, (source + 1) * count), target * count),
      );
      return output;
    };
    const xyz = select(original.xyz, 3)!;
    const bounds = new Float32Array([
      Infinity,
      Infinity,
      Infinity,
      -Infinity,
      -Infinity,
      -Infinity,
    ]);
    for (let i = 0; i < selected.length; i++)
      for (let axis = 0; axis < 3; axis++) {
        const n = xyz[i * 3 + axis]!;
        if (Number.isFinite(n)) {
          bounds[axis] = Math.min(bounds[axis]!, n);
          bounds[axis + 3] = Math.max(bounds[axis + 3]!, n);
        }
      }
    for (let axis = 0; axis < 6; axis++) if (!Number.isFinite(bounds[axis])) bounds[axis] = 0;
    return {
      ...original,
      pointCount: selected.length,
      xyz,
      bounds: selected.length ? bounds : new Float32Array(6),
      rgb: select(preview.colors, 3),
      intensity: select(original.intensity, 1),
      normals: select(original.normals, 3),
    };
  }, [enabled, preview, loaded, key, cloud]);
  return {
    cloud: displayed,
    preview,
    error: error || related.error,
    groupOutputs,
    indexOutputs,
    indicesPick,
    setIndicesPick,
    pick,
    setPick: (value: string) => {
      setPick(value);
      setSelectedId('');
      setIndicesPick('');
    },
    selectedId,
    setSelectedId,
  };
}
