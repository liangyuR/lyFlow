// 把 `lyflow dump --format ascii` 写出来的 PCD 变成 ADR-0006 的二进制点云载荷。
// 布局与 bridge/src/execution.rs 的 encode_cloud 一致。

import fs from "node:fs";

export const CLOUD_MAGIC = 0x4350594c;
export const CLOUD_HAS_INTENSITY = 1;
export const CLOUD_HAS_NORMALS = 2;

function parseAsciiPcd(text) {
  const lines = text.split(/\r?\n/);
  let fields = [];
  let points = 0;
  let start = -1;
  for (let i = 0; i < lines.length; i += 1) {
    const line = (lines[i] ?? "").trim();
    if (!line || line.startsWith("#")) continue;
    const [key, ...rest] = line.split(/\s+/);
    if (key === "FIELDS") fields = rest;
    else if (key === "POINTS") points = Number(rest[0]);
    else if (key === "WIDTH" && points === 0) points = Number(rest[0]);
    else if (key === "DATA") {
      if (rest[0] !== "ascii") throw new Error(`只支持 ascii PCD，拿到 ${rest[0]}`);
      start = i + 1;
      break;
    }
  }
  if (start < 0) throw new Error("PCD 里没有 DATA 段");

  const ix = fields.indexOf("x");
  const iy = fields.indexOf("y");
  const iz = fields.indexOf("z");
  if (ix < 0 || iy < 0 || iz < 0) throw new Error(`PCD 缺 x/y/z：FIELDS = ${fields.join(" ")}`);
  const ii = fields.indexOf("intensity");
  const inx = fields.indexOf("normal_x");
  const iny = fields.indexOf("normal_y");
  const inz = fields.indexOf("normal_z");
  const hasNormals = inx >= 0 && iny >= 0 && inz >= 0;

  const xyz = [];
  const intensity = [];
  const normals = [];
  for (let i = start; i < lines.length && xyz.length / 3 < points; i += 1) {
    const line = (lines[i] ?? "").trim();
    if (!line) continue;
    const cols = line.split(/\s+/);
    const x = Number(cols[ix]);
    const y = Number(cols[iy]);
    const z = Number(cols[iz]);
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    xyz.push(x, y, z);
    if (ii >= 0) intensity.push(Number(cols[ii]));
    if (hasNormals) normals.push(Number(cols[inx]), Number(cols[iny]), Number(cols[inz]));
  }
  return {
    count: xyz.length / 3,
    xyz,
    intensity: ii >= 0 ? intensity : null,
    normals: hasNormals ? normals : null,
  };
}

/** 抽稀到 maxPoints，等距取样，与 core 的 maxPoints 语义一致（只是更朴素）。 */
function stride(cloud, maxPoints) {
  if (!maxPoints || cloud.count <= maxPoints) return cloud;
  const step = Math.ceil(cloud.count / maxPoints);
  const xyz = [];
  const intensity = cloud.intensity ? [] : null;
  const normals = cloud.normals ? [] : null;
  for (let i = 0; i < cloud.count; i += step) {
    xyz.push(cloud.xyz[i * 3], cloud.xyz[i * 3 + 1], cloud.xyz[i * 3 + 2]);
    if (intensity) intensity.push(cloud.intensity[i]);
    if (normals) normals.push(cloud.normals[i * 3], cloud.normals[i * 3 + 1], cloud.normals[i * 3 + 2]);
  }
  return { count: xyz.length / 3, xyz, intensity, normals };
}

export function encodeCloudFromPcd(file, maxPoints) {
  const full = parseAsciiPcd(fs.readFileSync(file, "utf8"));
  const totalPoints = full.count;
  const cloud = stride(full, maxPoints);
  const n = cloud.count;

  const bounds = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
  for (let i = 0; i < totalPoints; i += 1) {
    for (let a = 0; a < 3; a += 1) {
      const v = full.xyz[i * 3 + a];
      if (v < bounds[a]) bounds[a] = v;
      if (v > bounds[a + 3]) bounds[a + 3] = v;
    }
  }
  if (totalPoints === 0) bounds.fill(0);

  let flags = 0;
  if (cloud.intensity) flags |= CLOUD_HAS_INTENSITY;
  if (cloud.normals) flags |= CLOUD_HAS_NORMALS;

  const extra = (cloud.intensity ? n * 4 : 0) + (cloud.normals ? n * 12 : 0);
  const buf = Buffer.alloc(16 + 24 + n * 12 + extra);
  buf.writeUInt32LE(CLOUD_MAGIC, 0);
  buf.writeUInt32LE(n, 4);
  buf.writeUInt32LE(totalPoints, 8);
  buf.writeUInt32LE(flags, 12);
  for (let i = 0; i < 6; i += 1) buf.writeFloatLE(bounds[i], 16 + i * 4);
  let off = 40;
  for (let i = 0; i < n * 3; i += 1, off += 4) buf.writeFloatLE(cloud.xyz[i], off);
  if (cloud.intensity) {
    for (let i = 0; i < n; i += 1, off += 4) buf.writeFloatLE(cloud.intensity[i], off);
  }
  if (cloud.normals) {
    for (let i = 0; i < n * 3; i += 1, off += 4) buf.writeFloatLE(cloud.normals[i], off);
  }
  return buf;
}
