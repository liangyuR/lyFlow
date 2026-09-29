// 编辑器实例的根元素（LyFlowEditor 上带 data-lyflow-editor 的那个 div）。拖动分栏、拖动参数这类
// 「整个编辑器都要换光标 / 禁指针」的状态类挂在它上面而不是 body：同一页面的宿主内容不受影响，
// 类名带 lyflow- 前缀，不和宿主的样式撞（docs/multi-instance-research.md）。

export function rootOf(el: Element | null | undefined): HTMLElement | null {
  return el?.closest<HTMLElement>("[data-lyflow-editor]") ?? null;
}
