// 底部状态栏：这一层的节点 / 连线数、选中、库算子、缓存、core 版本与热重载、传输方式。

import { useEffect, useState } from "react";

import { levelOf } from "../lib/subgraph";
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
