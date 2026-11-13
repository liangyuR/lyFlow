import * as THREE from 'three';
import {
  renderSvg,
  renderGridSvg,
  type Frame,
  type Layer,
  type SceneSpec,
} from '@lyflow/visualization';
import type { Scene } from './cloudScene';
import type { RoiItem } from '../components/RoiLayer';

const frame: Frame = { id: 'editor-world', unit: 'm', axes: ['X', 'Y'], yDirection: 'up' };

function layersOf(root: THREE.Object3D, prefix: string): Layer[] {
  const result: Layer[] = [];
  root.updateWorldMatrix(true, true);
  root.traverseVisible((object) => {
    if (!(object instanceof THREE.Points) && !(object instanceof THREE.Line)) return;
    const attr = object.geometry.getAttribute('position');
    if (!attr) return;
    const materials = Array.isArray(object.material) ? object.material : [object.material];
    const material = materials[0] as THREE.PointsMaterial | THREE.LineBasicMaterial;
    const points: [number, number][] = [];
    const colors: string[] = [];
    const rgb = object.geometry.getAttribute('color');
    const vertex = new THREE.Vector3();
    const color = new THREE.Color();
    const index = object.geometry.index;
    const count = index?.count ?? attr.count;
    for (let n = 0; n < count; n += 1) {
      const i = index ? index.getX(n) : n;
      vertex.fromBufferAttribute(attr, i).applyMatrix4(object.matrixWorld);
      points.push([vertex.x, vertex.y]);
      if (rgb && material.vertexColors)
        colors.push(`#${color.fromBufferAttribute(rgb, i).getHexString()}`);
    }
    const common = {
      id: `${prefix}-${result.length}`,
      label: prefix,
      frame,
      color: material.vertexColors ? '#2563eb' : `#${material.color.getHexString()}`,
      opacity: material.opacity,
    };
    if (object instanceof THREE.Points)
      result.push({
        ...common,
        kind: 'points',
        points,
        colors,
        totalPoints: attr.count,
        radius: 1.5,
      });
    else if (object instanceof THREE.LineSegments)
      result.push({
        ...common,
        kind: 'segments',
        segments: Array.from(
          { length: Math.floor(points.length / 2) },
          (_, i) => [points[i * 2]!, points[i * 2 + 1]!] as [[number, number], [number, number]],
        ),
      });
    else
      result.push({
        ...common,
        kind: 'polyline',
        points,
        closed: object instanceof THREE.LineLoop,
      });
  });
  return result;
}

export function sceneSpecsOf(
  scene: Scene,
  titles: string[],
  roiItems: readonly RoiItem[] = [],
  identities: readonly {
    runId?: string | null;
    nodeId?: string | null;
    port?: string | null;
    totalPoints?: number;
  }[] = [],
): SceneSpec[] {
  if (scene.mode !== '2d')
    throw new Error('Shared profile export requires the explicit XY 2D view');
  scene.ortho.updateMatrixWorld();
  const lower = new THREE.Vector3(-1, -1, 0).unproject(scene.ortho);
  const upper = new THREE.Vector3(1, 1, 0).unproject(scene.ortho);
  const bounds: [number, number, number, number] = [lower.x, lower.y, upper.x, upper.y];
  return Array.from({ length: scene.views }, (_, i) => {
    const pane = i as 0 | 1;
    const layers = [
      ...(scene.points[pane] ? layersOf(scene.points[pane]!, 'cloud') : []),
      ...layersOf(scene.overlays[pane], 'algorithm geometry'),
      ...layersOf(scene.backdrop, 'backdrop'),
      ...layersOf(scene.measure, 'interactive measurement'),
    ];
    const identity = identities[i];
    if (identity?.totalPoints !== undefined && layers[0]?.kind === 'points')
      layers[0].totalPoints = identity.totalPoints;
    if (scene.views === 1)
      for (const roi of roiItems) {
        const v = roi.value;
        const b = v[2] > v[0] && v[3] > v[1] ? v.map((n) => n * roi.scale) : roi.placeholder;
        layers.push({
          id: `roi-${roi.param}`,
          label: roi.label,
          frame,
          kind: 'box',
          color: roi.color,
          bounds: b as [number, number, number, number],
        });
      }
    return {
      schemaVersion: 1,
      title: titles[i] ?? `View ${i + 1}`,
      frame,
      layers,
      annotations: identity
        ? [
            `Run: ${identity.runId ?? 'unknown'} | Output: ${identity.nodeId ?? 'unknown'}:${identity.port ?? 'geometry'}`,
          ]
        : [],
      view: { bounds, width: 1100, height: 700 },
      provenance: {
        renderer: 'editor XY export',
        camera: 'orthographic',
        axes: 'XY',
        unit: 'm',
        output: identity,
      },
    };
  });
}

export async function profileCanvas(
  scene: Scene,
  titles: string[],
  roiItems: readonly RoiItem[] = [],
  identities: readonly {
    runId?: string | null;
    nodeId?: string | null;
    port?: string | null;
    totalPoints?: number;
  }[] = [],
): Promise<HTMLCanvasElement> {
  const specs = sceneSpecsOf(scene, titles, roiItems, identities);
  const width = scene.views === 2 && scene.split === 'lr' ? 1800 : 1100;
  const height = scene.views === 2 && scene.split === 'tb' ? 1400 : 700;
  const svg =
    specs.length === 1
      ? renderSvg(specs[0]!)
      : renderGridSvg(specs, {
          columns: scene.split === 'lr' ? 2 : 1,
          width,
          cellHeight: 700,
          sharedBounds: specs[0]!.view!.bounds!,
        });
  const url = URL.createObjectURL(new Blob([svg], { type: 'image/svg+xml;charset=utf-8' }));
  try {
    const image = new Image();
    image.src = url;
    await image.decode();
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext('2d');
    if (!context) throw new Error('Canvas 2D is unavailable');
    context.drawImage(image, 0, 0);
    return canvas;
  } finally {
    URL.revokeObjectURL(url);
  }
}
