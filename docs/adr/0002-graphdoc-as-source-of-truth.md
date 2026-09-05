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

- 映射层放在单独模块，禁止其他地方直接引用 React Flow 的类型作为持久化结构
- change 动作是封闭集合：`addNode` / `deleteNodes` / `moveNodes` / `setParam` / `connect` / `disconnect` / `setNodeUi`
- 纯 UI 操作（平移、缩放、选中变化）不进 undo 栈
