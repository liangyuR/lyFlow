const POSE2 = new Set(['vision.Pose2D.v1', 'vision.Poses2D.v1']);
const POSE3 = new Set(['vision.Pose3D.v1', 'vision.Poses3D.v1']);
export const RESULT_COLORS = [
  '#5ec9d8',
  '#e9ac64',
  '#8bd49b',
  '#c195dd',
  '#ed8392',
  '#92b6f5',
  '#c9cd75',
  '#d79ebd',
];
export const RESULT_LIMITS = Object.freeze({
  instances: 4096,
  points: 2_000_000,
  memberships: 16_777_216,
  layers: 16384,
});

function requireThat(value, message) {
  if (!value) throw new Error(message);
}
export function profileDescriptor(value) {
  const fields = [
    'schemaVersion',
    'frame',
    'unit',
    'plane',
    'handedness',
    'sampleOrder',
    'sourceId',
    'sourceCount',
  ];
  requireThat(
    value && typeof value === 'object' && !Array.isArray(value),
    'Profile descriptor must be an object',
  );
  requireThat(
    Object.keys(value).length === fields.length && fields.every((key) => Object.hasOwn(value, key)),
    'Profile descriptor has missing or unknown fields',
  );
  const text = (item) =>
    typeof item === 'string' &&
    item.length > 0 &&
    !item.includes('\u0000') &&
    !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?:^|[^\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(item) &&
    new TextEncoder().encode(item).length <= 1024;
  requireThat(
    value.schemaVersion === 1 &&
      text(value.frame) &&
      text(value.sourceId) &&
      ['m', 'mm'].includes(value.unit) &&
      ['XY', 'XZ', 'YZ'].includes(value.plane) &&
      ['right', 'reflected'].includes(value.handedness) &&
      ['unknown', 'acquisition'].includes(value.sampleOrder) &&
      Number.isInteger(value.sourceCount) &&
      value.sourceCount >= 0 &&
      value.sourceCount <= 0xffffffff,
    'Invalid Profile unit, plane, ordering or source metadata',
  );
  const descriptor = { ...value };
  requireThat(
    new TextEncoder().encode(JSON.stringify(descriptor)).length <= 8192,
    'Profile descriptor exceeds 8KiB',
  );
  return Object.freeze(descriptor);
}
function vector(value, count) {
  return Array.isArray(value) && value.length === count && value.every(Number.isFinite);
}
function record(value) {
  return value?.kind === 'Record' ? { type: value.type, data: value.data } : value;
}
export function resultColor(identity) {
  let hash = 2166136261;
  for (const character of String(identity))
    hash = Math.imul(hash ^ character.codePointAt(0), 16777619);
  return RESULT_COLORS[(hash >>> 0) % RESULT_COLORS.length];
}
export function poseCollection(value) {
  const source = record(value);
  const dimension = POSE2.has(source?.type) ? 2 : POSE3.has(source?.type) ? 3 : 0;
  if (!dimension) return null;
  const data = source.data;
  requireThat(
    data && typeof data.frame === 'string' && data.frame,
    'Pose collection requires an explicit frame',
  );
  requireThat(
    dimension === 3 ? data.unit === 'm' : ['m', 'px'].includes(data.unit),
    'Invalid pose unit',
  );
  const items = source.type.includes('.Poses') ? data.items : [data];
  requireThat(
    Array.isArray(items) && items.length <= RESULT_LIMITS.instances,
    'Pose instance budget exceeded',
  );
  const ids = new Set();
  for (const item of items) {
    requireThat(
      item && typeof item.id === 'string' && item.id && !ids.has(item.id),
      'Pose IDs must be unique nonempty strings',
    );
    ids.add(item.id);
    requireThat(
      item.frame === data.frame && item.unit === data.unit,
      'Pose frame or unit mismatch',
    );
    requireThat(
      vector(item.position, dimension) &&
        vector(item.size, dimension) &&
        item.size.every((x) => x > 0),
      'Pose position and dimensions must be finite and positive',
    );
    if (dimension === 2) requireThat(Number.isFinite(item.angle), 'Pose angle must be finite');
    else
      requireThat(
        vector(item.rotation, 4) && Math.abs(Math.hypot(...item.rotation) - 1) <= 1e-6,
        'Pose quaternion must be normalized xyzw',
      );
  }
  return { dimension, frame: data.frame, unit: data.unit, items };
}
function rotate(point, q) {
  const [x, y, z, w] = q;
  const [a, b, c] = point;
  const tx = 2 * (y * c - z * b),
    ty = 2 * (z * a - x * c),
    tz = 2 * (x * b - y * a);
  return [a + w * tx + y * tz - z * ty, b + w * ty + z * tx - x * tz, c + w * tz + x * ty - y * tx];
}
function placed(point, item) {
  return rotate(point, item.rotation).map((x, i) => x + item.position[i]);
}
export function poseGeometry(value, options = {}) {
  const poses = poseCollection(value);
  if (!poses) return [];
  const result = [];
  const selected = options.selectedId ?? null;
  const showAxes = options.showAxes !== false;
  for (const item of poses.items) {
    if (selected && item.id !== selected) continue;
    const color =
      options.color ??
      resultColor(
        options.colorBy === 'class' ? (item.classLabel ?? item.classId ?? item.id) : item.id,
      );
    const common = {
      id: item.id,
      label: item.name || item.classLabel || item.id,
      frame: poses.frame,
      unit: poses.unit,
      color,
      dimension: poses.dimension,
    };
    if (poses.dimension === 2) {
      const c = Math.cos(item.angle),
        s = Math.sin(item.angle);
      const point = (x, y) => [
        item.position[0] + c * x - s * y,
        item.position[1] + s * x + c * y,
        0,
      ];
      const [x, y] = item.size.map((v) => v / 2);
      result.push({
        ...common,
        kind: 'box',
        points: [point(-x, -y), point(x, -y), point(x, y), point(-x, y)],
        closed: true,
      });
      if (showAxes)
        result.push({
          ...common,
          kind: 'axis',
          points: [[...item.position, 0], point(x, 0)],
          closed: false,
        });
    } else {
      const corners = [];
      for (let z = -1; z <= 1; z += 2)
        for (let y = -1; y <= 1; y += 2)
          for (let x = -1; x <= 1; x += 2)
            corners.push(
              placed(
                [(x * item.size[0]) / 2, (y * item.size[1]) / 2, (z * item.size[2]) / 2],
                item,
              ),
            );
      const points = [];
      for (let i = 0; i < 8; i++)
        for (const bit of [1, 2, 4]) if (!(i & bit)) points.push(corners[i], corners[i | bit]);
      result.push({ ...common, kind: 'box', points, segments: true, closed: false });
      if (showAxes)
        for (let axis = 0; axis < 3; axis++) {
          const end = [0, 0, 0];
          end[axis] = item.size[axis] / 2;
          result.push({
            ...common,
            color: ['#e87979', '#7bd48b', '#78aaf0'][axis],
            kind: 'axis',
            axis,
            points: [item.position, placed(end, item)],
            closed: false,
          });
        }
    }
  }
  return result;
}
export function poseScene2D(value, options = {}) {
  const poses = poseCollection(value);
  requireThat(poses?.dimension === 2, 'A 2D pose record is required');
  const frame = {
    id: poses.frame,
    unit: poses.unit,
    axes: poses.unit === 'px' ? ['u', 'v'] : ['X', 'Y'],
    yDirection: poses.unit === 'px' ? 'down' : 'up',
  };
  const layers = poseGeometry(value, options).map((shape, i) => ({
    id: `${shape.id}-${i}`,
    instanceId: shape.id,
    label: shape.label,
    frame,
    kind: 'polyline',
    points: shape.points.map((p) => p.slice(0, 2)),
    closed: shape.closed,
    color: shape.color,
    lineWidth: options.lineWidth,
  }));
  if (options.showLabels !== false)
    for (const item of poses.items) {
      if (options.selectedId && options.selectedId !== item.id) continue;
      layers.push({
        id: `${item.id}-label`,
        instanceId: item.id,
        label: item.name || item.id,
        frame,
        kind: 'text',
        position: item.position,
        text: item.name || item.classLabel || item.id,
        color:
          options.color ??
          resultColor(
            options.colorBy === 'class' ? (item.classLabel ?? item.classId ?? item.id) : item.id,
          ),
      });
    }
  return {
    schemaVersion: 1,
    title: options.title ?? '工件实例',
    frame,
    layers,
    provenance: { recordType: record(value).type, instanceCount: poses.items.length },
  };
}
export function structuredScene(value) {
  const source = record(value);
  if (source?.type !== 'visualization.SceneSpec.v1') return null;
  const scene = source.data;
  requireThat(
    scene?.schemaVersion === 1 &&
      Array.isArray(scene.layers) &&
      scene.layers.length <= RESULT_LIMITS.layers,
    'Invalid or excessive structured scene',
  );
  return scene;
}
export function cloudGroupPreview(value, indices, cloud, options = {}) {
  const source = record(value);
  requireThat(source?.type === 'cloud.Groups.v1', 'cloud.Groups.v1 is required');
  const data = source.data;
  requireThat(
    typeof cloud.sourceCloudId === 'string' &&
      cloud.sourceCloudId === data.sourceCloudId &&
      indices.sourceCloudIdString === data.sourceCloudId,
    'Group indices must reference the actual displayed cloud identity',
  );
  requireThat(
    data.unit === 'm' &&
      typeof data.frame === 'string' &&
      data.frame &&
      data.pointCount === cloud.totalPoints,
    'Group frame, unit or source count mismatch',
  );
  requireThat(
    Array.isArray(data.groups) &&
      data.groups.length <= RESULT_LIMITS.instances &&
      indices.values.length <= RESULT_LIMITS.memberships,
    'Cloud group budget exceeded',
  );
  requireThat(
    Number.isInteger(cloud.sampleStride) &&
      cloud.sampleStride > 0 &&
      cloud.pointCount === Math.ceil(cloud.totalPoints / cloud.sampleStride) &&
      cloud.pointCount <= RESULT_LIMITS.points,
    'Cloud sampling identity is unavailable',
  );
  const ids = new Set();
  let offset = 0;
  const membership = new Int32Array(cloud.pointCount).fill(-1);
  const selected = options.selectedId ?? null;
  for (const [number, group] of data.groups.entries()) {
    requireThat(
      typeof group.id === 'string' &&
        group.id &&
        !ids.has(group.id) &&
        group.offset === offset &&
        Number.isInteger(group.count) &&
        group.count >= 0,
      'Cloud group ranges and IDs must be valid',
    );
    ids.add(group.id);
    offset += group.count;
    requireThat(offset <= indices.values.length, 'Cloud group indices are truncated');
    for (let i = group.offset; i < offset; i++) {
      const index = indices.values[i];
      requireThat(
        Number.isInteger(index) && index >= 0 && index < cloud.totalPoints,
        'Cloud group index is outside its source',
      );
      if (index % cloud.sampleStride || (selected && group.id !== selected)) continue;
      const displayed = index / cloud.sampleStride;
      if (membership[displayed] === -1) membership[displayed] = number;
    }
  }
  requireThat(
    offset === indices.values.length && (!selected || ids.has(selected)),
    'Cloud group selection or complete index payload is invalid',
  );
  const colors = new Uint8Array(cloud.pointCount * 3),
    visible = new Uint8Array(cloud.pointCount);
  for (let i = 0; i < cloud.pointCount; i++) {
    const group = membership[i];
    visible[i] = selected ? Number(group >= 0) : 1;
    const hex = group < 0 ? '#74828a' : resultColor(data.groups[group].id);
    const rgb = parseInt(hex.slice(1), 16);
    colors.set([rgb >>> 16, (rgb >>> 8) & 255, rgb & 255], i * 3);
  }
  return {
    colors,
    visible,
    groups: data.groups,
    frame: data.frame,
    sourceCloudId: data.sourceCloudId,
    displayedPoints: cloud.pointCount,
    totalPoints: cloud.totalPoints,
  };
}
export function resultSummary(value) {
  if (value?.kind === 'Profile') {
    const descriptor = profileDescriptor(value.descriptor);
    requireThat(
      Number.isInteger(value.count) &&
        Number.isInteger(value.validCount) &&
        value.count >= 0 &&
        value.count <= descriptor.sourceCount &&
        value.validCount >= 0 &&
        value.validCount <= value.count,
      'Invalid Profile sample counts',
    );
    return { title: '轮廓', ...descriptor, count: value.count, validCount: value.validCount };
  }
  const poses = poseCollection(value);
  if (poses)
    return {
      title: poses.dimension === 2 ? '二维工件' : '三维工件',
      frame: poses.frame,
      unit: poses.unit,
      count: poses.items.length,
      items: poses.items.map((p) => ({
        id: p.id,
        name: p.name || p.classLabel || p.id,
        position: p.position,
        size: p.size,
        orientation: poses.dimension === 2 ? p.angle : p.rotation,
        confidence: p.confidence,
        pickable: p.pickable,
        height: poses.dimension === 3 ? p.size[2] : undefined,
      })),
    };
  const source = record(value);
  if (source?.type === 'std.Value.v1' && source.data && Object.hasOwn(source.data, 'value'))
    return { title: source.type, value: source.data.value, unit: source.data.unit };
  return null;
}

export function cameraProjector(value, sourceFrame, transform) {
  const source = record(value),
    data = source?.data;
  requireThat(
    source?.type === 'vision.CameraCalib.v1' &&
      data?.unit === 'm' &&
      typeof data.frame === 'string' &&
      data.frame,
    'A calibrated camera frame in metres is required',
  );
  const k = data.cameraMatrix,
    d = data.distortion;
  requireThat(
    vector(k, 9) &&
      k[0] > 0 &&
      k[4] > 0 &&
      k[1] === 0 &&
      k[3] === 0 &&
      k[6] === 0 &&
      k[7] === 0 &&
      k[8] === 1 &&
      vector(data.imageSize, 2) &&
      data.imageSize.every((n) => Number.isInteger(n) && n > 0),
    'Invalid camera intrinsics or resolution',
  );
  requireThat(
    Array.isArray(d) && [0, 4, 5, 8, 12, 14].includes(d.length) && d.every(Number.isFinite),
    'Unsupported camera distortion',
  );
  let m = null;
  if (sourceFrame !== data.frame) {
    requireThat(
      transform?.fromFrame === sourceFrame &&
        transform?.toFrame === data.frame &&
        vector(transform.matrix, 16),
      'Projection requires an explicit source-to-camera transform',
    );
    m = transform.matrix;
    requireThat(
      m.slice(12).every((n, i) => Math.abs(n - (i === 3 ? 1 : 0)) < 1e-8),
      'Projection requires an affine rigid transform',
    );
    for (let a = 0; a < 3; a++)
      for (let b = 0; b < 3; b++)
        requireThat(
          Math.abs(m[a] * m[b] + m[4 + a] * m[4 + b] + m[8 + a] * m[8 + b] - Number(a === b)) <
            1e-6,
          'Projection transform must be rigid',
        );
    const det =
      m[0] * (m[5] * m[10] - m[6] * m[9]) -
      m[1] * (m[4] * m[10] - m[6] * m[8]) +
      m[2] * (m[4] * m[9] - m[5] * m[8]);
    requireThat(det > 0, 'Projection transform must preserve handedness');
  }
  const coeff = (n) => d[n] ?? 0;
  const project = (point) => {
    let [x, y, z] = point;
    if (m)
      [x, y, z] = [
        m[0] * x + m[1] * y + m[2] * z + m[3],
        m[4] * x + m[5] * y + m[6] * z + m[7],
        m[8] * x + m[9] * y + m[10] * z + m[11],
      ];
    if (!(z > 1e-9) || ![x, y, z].every(Number.isFinite)) return null;
    x /= z;
    y /= z;
    const r2 = x * x + y * y,
      r4 = r2 * r2,
      r6 = r4 * r2;
    const radial =
      (1 + coeff(0) * r2 + coeff(1) * r4 + coeff(4) * r6) /
      (1 + coeff(5) * r2 + coeff(6) * r4 + coeff(7) * r6);
    let u =
      x * radial +
      2 * coeff(2) * x * y +
      coeff(3) * (r2 + 2 * x * x) +
      coeff(8) * r2 +
      coeff(9) * r4;
    let v =
      y * radial +
      coeff(2) * (r2 + 2 * y * y) +
      2 * coeff(3) * x * y +
      coeff(10) * r2 +
      coeff(11) * r4;
    if (d.length === 14) {
      const cx = Math.cos(d[12]),
        sx = Math.sin(d[12]),
        cy = Math.cos(d[13]),
        sy = Math.sin(d[13]);
      const rz = sy * u - cy * sx * v + cy * cx;
      const rx = cy * u + sy * sx * v - sy * cx;
      const ry = cx * v + sx;
      u = (cy * cx * rx + sy * cx * rz) / rz;
      v = (cy * cx * ry - sx * rz) / rz;
    }
    const result = [k[0] * u + k[2], k[4] * v + k[5]];
    return result.every(Number.isFinite) ? result : null;
  };
  return { project, frame: data.frame, imageSize: data.imageSize };
}

export function projectedPoseScene(value, camera, options = {}) {
  const poses = poseCollection(value);
  requireThat(poses?.dimension === 3, 'A 3D pose record is required');
  const projection = cameraProjector(camera, poses.frame, options.transform);
  const frame = { id: projection.frame, unit: 'px', axes: ['u', 'v'], yDirection: 'down' };
  const layers = [];
  for (const shape of poseGeometry(value, options)) {
    const segments = [];
    const step = shape.segments ? 2 : 1;
    for (let i = 0; i < shape.points.length - 1; i += step) {
      const a = projection.project(shape.points[i]),
        b = projection.project(shape.points[i + 1]);
      if (a && b) segments.push([a, b]);
    }
    if (segments.length)
      layers.push({
        id: `${shape.id}-${layers.length}`,
        instanceId: shape.id,
        label: shape.label,
        frame,
        color: shape.color,
        lineWidth: options.lineWidth,
        kind: 'segments',
        segments,
      });
  }
  if (options.showLabels !== false)
    for (const item of poses.items) {
      if (options.selectedId && item.id !== options.selectedId) continue;
      const position = projection.project(item.position);
      if (position)
        layers.push({
          id: `${item.id}-label`,
          instanceId: item.id,
          frame,
          kind: 'text',
          position,
          text: item.name || item.classLabel || item.id,
          color:
            options.color ??
            resultColor(
              options.colorBy === 'class' ? (item.classLabel ?? item.classId ?? item.id) : item.id,
            ),
        });
    }
  return {
    schemaVersion: 1,
    frame,
    layers,
    view: { bounds: [0, 0, ...projection.imageSize] },
    provenance: {
      sourceFrame: poses.frame,
      cameraFrame: projection.frame,
      projection: 'OpenCV pinhole and declared distortion',
      transform: options.transform ?? null,
    },
  };
}

export function maskPreview(value, masks, posesValue, options = {}) {
  const source = record(value),
    poses = poseCollection(posesValue),
    data = source?.data;
  requireThat(
    source?.type === 'vision.Masks2D.v1' &&
      poses?.dimension === 2 &&
      poses.unit === 'px' &&
      data.frame === poses.frame &&
      data.unit === 'px' &&
      data.layout === 'NHW',
    'Masks and 2D poses must share an explicit pixel frame',
  );
  const [n, h, w] = masks.shape;
  requireThat(
    masks.shape.length === 3 &&
      n === poses.items.length &&
      w === data.width &&
      h === data.height &&
      Array.isArray(data.items) &&
      data.items.length === n &&
      Number.isInteger(w) &&
      Number.isInteger(h) &&
      w > 0 &&
      h > 0 &&
      w * h <= RESULT_LIMITS.memberships &&
      n * h * w <= RESULT_LIMITS.memberships &&
      masks.data.length === n * h * w,
    'Invalid or excessive complete NHW mask tensor',
  );
  const opacity = options.alpha ?? 0.35;
  const pixels = new Uint8ClampedArray(w * h * 4),
    alpha = Math.round(255 * opacity);
  requireThat(
    opacity >= 0 && opacity <= 1 && Number.isFinite(opacity),
    'Mask opacity must be in [0,1]',
  );
  for (let i = 0; i < n; i++) {
    const item = data.items[i];
    requireThat(
      item.id === poses.items[i].id && item.index === i && typeof item.hasMask === 'boolean',
      'Masks must retain ordered instance identities',
    );
    const pose = poses.items[i];
    const color =
      options.color ??
      resultColor(
        options.colorBy === 'class' ? (pose.classLabel ?? pose.classId ?? item.id) : item.id,
      );
    requireThat(/^#[\da-f]{6}$/i.test(color), 'Mask color must be a six-digit hex color');
    const rgb = parseInt(color.slice(1), 16);
    for (let j = 0; j < w * h; j++) {
      const v = masks.data[i * w * h + j];
      requireThat(v === 0 || v === 1, 'Masks must contain finite binary values');
      requireThat(item.hasMask || v === 0, 'An absent instance mask contains pixels');
      if (v && (!options.selectedId || options.selectedId === item.id))
        pixels.set([rgb >>> 16, (rgb >>> 8) & 255, rgb & 255, alpha], j * 4);
    }
  }
  return { width: w, height: h, pixels };
}
