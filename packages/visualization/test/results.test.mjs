import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  cameraProjector,
  cloudGroupPreview,
  maskPreview,
  poseCollection,
  poseGeometry,
  profileDescriptor,
  poseScene2D,
  projectedPoseScene,
  renderImageOverlaySvg,
  resultColor,
  resultSummary,
  structuredScene,
} from '../index.js';

test('native Profile summaries preserve explicit units, planes, ordering and lineage', () => {
  const descriptor = {
    schemaVersion: 1,
    frame: 'scanner',
    unit: 'mm',
    plane: 'XZ',
    handedness: 'reflected',
    sampleOrder: 'acquisition',
    sourceId: 'raw-scan',
    sourceCount: 1280,
  };
  const summary = resultSummary({ kind: 'Profile', descriptor, count: 1280, validCount: 1279 });
  assert.equal(summary.unit, 'mm');
  assert.equal(summary.plane, 'XZ');
  assert.equal(summary.sampleOrder, 'acquisition');
  assert.equal(summary.validCount, 1279);
  assert.equal(summary.sourceId, 'raw-scan');
  assert.equal(Object.isFrozen(profileDescriptor(descriptor)), true);
  for (const patch of [
    { unit: 'px' },
    { sourceCount: true },
    { plane: 'auto' },
    { frame: '' },
    { frame: 'sensor\u0000other' },
    { sourceId: 'scan\u0000other' },
    { frame: '\ud800' },
    { frame: '文'.repeat(342) },
    { extra: 1 },
  ])
    assert.throws(() => profileDescriptor({ ...descriptor, ...patch }));
  assert.throws(() =>
    resultSummary({ kind: 'Profile', descriptor, count: 1280, validCount: 1281 }),
  );
  const boundary = { ...descriptor, frame: '\u0001'.repeat(670), sourceId: '\u0001'.repeat(670) };
  boundary.sourceId += 'x'.repeat(8192 - new TextEncoder().encode(JSON.stringify(boundary)).length);
  assert.equal(profileDescriptor(boundary).sourceId, boundary.sourceId);
  assert.throws(
    () => profileDescriptor({ ...boundary, sourceId: `${boundary.sourceId}x` }),
    /8KiB/,
  );
});

const pose = (dimension, overrides = {}) => ({
  kind: 'Record',
  type: `vision.Pose${dimension}D.v1`,
  data: {
    id: 'part-1',
    frame: 'camera',
    unit: dimension === 2 ? 'px' : 'm',
    position: dimension === 2 ? [20, 30] : [0, 0, 2],
    size: dimension === 2 ? [10, 6] : [0.2, 0.4, 0.6],
    ...(dimension === 2 ? { angle: Math.PI / 2 } : { rotation: [0, 0, 0, 1] }),
    ...overrides,
  },
});
const camera = {
  type: 'vision.CameraCalib.v1',
  data: {
    frame: 'camera',
    unit: 'm',
    cameraMatrix: [100, 0, 50, 0, 100, 40, 0, 0, 1],
    distortion: [],
    imageSize: [100, 80],
  },
};

test('typed pose records preserve rotation, identity, frames, source immutability and bounded summaries', () => {
  const value = pose(2),
    before = JSON.stringify(value);
  const scene = poseScene2D(value);
  assert.deepEqual(scene.layers[0].points[0], [23, 25]);
  assert.equal(scene.frame.unit, 'px');
  assert.equal(scene.frame.yDirection, 'down');
  assert.equal(scene.layers[0].instanceId, 'part-1');
  const geometry = poseGeometry(pose(3));
  assert.equal(geometry.length, 4);
  assert.equal(geometry[0].points.length, 24);
  assert.equal(poseGeometry(value, { selectedId: 'missing' }).length, 0);
  assert.equal(resultSummary(pose(3)).items[0].height, 0.6);
  assert.deepEqual(
    resultSummary({ type: 'std.Value.v1', data: { value: '中文\n<script>', unit: 'text' } }).value,
    '中文\n<script>',
  );
  assert.equal(JSON.stringify(value), before);
  assert.equal(scene.layers[0].color, resultColor(value.data.id));
  const collection = {
    type: 'vision.Poses2D.v1',
    data: { frame: 'camera', unit: 'px', items: [value.data, value.data] },
  };
  assert.throws(() => poseCollection(collection), /unique/);
  assert.throws(() => poseCollection(pose(3, { rotation: [0, 0, 0, 2] })), /normalized/);
  assert.throws(
    () =>
      poseCollection({
        ...collection,
        data: { ...collection.data, items: Array(4097).fill(value.data) },
      }),
    /budget/,
  );
  assert.throws(
    () =>
      poseCollection({
        ...collection,
        data: { ...collection.data, items: [{ ...value.data, frame: 'world' }] },
      }),
    /mismatch/,
  );
});

test('camera projection is frame explicit, positive-depth only and follows OpenCV distortion and rigid extrinsics', () => {
  const projector = cameraProjector(camera, 'camera');
  assert.deepEqual(projector.project([0, 0, 2]), [50, 40]);
  assert.equal(projector.project([0, 0, -1]), null);
  assert.throws(() => cameraProjector(camera, 'world'), /explicit/);
  const matrix = [1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  assert.deepEqual(
    cameraProjector(camera, 'world', { fromFrame: 'world', toFrame: 'camera', matrix }).project([
      0, 0, 2,
    ]),
    [100, 40],
  );
  assert.throws(
    () =>
      cameraProjector(camera, 'world', {
        fromFrame: 'world',
        toFrame: 'camera',
        matrix: matrix.map((n, i) => (i === 0 ? 2 : n)),
      }),
    /rigid/,
  );
  for (const count of [4, 5, 8, 12, 14]) {
    const distorted = { ...camera, data: { ...camera.data, distortion: Array(count).fill(0) } };
    const actual = cameraProjector(distorted, 'camera').project([0.2, 0.1, 2]);
    assert.ok(Math.abs(actual[0] - 60) < 1e-10 && Math.abs(actual[1] - 45) < 1e-10);
  }
  const distortion = { ...camera, data: { ...camera.data, distortion: [1, 0, 0, 0] } };
  assert.ok(Math.abs(cameraProjector(distortion, 'camera').project([1, 0, 2])[0] - 112.5) < 1e-10);
  const scene = projectedPoseScene(pose(3), camera);
  assert.equal(scene.frame.unit, 'px');
  assert.equal(scene.layers[0].instanceId, 'part-1');
  assert.equal(projectedPoseScene(pose(3, { position: [0, 0, -2] }), camera).layers.length, 0);
});

test('cloud group preview requires lossless actual cloud identity and exact ordered sampling, including overlap and empty groups', () => {
  const sourceCloudId = '18446744073709551614';
  const value = {
    type: 'cloud.Groups.v1',
    data: {
      sourceCloudId,
      frame: 'world',
      unit: 'm',
      pointCount: 10,
      groups: [
        { id: 'a', offset: 0, count: 3 },
        { id: 'b', offset: 3, count: 2 },
        { id: 'empty', offset: 5, count: 0 },
      ],
    },
  };
  const indices = { sourceCloudIdString: sourceCloudId, values: new Int32Array([0, 3, 6, 3, 9]) };
  const cloud = { sourceCloudId, pointCount: 4, totalPoints: 10, sampleStride: 3 };
  const all = cloudGroupPreview(value, indices, cloud);
  assert.deepEqual(Array.from(all.visible), [1, 1, 1, 1]);
  assert.equal(all.frame, 'world');
  assert.deepEqual(
    Array.from(cloudGroupPreview(value, indices, cloud, { selectedId: 'b' }).visible),
    [0, 1, 0, 1],
  );
  assert.deepEqual(
    Array.from(cloudGroupPreview(value, indices, cloud, { selectedId: 'empty' }).visible),
    [0, 0, 0, 0],
  );
  assert.throws(
    () => cloudGroupPreview(value, indices, { ...cloud, sourceCloudId: '1' }),
    /identity/,
  );
  assert.throws(
    () => cloudGroupPreview(value, { ...indices, sourceCloudIdString: undefined }, cloud),
    /identity/,
  );
  assert.throws(
    () => cloudGroupPreview(value, indices, { ...cloud, sampleStride: undefined }),
    /sampling/,
  );
  assert.throws(
    () => cloudGroupPreview(value, { ...indices, values: new Int32Array([0, 3, 6, 3, 10]) }, cloud),
    /outside/,
  );
  assert.throws(
    () => cloudGroupPreview(value, { ...indices, values: indices.values.subarray(1) }, cloud),
    /truncated/,
  );
});

test('instance masks bind NHW binary payload to ordered IDs and render to a safe shared pixel SVG', () => {
  const poses = pose(2);
  const value = {
    type: 'vision.Masks2D.v1',
    data: {
      frame: 'camera',
      unit: 'px',
      width: 2,
      height: 2,
      layout: 'NHW',
      items: [{ id: 'part-1', index: 0, hasMask: true }],
    },
  };
  const masks = { shape: [1, 2, 2], data: new Float32Array([1, 0, 0, 1]) };
  const result = maskPreview(value, masks, poses, { alpha: 0.5 });
  const emptyPoses = {
    type: 'vision.Poses2D.v1',
    data: { frame: 'camera', unit: 'px', items: [] },
  };
  const emptyMasks = { ...value, data: { ...value.data, items: [] } };
  assert.deepEqual(
    Array.from(
      maskPreview(emptyMasks, { shape: [0, 2, 2], data: new Float32Array() }, emptyPoses).pixels,
    ),
    Array(16).fill(0),
  );
  assert.throws(
    () =>
      maskPreview(
        { ...emptyMasks, data: { ...emptyMasks.data, width: 100000, height: 100000 } },
        { shape: [0, 100000, 100000], data: new Float32Array() },
        emptyPoses,
      ),
    /excessive/,
  );
  assert.equal(result.pixels[3], 128);
  assert.equal(result.pixels[7], 0);
  assert.throws(
    () => maskPreview(value, { ...masks, data: new Float32Array([1, NaN, 0, 1]) }, poses),
    /binary/,
  );
  assert.throws(
    () =>
      maskPreview(
        { ...value, data: { ...value.data, items: [{ id: 'other', index: 0, hasMask: true }] } },
        masks,
        poses,
      ),
    /identities/,
  );
  const scene = poseScene2D(pose(2, { name: '<script>alert(1)</script>' }));
  const svg = renderImageOverlaySvg(scene, 100, 80);
  assert.ok(svg.includes('M23.5,25.5'));
  assert.ok(svg.includes('&lt;script&gt;'));
  assert.ok(!svg.includes('<script>'));
  assert.throws(() => renderImageOverlaySvg(scene, 100000, 100000), /16777216/);
  assert.equal(structuredScene({ type: 'std.Value.v1', data: { value: '<svg/>' } }), null);
  assert.equal(structuredScene({ type: 'visualization.SceneSpec.v1', data: scene }), scene);
});
