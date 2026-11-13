import { useEffect, useState } from 'react';

import { dialogs } from '../lib/dialogs';
import { cleanPathText } from '../lib/params';
import { pythonConfigProblem } from '../lib/pythonConfig';
import { usePythonStore } from '../store/python';

import '../styles.python.css';

export function PythonLibrary() {
  const state = usePythonStore();
  const [path, setPath] = useState('');
  useEffect(() => {
    void usePythonStore.getState().load();
  }, []);
  const editable = state.settings?.supported === true && state.settings.editable;
  const disabled = state.loading || state.busy;
  const pickPath = dialogs().pickPath;
  const add = (value: string) => {
    const item = cleanPathText(value);
    if (!item || !editable || disabled) return;
    state.edit({ packagePaths: [...new Set([...state.draft.packagePaths, item])] });
    setPath('');
  };
  const browse = async (kind: 'executable' | 'file' | 'dir') => {
    if (!pickPath || !editable || disabled) return;
    try {
      const chosen = await pickPath(
        kind === 'dir'
          ? { mode: 'dir' }
          : {
              mode: 'open',
              filters: [
                {
                  name: kind === 'executable' ? 'Python 解释器' : 'Python 节点包',
                  extensions: [kind === 'executable' ? 'exe' : 'py'],
                },
              ],
              ...(kind === 'executable' && state.draft.executable
                ? { defaultPath: cleanPathText(state.draft.executable) }
                : {}),
            },
      );
      if (!chosen) return;
      if (kind === 'executable') state.edit({ executable: cleanPathText(chosen) });
      else add(chosen);
    } catch (error) {
      usePythonStore.setState({ error: error instanceof Error ? error.message : String(error) });
    }
  };
  const status = state.settings?.status;
  const draftProblem = pythonConfigProblem(state.draft);
  return (
    <section
      className="python-library"
      aria-label="Python 节点管理"
      aria-busy={disabled}
      data-testid="python-library"
    >
      {state.loading && <p role="status">正在读取 Python 配置…</p>}
      {state.error && (
        <p className="python-library__error" role="alert" data-testid="python-error">
          {state.error}
        </p>
      )}
      {!state.loading && !state.settings?.supported ? (
        <div className="python-library__unavailable" data-testid="python-unavailable">
          <p>当前宿主未提供 Python 节点管理。请使用支持此功能的桌面应用或 HTTP 后端。</p>
          <button type="button" onClick={() => void state.load()} disabled={disabled}>
            重新读取
          </button>
        </div>
      ) : (
        state.settings && (
          <>
            {!editable && (
              <p className="python-library__note" data-testid="python-readonly">
                Python 配置由宿主管理，此处只读。
              </p>
            )}
            <fieldset disabled={!editable || disabled}>
              <label className="python-library__field">
                <span>Python 解释器</span>
                <span className="python-library__row">
                  <input
                    value={state.draft.executable}
                    placeholder="选择 Python 环境中的 python.exe"
                    data-testid="python-executable"
                    onChange={(event) => state.edit({ executable: event.target.value })}
                  />
                  {pickPath && (
                    <button
                      type="button"
                      data-testid="python-browse-executable"
                      onClick={() => void browse('executable')}
                    >
                      浏览…
                    </button>
                  )}
                </span>
              </label>
              <label className="python-library__field">
                <span>执行超时（秒）</span>
                <input
                  type="number"
                  min="0.001"
                  max="86400"
                  step="0.001"
                  value={Number.isFinite(state.draft.timeoutMs) ? state.draft.timeoutMs / 1000 : ''}
                  data-testid="python-timeout"
                  onChange={(event) =>
                    state.edit({
                      timeoutMs:
                        event.target.value === ''
                          ? Number.NaN
                          : Math.round(Number(event.target.value) * 1000),
                    })
                  }
                />
              </label>
              <div className="python-library__field">
                <span>节点包（.py 文件或包目录）</span>
                <ul className="python-library__paths">
                  {state.draft.packagePaths.map((item) => (
                    <li key={item} data-testid="python-package-path">
                      <span title={item}>{item}</span>
                      <button
                        type="button"
                        aria-label={`移除 ${item}`}
                        data-testid="python-remove-package"
                        onClick={() =>
                          state.edit({
                            packagePaths: state.draft.packagePaths.filter(
                              (entry) => entry !== item,
                            ),
                          })
                        }
                      >
                        ×
                      </button>
                    </li>
                  ))}
                </ul>
                <span className="python-library__row">
                  <input
                    value={path}
                    aria-label="节点包路径"
                    placeholder="粘贴节点包路径"
                    data-testid="python-package-input"
                    onChange={(event) => setPath(event.target.value)}
                    onKeyDown={(event) => {
                      if (event.key === 'Enter') {
                        event.preventDefault();
                        add(path);
                      }
                    }}
                  />
                  <button
                    type="button"
                    disabled={!path.trim()}
                    onClick={() => add(path)}
                    data-testid="python-add-package"
                  >
                    添加
                  </button>
                </span>
                {pickPath && (
                  <span className="python-library__row">
                    <button
                      type="button"
                      data-testid="python-browse-file"
                      onClick={() => void browse('file')}
                    >
                      选择 .py 文件…
                    </button>
                    <button
                      type="button"
                      data-testid="python-browse-directory"
                      onClick={() => void browse('dir')}
                    >
                      选择包目录…
                    </button>
                  </span>
                )}
              </div>
            </fieldset>
            <p className="python-library__note">
              使用已安装 NumPy 的 Python 环境。保存后导入节点包；修改包文件后手动刷新。
            </p>
            {state.dirty && draftProblem && (
              <p className="python-library__error" role="alert">
                {draftProblem}
              </p>
            )}
            <div className="python-library__row python-library__actions">
              <button
                type="button"
                data-testid="python-check"
                disabled={!editable || disabled || draftProblem !== null}
                onClick={() => void state.save()}
              >
                {state.busy ? '处理中…' : '保存并检查环境'}
              </button>
              <button
                type="button"
                data-testid="python-refresh"
                disabled={!editable || disabled || state.dirty || !status?.configured}
                onClick={() => void state.refresh()}
              >
                刷新节点包
              </button>
              {state.dirty && <span className="python-library__note">配置尚未保存</span>}
            </div>
            {status && (
              <div className="python-library__status" role="status" data-testid="python-status">
                <strong>
                  {status.ready ? '环境就绪' : status.configured ? '环境未就绪' : '尚未配置'}
                </strong>
                <span>{status.operatorCount} 个 Python 算子</span>
                {status.pythonVersion && <span>Python {status.pythonVersion}</span>}
                {status.numpyVersion && <span>NumPy {status.numpyVersion}</span>}
              </div>
            )}
            {status && status.problems.length > 0 && (
              <div className="python-library__problems" data-testid="python-problems">
                {status.problems.map((problem, index) => (
                  <details key={`${problem.path}:${index}`}>
                    <summary>
                      <strong>{problem.path || '运行环境'}</strong> · {problem.message}
                    </summary>
                    {problem.traceback && <pre>{problem.traceback}</pre>}
                  </details>
                ))}
              </div>
            )}
          </>
        )
      )}
    </section>
  );
}

export function PythonLibraryGuide() {
  return (
    <button
      type="button"
      className="python-library__guide nodrag nopan"
      data-testid="open-python-library"
      onPointerDown={(event) => event.stopPropagation()}
      onDoubleClick={(event) => event.stopPropagation()}
      onClick={(event) => {
        event.stopPropagation();
        usePythonStore.getState().open();
      }}
    >
      打开库 → Python 节点，导入对应节点包
    </button>
  );
}
