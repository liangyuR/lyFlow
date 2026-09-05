# ADR-0002：GraphDoc 是唯一数据模型，不用 React Flow 的类型

- 状态：已采纳
- 日期：2026-09-05

## 决策

自定义一套 `GraphDoc`（算子 id、参数值、连接关系、UI 位置）作为唯一真实数据源。
渲染时映射成 React Flow 的 `Node[]` / `Edge[]`，交互后通过语义化 change 动作写回 GraphDoc。

**不要**把 React Flow 的 `Node` / `Edge` 直接当作数据模型。

## 理由

1. **React Flow 的类型里塞了一堆 UI 运行时状态** —— `selected`、`dragging`、`measured`、`positionAbsolute`。
   这些不该被序列化进文件。混在一起就一定会漏进去。
2. **后端只关心拓扑和参数，不关心坐标。** 数据模型应该反映这个边界，而不是让 C++ 侧去过滤 UI 字段。
3. **隔离升级风险。** React Flow 大版本升级或将来换库时，只改映射层，不动数据模型、不动文件格式、不动后端契约。

## 后果

**正面**

- 文件格式干净、可稳定序列化，git diff 有意义
- 撤销重做基于语义化 patch，而不是节点数组的结构 diff
- 脚本 / headless 生成的图和 UI 生成的图是同一种东西

**负面**

- 多一层映射，有同步成本
- 高频操作（拖动节点）需要注意：拖动过程中的位置更新不应每帧都写回 GraphDoc 并压入 undo 栈，
  应在拖动结束时合成一条 `moveNodes` 记录

## 实施要点

- 映射层放在单独模块（`app/src/lib/mapping.ts`），禁止其他地方直接引用 React Flow
  的类型作为持久化结构
- change 动作是封闭集合：`addNode` / `deleteNodes` / `moveNodes` / `setParam` /
  `connect` / `disconnect` / `setNodeUi` / `pasteNodes`
- 纯 UI 操作（平移、缩放、选中变化）不进 undo 栈

## M1 实施后的两条补充

**撤销历史存整份 doc 快照，不存 patch。** 原文说「基于语义化 patch」，实际落地时
选了快照。语义化的部分保留在 change 动作上 —— 它决定了「一步撤销」的边界，这才是
关键。存储形式则用快照：immer 的结构共享让未改动的节点在新旧快照间共用同一份对象，
几十个节点的图一次快照只增量存被改动的那部分；而 patch 的路径基于数组下标，删一个
节点会让此前所有 patch 的下标失效，复杂度换不来这点内存。

**UI 运行时状态必须有个去处，不能只是「丢掉」。** 节点的量测尺寸是纯 UI 状态，
按本 ADR 不进 GraphDoc，我们在 `onNodesChange` 里丢弃了 dimensions 事件 ——
结果 React Flow 的 MiniMap 一个节点都不画，因为它判断「节点有没有尺寸」看的正是
我们传回去的那个对象。

修法不是妥协把尺寸塞进 GraphDoc，而是给它一个 UI 侧的旁路缓存（画布组件里的 ref），
映射时合并进去。教训是：**「不进数据模型」和「不存在」是两回事**，丢弃 UI 状态前
要先问一句渲染层还需不需要它。
