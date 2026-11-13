import { useEffect, useRef, type KeyboardEvent, type ReactNode } from 'react';

import '../styles.navigation.css';

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
  onRename: (id: string) => void;
  onNew: () => void;
  disabled?: boolean;
  windowChrome?: FlowWindowChrome | undefined;
}

export interface FlowWindowChrome {
  controls: ReactNode;
  focused: boolean;
  onDragStart(): void;
  onToggleMaximize(): void;
}

export function FlowTabs({
  tabs,
  activeId,
  onSelect,
  onClose,
  onRename,
  onNew,
  disabled = false,
  windowChrome,
}: FlowTabsProps) {
  const tablist = useRef<HTMLDivElement>(null);
  const pendingFocus = useRef<{ kind: 'select'; id: string } | { kind: 'close' } | null>(null);

  useEffect(() => {
    const owner = tablist.current?.ownerDocument;
    const clearPendingFocus = () => {
      pendingFocus.current = null;
    };
    owner?.addEventListener('pointerdown', clearPendingFocus, true);
    return () => owner?.removeEventListener('pointerdown', clearPendingFocus, true);
  }, []);

  useEffect(() => {
    tablist.current
      ?.querySelector<HTMLElement>('[aria-selected="true"]')
      ?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }, [activeId]);

  useEffect(() => {
    const pending = pendingFocus.current;
    if (disabled || !pending || (pending.kind === 'select' && pending.id !== activeId)) return;
    const activeTab = tablist.current?.querySelector<HTMLButtonElement>(
      '[role="tab"][aria-selected="true"]',
    );
    if (!activeTab) return;
    pendingFocus.current = null;
    activeTab.focus({ preventScroll: true });
  }, [activeId, disabled, tabs]);

  const onKeyDown = (event: KeyboardEvent<HTMLButtonElement>, id: string) => {
    if (disabled || event.altKey || event.ctrlKey || event.metaKey) return;
    if (event.key === 'F2') {
      event.preventDefault();
      event.stopPropagation();
      onRename(id);
      return;
    }
    if (event.key === 'Delete') {
      event.preventDefault();
      event.stopPropagation();
      pendingFocus.current = { kind: 'close' };
      onClose(id);
      return;
    }
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    const index = tabs.findIndex((tab) => tab.id === id);
    const next =
      event.key === 'Home'
        ? 0
        : event.key === 'End'
          ? tabs.length - 1
          : (index + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
    const target = tabs[next];
    if (!target) return;
    event.preventDefault();
    event.stopPropagation();
    pendingFocus.current = { kind: 'select', id: target.id };
    onSelect(target.id);
    if (target.id === activeId) {
      pendingFocus.current = null;
      event.currentTarget.focus({ preventScroll: true });
    }
  };

  return (
    <div
      className={`flow-tabs${windowChrome ? ' flow-tabs--titlebar' : ''}`}
      data-testid="flow-tabs"
      data-window-focused={windowChrome?.focused}
      role={windowChrome ? 'banner' : undefined}
      aria-label={windowChrome ? 'LyFlow 标题栏' : undefined}
      onMouseDown={
        windowChrome
          ? (event) => {
              if (event.button !== 0 || (event.target as Element).closest('button, [role=tablist]'))
                return;
              event.preventDefault();
              if (event.detail === 2) windowChrome.onToggleMaximize();
              else windowChrome.onDragStart();
            }
          : undefined
      }
    >
      <div className="flow-tabs__brand" aria-label="LyFlow">
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
          <rect x="3" y="3" width="6" height="6" rx="2" />
          <rect x="15" y="15" width="6" height="6" rx="2" />
          <path d="M9 6h6a3 3 0 0 1 3 3v6M6 9v6a3 3 0 0 0 3 3h6" />
        </svg>
        <strong>LyFlow</strong>
      </div>
      <div
        className="flow-tabs__list"
        ref={tablist}
        role="tablist"
        aria-label="已打开的工作流"
        aria-orientation="horizontal"
      >
        {tabs.map((tab) => {
          const active = tab.id === activeId;
          const title = tab.title.trim() || '未命名工作流';
          return (
            <div
              className={`flow-tab${active ? ' is-active' : ''}`}
              key={tab.id}
              role="presentation"
              data-testid="flow-tab"
              data-id={tab.id}
            >
              <button
                type="button"
                className="flow-tab__select"
                role="tab"
                aria-selected={active}
                tabIndex={active ? 0 : -1}
                disabled={disabled}
                title={`${tab.path ?? title}\n双击或按 F2 重命名`}
                onClick={(event) => {
                  pendingFocus.current =
                    event.detail === 0 && tab.id !== activeId
                      ? { kind: 'select', id: tab.id }
                      : null;
                  onSelect(tab.id);
                }}
                onDoubleClick={(event) => {
                  event.preventDefault();
                  event.stopPropagation();
                  if (!disabled) onRename(tab.id);
                }}
                onKeyDown={(event) => onKeyDown(event, tab.id)}
                data-testid="flow-tab-select"
                data-id={tab.id}
              >
                <span className="flow-tab__title">{title}</span>
                {tab.dirty && (
                  <span
                    className="flow-tab__dirty"
                    aria-label="未保存"
                    data-testid="flow-tab-dirty"
                  />
                )}
              </button>
              <button
                type="button"
                className="flow-tab__close qs-btn icon xs"
                aria-label={`关闭 ${title}`}
                title={`关闭 ${title}（标签获得焦点时按 Delete）`}
                tabIndex={-1}
                disabled={disabled}
                onClick={(event) => {
                  pendingFocus.current = event.detail === 0 ? { kind: 'close' } : null;
                  onClose(tab.id);
                }}
                data-testid="flow-tab-close"
                data-id={tab.id}
              >
                <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
                  <path d="m7 7 10 10M17 7 7 17" />
                </svg>
              </button>
            </div>
          );
        })}
      </div>
      <button
        type="button"
        className="qs-btn icon flow-tabs__new"
        disabled={disabled}
        onClick={onNew}
        aria-label="新建工作流"
        title="新建工作流 (Ctrl+N)"
        data-testid="flow-tab-new"
      >
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
          <path d="M12 5v14M5 12h14" />
        </svg>
      </button>
      {windowChrome && (
        <>
          <div className="flow-tabs__drag-space" data-testid="titlebar-drag-region" />
          {windowChrome.controls}
        </>
      )}
    </div>
  );
}
