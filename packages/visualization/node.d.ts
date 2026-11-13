import type { SceneSpec, GridOptions } from './index.js';
export interface RenderOptions {
  browserPath?: string;
  timeoutMs?: number;
}
export function renderPng(
  scene: SceneSpec,
  options?: RenderOptions,
): Promise<{ png: Buffer; svg: string; metadata: unknown }>;
export function renderGridPng(
  scenes: SceneSpec[],
  options?: GridOptions & RenderOptions,
): Promise<{ png: Buffer; svg: string; metadata: unknown }>;
