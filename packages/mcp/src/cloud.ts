export const CLOUD_MAGIC = 0x4350594c;
export const CLOUD_HAS_INTENSITY = 1;
export const CLOUD_HAS_NORMALS = 2;

export interface CloudPayload {
  pointCount: number;
  totalPoints: number;
  bounds: Float32Array;
  xyz: Float32Array;
  intensity: Float32Array | null;
  normals: Float32Array | null;
}

export function decodeCloud(buffer: ArrayBuffer): CloudPayload {
  if (buffer.byteLength < 40) {
    throw new Error(`点云载荷太短（${buffer.byteLength} 字节），多半不是点云数据`);
  }
  const header = new DataView(buffer);
  const magic = header.getUint32(0, true);
  if (magic !== CLOUD_MAGIC) {
    throw new Error(`点云载荷的 magic 不对（0x${magic.toString(16)}），响应不是点云`);
  }
  const pointCount = header.getUint32(4, true);
  const totalPoints = header.getUint32(8, true);
  const flags = header.getUint32(12, true);

  const bounds = new Float32Array(buffer, 16, 6);
  const xyz = new Float32Array(buffer, 40, pointCount * 3);
  let offset = 40 + pointCount * 12;
  let intensity: Float32Array | null = null;
  if (flags & CLOUD_HAS_INTENSITY) {
    intensity = new Float32Array(buffer, offset, pointCount);
    offset += pointCount * 4;
  }
  let normals: Float32Array | null = null;
  if (flags & CLOUD_HAS_NORMALS) {
    normals = new Float32Array(buffer, offset, pointCount * 3);
    offset += pointCount * 12;
  }
  return { pointCount, totalPoints, bounds, xyz, intensity, normals };
}

export interface ChannelStat {
  min: number | null;
  max: number | null;
  mean: number | null;
}

function statOf(values: Float32Array, stride: number, offset: number): ChannelStat {
  const n = Math.floor((values.length - offset + stride - 1) / stride);
  if (n <= 0) return { min: null, max: null, mean: null };
  let min = Infinity;
  let max = -Infinity;
  let sum = 0;
  let count = 0;
  for (let i = offset; i < values.length; i += stride) {
    const v = values[i] as number;
    if (!Number.isFinite(v)) continue;
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
    count += 1;
  }
  if (count === 0) return { min: null, max: null, mean: null };
  return { min, max, mean: sum / count };
}

export interface CloudPoint {
  x: number;
  y: number;
  z: number;
  intensity?: number;
  normal?: [number, number, number];
}

export interface CloudSummary {
  kind: "cloud";
  pointCount: number;
  totalPoints: number;
  bbox: { min: [number, number, number]; max: [number, number, number] };
  channels: Record<string, ChannelStat>;
  head: CloudPoint[];
}

export function summarizeCloud(payload: CloudPayload, head: number): CloudSummary {
  const channels: Record<string, ChannelStat> = {
    x: statOf(payload.xyz, 3, 0),
    y: statOf(payload.xyz, 3, 1),
    z: statOf(payload.xyz, 3, 2),
  };
  if (payload.intensity) channels["intensity"] = statOf(payload.intensity, 1, 0);
  if (payload.normals) {
    channels["nx"] = statOf(payload.normals, 3, 0);
    channels["ny"] = statOf(payload.normals, 3, 1);
    channels["nz"] = statOf(payload.normals, 3, 2);
  }

  const points: CloudPoint[] = [];
  const take = Math.max(0, Math.min(head, payload.pointCount));
  for (let i = 0; i < take; i += 1) {
    const point: CloudPoint = {
      x: payload.xyz[i * 3] as number,
      y: payload.xyz[i * 3 + 1] as number,
      z: payload.xyz[i * 3 + 2] as number,
    };
    if (payload.intensity) point.intensity = payload.intensity[i] as number;
    if (payload.normals) {
      point.normal = [
        payload.normals[i * 3] as number,
        payload.normals[i * 3 + 1] as number,
        payload.normals[i * 3 + 2] as number,
      ];
    }
    points.push(point);
  }

  return {
    kind: "cloud",
    pointCount: payload.pointCount,
    totalPoints: payload.totalPoints,
    bbox: {
      min: [payload.bounds[0] as number, payload.bounds[1] as number, payload.bounds[2] as number],
      max: [payload.bounds[3] as number, payload.bounds[4] as number, payload.bounds[5] as number],
    },
    channels,
    head: points,
  };
}
