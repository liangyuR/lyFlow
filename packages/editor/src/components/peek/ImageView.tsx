// 连线查看器的图像视图（docs/image-plan.md §5.4）：画布与取数都在 ImageCanvas（主预览的图像模式共用）。
import { useEffect, useRef } from "react";

import { registerPeekCanvas } from "../../lib/peekCanvas";
import { PEEK_FROZEN } from "../../store/peek";
import { ImageCanvas } from "../ImageCanvas";
import type { PeekViewProps } from "./types";

export function ImageView({ win, src }: PeekViewProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  useEffect(() => registerPeekCanvas(win.id, () => canvasRef.current), [win.id]);

  const value = src.stat?.value;
  const message = src.status !== null ? src.status : !src.stat ? "该节点尚未产出结果" : null;
  if (message !== null) {
    return (
      <div className="peek-image" data-testid="peek-image">
        <p className="peek-tensor__msg" data-testid="peek-image-msg">
          {message}
        </p>
      </div>
    );
  }
  const lockedRun = win.locked?.runId ?? null;
  return (
    <ImageCanvas
      runId={lockedRun ?? src.runId}
      nodeId={src.resolved?.nodeId ?? ""}
      port={src.resolved?.port ?? ""}
      fullW={typeof value?.width === "number" ? value.width : 0}
      fullH={typeof value?.height === "number" ? value.height : 0}
      channels={typeof value?.channels === "number" ? value.channels : 0}
      depth={value?.depth ?? "u8"}
      errorText={lockedRun ? PEEK_FROZEN : null}
      testid="peek-image"
      onCanvas={(el) => {
        canvasRef.current = el;
      }}
    />
  );
}
