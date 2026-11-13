import { useEffect, useMemo, useState } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import type { UnlistenFn } from "@tauri-apps/api/event";
import { LyFlowEditor, useUiStore, type Transport } from "@lyflow/editor";

import { tauriDialogs } from "./dialogs";

export function DesktopEditor({ transport, closeReady }: { transport: Transport; closeReady: Promise<void> }) {
  const appWindow = useMemo(() => getCurrentWindow(), []);
  const [maximized, setMaximized] = useState(false);
  const [focused, setFocused] = useState(true);

  const runAction = (label: string, action: () => Promise<unknown>) => {
    void action().catch(() => useUiStore.getState().showToast(`无法${label}，请重试`, "warn"));
  };

  useEffect(() => {
    let disposed = false;
    const listeners: UnlistenFn[] = [];
    const listen = async (subscription: Promise<UnlistenFn>) => {
      const unlisten = await subscription;
      if (disposed) unlisten();
      else listeners.push(unlisten);
    };
    const updateMaximized = async () => {
      const value = await appWindow.isMaximized();
      if (!disposed) setMaximized(value);
    };
    void Promise.all([
      updateMaximized(),
      appWindow.isFocused().then((value) => { if (!disposed) setFocused(value); }),
      listen(appWindow.onResized(() => { void updateMaximized().catch(() => {}); })),
      listen(appWindow.onFocusChanged(({ payload }) => { if (!disposed) setFocused(payload); })),
    ]).catch(() => {
      if (!disposed) useUiStore.getState().showToast("窗口状态同步失败，请重试", "warn");
    });
    return () => {
      disposed = true;
      listeners.forEach((unlisten) => unlisten());
    };
  }, [appWindow]);

  const toggleMaximize = () => runAction("切换窗口大小", () => appWindow.toggleMaximize());
  const maximizeLabel = maximized ? "向下还原" : "最大化";

  return <LyFlowEditor
    transport={transport}
    dialogs={tauriDialogs}
    windowChrome={{
      focused,
      onDragStart: () => runAction("拖动窗口", () => appWindow.startDragging()),
      onToggleMaximize: toggleMaximize,
      controls: <div className="window-controls" role="group" aria-label="窗口控制">
        <button type="button" aria-label="最小化" title="最小化" data-testid="window-minimize" onClick={() => runAction("最小化窗口", () => appWindow.minimize())}>
          <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 8.5h10" /></svg>
        </button>
        <button type="button" aria-label={maximizeLabel} title={maximizeLabel} data-testid="window-maximize" aria-pressed={maximized} onClick={toggleMaximize}>
          <svg viewBox="0 0 16 16" aria-hidden="true">
            {maximized ? <path d="M5.5 5.5v-2h7v7h-2m-7-5h7v7h-7z" /> : <rect x="3.5" y="3.5" width="9" height="9" />}
          </svg>
        </button>
        <button type="button" className="window-controls__close" aria-label="关闭窗口" title="关闭窗口" data-testid="window-close" onClick={() => runAction("关闭窗口", async () => { await closeReady; await appWindow.close(); })}>
          <svg viewBox="0 0 16 16" aria-hidden="true"><path d="m3.5 3.5 9 9m0-9-9 9" /></svg>
        </button>
      </div>,
    }}
  />;
}
