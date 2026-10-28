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
export function renderGridSvg(scenes: SceneSpec[], options?: GridOptions): string;
export function sceneMetadata(scene: SceneSpec): {
  bounds: Bounds;
  layers: unknown[];
  frame: Frame;
  provenance?: unknown;
};

export function gridScenes(scenes: SceneSpec[], options?: GridOptions): SceneSpec[];
