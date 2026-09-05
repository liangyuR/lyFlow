import { useEffect, useState } from "react";

import { NodePalette } from "./components/NodePalette";
import { OperatorDetail } from "./components/OperatorDetail";
import { useManifestStore } from "./store/manifest";

function StatusBar() {
  const { coreInfo, transportKind, status } = useManifestStore();

  return (
    <footer className="statusbar">
      <span className="statusbar__milestone">M0 · 契约先行</span>
      <span className="statusbar__spacer" />
      {coreInfo && (
        <>
          <span>lyflow-core {coreInfo.version}</span>
          <span>{coreInfo.operatorCount} 算子</span>
          <span>{coreInfo.typeCount} 端口类型</span>
        </>
      )}
      {/* transport 必须显眼：static 模式下看到的是 dump 出来的静态快照，
          不标出来的话迟早有人对着三天前的 manifest 调半天。 */}
      <span
        className={`statusbar__transport statusbar__transport--${transportKind}`}
        title={
          transportKind === "tauri"
            ? "实时读取 C++ 注册表"
            : "浏览器模式：读的是 public/manifest.dev.json 静态快照，可能过期"
        }
      >
        {transportKind === "tauri" ? "Tauri · 实时" : "静态快照"}
      </span>
      <span className="statusbar__status">{status}</span>
    </footer>
  );
}

function Placeholder() {
  return (
    <div className="placeholder">
      <h1>LyFlow</h1>
      <p className="placeholder__lead">
        左侧的分类树、搜索结果、端口颜色和参数定义，全部来自 C++ 侧的算子注册表。
        前端没有硬编码任何一个算子。
      </p>
      <ol className="placeholder__steps">
        <li>
          在 <code>core/src/ops/</code> 加一个 <code>.cpp</code>
        </li>
        <li>
          在 <code>core/src/builtin_ops.cpp</code> 加一行调用
        </li>
        <li>
          重启 <code>pnpm tauri dev</code> —— 它就出现在左边了
        </li>
      </ol>
      <p className="placeholder__note">
        这条链路就是 M0 的全部产出。它通了，后面加几十个算子都是无痛的。
        <br />
        选一个算子看看它的完整描述。
      </p>
    </div>
  );
}

export default function App() {
  const { status, error, load, operatorsById } = useManifestStore();
  const [selectedId, setSelectedId] = useState<string | null>(null);

  useEffect(() => {
    void load();
  }, [load]);

  const selected = selectedId ? operatorsById.get(selectedId) : undefined;

  return (
    <div className="app">
      <main className="app__body">
        <aside className="app__sidebar">
          {status === "loading" && <p className="app__hint">正在读取算子描述…</p>}
          {status === "error" && (
            <div className="app__error">
              <strong>读不到算子描述</strong>
              <p>{error}</p>
              <button type="button" onClick={() => void load()}>
                重试
              </button>
            </div>
          )}
          {status === "ready" && (
            <NodePalette selectedId={selectedId} onSelect={setSelectedId} />
          )}
        </aside>

        <section className="app__content">
          {selected ? <OperatorDetail op={selected} /> : <Placeholder />}
        </section>
      </main>

      <StatusBar />
    </div>
  );
}
