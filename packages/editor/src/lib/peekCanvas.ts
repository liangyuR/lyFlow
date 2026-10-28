export type PeekCanvasGetter = () => HTMLCanvasElement | null | Promise<HTMLCanvasElement | null>;

const getters = new Map<string, PeekCanvasGetter>();

export function registerPeekCanvas(id: string, get: PeekCanvasGetter): () => void {
  getters.set(id, get);
  return () => {
    if (getters.get(id) === get) getters.delete(id);
  };
}

export async function takePeekCanvas(id: string): Promise<HTMLCanvasElement | null> {
  const get = getters.get(id);
  if (!get) return null;
  return get();
}
