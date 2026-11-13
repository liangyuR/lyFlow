import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { z } from 'zod';
import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { sceneBounds, type SceneSpec, type Layer } from '@lyflow/visualization';
import { renderPng, renderGridPng } from '@lyflow/visualization/node';
import type { Config } from './config.js';
import { HttpError, type LyFlowHttp } from './http.js';
import { decodeCloud } from './cloud.js';
import { fetchImage, toPicture, encodePng } from './image.js';
import { EvaluationStore, readRows, rowFailed } from './evaluations.js';
import { object } from './summary.js';
import { digest } from './inspect.js';

export const captureSchema = z
  .object({
    policy: z.enum(['all', 'selected', 'failures', 'metrics']).default('all'),
    outputs: z.array(z.object({ nodeId: z.string().min(1), port: z.string().min(1) })).optional(),
    maxBytes: z.number().int().nonnegative().max(2147483648).optional(),
  })
  .refine((c) => c.policy !== 'selected' || !!c.outputs?.length, 'selected requires outputs');
const pair = z.tuple([z.number().finite(), z.number().finite()]);
const boundsSchema = z.tuple([
  z.number().finite(),
  z.number().finite(),
  z.number().finite(),
  z.number().finite(),
]);
const frameSchema = z.object({
  id: z.string().min(1),
  unit: z.string().min(1),
  axes: z.tuple([z.string().min(1), z.string().min(1)]),
  yDirection: z.enum(['up', 'down']),
});
const layerSchema = z.object({
  nodeId: z.string(),
  port: z.string(),
  label: z.string().optional(),
  color: z.string().optional(),
  frame: frameSchema,
  axes: z
    .tuple([z.enum(['x', 'y', 'z']), z.enum(['x', 'y', 'z'])])
    .optional()
    .describe('PointCloud projection; must be explicit'),
  transform: z
    .tuple([z.number(), z.number(), z.number(), z.number(), z.number(), z.number()])
    .optional(),
  kind: z.enum(['points', 'polyline', 'segments', 'circle', 'box', 'text', 'overlay2d']).optional(),
  valuePath: z.string().optional().describe('JSON pointer within captured output.value'),
  fields: z
    .record(z.string())
    .optional()
    .describe(
      'Geometry fields -> JSON pointers in selected value; all coordinates come from stored output',
    ),
  position: pair.optional().describe('Presentation position for a scalar text label'),
});
const viewSchema = z.object({
  bounds: boundsSchema.optional(),
  width: z.number().int().min(320).max(4096).optional(),
  height: z.number().int().min(240).max(4096).optional(),
});
const sceneInput = {
  imageRange: pair
    .optional()
    .describe('Shared image intensity range; default computed from captured image'),
  frame: frameSchema,
  layers: z.array(layerSchema).min(1).max(32),
  view: viewSchema.optional(),
  maxPoints: z.number().int().min(1).max(200000).default(50000),
};
type LayerInput = z.infer<typeof layerSchema>;
export type ViewInput = {
  imageRange?: [number, number] | undefined;
  frame: z.infer<typeof frameSchema>;
  layers: LayerInput[];
  view?: z.infer<typeof viewSchema> | undefined;
  maxPoints?: number | undefined;
};
export interface RunSnapshot {
  schemaVersion: number;
  snapshotId: string;
  runId: string;
  status: string;
  inputs: Record<string, unknown>[];
  outputs: Record<string, unknown>[];
  effectiveParams: unknown;
  summary?: unknown;
  engine?: unknown;
}

export function displayNumber(value: unknown): string {
  return typeof value === 'number' && Number.isFinite(value)
    ? String(Number(value.toPrecision(6)))
    : String(value);
}

export function pointer(value: unknown, p = ''): unknown {
  if (!p) return value;
  if (!p.startsWith('/')) throw new Error('valuePath/fields use JSON pointers beginning with /');
  let result = value;
  for (const raw of p.slice(1).split('/')) {
    const key = raw.replace(/~1/g, '/').replace(/~0/g, '~');
    if (result === null || typeof result !== 'object' || !Object.hasOwn(result, key))
      throw new Error(`Stored output has no field ${p}`);
    result = (result as Record<string, unknown>)[key];
  }
  return result;
}
export function inputIdentity(snapshot: RunSnapshot): string {
  if (!snapshot.inputs.length) throw new Error('Snapshot has no verifiable input identity');
  const identities = snapshot.inputs
    .map((i) => {
      if (typeof i['sha256'] !== 'string' || i['unchangedDuringRun'] !== true)
        throw new Error('Input identity is unavailable or changed during execution');
      return [i['sha256'], i['bytes']];
    })
    .sort((a, b) => JSON.stringify(a).localeCompare(JSON.stringify(b)));
  return digest(identities);
}
export function visualizationCapability(output: Record<string, unknown>) {
  return {
    tool: 'view_outputs',
    supported:
      output['type'] === 'PointCloud' ||
      output['type'] === 'Image' ||
      output['value'] !== undefined,
    requiresExplicitFrame: true,
    requiresProjection: output['type'] === 'PointCloud',
    missing: output['missing'] ?? null,
  };
}
const colors = ['#34b3e4', '#f6b44b', '#8dd47a', '#dc83ce', '#e47171'];
function fieldGeometry(value: unknown, spec: LayerInput): Record<string, unknown> {
  const selected = pointer(value, spec.valuePath);
  if (spec.fields)
    return Object.fromEntries(
      Object.entries(spec.fields).map(([name, p]) => [name, pointer(selected, p)]),
    );
  if (spec.kind === 'text')
    return {
      text: typeof selected === 'object' ? JSON.stringify(selected) : String(selected),
      position: spec.position ?? [0, 0],
    };
  if (Array.isArray(selected)) return { points: selected };
  return object(selected);
}
function overlayLayers(value: unknown, base: Record<string, unknown>): Record<string, unknown>[] {
  const rec = object(value),
    data = object(rec['data']);
  if (
    rec['type'] !== 'lyflow.overlay2d' ||
    data['frame'] !== 'image' ||
    !Array.isArray(data['items'])
  )
    throw new Error('Expected captured lyflow.overlay2d frame=image');
  return data['items'].map((raw, i) => {
    const item = object(raw),
      kind = item['kind'];
    const common = {
      ...base,
      id: `${base['id']}-${i}`,
      kind,
      label: item['label'] ?? item['role'] ?? base['label'],
    };
    if (kind === 'segments') {
      const points = item['points'] as unknown[];
      if (!Array.isArray(points) || points.length % 2) throw new Error('Invalid stored segments');
      return {
        ...common,
        segments: Array.from({ length: points.length / 2 }, (_, j) => [
          points[j * 2],
          points[j * 2 + 1],
        ]),
      };
    }
    if (kind === 'box') {
      const a = item['min'] as number[],
        b = item['max'] as number[];
      return { ...common, bounds: [a?.[0], a?.[1], b?.[0], b?.[1]] };
    }
    if (kind === 'text') return { ...common, position: item['at'], text: item['text'] };
    return { ...item, ...common };
  });
}
export class Visualizations {
  readonly root: string;
  constructor(
    readonly config: Config,
    readonly http: LyFlowHttp,
    readonly evaluations: EvaluationStore,
  ) {
    this.root = path.join(config.workDir, 'visualizations');
  }
  async snapshot(id: string): Promise<RunSnapshot> {
    return this.http.getJson<RunSnapshot>(`/lyflow/snapshots/${encodeURIComponent(id)}`);
  }
  async scene(
    id: string,
    input: ViewInput,
  ): Promise<{
    scene: SceneSpec;
    snapshot: RunSnapshot;
    missing: unknown[];
    statistics: unknown[];
    assessments: unknown[];
  }> {
    const snapshot = await this.snapshot(id),
      layers: Record<string, unknown>[] = [],
      missing: unknown[] = [],
      statistics: unknown[] = [],
      annotations: string[] = [];
    for (const [index, spec] of input.layers.entries()) {
      const output = snapshot.outputs.find(
        (o) => o['nodeId'] === spec.nodeId && o['port'] === spec.port,
      );
      if (!output || output['missing']) {
        missing.push({
          nodeId: spec.nodeId,
          port: spec.port,
          reason: output?.['missing'] ?? 'output_unavailable',
        });
        continue;
      }
      const base = {
        id: `${spec.nodeId}:${spec.port}:${index}`,
        label: spec.label ?? `${spec.nodeId}:${spec.port}`,
        frame: spec.frame,
        color: spec.color ?? colors[index % colors.length],
        source: { snapshotId: id, nodeId: spec.nodeId, port: spec.port },
        ...(spec.transform ? { transform: spec.transform } : {}),
      };
      const prefix = `/lyflow/snapshots/${encodeURIComponent(id)}`;
      if (output['type'] === 'PointCloud') {
        if (!spec.axes || spec.axes[0] === spec.axes[1])
          throw new Error('PointCloud layers require two distinct explicit axes');
        const response = await this.http.request(
          `${prefix}/clouds/${encodeURIComponent(spec.nodeId)}/${encodeURIComponent(spec.port)}?maxPoints=${input.maxPoints ?? 50000}`,
        );
        const cloud = decodeCloud(await response.arrayBuffer()),
          axes = spec.axes.map((a) => ({ x: 0, y: 1, z: 2 })[a]);
        const points: number[][] = [];
        let nonFinite = 0;
        for (let i = 0; i < cloud.pointCount; i++) {
          const p = [cloud.xyz[i * 3 + axes[0]!]!, cloud.xyz[i * 3 + axes[1]!]!];
          if (p.every(Number.isFinite)) points.push(p);
          else nonFinite++;
        }
        layers.push({
          ...base,
          kind: 'points',
          points,
          totalPoints: cloud.totalPoints,
          radius: 1.3,
        });
        statistics.push({
          nodeId: spec.nodeId,
          port: spec.port,
          totalPoints: cloud.totalPoints,
          receivedPoints: cloud.pointCount,
          displayedPoints: points.length,
          nonFinite,
          downsampled: cloud.totalPoints > cloud.pointCount,
          axes: spec.axes,
        });
      } else if (output['type'] === 'Image') {
        const image = await fetchImage(
          async (_level, row) =>
            (
              await this.http.request(
                `${prefix}/images/${encodeURIComponent(spec.nodeId)}/${encodeURIComponent(spec.port)}?row=${row}&rows=0`,
              )
            ).arrayBuffer(),
          0,
        );
        const p = toPicture(
          image.first.width,
          image.first.height,
          image.first.channels,
          image.first.depth,
          image.values,
          2048,
          input.imageRange,
        );
        layers.push({
          ...base,
          kind: 'image',
          bounds: [0, 0, image.first.fullWidth, image.first.fullHeight],
          dataUrl: `data:image/png;base64,${encodePng(p).toString('base64')}`,
        });
        statistics.push({
          nodeId: spec.nodeId,
          port: spec.port,
          width: image.first.fullWidth,
          height: image.first.fullHeight,
          range: p.range,
        });
      } else if (spec.kind === 'overlay2d') {
        if (spec.frame.id !== 'image' || spec.frame.unit !== 'px')
          throw new Error('overlay2d requires image frame in px');
        layers.push(...overlayLayers(pointer(output['value'], spec.valuePath), base));
      } else {
        const value = object(output['value']);
        if (!spec.kind) {
          const kind = value['kind'] ?? output['type'];
          if (kind === 'Line2D' && value['hasSegment'] === true)
            layers.push({ ...base, kind: 'segments', segments: [[value['start'], value['end']]] });
          else if (kind === 'Circle2D')
            layers.push({
              ...base,
              kind: 'circle',
              center: value['center'],
              radius: value['radius'],
            });
          else if (kind === 'Point2D')
            layers.push({ ...base, kind: 'points', points: [value['p']], radius: 4 });
          else if (kind === 'Measurement') {
            const text = `${base.label}: ${displayNumber(value['value'])} ${value['unit'] ?? ''} (${value['ok'] === true ? 'valid' : value['ok'] === false ? 'invalid' : 'validity unknown'}${value['verdict'] === undefined ? '' : `; verdict=${String(value['verdict'])}`})`;
            if (spec.position)
              layers.push({ ...base, kind: 'text', position: spec.position, text });
            else annotations.push(text);
          } else
            throw new Error(
              `Choose explicit geometry kind and field pointers for ${spec.nodeId}:${spec.port}`,
            );
        } else layers.push({ ...fieldGeometry(output['value'], spec), ...base, kind: spec.kind });
      }
    }
    const scene = {
      schemaVersion: 1,
      title: `${id} · ${snapshot.status}${missing.length ? ` · ${missing.length} missing outputs` : ''}`,
      frame: input.frame,
      annotations,
      layers: layers as unknown as Layer[],
      ...(input.view ? { view: input.view } : {}),
      provenance: {
        snapshotId: id,
        status: snapshot.status,
        engine: snapshot.engine,
        statistics,
        missing,
      },
    } as SceneSpec;
    const assessments = snapshot.outputs
      .filter((o) => input.layers.some((l) => l.nodeId === o['nodeId'] && l.port === o['port']))
      .flatMap((o) => {
        const value = object(o['value']);
        return (value['kind'] ?? o['type']) === 'Measurement'
          ? [
              {
                nodeId: o['nodeId'],
                port: o['port'],
                valid: typeof value['ok'] === 'boolean' ? value['ok'] : null,
                ...(value['verdict'] === undefined ? {} : { verdict: value['verdict'] }),
                value: value['value'],
                unit: value['unit'],
              },
            ]
          : [];
      });
    scene.provenance = { ...object(scene.provenance), assessments };
    return { scene, snapshot, missing, statistics, assessments };
  }
  save(png: Buffer, scene: unknown, svg: string) {
    fs.mkdirSync(this.root, { recursive: true });
    const id = crypto.randomUUID(),
      base = path.join(this.root, id);
    fs.writeFileSync(`${base}.png`, png, { flag: 'wx' });
    fs.writeFileSync(`${base}.scene.json`, JSON.stringify(scene), { flag: 'wx' });
    fs.writeFileSync(`${base}.svg`, svg, { flag: 'wx' });
    return {
      imagePath: `${base}.png`,
      scenePath: `${base}.scene.json`,
      svgPath: `${base}.svg`,
      bytes: png.length,
    };
  }
  async view(id: string, input: ViewInput) {
    const result = await this.scene(id, input),
      rendered = await renderPng(result.scene);
    return {
      png: rendered.png,
      details: {
        snapshotId: id,
        ...this.save(rendered.png, result.scene, rendered.svg),
        missing: result.missing,
        statistics: result.statistics,
        assessments: result.assessments,
        metadata: rendered.metadata,
      },
    };
  }
}
function response(png: Buffer, details: Record<string, unknown>) {
  return {
    content: [
      { type: 'image' as const, mimeType: 'image/png', data: png.toString('base64') },
      { type: 'text' as const, text: JSON.stringify(details) },
    ],
    structuredContent: details,
  };
}
function failed(e: unknown) {
  return {
    isError: true,
    content: [
      {
        type: 'text' as const,
        text: JSON.stringify({ error: e instanceof Error ? e.message : String(e) }),
      },
    ],
  };
}
export function metricValue(row: Record<string, unknown>, metric: string): number | null {
  const metrics = object(row['metrics']);
  const v = metrics[metric];
  if (typeof v === 'number' && Number.isFinite(v)) return v;
  const n = object(v)['value'];
  return typeof n === 'number' && Number.isFinite(n) ? n : null;
}
export function registerVisualizationTools(
  server: McpServer,
  config: Config,
  http: LyFlowHttp,
  evaluations: EvaluationStore,
) {
  const visuals = new Visualizations(config, http, evaluations);
  server.registerTool(
    'view_outputs',
    {
      title: '从不可变运行快照出图',
      description:
        '从同次运行保存的点云、图像和实际几何输出原生渲染。必须声明坐标系、单位及投影轴；不重新执行或重新拟合。缺失输出明确列出。直接返回PNG与高清文件、SceneSpec。',
      inputSchema: { snapshotId: z.string(), ...sceneInput },
    },
    async (a) => {
      try {
        const r = await visuals.view(a.snapshotId, a);
        return response(r.png, r.details);
      } catch (e) {
        return failed(e);
      }
    },
  );
  server.registerTool(
    'compare_views',
    {
      title: '同输入快照 A/B 原生对比',
      description:
        '核对输入内容身份并使用统一坐标范围。a/b layers可显式映射不同节点端口；geometry从保存结果读取。overlay时A蓝B橙。',
      inputSchema: {
        a: z.object({ snapshotId: z.string(), layers: z.array(layerSchema).min(1).max(32) }),
        b: z.object({ snapshotId: z.string(), layers: z.array(layerSchema).min(1).max(32) }),
        frame: frameSchema,
        view: viewSchema.optional(),
        imageRange: pair.optional(),
        maxPoints: sceneInput.maxPoints,
        layout: z.enum(['side-by-side', 'overlay']).default('side-by-side'),
      },
    },
    async (a) => {
      try {
        let [left, right] = await Promise.all([
          visuals.scene(a.a.snapshotId, { ...a, layers: a.a.layers }),
          visuals.scene(a.b.snapshotId, { ...a, layers: a.b.layers }),
        ]);
        const ranges = [...left.statistics, ...right.statistics]
          .map((x) => object(x)['range'])
          .filter((r): r is [number, number] => Array.isArray(r) && r.length === 2);
        const imageRange =
          a.imageRange ??
          (ranges.length
            ? ([Math.min(...ranges.map((r) => r[0])), Math.max(...ranges.map((r) => r[1]))] as [
                number,
                number,
              ])
            : undefined);
        if (imageRange)
          [left, right] = await Promise.all([
            visuals.scene(a.a.snapshotId, { ...a, imageRange, layers: a.a.layers }),
            visuals.scene(a.b.snapshotId, { ...a, imageRange, layers: a.b.layers }),
          ]);
        if (inputIdentity(left.snapshot) !== inputIdentity(right.snapshot))
          throw new Error('A/B inputs differ; not a same-sample comparison');
        const lb = sceneBounds(left.scene),
          rb = sceneBounds(right.scene),
          bounds = a.view?.bounds ?? [
            Math.min(lb[0], rb[0]),
            Math.min(lb[1], rb[1]),
            Math.max(lb[2], rb[2]),
            Math.max(lb[3], rb[3]),
          ];
        const scenes = [left.scene, right.scene].map((s, i) => ({
          ...s,
          title: `${i ? 'B' : 'A'} · ${s.title}`,
          view: { ...s.view, bounds },
          layers:
            a.layout === 'overlay'
              ? s.layers.map((l) => ({ ...l, color: i ? '#ed9b40' : '#37aee0' }))
              : s.layers,
        })) as SceneSpec[];
        const rendered =
          a.layout === 'overlay'
            ? await renderPng({
                ...scenes[0]!,
                title: 'A / B · same input',
                annotations: scenes.flatMap((s, i) =>
                  (s.annotations ?? []).map((t) => `${i ? 'B' : 'A'} ${t}`),
                ),
                layers: scenes.flatMap((s, i) =>
                  s.layers.map((l) => ({
                    ...l,
                    id: `${i}:${l.id}`,
                    label: `${i ? 'B' : 'A'} ${l.label ?? l.id}`,
                  })),
                ),
              })
            : await renderGridPng(scenes, { columns: 2, sharedBounds: true });
        return response(rendered.png, {
          a: a.a.snapshotId,
          b: a.b.snapshotId,
          inputIdentity: inputIdentity(left.snapshot),
          ...visuals.save(rendered.png, scenes, rendered.svg),
          effectiveParams: { a: left.snapshot.effectiveParams, b: right.snapshot.effectiveParams },
          summary: { a: left.snapshot.summary, b: right.snapshot.summary },
          missing: { a: left.missing, b: right.missing },
          statistics: { a: left.statistics, b: right.statistics },
          assessments: { a: left.assessments, b: right.assessments },
          bounds,
        });
      } catch (e) {
        return failed(e);
      }
    },
  );
  server.registerTool(
    'view_evaluation',
    {
      title: '分页查看批量复测图像',
      description:
        '读取已有eval逐样本快照，不补跑。按指标或相对基线的绝对变化排序，筛选失败、sample与参数组；PNG缩略图及指标图保留snapshotId可下钻。',
      inputSchema: {
        evaluationId: z.string(),
        ...sceneInput,
        metric: z.string().optional(),
        metricUnit: z
          .string()
          .min(1)
          .optional()
          .describe('Explicit metric unit for plot; otherwise shown as unspecified'),
        baselineEvaluationId: z.string().optional(),
        baselineParamSet: z.number().int().nonnegative().default(0),
        paramSet: z.number().int().nonnegative().optional(),
        failuresOnly: z.boolean().default(false),
        sample: z.string().optional(),
        order: z.enum(['ascending', 'descending']).default('descending'),
        offset: z.number().int().nonnegative().default(0),
        limit: z.number().int().min(1).max(20).default(8),
      },
    },
    async (a) => {
      try {
        const { state } = evaluations.load(a.evaluationId),
          rows: Record<string, unknown>[] = [],
          baseline = new Map<string, Record<string, unknown>>();
        if (a.baselineEvaluationId) {
          evaluations.load(a.baselineEvaluationId);
          await readRows(path.join(evaluations.dir(a.baselineEvaluationId), 'rows.jsonl'), (r) => {
            if (r['paramSet'] === a.baselineParamSet) baseline.set(String(r['sample']), r);
          });
        }
        await readRows(path.join(evaluations.dir(a.evaluationId), 'rows.jsonl'), (r) => {
          if (
            (a.paramSet === undefined || r['paramSet'] === a.paramSet) &&
            (!a.failuresOnly || rowFailed(r)) &&
            (a.sample === undefined || r['sample'] === a.sample)
          )
            rows.push(r);
        });
        const score = (r: Record<string, unknown>) => {
          if (!a.metric) return null;
          const v = metricValue(r, a.metric);
          if (!a.baselineEvaluationId) return v;
          const b = baseline.get(String(r['sample']));
          const bv = b ? metricValue(b, a.metric) : null;
          return v === null || bv === null ? null : Math.abs(v - bv);
        };
        if (a.metric)
          rows.sort((x, y) => {
            const u = score(x),
              v = score(y);
            return u === null
              ? v === null
                ? 0
                : 1
              : v === null
                ? -1
                : a.order === 'ascending'
                  ? u - v
                  : v - u;
          });
        const page = rows.slice(a.offset, a.offset + a.limit),
          scenes: SceneSpec[] = [],
          entries: Record<string, unknown>[] = [];
        for (const row of page) {
          const entry: Record<string, unknown> = {
            sample: row['sample'],
            paramSet: row['paramSet'],
            snapshotId: row['snapshotId'] ?? null,
            status: row['status'],
            summaryStatus: row['summaryStatus'],
            quality: row['quality'],
            metrics: row['metrics'],
            sortValue: score(row),
          };
          try {
            if (typeof row['snapshotId'] === 'string') {
              const result = await visuals.scene(row['snapshotId'], a);
              result.scene.title = `${row['sample']} · set ${row['paramSet']} · ${row['status']}`;
              if (a.metric) {
                const value = metricValue(row, a.metric),
                  prior = baseline.get(String(row['sample'])),
                  baselineValue = prior ? metricValue(prior, a.metric) : null;
                const delta =
                  value === null || baselineValue === null ? null : value - baselineValue;
                const unit = a.metricUnit ? ` ${a.metricUnit}` : '';
                let annotation = `${a.metric}: ${value === null ? 'unavailable' : displayNumber(value) + unit}`;
                if (a.baselineEvaluationId)
                  annotation +=
                    delta === null
                      ? ' · delta unavailable'
                      : ` · delta ${delta > 0 ? '+' : ''}${displayNumber(delta)}${unit} (|delta| ${displayNumber(Math.abs(delta))}${unit})`;
                result.scene.annotations = [annotation, ...(result.scene.annotations ?? [])];
                result.scene.provenance = {
                  ...object(result.scene.provenance),
                  evaluationMetric: {
                    path: a.metric,
                    value,
                    unit: a.metricUnit ?? null,
                    baselineValue,
                    delta,
                    sortValue: score(row),
                  },
                };
              }
              const b = baseline.get(String(row['sample']));
              if (b && typeof b['snapshotId'] === 'string') {
                const old = await visuals.snapshot(b['snapshotId']);
                if (inputIdentity(old) !== inputIdentity(result.snapshot))
                  throw new Error(`Baseline input mismatch for ${row['sample']}`);
                entry['baselineSnapshotId'] = b['snapshotId'];
              }
              scenes.push(result.scene);
              entry['missing'] = result.missing;
              entry['statistics'] = result.statistics;
              entry['assessments'] = result.assessments;
            } else
              entry['missing'] =
                'snapshot_not_captured; explicitly run a new evaluation with capture';
          } catch (e) {
            if (!(e instanceof HttpError) || ![404, 410].includes(e.status)) throw e;
            entry['missing'] = {
              reason: 'snapshot_unavailable',
              httpStatus: e.status,
              message: e.message,
            };
            entry['comparisonVerified'] = false;
          }
          if (
            typeof entry['missing'] === 'string' ||
            (!Array.isArray(entry['missing']) && entry['missing'])
          ) {
            scenes.push({
              schemaVersion: 1,
              title: `${row['sample']} · set ${row['paramSet']} · ${row['status']}`,
              frame: a.frame,
              layers: [],
              annotations: [
                typeof entry['missing'] === 'string'
                  ? entry['missing']
                  : String(object(entry['missing'])['message']),
              ],
              provenance: { snapshotId: row['snapshotId'], missing: entry['missing'] },
            });
          }
          entries.push(entry);
        }
        const details: Record<string, unknown> = {
          evaluationId: a.evaluationId,
          status: state.status,
          total: rows.length,
          offset: a.offset,
          limit: a.limit,
          nextOffset: a.offset + page.length < rows.length ? a.offset + page.length : null,
          metric: a.metric ?? null,
          sortMeaning: a.baselineEvaluationId ? 'absolute metric delta' : 'metric value',
          entries,
        };
        if (!scenes.length)
          return {
            content: [{ type: 'text' as const, text: JSON.stringify(details) }],
            structuredContent: details,
          };
        const populated = scenes.filter((s) => s.layers.length).map(sceneBounds);
        const sharedBounds =
          a.view?.bounds ??
          (populated.length
            ? ([
                Math.min(...populated.map((b) => b[0])),
                Math.min(...populated.map((b) => b[1])),
                Math.max(...populated.map((b) => b[2])),
                Math.max(...populated.map((b) => b[3])),
              ] as [number, number, number, number])
            : true);
        const rendered = await renderGridPng(scenes, {
          columns: 2,
          sharedBounds,
          title: `${a.evaluationId} · ${a.offset + 1}-${a.offset + page.length}/${rows.length}`,
        });
        Object.assign(details, visuals.save(rendered.png, scenes, rendered.svg));
        const preview =
          scenes.length > 8
            ? await renderGridPng(scenes, {
                columns: 2,
                width: 900,
                cellHeight: 220,
                sharedBounds,
                title: `${a.evaluationId} · preview`,
              })
            : rendered;
        const result = response(preview.png, details);
        if (a.metric) {
          const metricFrame = {
            id: 'evaluation-metric',
            unit: a.metricUnit ?? 'unspecified',
            axisUnits: ['rank', a.metricUnit ?? 'unspecified'] as [string, string],
            axes: ['sample rank', a.metric] as [string, string],
            yDirection: 'up' as const,
          };
          const points = page.flatMap((r, i) => {
            const value = score(r);
            return value === null ? [] : [[a.offset + i + 1, value] as [number, number]];
          });
          if (points.length) {
            const plot: SceneSpec = {
              schemaVersion: 1,
              title: `${a.metric}${a.baselineEvaluationId ? ' |delta|' : ''}`,
              frame: metricFrame,
              view: { equalAspect: false },
              layers: [
                { id: 'metric', frame: metricFrame, kind: 'points', points, color: '#34b3e4' },
              ],
              provenance: { evaluationId: a.evaluationId, entries },
            };
            const p = await renderPng(plot);
            details['metricPlot'] = visuals.save(p.png, plot, p.svg);
            result.content.unshift({
              type: 'image',
              mimeType: 'image/png',
              data: p.png.toString('base64'),
            });
            result.content[result.content.length - 1] = {
              type: 'text',
              text: JSON.stringify(details),
            };
          }
        }
        return result;
      } catch (e) {
        return failed(e);
      }
    },
  );
}
