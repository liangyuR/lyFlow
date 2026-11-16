import assert from 'node:assert/strict';
import { test } from 'node:test';
import { firstCloudPort } from '../src/lib/basecloud.ts';
import { fetchCloudPayload } from '../src/lib/profileFetch.ts';
import { roiBackdropPoints } from '../src/lib/roiFrames.ts';
import { formatOutputValue } from '../src/lib/outputs.ts';
import { defaultViewFor, viewerContentFor, viewsFor } from '../src/lib/viewRule.ts';
import {
  CLOUD_MAGIC,
  PROFILE_MAGIC,
  decodeProfile,
  parseProfileDescriptor,
  profileToCloudPayload,
} from '../src/types/execution.ts';

function packet(options = {}) {
  const xyz = options.xyz ?? [1, 2, 3, 100, NaN, 50, 4, 5, 6];
  const count = xyz.length / 3;
  const descriptor = {
    schemaVersion: 1,
    frame: '传感器A',
    unit: 'mm',
    plane: 'XZ',
    handedness: 'reflected',
    sampleOrder: 'acquisition',
    sourceId: 'sample-origin',
    sourceCount: count ? 8 : 0,
    ...options.descriptor,
  };
  const metadata = new TextEncoder().encode(
    JSON.stringify({ descriptor, cloudId: options.cloudId ?? '18446744073709551615' }),
  );
  const align = (n) => Math.ceil(n / 4) * 4;
  const flags = options.flags ?? 7;
  const start = align(32 + metadata.length);
  const intensities = flags & 1 ? count * 4 : 0;
  const normals = flags & 2 ? count * 12 : 0;
  const colors = flags & 4 ? align(count * 3) : 0;
  const validAt = start + count * 12 + intensities + normals + colors;
  const indicesAt = align(validAt + count);
  const buffer = new ArrayBuffer(indicesAt + count * 4);
  const view = new DataView(buffer);
  [
    PROFILE_MAGIC,
    1,
    count,
    count + (options.offset ?? 0),
    options.offset ?? 0,
    flags,
    metadata.length,
    0,
  ].forEach((v, i) => view.setUint32(i * 4, v, true));
  new Uint8Array(buffer, 32, metadata.length).set(metadata);
  xyz.forEach((v, i) => view.setFloat32(start + i * 4, v, true));
  if (flags & 1)
    for (let i = 0; i < count; i++) view.setFloat32(start + count * 12 + i * 4, i + 10, true);
  if (flags & 2)
    for (let i = 0; i < count; i++)
      view.setFloat32(start + count * 12 + intensities + i * 12, 1, true);
  if (flags & 4)
    new Uint8Array(buffer, start + count * 12 + intensities + normals, count * 3).fill(123);
  new Uint8Array(buffer, validAt, count).set(options.validity ?? (count ? [1, 0, 1] : []));
  (options.indices ?? (count ? [0, 3, 7] : [])).forEach((v, i) =>
    view.setUint32(indicesAt + i * 4, v, true),
  );
  return buffer;
}

test('Profile native payload preserves slots, lineage, channels and explicit display units', () => {
  const raw = packet();
  const profile = decodeProfile(raw);
  assert.equal(profile.xyz.buffer, raw);
  assert.equal(profile.descriptor.unit, 'mm');
  assert.equal(profile.descriptor.plane, 'XZ');
  assert.equal(profile.descriptor.handedness, 'reflected');
  assert.equal(Object.isFrozen(profile.descriptor), true);
  assert.deepEqual(Array.from(profile.sourceIndices), [0, 3, 7]);
  assert.deepEqual(Array.from(profile.validity), [1, 0, 1]);
  assert.deepEqual(Array.from(profile.intensity), [10, 11, 12]);
  assert.deepEqual(Array.from(profile.normals), [1, 0, 0, 1, 0, 0, 1, 0, 0]);
  assert.deepEqual(Array.from(profile.rgb), Array(9).fill(123));
  const cloud = profileToCloudPayload(profile);
  assert.notEqual(cloud.xyz.buffer, raw);
  assert.equal(cloud.pointCount, 3);
  assert.equal(cloud.profile.validCount, 2);
  assert.equal(cloud.sourceCloudId, '18446744073709551615');
  assert.notEqual(cloud.sourceCloudId, cloud.profile.descriptor.sourceId);
  assert.ok(Math.abs(cloud.xyz[0] - 0.001) < 1e-9);
  assert.equal(cloud.xyz[3], NaN);
  assert.equal(cloud.xyz[4], NaN);
  assert.equal(profile.xyz[3], 100);
  assert.equal(profile.xyz[4], NaN);
  assert.ok(Math.abs(cloud.bounds[5] - 0.006) < 1e-9);
  assert.equal(decodeProfile(packet({ xyz: [], validity: [], indices: [] })).pointCount, 0);
  assert.deepEqual(
    Array.from(profileToCloudPayload(decodeProfile(packet({ xyz: [] }))).bounds),
    [0, 0, 0, 0, 0, 0],
  );
  assert.equal(
    decodeProfile(packet({ descriptor: { sampleOrder: 'unknown' }, indices: [7, 0, 3] }))
      .sourceIndices[0],
    7,
  );
  for (const modify of [
    (b) => new DataView(b).setUint32(0, 0, true),
    (b) => new DataView(b).setUint32(4, 2, true),
    (b) => new DataView(b).setUint32(12, 9, true),
    (b) => new DataView(b).setUint32(20, 8, true),
    (b) => new DataView(b).setUint32(24, 16385, true),
    (b) => new DataView(b).setUint32(28, 1, true),
    (b) => (new Uint8Array(b)[32] = 255),
  ]) {
    const bad = packet();
    modify(bad);
    assert.throws(() => decodeProfile(bad));
  }
  for (const options of [
    { validity: [1, 2, 1] },
    { validity: [1, 1, 1] },
    { indices: [0, 0, 7] },
    { indices: [0, 7, 3] },
    { indices: [0, 3, 8] },
    { descriptor: { sampleOrder: 'unknown' }, indices: [0, 0, 7] },
    { descriptor: { unit: 'px' } },
    { cloudId: '01' },
    { cloudId: '18446744073709551616' },
    { xyz: [1, 2, 3, Infinity, 0, 0, 4, 5, 6] },
  ])
    assert.throws(() => decodeProfile(packet(options)));
  assert.throws(() => decodeProfile(raw.slice(0, -1)));
  assert.throws(() => decodeProfile(new Uint8Array([...new Uint8Array(raw), 0]).buffer));
  assert.throws(() => parseProfileDescriptor({ ...profile.descriptor, sourceCount: true }));
  assert.throws(() => decodeProfile(packet({ descriptor: { frame: 'sensor\u0000other' } })));
  assert.throws(() => parseProfileDescriptor({ ...profile.descriptor, frame: '\ud800' }));
  assert.throws(() => parseProfileDescriptor({ ...profile.descriptor, frame: '文'.repeat(342) }));
  const boundaryDescriptor = {
    schemaVersion: 1,
    frame: '\u0001'.repeat(670),
    unit: 'm',
    plane: 'XZ',
    handedness: 'right',
    sampleOrder: 'acquisition',
    sourceId: '\u0001'.repeat(670),
    sourceCount: 0,
  };
  assert.equal(new TextEncoder().encode(JSON.stringify(boundaryDescriptor)).length, 8173);
  const boundary = packet({ xyz: [], descriptor: boundaryDescriptor, cloudId: '1' });
  assert.equal(new DataView(boundary).getUint32(24, true), 8202);
  assert.deepEqual(decodeProfile(boundary).descriptor, boundaryDescriptor);
  const oversizedDescriptor = { ...boundaryDescriptor, sourceId: '\u0001'.repeat(674) };
  assert.throws(() => parseProfileDescriptor(oversizedDescriptor), /8KiB/);
  assert.throws(() => decodeProfile(packet({ xyz: [], descriptor: oversizedDescriptor })), /8KiB/);
  assert.match(
    formatOutputValue({
      elementCount: 3,
      value: { kind: 'Profile', descriptor: profile.descriptor, validCount: 2 },
    }),
    /mm.*XZ.*reflected.*acquisition/,
  );
  assert.equal(defaultViewFor('Profile'), 'cloud3d');
  assert.deepEqual(viewsFor('Profile'), ['cloud3d', 'value']);
  assert.equal(viewerContentFor(['Profile'], undefined, ['Image']), 'cloud');
  assert.equal(
    firstCloudPort(
      new Map([['util.reroute', { outputs: [{ name: 'out', type: 'Any' }] }]]),
      'util.reroute',
      undefined,
      [{ port: 'out', type: 'Profile' }],
    ),
    'out',
  );
});

test('Profile Transport uses its explicit capability and never downsamples or coerces PointCloud', async () => {
  const calls = [];
  const transport = {
    async getOutputProfile(...args) {
      calls.push(args);
      return packet();
    },
    async getOutputCloud() {
      throw new Error('must not reinterpret Profile as PointCloud');
    },
  };
  const cloud = await fetchCloudPayload(transport, 'Profile', 'run', 'node', 'profile', 1);
  assert.equal(cloud.pointCount, 3);
  assert.equal(cloud.sampleStride, 1);
  const fieldCloud = await fetchCloudPayload(
    transport,
    'Profile',
    'run',
    'node',
    'templates.left',
    1,
  );
  assert.deepEqual(calls[1], ['run', 'node', 'templates.left']);
  const projected = roiBackdropPoints(fieldCloud);
  assert.equal(projected.length, 6);
  assert.ok(Math.abs(projected[0] - 0.001) < 1e-9);
  assert.ok(Math.abs(projected[1] - 0.003) < 1e-9);
  assert.ok(Math.abs(projected[3] - 0.004) < 1e-9);
  assert.ok(Math.abs(projected[4] - 0.006) < 1e-9);
  assert.equal(fieldCloud.profile.sourceIndices[2], 7);
  assert.equal(fieldCloud.xyz[3], NaN);
  const yz = profileToCloudPayload(
    decodeProfile(packet({ descriptor: { plane: 'YZ', unit: 'm' } })),
  );
  assert.deepEqual(Array.from(roiBackdropPoints(yz)), [2, 3, 0, 5, 6, 0]);

  assert.deepEqual(calls, [
    ['run', 'node', 'profile'],
    ['run', 'node', 'templates.left'],
  ]);
  await assert.rejects(
    fetchCloudPayload({ getOutputCloud: transport.getOutputCloud }, 'Profile', 'r', 'n', 'p', 1),
    /宿主不支持/,
  );
  const raw = new ArrayBuffer(52);
  const head = new DataView(raw);
  [CLOUD_MAGIC, 1, 1, 0].forEach((v, i) => head.setUint32(i * 4, v, true));
  new Float32Array(raw, 40, 3).set([10, 20, 30]);
  const unchanged = await fetchCloudPayload(
    {
      async getOutputCloud() {
        return raw;
      },
    },
    'PointCloud',
    'r',
    'n',
    'p',
    1,
  );
  assert.deepEqual(Array.from(unchanged.xyz), [10, 20, 30]);
  assert.equal(unchanged.profile, undefined);
});
