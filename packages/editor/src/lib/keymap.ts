// 快捷键的唯一那张表（E7）。useShortcuts 的分发和 `?` 面板都从它生成 ——
// 两处各维护一份必然漂移，用户看到的说明会和实际按键对不上。

export type ShortcutScope = "global" | "canvas" | "inspector";

export interface Shortcut {
  /** 稳定的动作 id，处理器按它注册。 */
  id: string;
  /** 显示用的按键组合。第一个是主键位，其余是别名（Ctrl+Y 之于 Ctrl+Shift+Z）。 */
  keys: string[];
  label: string;
  scope: ShortcutScope;
  /** 分组标题，只影响 `?` 面板的排版。 */
  group: string;
  /** 在输入框里也要响应。默认 false —— 打字时按 D 却删了节点是经典事故。 */
  inTextField?: boolean;
}

export const SCOPE_LABEL: Record<ShortcutScope, string> = {
  global: "全局",
  canvas: "画布",
  inspector: "参数",
};

/// keys 的写法：Ctrl/Shift/Alt + 键名，键名用 KeyboardEvent.key 的可读形式。
/// 匹配逻辑在 matchShortcut，Mac 上 Ctrl 与 Cmd 等价。
export const SHORTCUTS: readonly Shortcut[] = [
  { id: "run", keys: ["F5"], label: "运行", scope: "global", group: "运行", inTextField: true },
  {
    id: "runToNode",
    keys: ["Shift+F5"],
    label: "运行到选中节点",
    scope: "global",
    group: "运行",
    inTextField: true,
  },
  { id: "cancel", keys: ["Escape"], label: "取消运行", scope: "global", group: "运行", inTextField: true },

  { id: "new", keys: ["Ctrl+N"], label: "新建", scope: "global", group: "文件" },
  { id: "open", keys: ["Ctrl+O"], label: "打开", scope: "global", group: "文件" },
  { id: "save", keys: ["Ctrl+S"], label: "保存", scope: "global", group: "文件" },
  { id: "saveAs", keys: ["Ctrl+Shift+S"], label: "另存为", scope: "global", group: "文件" },

  { id: "undo", keys: ["Ctrl+Z"], label: "撤销", scope: "global", group: "编辑" },
  { id: "redo", keys: ["Ctrl+Shift+Z", "Ctrl+Y"], label: "重做", scope: "global", group: "编辑" },
  { id: "copy", keys: ["Ctrl+C"], label: "复制", scope: "canvas", group: "编辑" },
  { id: "cut", keys: ["Ctrl+X"], label: "剪切", scope: "canvas", group: "编辑" },
  { id: "paste", keys: ["Ctrl+V"], label: "粘贴", scope: "canvas", group: "编辑" },
  { id: "duplicate", keys: ["Ctrl+D"], label: "原地复制", scope: "canvas", group: "编辑" },
  { id: "selectAll", keys: ["Ctrl+A"], label: "全选", scope: "canvas", group: "编辑" },
  { id: "delete", keys: ["Delete"], label: "删除选中", scope: "canvas", group: "编辑" },

  { id: "mute", keys: ["Ctrl+M"], label: "静音 / 取消静音", scope: "canvas", group: "节点" },
  { id: "collapse", keys: ["Ctrl+E"], label: "折叠 / 展开", scope: "canvas", group: "节点" },
  { id: "search", keys: ["Tab", "Space"], label: "搜索并添加算子", scope: "canvas", group: "节点" },

  { id: "compose", keys: ["Ctrl+G"], label: "合成子图", scope: "canvas", group: "节点" },
  { id: "dissolve", keys: ["Ctrl+Shift+G"], label: "解散子图", scope: "canvas", group: "节点" },
  { id: "enterSubgraph", keys: ["Ctrl+Enter"], label: "进入子图", scope: "canvas", group: "节点" },

  { id: "layout", keys: ["Ctrl+L"], label: "整理布局", scope: "canvas", group: "视图" },
  { id: "fitView", keys: ["Ctrl+Shift+F"], label: "适配视图", scope: "canvas", group: "视图" },
  { id: "toggleDrawer", keys: ["Ctrl+`"], label: "日志与诊断抽屉", scope: "global", group: "视图" },
  { id: "paramPanel", keys: ["Ctrl+Shift+P"], label: "参数面板", scope: "global", group: "视图" },
  { id: "help", keys: ["?"], label: "快捷键面板", scope: "global", group: "视图" },
];

export const SHORTCUTS_BY_ID = new Map(SHORTCUTS.map((s) => [s.id, s]));

/** 显示用的第一个键位，按钮 title 里用。 */
export function keyHint(id: string): string {
  return SHORTCUTS_BY_ID.get(id)?.keys[0] ?? "";
}

/** `?` 面板的分组，顺序按表里第一次出现的先后。 */
export function groupedShortcuts(): { group: string; items: Shortcut[] }[] {
  const groups: { group: string; items: Shortcut[] }[] = [];
  for (const s of SHORTCUTS) {
    let bucket = groups.find((g) => g.group === s.group);
    if (!bucket) {
      bucket = { group: s.group, items: [] };
      groups.push(bucket);
    }
    bucket.items.push(s);
  }
  return groups;
}

interface KeyEventLike {
  key: string;
  ctrlKey: boolean;
  metaKey: boolean;
  shiftKey: boolean;
  altKey: boolean;
}

/** 一个键位串是否匹配这次按键。Mac 的 Cmd 与 Ctrl 等价。 */
function matchOne(combo: string, e: KeyEventLike): boolean {
  const parts = combo.split("+");
  const key = parts[parts.length - 1] ?? "";
  const wantCtrl = parts.includes("Ctrl");
  const wantShift = parts.includes("Shift");
  const wantAlt = parts.includes("Alt");

  if (wantCtrl !== (e.ctrlKey || e.metaKey)) return false;
  if (wantAlt !== e.altKey) return false;
  // Shift 必须精确匹配，否则 Shift+F5 会先撞上 F5。例外是本来就要按 Shift 才打得出
  // 的符号（`?`），那种键的 shiftKey 恒为 true，写成 Shift+? 反而反直觉。
  const shiftedSymbol = key.length === 1 && !/[a-z0-9]/i.test(key);
  if (!shiftedSymbol && wantShift !== e.shiftKey) return false;

  const pressed = e.key === " " ? "Space" : e.key;
  return pressed.toLowerCase() === key.toLowerCase();
}

/** 这次按键对应哪个动作。没有匹配时返回 null。 */
export function matchShortcut(e: KeyEventLike): Shortcut | null {
  for (const s of SHORTCUTS) {
    if (s.keys.some((k) => matchOne(k, e))) return s;
  }
  return null;
}
