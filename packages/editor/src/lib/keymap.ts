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
  // 输入框里的 Esc 归那个框（撤回打的字），不取消运行（useShortcuts）
  { id: "cancel", keys: ["Escape"], label: "取消运行", scope: "global", group: "运行" },
  // 在出错的节点之间跳（子图里的也打开进去）。F8 打不出字符，输入框里照样响应
  { id: "nextError", keys: ["F8"], label: "下一个出错的节点", scope: "global", group: "运行", inTextField: true },
  { id: "prevError", keys: ["Shift+F8"], label: "上一个出错的节点", scope: "global", group: "运行", inTextField: true },

  { id: "new", keys: ["Ctrl+N"], label: "新建", scope: "global", group: "文件" },
  { id: "open", keys: ["Ctrl+O"], label: "打开", scope: "global", group: "文件" },
  // 存盘打不出字符，输入框里照样响应：先提交那个框里打的字再存（useShortcuts 的 commitFocusedField）
  { id: "save", keys: ["Ctrl+S"], label: "保存", scope: "global", group: "文件", inTextField: true },
  { id: "saveAs", keys: ["Ctrl+Shift+S"], label: "另存为", scope: "global", group: "文件", inTextField: true },

  { id: "undo", keys: ["Ctrl+Z"], label: "撤销", scope: "global", group: "编辑" },
  { id: "redo", keys: ["Ctrl+Shift+Z", "Ctrl+Y"], label: "重做", scope: "global", group: "编辑" },
  { id: "copy", keys: ["Ctrl+C"], label: "复制", scope: "canvas", group: "编辑" },
  { id: "cut", keys: ["Ctrl+X"], label: "剪切", scope: "canvas", group: "编辑" },
  { id: "paste", keys: ["Ctrl+V"], label: "粘贴", scope: "canvas", group: "编辑" },
  { id: "duplicate", keys: ["Ctrl+D"], label: "原地复制", scope: "canvas", group: "编辑" },
  // 输入框里 Shift+D 是打一个大写 D：不在输入框里响应
  {
    id: "duplicateWired",
    keys: ["Shift+D"],
    label: "复制并保留输入（副本接同一个上游，并排调两组参数）",
    scope: "canvas",
    group: "编辑",
  },
  { id: "selectAll", keys: ["Ctrl+A"], label: "全选", scope: "canvas", group: "编辑" },
  { id: "delete", keys: ["Delete"], label: "删除选中", scope: "canvas", group: "编辑" },
  { id: "deleteHeal", keys: ["Ctrl+Delete"], label: "删除选中的节点并接通上下游", scope: "canvas", group: "编辑" },

  { id: "mute", keys: ["Ctrl+M"], label: "静音 / 取消静音", scope: "canvas", group: "节点" },
  { id: "collapse", keys: ["Ctrl+E"], label: "折叠 / 展开", scope: "canvas", group: "节点" },
  { id: "rename", keys: ["F2"], label: "改名（选中一个节点时）", scope: "canvas", group: "节点" },
  { id: "search", keys: ["Tab", "Space"], label: "搜索并添加算子（只选中一条连线时插到它中间）", scope: "canvas", group: "节点" },

  { id: "compose", keys: ["Ctrl+G"], label: "合成子图", scope: "canvas", group: "节点" },
  { id: "dissolve", keys: ["Ctrl+Shift+G"], label: "解散子图", scope: "canvas", group: "节点" },
  { id: "enterSubgraph", keys: ["Ctrl+Enter"], label: "进入子图", scope: "canvas", group: "节点" },
  // 沿连线走（数据从左往右流）：选中跟着走、画布移过去、预览跟着换。不按 Alt 的方向键仍是挪节点
  { id: "navUp", keys: ["Alt+ArrowLeft"], label: "选中上游节点（沿连线）", scope: "canvas", group: "节点" },
  { id: "navDown", keys: ["Alt+ArrowRight"], label: "选中下游节点（沿连线）", scope: "canvas", group: "节点" },
  { id: "navPrev", keys: ["Alt+ArrowUp"], label: "选中上一个同级节点（同一个上游的另一个下游）", scope: "canvas", group: "节点" },
  { id: "navNext", keys: ["Alt+ArrowDown"], label: "选中下一个同级节点", scope: "canvas", group: "节点" },

  { id: "findNode", keys: ["Ctrl+F"], label: "查找节点（连子图里面的，回车跳过去）", scope: "global", group: "视图" },
  { id: "layout", keys: ["Ctrl+L"], label: "整理布局", scope: "canvas", group: "视图" },
  { id: "fitView", keys: ["Ctrl+Shift+F"], label: "适配视图", scope: "canvas", group: "视图" },
  { id: "fitSelection", keys: ["F"], label: "适配选中的节点", scope: "canvas", group: "视图" },
  { id: "toggleDrawer", keys: ["Ctrl+`"], label: "日志与诊断抽屉", scope: "global", group: "视图" },
  { id: "maximizeViewer", keys: ["Shift+Space"], label: "最大化 / 还原预览（Esc 也还原）", scope: "global", group: "视图" },
  { id: "paramPanel", keys: ["Ctrl+Shift+P"], label: "参数面板", scope: "global", group: "视图" },
  { id: "compare", keys: ["Ctrl+Shift+D"], label: "对比 / 退出对比", scope: "global", group: "视图" },
  { id: "measure", keys: ["M"], label: "测量（预览里选点 / 测距）", scope: "global", group: "视图" },
  { id: "help", keys: ["?"], label: "快捷键面板", scope: "global", group: "视图" },
];

export const SHORTCUTS_BY_ID = new Map(SHORTCUTS.map((s) => [s.id, s]));

/** 显示用的第一个键位，按钮 title 里用。 */
export function keyHint(id: string): string {
  return keyLabel(SHORTCUTS_BY_ID.get(id)?.keys[0] ?? "");
}

const ARROWS: Record<string, string> = { ArrowLeft: "←", ArrowRight: "→", ArrowUp: "↑", ArrowDown: "↓" };

/** 键位串给人看的写法：方向键写成箭头。 */
export function keyLabel(combo: string): string {
  return combo.replace(/Arrow(Left|Right|Up|Down)/, (m) => ARROWS[m] ?? m);
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

/** 鼠标的用法，`?` 面板上与键表并列。按键能从键表生成，手势不能 —— 这张表就是唯一的那份，改了手势改这里。 */
export const MOUSE_GESTURES: readonly { area: string; items: readonly { gesture: string; does: string }[] }[] = [
  {
    area: "画布",
    items: [
      { gesture: "左键在空白处拖", does: "框选" },
      { gesture: "Ctrl / Shift + 拖框", does: "框住的加进选中" },
      { gesture: "Ctrl / Shift + 单击节点", does: "加选或取消这一个" },
      { gesture: "拖其中一个选中的节点", does: "整组一起移动" },
      { gesture: "中键或右键拖", does: "平移（右键按在节点、连线上拖也行）" },
      { gesture: "滚轮", does: "缩放" },
      { gesture: "双击空白处", does: "搜索并添加算子" },
      { gesture: "双击节点标题 / 子图节点", does: "改名 / 进入子图" },
      { gesture: "双击连线", does: "打开连线查看器" },
      { gesture: "从端口拖出、松在空白处", does: "搜索算子，选中后自动接上" },
      { gesture: "从端口拖出、松在节点身上", does: "接到它唯一能接的那个端口" },
      { gesture: "拖输入端连线的线头", does: "改接；松在空白处断开" },
      { gesture: "把节点（或面板里的算子）拖到连线上", does: "插到中间" },
      { gesture: "右键节点 / 连线 / 空白处", does: "菜单（空白处：添加算子、全选、整理、适配）" },
    ],
  },
  {
    area: "预览",
    items: [
      { gesture: "左键拖", does: "3D 下转视角，2D 剖面下平移" },
      { gesture: "右键拖 / 滚轮", does: "平移 / 缩放" },
      { gesture: "双击一个点", does: "转心挪到它上面（之后绕着它转）" },
      { gesture: "测量开着时单击", does: "选点、两点测距" },
    ],
  },
];
