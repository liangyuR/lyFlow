// 窗口标题 `文件名 *`。没有它用户开两个窗口就分不清哪个是哪个。
// 这是壳的事，不是编辑器的事（A2-1）。

import { useGraphStore } from "@lyflow/editor";

function baseName(path: string): string {
  const parts = path.split(/[\/]/);
  return parts[parts.length - 1] ?? path;
}

function apply(): void {
  const { filePath, dirty, doc } = useGraphStore.getState();
  const label = filePath ? baseName(filePath) : (doc.name ?? "未命名");
  document.title = `${label}${dirty ? " *" : ""} — LyFlow`;
}

export function installWindowTitle(): void {
  apply();
  useGraphStore.subscribe(apply);
}
