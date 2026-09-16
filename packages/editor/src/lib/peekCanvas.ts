export type PeekCanvasGetter = () => HTMLCanvasElement | null;

const getters = new Map<string, PeekCanvasGetter>();

export function registerPeekCanvas(id: string, get: PeekCanvasGetter): () => void {
  getters.set(id, get);
  return () => {
    if (getters.get(id) === get) getters.delete(id);
  };
}

export function takePeekCanvas(id: string): HTMLCanvasElement | null {
  const get = getters.get(id);
  if (!get) return null;
  return get();
}
