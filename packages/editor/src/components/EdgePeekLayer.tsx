import { useMemo } from "react";

import { usePeekStore } from "../store/peek";

import { EdgePeek } from "./EdgePeek";

export function EdgePeekLayer() {
  const windows = usePeekStore((s) => s.windows);
  const ranks = useMemo(() => {
    const order = [...windows].sort((a, b) => a.z - b.z);
    return new Map(order.map((w, i) => [w.id, i]));
  }, [windows]);
  if (windows.length === 0) return null;
  return (
    <>
      {windows.map((w) => (
        <EdgePeek key={w.id} win={w} rank={ranks.get(w.id) ?? 0} />
      ))}
    </>
  );
}
