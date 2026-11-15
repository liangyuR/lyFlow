export type Point = [number, number];
export type Bounds = [number, number, number, number];
export interface Frame {
  id: string;
  unit: string;
  axisUnits?: [string, string];
  axes: [string, string];
  yDirection: 'up' | 'down';
}
export interface Layer {
  id: string;
  instanceId?: string;
  label?: string;
  frame: Frame;
  source?: { snapshotId: string; nodeId: string; port: string };
  transform?: [number, number, number, number, number, number];
  color?: string;
  colors?: string[];
  radius?: number;
  lineWidth?: number;
  opacity?: number;
  kind: 'points' | 'polyline' | 'segments' | 'circle' | 'box' | 'text' | 'image';
  points?: Point[];
  segments?: [Point, Point][];
  center?: Point;
  bounds?: Bounds;
  position?: Point;
  text?: string;
  dataUrl?: string;
  closed?: boolean;
  totalPoints?: number;
  missing?: string;
}
export interface SceneSpec {
  schemaVersion: 1;
  title?: string;
  annotations?: string[];
  frame: Frame;
  layers: Layer[];
  view?: {
    bounds?: Bounds;
    width?: number;
    height?: number;
    equalAspect?: boolean;
    annotationRows?: number;
    legendRows?: number;
  };
  provenance?: unknown;
}
export interface GridOptions {
  columns?: number;
  width?: number;
  cellHeight?: number;
  title?: string;
  sharedBounds?: boolean | Bounds;
}
export interface GeometryPath {
  points: number[];
  loop: boolean;
}
export function geometryPaths(value: Record<string, any>, span: number): GeometryPath[];
export function sceneBounds(scene: SceneSpec): Bounds;
export function renderSvg(scene: SceneSpec): string;
export function renderImageOverlaySvg(scene: SceneSpec, width: number, height: number): string;
export function renderGridSvg(scenes: SceneSpec[], options?: GridOptions): string;
export function sceneMetadata(scene: SceneSpec): {
  bounds: Bounds;
  layers: unknown[];
  frame: Frame;
  provenance?: unknown;
};

export function gridScenes(scenes: SceneSpec[], options?: GridOptions): SceneSpec[];

export interface ResultOptions {
  selectedId?: string | null;
  showAxes?: boolean;
  showLabels?: boolean;
  color?: string;
  colorBy?: 'id' | 'class';
  title?: string;
  alpha?: number;
  lineWidth?: number;
  transform?: { fromFrame: string; toFrame: string; matrix: number[] };
}
export interface PoseItem {
  id: string;
  frame: string;
  unit: string;
  position: number[];
  size: number[];
  angle?: number;
  rotation?: number[];
  name?: string;
  classLabel?: string;
  classId?: string;
  confidence?: number;
  pickable?: boolean;
  [key: string]: unknown;
}
export interface PoseCollection {
  dimension: 2 | 3;
  frame: string;
  unit: string;
  items: PoseItem[];
}
export interface PoseShape {
  id: string;
  label: string;
  frame: string;
  unit: string;
  dimension: number;
  kind: 'box' | 'axis';
  points: number[][];
  closed: boolean;
  segments?: boolean;
  color: string;
}
export const RESULT_COLORS: string[];
export const RESULT_LIMITS: Readonly<{
  instances: number;
  points: number;
  memberships: number;
  layers: number;
}>;
export function resultColor(identity: unknown): string;
export function poseCollection(value: unknown): PoseCollection | null;
export function poseGeometry(value: unknown, options?: ResultOptions): PoseShape[];
export function poseScene2D(value: unknown, options?: ResultOptions): SceneSpec;
export function structuredScene(value: unknown): SceneSpec | null;
export function resultSummary(value: unknown): {
  title: string;
  frame?: string;
  unit?: string;
  count?: number;
  items?: unknown[];
  value?: unknown;
} | null;
export function cameraProjector(
  value: unknown,
  sourceFrame: string,
  transform?: ResultOptions['transform'],
): { project(point: number[]): Point | null; frame: string; imageSize: Point };
export function projectedPoseScene(
  value: unknown,
  camera: unknown,
  options?: ResultOptions,
): SceneSpec;
export function maskPreview(
  value: unknown,
  masks: { shape: number[]; data: Float32Array },
  poses: unknown,
  options?: ResultOptions,
): { width: number; height: number; pixels: Uint8ClampedArray };
export function cloudGroupPreview(
  value: unknown,
  indices: { values: Int32Array; sourceCloudIdString?: string },
  cloud: { sourceCloudId?: string; sampleStride?: number; totalPoints: number; pointCount: number },
  options?: ResultOptions,
): {
  colors: Uint8Array;
  visible: Uint8Array;
  groups: { id: string; count: number; offset: number }[];
  frame: string;
  sourceCloudId: string;
  displayedPoints: number;
  totalPoints: number;
};
