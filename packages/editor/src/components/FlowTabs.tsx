import { useEffect, useRef, type KeyboardEvent } from "react";

import "../styles.navigation.css";

export interface FlowTabDocument {
  id: string;
  title: string;
  dirty: boolean;
  path?: string | undefined;
}

export interface FlowTabsProps {
  tabs: readonly FlowTabDocument[];
  activeId: string;
  onSelect: (id: string) => void;
  onClose: (id: string) => void;
  onNew: () => void;
  disabled?: boolean;
}

export function FlowTabs({ tabs, activeId, onSelect, onClose, onNew, disabled = false }: FlowTabsProps) {
  const tablist = useRef<HTMLDivElement>(null);
  const pendingFocus = useRef<{ kind: "select"; id: string } | { kind: "close" } | null>(null);

  useEffect(() => {
    const owner = tablist.current?.ownerDocument;
    const clearPendingFocus = () => { pendingFocus.current = null; };
    owner?.addEventListener("pointerdown", clearPendingFocus, true);
    return () => owner?.removeEventListener("pointerdown", clearPendingFocus, true);
  }, []);

  useEffect(() => {
    tablist.current?.querySelector<HTMLElement>('[aria-selected="true"]')?.scrollIntoView({ block: "nearest", inline: "nearest" });
  }, [activeId]);

  useEffect(() => {
    const pending = pendingFocus.current;
    if (disabled || !pending || (pending.kind === "select" && pending.id !== activeId)) return;
    const activeTab = tablist.current?.querySelector<HTMLButtonElement>('[role="tab"][aria-selected="true"]');
    if (!activeTab) return;
    pendingFocus.current = null;
    activeTab.focus({ preventScroll: true });
  }, [activeId, disabled, tabs]);

  const onKeyDown = (event: KeyboardEvent<HTMLButtonElement>, id: string) => {
    if (disabled || event.altKey || event.ctrlKey || event.metaKey) return;
    if (event.key === "Delete") {
      event.preventDefault();
      event.stopPropagation();
      pendingFocus.current = { kind: "close" };
      onClose(id);
      return;
    }
    if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
    const index = tabs.findIndex((tab) => tab.id === id);
    const next = event.key === "Home" ? 0 : event.key === "End" ? tabs.length - 1
      : (index + (event.key === "ArrowRight" ? 1 : -1) + tabs.length) % tabs.length;
    const target = tabs[next];
    if (!target) return;
    event.preventDefault();
    event.stopPropagation();
    pendingFocus.current = { kind: "select", id: target.id };
    onSelect(target.id);
    if (target.id === activeId) {
      pendingFocus.current = null;
      event.currentTarget.focus({ preventScroll: true });
    }
  };

  return (
    <div className="flow-tabs" data-testid="flow-tabs">
      <div className="flow-tabs__brand" aria-label="LyFlow">
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="3" width="6" height="6" rx="2" /><rect x="15" y="15" width="6" height="6" rx="2" /><path d="M9 6h6a3 3 0 0 1 3 3v6M6 9v6a3 3 0 0 0 3 3h6" /></svg>
        <strong>LyFlow</strong>
      </div>
      <div className="flow-tabs__list" ref={tablist} role="tablist" aria-label="已打开的工作流" aria-orientation="horizontal">
        {tabs.map((tab) => {
          const active = tab.id === activeId;
          const title = tab.title.trim() || "未命名工作流";
          return (
            <div className={`flow-tab${active ? " is-active" : ""}`} key={tab.id} role="presentation" data-testid="flow-tab" data-id={tab.id}>
              <button
                type="button"
                className="flow-tab__select"
                role="tab"
                aria-selected={active}
                tabIndex={active ? 0 : -1}
                disabled={disabled}
                title={tab.path ?? title}
                onClick={(event) => {
                  pendingFocus.current = event.detail === 0 && tab.id !== activeId ? { kind: "select", id: tab.id } : null;
                  onSelect(tab.id);
                }}
                onKeyDown={(event) => onKeyDown(event, tab.id)}
                data-testid="flow-tab-select"
                data-id={tab.id}
              >
                <span className="flow-tab__title">{title}</span>
                {tab.dirty && <span className="flow-tab__dirty" aria-label="未保存" data-testid="flow-tab-dirty" />}
              </button>
              <button
                type="button"
                className="flow-tab__close qs-btn icon xs"
                aria-label={`关闭 ${title}`}
                title={`关闭 ${title}（标签获得焦点时按 Delete）`}
                tabIndex={-1}
                disabled={disabled}
                onClick={(event) => {
                  pendingFocus.current = event.detail === 0 ? { kind: "close" } : null;
                  onClose(tab.id);
                }}
                data-testid="flow-tab-close"
                data-id={tab.id}
              >
                <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true"><path d="m7 7 10 10M17 7 7 17" /></svg>
              </button>
            </div>
          );
        })}
      </div>
      <button type="button" className="qs-btn icon flow-tabs__new" disabled={disabled} onClick={onNew} aria-label="新建工作流" title="新建工作流 (Ctrl+N)" data-testid="flow-tab-new">
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg>
      </button>
    </div>
  );
}
