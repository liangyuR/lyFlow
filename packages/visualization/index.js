const esc = (v) =>
  String(v).replace(
    /[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c],
  );
const shorten = (value, count) => {
  const s = String(value);
  return s.length > count ? s.slice(0, Math.max(1, count - 3)) + '...' : s;
};
const finitePoint = (p) =>
  Array.isArray(p) && p.length >= 2 && p.slice(0, 2).every(Number.isFinite);
const validBounds = (b) =>
  Array.isArray(b) && b.length === 4 && b.every(Number.isFinite) && b[2] >= b[0] && b[3] >= b[1];
function validateFrame(f) {
  if (
    !f ||
    !f.id ||
    !f.unit ||
    !Array.isArray(f.axes) ||
    f.axes.length !== 2 ||
    !f.axes.every((x) => typeof x === 'string' && x) ||
    !['up', 'down'].includes(f.yDirection)
  )
    throw new Error('Explicit frame, unit, axes and yDirection are required');
}
function check(scene) {
  if (scene.schemaVersion !== 1 || !Array.isArray(scene.layers))
    throw new Error('Invalid SceneSpec');
  validateFrame(scene.frame);
  if (scene.view?.bounds && !validBounds(scene.view.bounds)) throw new Error('Invalid view bounds');
  for (const layer of scene.layers) {
    validateFrame(layer.frame);
    const same =
      JSON.stringify(layer.frame.axes) === JSON.stringify(scene.frame.axes) &&
      layer.frame.id === scene.frame.id &&
      layer.frame.unit === scene.frame.unit &&
      JSON.stringify(layer.frame.axisUnits) === JSON.stringify(scene.frame.axisUnits) &&
      layer.frame.yDirection === scene.frame.yDirection;
    if (!same && !layer.transform)
      throw new Error(`Layer ${layer.id}: unknown coordinate transform`);
    if (
      layer.transform &&
      (layer.transform.length !== 6 || !layer.transform.every(Number.isFinite))
    )
      throw new Error(`Layer ${layer.id}: invalid transform`);
    if (
      layer.transform &&
      Math.abs(layer.transform[0] * layer.transform[3] - layer.transform[1] * layer.transform[2]) <
        1e-15
    )
      throw new Error(`Layer ${layer.id}: singular transform`);
    if (!['points', 'polyline', 'segments', 'circle', 'box', 'text', 'image'].includes(layer.kind))
      throw new Error(`Unsupported layer ${layer.kind}`);
    if (
      layer.kind === 'circle' &&
      (!finitePoint(layer.center) || !Number.isFinite(layer.radius) || layer.radius < 0)
    )
      throw new Error(`Layer ${layer.id}: invalid circle`);
    if (['box', 'image'].includes(layer.kind) && !validBounds(layer.bounds))
      throw new Error(`Layer ${layer.id}: invalid bounds`);
    if (
      layer.kind === 'image' &&
      !/^data:image\/(png|jpeg|webp);base64,[A-Za-z0-9+/=]+$/.test(layer.dataUrl ?? '')
    )
      throw new Error('Images must be embedded PNG/JPEG/WebP data URLs');
  }
}
function transform(p, l) {
  const t = l.transform;
  return t ? [t[0] * p[0] + t[2] * p[1] + t[4], t[1] * p[0] + t[3] * p[1] + t[5]] : p;
}
function paths(l) {
  if (l.kind === 'circle') {
    const p = geometryPaths({ kind: 'Circle2D', center: l.center, radius: l.radius }, 1)[0].points;
    const q = Array.from({ length: p.length / 3 }, (_, i) => [p[i * 3], p[i * 3 + 1]]);
    return [[...q, q[0]]];
  }
  if (l.kind === 'segments') return l.segments ?? [];
  if (l.kind === 'box' || l.kind === 'image') {
    const [a, b, c, d] = l.bounds;
    return [
      [
        [a, b],
        [c, b],
        [c, d],
        [a, d],
        [a, b],
      ],
    ];
  }
  if (l.kind === 'text') return [[l.position]];
  return [l.points ?? []];
}
export function sceneBounds(scene) {
  check(scene);
  if (scene.view?.bounds) return [...scene.view.bounds];
  let b = [Infinity, Infinity, -Infinity, -Infinity];
  for (const l of scene.layers)
    for (const path of paths(l))
      for (const p of path)
        if (finitePoint(p)) {
          const [x, y] = transform(p, l);
          if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
          b = [Math.min(b[0], x), Math.min(b[1], y), Math.max(b[2], x), Math.max(b[3], y)];
        }
  if (!b.every(Number.isFinite)) return [-1, -1, 1, 1];
  const size = Math.max(b[2] - b[0], b[3] - b[1]) || 1;
  const padX =
    (scene.view?.equalAspect === false ? b[2] - b[0] || Math.abs(b[0]) || 1 : size) * 0.04;
  const padY =
    (scene.view?.equalAspect === false ? b[3] - b[1] || Math.abs(b[1]) || 1 : size) * 0.04;
  return [b[0] - padX, b[1] - padY, b[2] + padX, b[3] + padY];
}
export function sceneMetadata(scene) {
  return {
    bounds: sceneBounds(scene),
    frame: scene.frame,
    provenance: scene.provenance,
    annotations: scene.annotations,
    equalAspect: scene.view?.equalAspect !== false,
    layers: scene.layers.map((l) => ({
      id: l.id,
      source: l.source,
      missing: l.missing,
      displayedPoints:
        l.kind === 'points' ? (l.points ?? []).filter(finitePoint).length : undefined,
      totalPoints: l.totalPoints,
      nonFinitePoints:
        l.kind === 'points' ? (l.points ?? []).filter((p) => !finitePoint(p)).length : undefined,
    })),
  };
}
function dimensions(scene) {
  const width = scene.view?.width ?? 1100,
    height = scene.view?.height ?? 700;
  if (
    !Number.isInteger(width) ||
    !Number.isInteger(height) ||
    width < 200 ||
    height < 160 ||
    width > 8192 ||
    height > 8192
  )
    throw new Error('Image dimensions must be integers within 200x160 and 8192x8192');
  return { width, height };
}
export function renderSvg(scene) {
  const b = sceneBounds(scene),
    { width: w, height: h } = dimensions(scene);
  const legendRows = scene.view?.legendRows ?? Math.ceil(scene.layers.length / 3),
    bottom = 65 + Math.min(legendRows, 8) * 19;
  const left = 85,
    top = 55 + Math.min(scene.view?.annotationRows ?? scene.annotations?.length ?? 0, 8) * 16,
    pw = w - left - 35,
    ph = Math.max(30, h - top - bottom);
  const scale = Math.min(pw / Math.max(b[2] - b[0], 1e-12), ph / Math.max(b[3] - b[1], 1e-12));
  const scaleX = scene.view?.equalAspect === false ? pw / Math.max(b[2] - b[0], 1e-12) : scale;
  const scaleY = scene.view?.equalAspect === false ? ph / Math.max(b[3] - b[1], 1e-12) : scale;
  const cx = (b[0] + b[2]) / 2,
    cy = (b[1] + b[3]) / 2;
  const project = ([x, y]) => [
    left + pw / 2 + (x - cx) * scaleX,
    top + ph / 2 + (y - cy) * scaleY * (scene.frame.yDirection === 'down' ? 1 : -1),
  ];
  const colors = ['#2563eb', '#e65c20', '#188554', '#b633b8', '#d49b00', '#0094a8'];
  let body = `<rect width="${w}" height="${h}" fill="#ffffff"/><text x="24" y="30" font-size="18" font-weight="600">${esc(shorten(scene.title ?? 'LyFlow output', Math.floor((w - 48) / 10)))}</text><defs><clipPath id="plot"><rect x="${left}" y="${top}" width="${pw}" height="${ph}"/></clipPath></defs>`;
  for (const [i, annotation] of (scene.annotations ?? []).slice(0, 8).entries())
    body += `<text x="24" y="${48 + i * 16}" font-size="12">${esc(shorten(annotation, Math.floor((w - 48) / 7)))}</text>`;
  for (let i = 0; i <= 5; i++) {
    const x = b[0] + ((b[2] - b[0]) * i) / 5,
      y = b[1] + ((b[3] - b[1]) * i) / 5;
    const px = project([x, cy])[0],
      py = project([cx, y])[1];
    body += `<path d="M${px},${top}V${top + ph}M${left},${py}H${left + pw}" stroke="#e8edf3"/><text x="${px}" y="${top + ph + 20}" text-anchor="middle" font-size="11">${Number(x.toPrecision(5))}</text><text x="${left - 8}" y="${py + 4}" text-anchor="end" font-size="11">${Number(y.toPrecision(5))}</text>`;
  }
  body += `<rect x="${left}" y="${top}" width="${pw}" height="${ph}" fill="none" stroke="#aab5c4"/><text x="${left + pw / 2}" y="${top + ph + 40}" text-anchor="middle" font-size="12">${esc(scene.frame.axes[0])} (${esc(scene.frame.axisUnits?.[0] ?? scene.frame.unit)})</text><text x="18" y="${top + ph / 2}" transform="rotate(-90 18 ${top + ph / 2})" text-anchor="middle" font-size="12">${esc(scene.frame.axes[1])} (${esc(scene.frame.axisUnits?.[1] ?? scene.frame.unit)})</text><g clip-path="url(#plot)">`;
  scene.layers.forEach((l, i) => {
    const color = esc(l.color ?? colors[i % colors.length]);
    const projectLayer = (p) => project(transform(p, l));
    const opacity = Number.isFinite(l.opacity) ? Math.max(0, Math.min(1, l.opacity)) : 1;
    const width = Number.isFinite(l.lineWidth) ? Math.max(0.1, Math.min(20, l.lineWidth)) : 1.7;
    body += `<g opacity="${opacity}" stroke="${color}" fill="none" stroke-width="${width}">`;
    if (l.kind === 'image') {
      const [x0, y0, x1, y1] = l.bounds;
      const p = projectLayer([x0, y0]),
        q = projectLayer([x1, y0]),
        r = projectLayer([x0, y1]);
      body += `<image href="${l.dataUrl}" width="1" height="1" preserveAspectRatio="none" transform="matrix(${q[0] - p[0]} ${q[1] - p[1]} ${r[0] - p[0]} ${r[1] - p[1]} ${p[0]} ${p[1]})"/>`;
    } else if (l.kind === 'text') {
      if (finitePoint(l.position)) {
        const [x, y] = projectLayer(l.position);
        body += `<text x="${x}" y="${y}" fill="${color}" stroke="none" font-size="12">${esc(l.text ?? '')}</text>`;
      }
    } else if (l.kind === 'points') {
      const radius = Number.isFinite(l.radius) ? Math.max(0.2, Math.min(20, l.radius)) : 1.5;
      for (const [pointIndex, p] of (l.points ?? []).entries())
        if (finitePoint(p)) {
          const [x, y] = projectLayer(p);
          body += `<circle cx="${x}" cy="${y}" r="${radius}" fill="${esc(l.colors?.[pointIndex] ?? l.color ?? colors[i % colors.length])}" stroke="none"/>`;
        }
    } else {
      for (const points of paths(l)) {
        let d = '',
          broken = true;
        for (const p of points) {
          if (!finitePoint(p)) {
            broken = true;
            continue;
          }
          const [x, y] = projectLayer(p);
          d += `${broken ? 'M' : 'L'}${x},${y}`;
          broken = false;
        }
        if (l.closed && points.every(finitePoint)) d += 'Z';
        body += `<path d="${d}"/>`;
      }
    }
    body += '</g>';
  });
  body += '</g>';
  if (!scene.layers.some((l) => paths(l).some((p) => p.some(finitePoint))))
    body += `<text x="${left + pw / 2}" y="${top + ph / 2}" text-anchor="middle" fill="#a13b22">No finite output data</text>`;
  scene.layers.slice(0, 24).forEach((l, i) => {
    const x = 24 + ((i % 3) * (w - 48)) / 3,
      y = h - Math.min(legendRows, 8) * 19 + Math.floor(i / 3) * 19;
    const points =
      l.kind === 'points'
        ? ` (${(l.points ?? []).filter(finitePoint).length}/${l.totalPoints ?? (l.points ?? []).length})`
        : '';
    const bad = (l.points ?? []).filter((p) => !finitePoint(p)).length;
    body += `<text x="${x}" y="${y}" fill="${esc(l.color ?? colors[i % colors.length])}" font-size="11">${esc(shorten(l.label ?? l.id, Math.max(8, Math.floor((w - 48) / 3 / 6) - points.length - 8)) + points + (bad ? ` | ${bad} non-finite` : '') + (l.missing ? ` | MISSING` : ''))}</text>`;
  });
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" font-family="Arial, sans-serif" fill="#253247">${body}</svg>`;
}
export function gridScenes(scenes, options = {}) {
  if (!scenes.length || scenes.length > 100) throw new Error('Grid requires 1..100 scenes');
  const columns = options.columns ?? Math.min(2, scenes.length),
    width = options.width ?? 1600,
    cellHeight = options.cellHeight ?? 500;
  if (
    !Number.isInteger(columns) ||
    columns < 1 ||
    columns > 8 ||
    !Number.isInteger(width) ||
    width < 400 ||
    width > 8192 ||
    !Number.isInteger(cellHeight) ||
    cellHeight < 160 ||
    cellHeight > 2048
  )
    throw new Error('Invalid grid dimensions');
  const cellWidth = Math.floor(width / columns),
    header = options.title ? 45 : 0,
    height = header + Math.ceil(scenes.length / columns) * cellHeight;
  if (height > 32768) throw new Error('Grid too tall; paginate the results');
  let bounds = Array.isArray(options.sharedBounds) ? options.sharedBounds : undefined;
  if (options.sharedBounds === true) {
    const frame = JSON.stringify(scenes[0].frame);
    if (scenes.some((s) => JSON.stringify(s.frame) !== frame))
      throw new Error('Shared bounds require identical frames');
    bounds = scenes
      .map(sceneBounds)
      .reduce((a, b) => [
        Math.min(a[0], b[0]),
        Math.min(a[1], b[1]),
        Math.max(a[2], b[2]),
        Math.max(a[3], b[3]),
      ]);
  }
  const annotationRows = Math.max(...scenes.map((s) => s.annotations?.length ?? 0));
  const legendRows = Math.max(...scenes.map((s) => Math.ceil(s.layers.length / 3)));
  return scenes.map((s) => ({
    ...s,
    view: {
      ...s.view,
      ...(bounds ? { bounds } : {}),
      width: cellWidth,
      height: cellHeight,
      annotationRows,
      legendRows,
    },
  }));
}
export function renderGridSvg(scenes, options = {}) {
  const prepared = gridScenes(scenes, options),
    columns = options.columns ?? Math.min(2, scenes.length),
    width = options.width ?? 1600,
    cellHeight = options.cellHeight ?? 500,
    cellWidth = Math.floor(width / columns),
    header = options.title ? 45 : 0,
    height = header + Math.ceil(scenes.length / columns) * cellHeight;
  const items = prepared.map((s, i) =>
    renderSvg(s)
      .replaceAll('id="plot"', `id="plot-${i}"`)
      .replaceAll('url(#plot)', `url(#plot-${i})`)
      .replace(
        '<svg ',
        `<svg x="${(i % columns) * cellWidth}" y="${header + Math.floor(i / columns) * cellHeight}" `,
      ),
  );
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" font-family="Arial, sans-serif"><rect width="100%" height="100%" fill="white"/>${options.title ? `<text x="24" y="29" font-size="20">${esc(options.title)}</text>` : ''}${items.join('')}</svg>`;
}
export function geometryPaths(v, span) {
  if (!v || v.unit === 'px') return [];
  const path = (points, loop = false) => ({ points, loop });
  switch (v.kind) {
    case 'Box2D': {
      const [x0, y0] = v.min ?? [0, 0],
        [x1, y1] = v.max ?? [0, 0];
      return [path([x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0], true)];
    }
    case 'Line2D': {
      if (v.hasSegment && v.start && v.end)
        return [path([v.start[0], v.start[1], 0, v.end[0], v.end[1], 0])];
      const [x, y] = v.point ?? [0, 0],
        [dx, dy] = v.dir ?? [1, 0],
        n = Math.hypot(dx, dy) || 1,
        h = span / 2;
      return [path([x - (dx / n) * h, y - (dy / n) * h, 0, x + (dx / n) * h, y + (dy / n) * h, 0])];
    }
    case 'Circle2D': {
      const [x, y] = v.center ?? [0, 0],
        r = v.radius ?? 0,
        points = [];
      for (let i = 0; i < 72; i++) {
        const t = (i * Math.PI) / 36;
        points.push(x + r * Math.cos(t), y + r * Math.sin(t), 0);
      }
      return [path(points, true)];
    }
    case 'Point2D': {
      const [x, y] = v.p ?? [0, 0],
        s = span * 0.01 || 0.001;
      return [path([x - s, y, 0, x + s, y, 0]), path([x, y - s, 0, x, y + s, 0])];
    }
    default:
      return [];
  }
}
