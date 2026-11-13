import { useEffect, useState } from 'react';

import { baseName, parentName, recentFiles } from '../lib/files';
import type { RecentEntry } from '../transport';
import type { FlowTabDocument } from './FlowTabs';

export interface WorkspaceLibraryProps {
  tabs: readonly FlowTabDocument[];
  activeId: string;
  onSelect: (id: string) => void;
  onOpen: () => void;
  onNew: () => void;
  onOpenRecent: (path: string) => void;
  onClose?: () => void;
}

function FileIcon() {
  return (
    <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
      <path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9l-6-6Z" />
      <path d="M14 3v6h6M8 14h8M8 17h5" />
    </svg>
  );
}

export function WorkspaceLibrary({
  tabs,
  activeId,
  onSelect,
  onOpen,
  onNew,
  onOpenRecent,
  onClose,
}: WorkspaceLibraryProps) {
  const [recent, setRecent] = useState<RecentEntry[] | null>(null);
  const [query, setQuery] = useState('');

  useEffect(() => {
    let mounted = true;
    void recentFiles().then((items) => {
      if (mounted) setRecent(items);
    });
    return () => {
      mounted = false;
    };
  }, []);

  const needle = query.trim().toLocaleLowerCase();
  const matches = (title: string, path?: string) =>
    !needle || `${title} ${path ?? ''}`.toLocaleLowerCase().includes(needle);
  const opened = tabs.filter((tab) => matches(tab.title, tab.path));
  const history = (recent ?? []).filter((entry) => matches(baseName(entry.path), entry.path));

  return (
    <section className="workspace-library" aria-label="工作流库" data-testid="workspace-library">
      <div className="workspace-library__heading">
        <h2>工作流</h2>
        <span className="workspace-library__heading-actions">
          <span className="qs-chip" title="已打开的工作流数量">
            {tabs.length}
          </span>
          {onClose && (
            <button
              type="button"
              className="qs-btn icon xs"
              onClick={onClose}
              title="收起工作流面板"
              aria-label="收起工作流面板"
              data-testid="library-close"
            >
              <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
                <path d="m7 7 10 10M17 7 7 17" />
              </svg>
            </button>
          )}
        </span>
      </div>
      <div className="workspace-library__actions">
        <button
          type="button"
          className="qs-btn tonal sm with-leading"
          onClick={onNew}
          data-testid="library-new-flow"
        >
          <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
            <path d="M12 5v14M5 12h14" />
          </svg>
          新建
        </button>
        <button
          type="button"
          className="qs-btn text sm with-leading"
          onClick={onOpen}
          data-testid="library-open-flow"
        >
          <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
            <path d="M3 8V6a2 2 0 0 1 2-2h5l2 3h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8Z" />
          </svg>
          打开文件
        </button>
      </div>
      <label className="qs-input workspace-library__search">
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
          <circle cx="10.5" cy="10.5" r="6.5" />
          <path d="m16 16 5 5" />
        </svg>
        <input
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          type="search"
          placeholder="搜索工作流…"
          aria-label="搜索工作流"
          data-testid="library-flow-search"
        />
      </label>
      <div className="workspace-library__content">
        <div className="workspace-library__section-heading">
          <h3>已打开</h3>
          <span>{opened.length}</span>
        </div>
        <ul className="workspace-library__list">
          {opened.map((tab) => (
            <li key={tab.id}>
              <button
                type="button"
                className={`workspace-file${tab.id === activeId ? ' is-active' : ''}`}
                aria-current={tab.id === activeId ? 'page' : undefined}
                title={tab.path ?? tab.title}
                onClick={() => onSelect(tab.id)}
                data-testid="library-open-document"
                data-id={tab.id}
              >
                <FileIcon />
                <span className="workspace-file__text">
                  <span className="workspace-file__title">
                    {tab.title.trim() || '未命名工作流'}
                  </span>
                  <span className="workspace-file__path">
                    {tab.path ? parentName(tab.path) : '尚未保存'}
                  </span>
                </span>
                {tab.dirty && <span className="flow-tab__dirty" aria-label="未保存" />}
              </button>
            </li>
          ))}
        </ul>
        {opened.length === 0 && (
          <p className="workspace-library__empty">
            {needle ? '没有匹配的已打开工作流' : '还没有打开的工作流'}
          </p>
        )}
        <div className="workspace-library__section-heading workspace-library__section-heading--recent">
          <h3>最近打开</h3>
          <button
            type="button"
            className="qs-btn icon xs"
            title="刷新最近文件"
            aria-label="刷新最近文件"
            data-testid="library-refresh-recent"
            onClick={() => void recentFiles().then(setRecent)}
          >
            <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
              <path d="M20 7v5h-5M4 17v-5h5" />
              <path d="M6 7a7 7 0 0 1 12-1l2 6M4 12l2 6a7 7 0 0 0 12-1" />
            </svg>
          </button>
        </div>
        <ul className="workspace-library__list">
          {history.map((entry) => (
            <li key={entry.path}>
              <button
                type="button"
                className="workspace-file"
                title={entry.path}
                onClick={() => onOpenRecent(entry.path)}
                data-testid="library-recent-document"
                data-path={entry.path}
              >
                <FileIcon />
                <span className="workspace-file__text">
                  <span className="workspace-file__title">{baseName(entry.path)}</span>
                  <span className="workspace-file__path">{parentName(entry.path)}</span>
                </span>
              </button>
            </li>
          ))}
        </ul>
        {recent === null && (
          <p className="workspace-library__empty" role="status">
            正在读取最近文件…
          </p>
        )}
        {recent !== null && history.length === 0 && (
          <p className="workspace-library__empty">
            {needle ? '没有匹配的最近文件' : '打开的文件会显示在这里'}
          </p>
        )}
      </div>
    </section>
  );
}
