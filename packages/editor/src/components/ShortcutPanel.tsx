// `?` 快捷键面板。整张表从 lib/keymap.ts 生成（E7）——
// 手抄一份说明必然和实际按键漂移，而用户只会相信面板上写的那个。

import { groupedShortcuts, SCOPE_LABEL } from "../lib/keymap";
import { useUiStore } from "../store/ui";

export function ShortcutPanel() {
  const open = useUiStore((s) => s.helpOpen);
  const setOpen = useUiStore((s) => s.setHelpOpen);
  if (!open) return null;

  return (
    <div className="sheet" data-testid="shortcut-panel" onClick={() => setOpen(false)}>
      <div className="sheet__box" onClick={(e) => e.stopPropagation()}>
        <header className="sheet__head">
          <h2>快捷键</h2>
          <button type="button" data-testid="shortcut-close" onClick={() => setOpen(false)}>
            关闭
          </button>
        </header>
        <div className="sheet__body">
          {groupedShortcuts().map((g) => (
            <section key={g.group} className="sheet__group">
              <h3>{g.group}</h3>
              <ul>
                {g.items.map((s) => (
                  <li key={s.id} data-testid={`shortcut-${s.id}`}>
                    <span className="sheet__label">{s.label}</span>
                    <span className="sheet__scope">{SCOPE_LABEL[s.scope]}</span>
                    <span className="sheet__keys">
                      {s.keys.map((k) => (
                        <kbd key={k}>{k}</kbd>
                      ))}
                    </span>
                  </li>
                ))}
              </ul>
            </section>
          ))}
          <section className="sheet__group">
            <h3>连线查看器</h3>
            <ul>
              <li data-testid="shortcut-peekEscape">
                <span className="sheet__label">
                  有浮窗且没有运行在跑时，关掉最前面的那一个（否则 Esc 仍是「取消运行」）
                </span>
                <span className="sheet__scope">{SCOPE_LABEL.global}</span>
                <span className="sheet__keys">
                  <kbd>Escape</kbd>
                </span>
              </li>
            </ul>
          </section>
        </div>
      </div>
    </div>
  );
}
