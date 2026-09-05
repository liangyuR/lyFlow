# ADR-0001：前端用 React Flow，不用 Rete

- 状态：已采纳
- 日期：2026-09-05

## 背景

节点编辑器前端有两个主流选择：Rete.js（自带 dataflow 执行引擎）和 React Flow（纯画布 + 交互）。
初步倾向是 Rete，因为它"自带引擎"听起来省事。

## 决策

用 React Flow。

## 理由

**Rete 的核心卖点对本项目不成立。** Rete 的引擎是让计算在 JS 里跑的——节点的 `data()` 方法里做实际运算，
引擎负责传值和调度。而 LyFlow 的计算全部在 C++ 里。

真正需要做执行调度的那一层，必须知道点云有多大、中间结果能不能复用、显存够不够、能不能并行。
这些信息只有 C++ 侧有，不可能让前端的 JS 引擎去调度。

所以前端根本不需要执行引擎，只需要：**编辑图 → 序列化 → 丢给后端 → 显示状态**。
这样 Rete 一半的价值直接归零。剩下的部分（Control、Socket 类型）用 React Flow 的
custom node 和 `isValidConnection` 也能做，而且更直白。

## React Flow 白送的

画布、拖拽、连线交互、缩放平移、框选、Handle、minimap、`isValidConnection` 校验钩子。

## 要自己补的

见 [interaction-checklist.md](../interaction-checklist.md)。摘要：

- 便宜（几百行内）：拓扑排序+环检测、端口类型校验、序列化、状态高亮
- 中等：参数表单动态生成、撤销重做、复制粘贴、节点搜索面板
- 贵（可延后）：子图/复合算子、大图性能

参数表单这块**无论用哪个库都得自己写**，Rete 的 `Control` 只帮忙省了挂载那一步。

## 代价

- 撤销重做 React Flow 没有内置，要自己实现（用 immer patch 或 zundo，别去 diff 节点数组）
- 大图性能：每个节点是真实 DOM，几百个开始掉帧。点云 pipeline 一般几十个节点够用；
  不够时开 `onlyRenderVisibleElements`
- 交互细节的长尾需要自己补齐，这是主要风险，靠交互清单来管理

## 复议条件

如果哪天前端需要真的在 JS 里跑一部分计算（比如纯前端的轻量预览变换），重新评估。
但即便那时，也更可能是引入一个小的求值器，而不是换掉画布库。
