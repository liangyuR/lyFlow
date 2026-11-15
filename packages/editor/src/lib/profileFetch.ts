import type { Transport } from '../transport/types';
import {
  decodeCloud,
  decodeProfile,
  profileToCloudPayload,
  type CloudPayload,
} from '../types/execution';

export async function fetchCloudPayload(
  transport: Pick<Transport, 'getOutputCloud' | 'getOutputProfile'>,
  type: 'PointCloud' | 'Profile',
  runId: string,
  nodeId: string,
  port: string,
  maxPoints: number,
): Promise<CloudPayload> {
  if (type === 'Profile') {
    if (!transport.getOutputProfile) throw new Error('当前宿主不支持轮廓原生数据，请升级宿主');
    return {
      ...profileToCloudPayload(
        decodeProfile(await transport.getOutputProfile(runId, nodeId, port)),
      ),
      sampleStride: 1,
    };
  }
  return decodeCloud(await transport.getOutputCloud(runId, nodeId, port, maxPoints));
}
