import { useGraphStore } from "../store/graph";

export interface ToolbarActions {
  onNew: () => void;
  onOpen: () => void;
  onSave: () => void;
  onSaveAs: () => void;
}

/** 从完整路径里取文件名，用于标题栏显示。 */
function baseName(path: string): string {
  const parts = path.split(/[\\/]/);
  return parts[parts.length - 1] ?? path;
}

export function Toolbar({ onNew, onOpen, onSave, onSaveAs }: ToolbarActions) {
  const undo = useGraphStore((s) => s.undo);
  const redo = useGraphStore((s) => s.redo);
  // 选长度而不是调 canUndo()：函数引用不变，组件不会因为栈变化而重渲染。
  const pastLen = useGraphStore((s) => s.past.length);
  const futureLen = useGraphStore((s) => s.future.length);
  const nextUndo = useGraphStore((s) => s.past[s.past.length - 1]?.label);
  const nextRedo = useGraphStore((s) => s.future[s.future.length - 1]?.label);
  const dirty = useGraphStore((s) => s.dirty);
  const filePath = useGraphStore((s) => s.filePath);
  const name = useGraphStore((s) => s.doc.name);
  const setName = useGraphStore((s) => s.setName);

  return (
    <header className="toolbar">
      <div className="toolbar__group">
        <button type="button" onClick={onNew} title="新建 (Ctrl+N)">新建</button>
        <button type="button" onClick={onOpen} title="打开 (Ctrl+O)">打开</button>
        <button type="button" onClick={onSave} title="保存 (Ctrl+S)">保存</button>
        <button type="button" onClick={onSaveAs} title="另存为 (Ctrl+Shift+S)">另存为</button>
      </div>

      <div className="toolbar__group">
        <button
          type="button"
          disabled={pastLen === 0}
          onClick={undo}
          title={nextUndo ? `撤销：${nextUndo} (Ctrl+Z)` : "撤销 (Ctrl+Z)"}
        >
          ↶ 撤销
        </button>
        <button
          type="button"
          disabled={futureLen === 0}
          onClick={redo}
          title={nextRedo ? `重做：${nextRedo} (Ctrl+Shift+Z)` : "重做 (Ctrl+Shift+Z)"}
        >
          ↷ 重做
        </button>
      </div>

      <div className="toolbar__doc">
        <input
          className="toolbar__name"
          value={name ?? ""}
          spellCheck={false}
          placeholder="未命名"
          onChange={(e) => setName(e.target.value)}
        />
        {/* 脏标记：没有它用户不知道自己有没有存过（交互清单 P0 #13） */}
        {dirty && <span className="toolbar__dirty" title="有未保存的改动">●</span>}
        {filePath && (
          <span className="toolbar__path" title={filePath}>
            {baseName(filePath)}
          </span>
        )}
      </div>
    </header>
  );
}
