import test from 'node:test';
import assert from 'node:assert/strict';
import {
  sceneBounds,
  renderSvg,
  renderGridSvg,
  geometryPaths,
  sceneMetadata,
  gridScenes,
} from '../index.js';
const frame = { id: 'sensor', unit: 'mm', axes: ['u', 'h'], yDirection: 'down' };
const scene = {
  schemaVersion: 1,
  frame,
  layers: [
    {
      id: 'cloud',
      kind: 'points',
      frame,
      points: [
        [0, 0],
        [10, 20],
        [NaN, 2],
      ],
      totalPoints: 100,
    },
  ],
};
test('explicit coordinates, affine overlay and nonfinite metadata', () => {
  assert.deepEqual(sceneBounds(scene), [-0.8, -0.8, 10.8, 20.8]);
  assert.equal(sceneMetadata(scene).layers[0].nonFinitePoints, 1);
  const metricBounds = sceneBounds({
    ...scene,
    view: { equalAspect: false },
    layers: [
      {
        ...scene.layers[0],
        points: [
          [0, 0.006],
          [19, 0.066],
        ],
      },
    ],
  });
  assert.ok(metricBounds[1] > 0 && metricBounds[3] < 0.07);
  assert.equal(sceneMetadata(scene).layers[0].displayedPoints, 2);
  assert.throws(
    () => renderSvg({ ...scene, layers: [{ ...scene.layers[0], frame: { ...frame, unit: 'm' } }] }),
    /unknown coordinate transform/,
  );
  sceneBounds({
    ...scene,
    layers: [{ ...scene.layers[0], frame: { ...frame, unit: 'm' }, transform: [2, 0, 0, 2, 1, 2] }],
  }).forEach((v, i) => assert.ok(Math.abs(v - [-0.6, 0.4, 22.6, 43.6][i]) < 1e-12));
  assert.throws(
    () =>
      sceneBounds({ ...scene, layers: [{ ...scene.layers[0], transform: [0, 0, 0, 0, 0, 0] }] }),
    /singular/,
  );
  assert.throws(() => renderSvg({ ...scene, view: { bounds: [2, 0, 1, 0] } }), /bounds/);
});
test('geometry paths retain algorithm values without fitting', () => {
  assert.deepEqual(
    geometryPaths({ kind: 'Line2D', hasSegment: true, start: [2, 3], end: [7, 9] }, 10),
    [{ points: [2, 3, 0, 7, 9, 0], loop: false }],
  );
  const circle = geometryPaths({ kind: 'Circle2D', center: [4, 7], radius: 3 }, 1)[0];
  assert.equal(circle.points.length, 216);
  for (let i = 0; i < circle.points.length; i += 3)
    assert.ok(Math.abs(Math.hypot(circle.points[i] - 4, circle.points[i + 1] - 7) - 3) < 1e-12);
  assert.deepEqual(geometryPaths({ kind: 'Circle2D', unit: 'px' }, 10), []);
});
test('scene escaping, layer support, empty output and shared views', () => {
  const all = {
    ...scene,
    title: '<script>',
    layers: [
      ...scene.layers,
      { id: 'fit', kind: 'circle', frame, center: [5, 10], radius: 2 },
      { id: 'roi', kind: 'box', frame, bounds: [1, 2, 3, 4] },
      {
        id: 'measure',
        kind: 'segments',
        frame,
        segments: [
          [
            [1, 2],
            [3, 4],
          ],
        ],
      },
      {
        id: 'path',
        kind: 'polyline',
        frame,
        points: [
          [0, 0],
          [NaN, 1],
          [1, 1],
        ],
      },
      { id: 'label', kind: 'text', frame, position: [2, 2], text: '<b>' },
    ],
  };
  const aligned = gridScenes([scene, { ...all, annotations: ['result = 1.2 mm'] }], {
    sharedBounds: true,
  });
  assert.equal(aligned[0].view.annotationRows, aligned[1].view.annotationRows);
  assert.equal(aligned[0].view.legendRows, aligned[1].view.legendRows);
  const metric = renderSvg({
    ...scene,
    frame: { ...frame, axisUnits: ['sample', 'mm'] },
    layers: [],
    annotations: ['Metric source: actual output'],
    view: { equalAspect: false },
  });
  assert.ok(metric.includes('(sample)'));
  assert.ok(metric.includes('Metric source: actual output'));
  const svg = renderSvg(all);
  assert.ok(svg.includes('&lt;script&gt;'));
  assert.ok(svg.includes('1 non-finite'));
  assert.ok(!svg.includes('NaN'));
  assert.ok(renderSvg({ ...scene, layers: [] }).includes('No finite output data'));
  assert.ok(renderGridSvg([scene, all], { sharedBounds: true }).includes('<svg x="800"'));
  assert.throws(
    () =>
      renderGridSvg([scene, { ...scene, frame: { ...frame, id: 'other' } }], {
        sharedBounds: true,
      }),
    /identical frames/,
  );
  assert.throws(
    () =>
      renderSvg({
        ...scene,
        layers: [
          {
            id: 'image',
            kind: 'image',
            frame,
            bounds: [0, 0, 1, 1],
            dataUrl: 'https://example.com/image.png',
          },
        ],
      }),
    /embedded/,
  );
});
