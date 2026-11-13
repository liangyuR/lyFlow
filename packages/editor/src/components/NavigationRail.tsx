export type NavigationPanel = 'nodes' | 'flows' | null;

export interface NavigationRailProps {
  active: NavigationPanel;
  onSelect: (pane: NavigationPanel) => void;
  onParameters: () => void;
  onLogs: () => void;
  onHelp: () => void;
  parametersOpen: boolean;
  logsOpen: boolean;
}

export function NavigationRail({
  active,
  onSelect,
  onParameters,
  onLogs,
  onHelp,
  parametersOpen,
  logsOpen,
}: NavigationRailProps) {
  return (
    <nav
      className="navigation-rail qs-nav rail"
      aria-label="工作区导航"
      data-testid="navigation-rail"
    >
      <button
        type="button"
        className="item"
        aria-current={active === 'nodes' ? 'page' : undefined}
        aria-expanded={active === 'nodes'}
        title={active === 'nodes' ? '收起节点面板' : '展开节点面板'}
        onClick={() => onSelect(active === 'nodes' ? null : 'nodes')}
        data-testid="nav-nodes"
      >
        <span className="pill" aria-hidden="true" />
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
          <rect x="3" y="3" width="6" height="6" rx="2" />
          <rect x="15" y="15" width="6" height="6" rx="2" />
          <path d="M9 6h6a3 3 0 0 1 3 3v6M6 9v6a3 3 0 0 0 3 3h6" />
        </svg>
        <span className="lbl">节点</span>
      </button>
      <button
        type="button"
        className="item"
        aria-current={active === 'flows' ? 'page' : undefined}
        aria-expanded={active === 'flows'}
        title={active === 'flows' ? '收起工作流面板' : '展开工作流面板'}
        onClick={() => onSelect(active === 'flows' ? null : 'flows')}
        data-testid="nav-flows"
      >
        <span className="pill" aria-hidden="true" />
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
          <path d="M3 8V6a2 2 0 0 1 2-2h5l2 3h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8Z" />
          <path d="M8 12h8M8 16h5" />
        </svg>
        <span className="lbl">工作流</span>
      </button>
      <span className="grp" aria-hidden="true" />
      <button
        type="button"
        className="item"
        aria-pressed={parametersOpen}
        title="参数面板"
        onClick={onParameters}
        data-testid="nav-parameters"
      >
        <span className="pill" aria-hidden="true" />
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
          <path d="M4 7h7m4 0h5M4 17h3m4 0h9" />
          <circle cx="13" cy="7" r="2" />
          <circle cx="9" cy="17" r="2" />
        </svg>
        <span className="lbl">参数</span>
      </button>
      <button
        type="button"
        className="item"
        aria-pressed={logsOpen}
        title="日志与诊断"
        onClick={onLogs}
        data-testid="nav-logs"
      >
        <span className="pill" aria-hidden="true" />
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
          <rect x="3" y="4" width="18" height="16" rx="3" />
          <path d="m7 9 3 3-3 3m6 0h4" />
        </svg>
        <span className="lbl">日志</span>
      </button>
      <div className="foot">
        <button
          type="button"
          className="item"
          title="快捷键帮助 (?)"
          onClick={onHelp}
          data-testid="nav-help"
        >
          <span className="pill" aria-hidden="true" />
          <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true">
            <circle cx="12" cy="12" r="9" />
            <path d="M9.5 9a2.5 2.5 0 0 1 5 0c0 2-2.5 2-2.5 4m0 3h.01" />
          </svg>
          <span className="lbl">帮助</span>
        </button>
      </div>
    </nav>
  );
}
