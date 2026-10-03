// 标准视角的一组按钮（主预览的工具栏、连线查看器的栏都用）与「鼠标在预览上按 1–4」那一路的接收端。

import { useEffect, type RefObject } from "react";

import { keyHint } from "../lib/keymap";
import { VIEW_PRESET_EVENT, type ViewPreset } from "../lib/viewFit";

const PRESETS: readonly { view: ViewPreset; label: string; key: string; what: string }[] = [
  { view: "top", label: "俯", key: "viewTop", what: "俯视：从 +Z 往下看，X 右 Y 上" },
  { view: "front", label: "前", key: "viewFront", what: "前视：从 −Y 往 +Y 看，X 右 Z 上" },
  { view: "side", label: "侧", key: "viewSide", what: "侧视：从 +X 往 −X 看，Y 右 Z 上" },
  { view: "iso", label: "轴", key: "viewIso", what: "轴测：与 ⤢ 取景同一个方向" },
];

export function ViewPresetButtons({
  disabled,
  onPick,
  className,
}: {
  /** 2D 剖面：本来就是俯视，置灰。 */
  disabled: boolean;
  onPick: (view: ViewPreset) => void;
  className: string;
}) {
  return (
    <span className="view-presets" role="group" aria-label="标准视角">
      {PRESETS.map((p) => (
        <button
          key={p.view}
          type="button"
          className={className}
          data-testid="view-preset"
          data-view={p.view}
          disabled={disabled}
          title={disabled ? "2D 剖面本来就是俯视" : `${p.what}（鼠标在预览上按 ${keyHint(p.key)}）。转心与远近不变，看全貌按 ⤢`}
          onClick={() => onPick(p.view)}
        >
          {p.label}
        </button>
      ))}
    </span>
  );
}

/** 接住 useShortcuts 发到这个预览根元素上的视角事件。apply 返回 false（2D、还没场景）就当没接。 */
export function useViewPresetEvents(root: RefObject<HTMLElement | null>, apply: (view: ViewPreset) => boolean): void {
  useEffect(() => {
    const el = root.current;
    if (!el) return;
    const on = (e: Event) => {
      const req = (e as CustomEvent<{ view: ViewPreset; done: boolean }>).detail;
      e.stopPropagation();
      if (apply(req.view)) req.done = true;
    };
    el.addEventListener(VIEW_PRESET_EVENT, on);
    return () => el.removeEventListener(VIEW_PRESET_EVENT, on);
  }, [root, apply]);
}
