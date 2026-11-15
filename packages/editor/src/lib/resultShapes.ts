import * as THREE from 'three';
import {
  poseCollection,
  poseGeometry,
  resultColor,
  type ResultOptions,
} from '@lyflow/visualization';
import type { CloudPayload, OutputStat } from '../types/execution';

export function poseRecords(outputs: readonly OutputStat[] | undefined) {
  const result = [];
  for (const output of outputs ?? []) {
    try {
      const poses = poseCollection(output.value);
      if (poses) result.push({ output, poses });
    } catch {}
  }
  return result;
}

export function poseCloudFrame(
  outputs: readonly OutputStat[],
  cloud: CloudPayload | null,
): string | null {
  if (!cloud?.sourceCloudId) return null;
  const frames = outputs
    .filter(
      (output) =>
        output.value?.type === 'cloud.Groups.v1' &&
        output.value.data?.sourceCloudId === cloud.sourceCloudId &&
        output.value.data?.unit === 'm',
    )
    .map((output) => output.value!.data!.frame);
  return frames.length > 0 &&
    frames.every((frame) => frame === frames[0]) &&
    typeof frames[0] === 'string'
    ? frames[0]
    : null;
}

export function poseLines(
  outputs: readonly OutputStat[],
  cloud: CloudPayload | null,
  options: ResultOptions & { solid?: boolean; confirmedFrame?: string } = {},
): THREE.Object3D[] {
  const result: THREE.Object3D[] = [];
  const cloudFrame = poseCloudFrame(outputs, cloud) ?? options.confirmedFrame;
  const records = poseRecords(outputs);
  const separateFrame =
    options.confirmedFrame || records.find(({ poses }) => poses.unit === 'm')?.poses.frame;
  for (const { output, poses } of records) {
    if (poses.unit !== 'm' || (cloud ? cloudFrame !== poses.frame : poses.frame !== separateFrame))
      continue;
    for (const shape of poseGeometry(output.value, options)) {
      const origin = shape.points[0]!;
      const positions = shape.points.flatMap((point) =>
        point.map((value, axis) => value - origin[axis]!),
      );
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
      const material = new THREE.LineBasicMaterial({ color: shape.color, depthTest: false });
      const line = shape.segments
        ? new THREE.LineSegments(geometry, material)
        : shape.closed
          ? new THREE.LineLoop(geometry, material)
          : new THREE.Line(geometry, material);
      line.position.set(origin[0]!, origin[1]!, origin[2]!);
      line.userData = {
        instanceId: shape.id,
        frame: poses.frame,
        unit: poses.unit,
        port: output.port,
      };
      line.renderOrder = 11;
      result.push(line);
    }
    if (options.solid && poses.dimension === 3)
      for (const item of poses.items) {
        if (options.selectedId && item.id !== options.selectedId) continue;
        const material = new THREE.MeshBasicMaterial({
          color: options.color ?? resultColor(item.id),
          transparent: true,
          opacity: 0.12,
          depthWrite: false,
          side: THREE.DoubleSide,
        });
        const mesh = new THREE.Mesh(
          new THREE.BoxGeometry(item.size[0], item.size[1], item.size[2]),
          material,
        );
        mesh.position.set(item.position[0]!, item.position[1]!, item.position[2]!);
        mesh.quaternion.set(
          item.rotation![0]!,
          item.rotation![1]!,
          item.rotation![2]!,
          item.rotation![3]!,
        );
        mesh.userData = {
          instanceId: item.id,
          frame: poses.frame,
          unit: poses.unit,
          port: output.port,
        };
        result.push(mesh);
      }
    if (options.showLabels === true && typeof document !== 'undefined')
      for (const item of poses.items.slice(0, 256)) {
        if (options.selectedId && item.id !== options.selectedId) continue;
        const canvas = document.createElement('canvas');
        canvas.width = 512;
        canvas.height = 64;
        const context = canvas.getContext('2d')!;
        context.font = '24px sans-serif';
        context.fillStyle = options.color ?? resultColor(item.id);
        context.textBaseline = 'middle';
        const label = item.name || item.classLabel || item.id;
        context.fillText(label.slice(0, 32), 4, 32, 504);
        const material = new THREE.SpriteMaterial({
          map: new THREE.CanvasTexture(canvas),
          depthTest: false,
          transparent: true,
        });
        const sprite = new THREE.Sprite(material);
        const height = Math.max(...item.size) * 0.12;
        sprite.scale.set(height * 8, height, 1);
        sprite.position.set(
          item.position[0]!,
          item.position[1]!,
          (item.position[2] ?? 0) + Math.max(...item.size) * 0.6,
        );
        sprite.userData = {
          instanceId: item.id,
          frame: poses.frame,
          unit: poses.unit,
          label,
          color: options.color ?? resultColor(item.id),
        };
        sprite.renderOrder = 12;
        result.push(sprite);
      }
  }
  return result;
}
