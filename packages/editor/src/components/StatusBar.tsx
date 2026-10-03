// 底部状态栏：这一层的节点 / 连线数、选中、库算子、缓存、core 版本与热重载、传输方式。

import { useEffect, useMemo, useState } from "react";

import { listGraphNodes } from "../lib/findNodes";
import { levelOf, pathPrefix } from "../lib/subgraph";
import { formatBytes, useCacheStore } from "../store/cache";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import { transport } from "../transport";

const TRANSPORT_LABEL: Record<string, string> = {
  tauri: "Tauri · 实时",
  http: "HTTP · 实时",
  static: "静态快照",
};

const TRANSPORT_TITLE: Record<string, string> = {
  tauri: "实时读取 C++ 注册表",
  http: "经 HTTP 后端读取 C++ 注册表",
  static: "静态模式：读的是 dump 出来的 manifest 快照，可能过期",
};

export function StatusBar() {
  const coreInfo = useManifestStore((s) => s.coreInfo);
  const transportKind = useManifestStore((s) => s.transportKind);
  const path = useUiStore((s) => s.path);
  const doc = useGraphStore((s) => s.doc);
  const level = levelOf(doc, path);
  const nodeCount = level.nodes.length;
  const edgeCount = level.edges.length;
  const selected = useUiStore((s) => s.selectedNodes.size);
  const stats = useCacheStore((s) => s.stats);
  const ops = useManifestStore((s) => s.operatorsById);
  // 静音的节点（连子图里面的）：忘了取消的静音会悄悄改掉结果，而节点上的斜纹在大图里常常不在视野里
  const muted = useMemo(() => {
    const all = listGraphNodes(doc, ops).filter((e) => e.muted);
    const here = pathPrefix(path);
    return { total: all.length, here: all.filter((e) => pathPrefix(e.path) === here).length };
  }, [doc, ops, path]);
  const [libraryCount, setLibraryCount] = useState(0);

  useEffect(() => {
    void transport
      .getLibraryStatus()
      .then((s) => setLibraryCount(s.count))
      .catch(() => setLibraryCount(0));
  }, []);

  return (
    <footer className="statusbar">
      <span className="statusbar__milestone">M4 · 能扩展</span>
      <span>{nodeCount} 节点</span>
      <span>{edgeCount} 连线</span>
      {libraryCount > 0 && (
        <span data-testid="statusbar-library" title="库算子（app data 下的 library/）">
          库 {libraryCount}
        </span>
      )}
      {selected > 0 && <span>已选 {selected}</span>}
      {muted.total > 0 && (
        <button
          type="button"
          className="statusbar__muted"
          data-testid="statusbar-muted"
          title={`静音的节点不参与计算、输出直通上游：这一层 ${muted.here} 个${
            muted.total > muted.here ? `，别的层 ${muted.total - muted.here} 个` : ""
          } —— 点一下列出来（Alt+Enter 选上这一层的）`}
          onClick={() => useUiStore.getState().setFinderOpen(true, "is:muted ")}
        >
          静音 {muted.total}
        </button>
      )}
      <span className="statusbar__spacer" />
      {stats && (
        <span
          className="statusbar__cache"
          data-testid="statusbar-cache"
          title={`结果缓存 ${stats.entries} 条，预算 ${formatBytes(stats.budgetBytes)}`}
        >
          缓存 {formatBytes(stats.bytes)}
        </span>
      )}
      {coreInfo && (
        <>
          <span>lyflow-core {coreInfo.version}</span>
          <span data-testid="statusbar-operators">{coreInfo.operatorCount} 算子</span>
          {coreInfo.hotReload && (
            <span
              className="statusbar__hot"
              data-testid="statusbar-generation"
              data-generation={coreInfo.generation ?? 0}
              title="开发期热重载已开启：改 C++ 存盘即生效"
            >
              热重载 · 第 {coreInfo.generation ?? 0} 代
            </span>
          )}
        </>
      )}
      <span className={`statusbar__transport statusbar__transport--${transportKind}`} title={TRANSPORT_TITLE[transportKind]}>
        {TRANSPORT_LABEL[transportKind]}
      </span>
    </footer>
  );
}
