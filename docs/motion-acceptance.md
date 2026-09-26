# 动效 验收记录

对应 [motion-plan.md](motion-plan.md) §3。2026-09-24，Windows 11，WebView2 153，`LYFLOW_PACKS=gap;dts`。

- `pnpm check`：退出码 0，「全链路绿」（C++ / schema / cargo test / 前端 typecheck + 单测 15/15 + 两个前端构建 / MCP 45/45）。
- `pnpm e2e`：退出码 0，**596/596 项通过**，其中新分组 `scripts/e2e/motion.mjs` 69 项。完整输出 grep
  「未验」「跳过」「FAIL」「✗」均为 0 行（gap / M8b / M8c 的分组都真的跑了）。
- **验收后的四项决定落地之后（第二个 commit，见文末）重跑**：`pnpm check` 退出码 0、全链路绿；
  `pnpm e2e` 退出码 0，**603/603**，动效分组 76 项，grep「未验」「跳过」「FAIL」「✗」仍为 0 行。
- `pnpm e2e:http`：`LYFLOW_E2E_HEADLESS=1` 下退出码 0，**35/35**，其中新增的 animations 开关一组 6 项。
  有头模式见文末「环境问题」。
- （2026-09-26 起 e2e 断言做过合并精简，这里的日志与条数是当时的快照；见 test/prune 精简提交）
  其中验收 9 的（6）三条（关动效时入边仍 `data-flowing`、流动层与 running 呼吸光的 animation-name 为 none）挪进了
  node-run 验收 11 的进度环采样（`noderun.mjs` 验收 10 那一组），动效分组不再另跑一条两百万点的慢链。

| # | 验收项 | 结果 |
|---|---|---|
| 1 | `pnpm check` 全过；`dependencies` 有 `motion`；`@keyframes` 只在 `styles.motion.css` | ✅ 通过 |
| 2 | 进场：addNode 淡入；loadDoc 与大图平移不闪 | ✅ 通过 |
| 3 | 删除：下一帧节点已不在、残影无 id/data-*、500 ms 消失、撤销再进场 | ✅ 通过 |
| 4 | 端点对齐：进场后、抖动后、hover 中、布局过渡中途与结束 ≤ 1 px | ✅ 通过（口径见下） |
| 5 | 连线生长：标记、长完无残留、惰性边恢复 `6 4` | ✅ 通过 |
| 6 | 流动：只有 running 目标的入边 `data-flowing="1"`，跑完清空 | ✅ 通过 |
| 7 | 状态反馈：done 恰闪一次、error 只抖标题栏、重新挂载不闪 | ✅ 通过 |
| 8 | hover（真鼠标）：节点、边、端口、拖线不淡化 | ✅ 通过 |
| 9 | 关动效：reduced-motion 下标记全无、流动边静态；`animations={false}` 验一次 | ✅ 通过 |
| 10 | 不回归：全部分组全绿，大图性能阈值不变 | ✅ 通过 |

## 1. 门禁、依赖、keyframes

```
$ git grep -n "@keyframes" packages/editor/src
packages/editor/src/styles.motion.css:43:@keyframes node-pulse {
packages/editor/src/styles.motion.css:148:@keyframes lyflow-edge-flow {
$ grep -n '"motion"' packages/editor/package.json
25:    "motion": "^12.43.0"
```

`git grep "transition:\|animation:"` 在 `styles.motion.css` 以外也是 0 行：原来散在 `styles.editor.css` 的
`node-pulse`、进度条过渡与 `styles.css` 里面板树箭头的过渡都迁了进来。代码只从 `motion/react` 导入。

## 2–9. `scripts/e2e/motion.mjs` 的实际输出

```
── 动效 验收 2：进场只给编辑出来的节点 —— addNode 淡入、loadDoc 与平移不闪
  ✓ addNode 后 32 ms（≤ 50）新节点 opacity < 1
  ✓ 400 ms 后 opacity 回到 1
  ✓ 播过一次进场标记，播完就撤了
  ✓ loadDoc 10 节点：画出了 10 个节点
  ✓ loadDoc 10 节点：任何节点在任何时刻都没有进场标记
  ✓ loadDoc 10 节点：任何连线都没有生长标记
  ✓ > 80 节点：开了虚拟化（只渲染了一部分）
  ✓ 平移之后有新节点进入视口（重新挂载了）
  ✓ 新进入视口的节点没有进场标记

── 动效 验收 3：删除 —— doc 立即改、残影无 id/data-*、500 ms 后消失、撤销再进场
  ✓ doc 里立即就没有这个节点了
  ✓ 下一帧 [data-testid=node-X] 已不存在
  ✓ 此时有一个残影元素，里面是节点的样子
  ✓ 残影子树里没有任何 id 与 data-* 属性
  ✓ 残影 aria-hidden、不接鼠标
  ✓ 500 ms 后残影消失
  ✓ 撤销一次恢复节点
  ✓ 恢复出来的节点播了进场
  ✓ 一次删除 31 个节点不出残影

── 动效 验收 4：端点对齐 —— 进场后、抖动后、hover 中、布局过渡中途与结束，误差 ≤ 1 px
  ✓ 进场结束：2 条边的端点与锚点最大偏差 0.002 px ≤ 1
  ✓ （对照）圆点中心离端点约半个圆点宽，锚点在圆点外缘不在圆心
  ✓ （前提）voxel 真的进了 error
  ✓ error 抖动结束：最大偏差 0.002 px ≤ 1
  ✓ （前提）hover 到了 voxel 上
  ✓ hover 节点期间：最大偏差 0.002 px ≤ 1
  ✓ Ctrl+L 之后进入了布局过渡
  ✓ 取样时还在过渡中途：画面上的位置还没到 doc 的终点
  ✓ doc 已经一步到位（一个撤销步）
  ✓ 布局过渡中途：最大偏差 0.003 px ≤ 1
  ✓ 过渡结束：画面位置与 doc 一致
  ✓ 布局过渡结束：最大偏差 0 px ≤ 1

── 动效 验收 5：连线生长 —— connect 后带生长标记，长完路径完整、惰性边虚线复原
  ✓ connect 后新边带生长标记
  ✓ 长完：标记撤掉、没有残留的 pathLength 与 dasharray
  ✓ 惰性边同样带生长标记（生长期间是实线）
  ✓ 惰性边长完 dasharray 恢复为 6 4
  ✓ 惰性边长完没有残留 pathLength

── 动效 验收 6：数据流动 —— 目标节点 running 时只有它的入边在流，跑完全图清空
  ✓ 拍到了一个有入边的节点正在 running
  ✓ 它的入边 data-flowing="1"
  ✓ 其余边没有 data-flowing
  ✓ 流动层在走（animation-name）
  ✓ 运行结束后全图没有 data-flowing="1"

── 动效 验收 7：状态反馈 —— done 恰闪一次、error 只抖标题栏、重新挂载不闪
  ✓ （前提）voxel 这次是真算的 done
  ✓ running → done 恰有一次 done 闪光标记
  ✓ （前提）voxel 进了 error
  ✓ → error 有一次 error 闪光标记
  ✓ 抖动期间 .node__head 有非零 translateX（最大 3.42 px）
  ✓ .node 本身始终没有 transform（35 帧）
  ✓ 抖完标题栏归位
  ✓ （前提）子图里挂上来的节点已经是 done
  ✓ （前提）回到顶层挂上来的节点也都跑完了
  ✓ 进出子图重新挂载的节点一次都没闪

── 动效 验收 8：hover（真鼠标）—— 节点高亮关联边、边亮两端、端口放大、拖线不淡化
  ✓ 鼠标到 voxel 上：它的两条边 is-related、另一条 is-dimmed
  ✓ 节点 hover 加深了阴影（不位移）
  ✓ 移开：全部清除
  ✓ 移到边上：两端端口 node-port--edge-end、两端节点 is-edge-end
  ✓ 边本身加粗了
  ✓ 移到端口：圆点计算后的缩放 1.35 > 1
  ✓ 端口 box-shadow 非 none
  ✓ （前提）确实在拖连线，而且鼠标此刻就停在 voxel 上（hoverNodeId 已经记上）
  ✓ 拖连线途中经过节点，不出现 is-dimmed

── 动效 验收 9：prefers-reduced-motion: reduce —— 标记全不出现，流动的边静态高亮
  ✓ 编辑器认出了系统设置（data-motion=off、根上 lyflow-motion-off）
  ✓ （2）addNode 两帧后 opacity 已经是 1
  ✓ （2）没有进场标记
  ✓ （3）删除不出残影
  ✓ （5）connect 没有生长标记
  ✓ （6）目标节点 running 时它的入边仍有 data-flowing="1"
  ✓ （6）流动层计算后的 animation-name 为 none
  ✓ running 的呼吸光也停了
  ✓ 运行结束后没有残留的 data-flowing
  ✓ 撤掉模拟之后动效恢复
```

几点做法：

- **标记怎么断言**：`data-entering` / `data-growing` / `data-flash` 只在动画期间挂着，页面里装一个
  MutationObserver 记下它们出现过几次（loadDoc 的「任何时刻都没有」、done 的「恰有一次」都靠它）。
- **第 2 条的大图**：96 个节点用 `addNode` 逐个**编辑**出来（每个都标过进场），等标记窗口过期后用真鼠标
  中键拖动平移两段，新挂上来的节点一个标记都没有。比 loadDoc 一张大图更狠：那条路径根本不标。
- **第 4 条的布局过渡**走真实入口 Ctrl+L；取样在 `data-layout-moving="1"` 出现后 130 ms，同时断言
  画面位置还没到 doc 的终点（证明取样确实在中途）、doc 已经是终点（「撤销栈顶是整理」那一条 2026-09-26 精简时删掉，m3 的「整理布局进了撤销栈」覆盖）。
- **第 7 条的重新挂载**：把 voxel 合成子图、跑完，然后进子图、退回顶层，两次挂上来的节点都已是 done，
  全程没有一次 `data-flash`。
- **第 9 条的 `animations={false}`** 在 `e2e:http` 里验：`examples/host-react` 的宿主栏加了一个「动效」开关
  （就是这个 prop 的示范），真鼠标点它：

```
── 宿主 animations={false}：关掉之后进场与生长都不播，打开恢复
  ✓ 开关关掉：编辑器根上是 lyflow-motion-off
  ✓ 关动效时新节点 40 ms 后就是不透明的
  ✓ 关动效时没有进场标记
  ✓ 关动效时新连线没有生长标记
  ✓ 开关打开：动效恢复
  ✓ 恢复之后新节点又播进场、新连线又会长
```

## 10. 不回归

`pnpm e2e` 全部分组 596/596。大图性能组的数（阈值没动）：

```
  ✓ 打开 300 节点的图用了 106 ms < 1000 ms
  ✓ 拖动时的帧率 56 fps ≥ 30
  ✓ 事件到渲染 60 ms < 100 ms
  ✓ 落点偏 16 个画布像素仍然吸附上了
```

既有脚本只动了一处：`phase_a.mjs` 读节点计算后的 opacity 之前，先等节点上有限的动画播完再过两帧。
原来是刚跑完就读：新节点还在播 200 ms 的进场，`is-not-demanded` 的 0.45 又有 S3 的过渡，读到的是半路上的值。
这是断言时机跟着动效调，不是放宽断言（阈值 `< 0.6` / `> 0.95` 没动）。

## 取舍与偏离（需要拍板的在最后）

1. **第 4 条的口径改成「端点对 React Flow 的锚点」，不是「端点对圆点中心」。** React Flow 的连线端点本来就
   不在圆心：源端口取圆点**右缘**、目标取**左缘**（`getHandlePosition`），和圆心差半个圆点宽。按字面断言
   「≤ 1 px 到圆心」在没有任何动效时也必然失败（实测约 5 px）。脚本断言的是端点对锚点 ≤ 1 px，另外把到圆心的
   距离作为对照记下来（全部 > 3 px，确认锚点口径没选错）。验的仍然是计划要的那件事：没有错位。
2. **（已按决定改掉，见文末第 1 项）A6 的前提不完全对：圆点放大并非「不改变量测」。** React Flow 量端口时 `x/y` 用 `getBoundingClientRect`
   （含 transform），宽高却用 `offsetWidth/Height`（不含）。圆点以圆心放大 1.35 倍的那一刻要是赶上量测，端点会
   偏约 1.75 px，直到下一次量测。照计划实现了（H4 放大的是圆点本身），目前只有端口 hover 会放大、hover 不改变
   节点尺寸，e2e 也没抓到错位；但「hover 着某个端口时这个节点的尺寸恰好变了」（比如运行结束多出状态行）理论上
   能触发。彻底的办法是把圆点画在 `::before` 上、只缩放伪元素（`getBoundingClientRect` 不含伪元素），这改了 A6
   允许的对象，没有自作主张，请定。已写进 `packages/editor/README.md` 的「踩过的坑」。
3. **布局过渡只给用户触发的「自动布局」**（工具栏按钮、Ctrl+L、右键「整理选中的布局」），经
   `withLayoutTransition()` 声明。`applyLayout` 本身不带过渡：打开缺坐标的文件时的初始布局属于「打开文件不该动」；
   验收脚本的 `placeAtScreen` 也用它摆位，紧接着就按屏幕坐标拖拽，位置还在飞会拖空。计划写的是
   「`applyLayout`（自动布局）」，我按「自动布局这个用户动作」理解；如果要所有 `applyLayout` 都过渡请说。
4. **一次冒出来超过 80 个节点（或 160 条边）不播进场**，与虚拟化阈值、布局过渡的上限同一个数。计划只给删除残影
   定了上限（30）；大粘贴时几百个节点同时淡入没有信息量，还拖慢那一帧。逐个加的不受影响。
5. **（已按决定改掉，见文末第 2 项）live preview 的每一次预览运行结束都会闪一次绿**（S2 对 → done 一视同仁）。拖滑块时预览一秒可能跑好几次，
   节点会连着闪。计划没覆盖预览运行，我没加例外；要不要在 `preview` 运行里不闪，请定。
6. **（决定：保留）graph store 加了一个 `epoch` 字段**（`newDoc` / `loadDoc` 各加一），画布差分靠它区分「编辑」与「换一整张图」。
   不进 doc、不进撤销栈，但 `useGraphStore` 是导出的，宿主能看到这个新字段。
7. **顺手修掉的三个老问题**（都是这次要动的规则，不修动效就叠不上去）：
   - 拖线时兼容端口的 `transform: scale(1.45)`、自动连线候选端口的 `scale(1.35)` 会把 `translateY(-50%)` 顶掉，
     圆点往下掉半个身位。现在所有放大都经 `--lyflow-handle-scale` 乘在居中的后面。
   - 连线的线宽写在映射层的 inline `style` 里，压过了 `.selected` 的 CSS 加粗 —— 选中的边其实从来没变粗过。
     现在 `style` 只放颜色与虚线，线宽在 CSS（`sameEdge` 不受影响，比的仍是 stroke 与 lazy）。
   - 映射层不再给惰性边写 `type: "lazy"`：唯一的边组件替换了 `default`，按 `data.lazy` 画虚线与 tooltip（E1）。
     `.react-flow__edge-lazy` 这个类随之消失，仓库里没有人用它。
8. **实现细节**：进场、闪光、抖动、残影这些元素动画用 motion 的 `animateMini`（纯 WAAPI）而不是完整版
   `animate(element)`。后者给元素建 VisualElement，终值在下一帧渲染批次里才写回，播完擦掉的 inline opacity 会被重新
   写成 1 —— 第一次全量 e2e 就这样把「未被需要」节点的半透明弄没了。连线生长是 `motion.path` 的 `pathLength`，
   布局过渡是 `animate(0, 1, { onUpdate })` 逐帧写一个经 `useSyncExternalStore` 读的位置覆盖（和 graph store 的
   更新落进同一次渲染，不会先闪一帧终点）。hover 状态在 ui store（`hoverNodeId` / `hoverEdge` / `hoverPaused`），
   流动看 `useNodeState(target)`，都不进节点/边的 `data`，`sameNode` / `sameEdge` 没有改。
9. **（已按决定改掉，见文末第 4 项）** 原先 `animations={false}` 不管编辑器自己调的 `fitView({ duration: 200 })`。

## 环境问题（与这次改动无关）

`pnpm e2e:http` **有头**跑时，改动前后都在「搭图 + 快捷键运行 + 坏参数」那一组超时（F5 触发不了运行）。
查下来是本机这次拉起的 Chrome 窗口 `document.visibilityState === "hidden"`：CDP 的键鼠事件送不进去，rAF 也不来。
`git stash` 掉全部改动后同样失败，所以不是回归。`LYFLOW_E2E_HEADLESS=1` 下 35/35 全过。新加的那一组
等待用 `setTimeout` 而不是 rAF，窗口被挡住时不会卡死。

## 验收后的四项决定（2026-09-24，第二个 commit）

1. **A6 修正：端口 hover 的放大画在圆点的 `::before` 上，圆点本身不变换。** 原因写进了 plan 的 A6 行：
   React Flow 量端口时位置取 `getBoundingClientRect`（含 transform）、尺寸取 `offsetWidth/Height`（不含），
   缩放赶上一次量测端点就偏约 1.75 px；伪元素不进 `getBoundingClientRect`。拖线期间的
   `.react-flow__handle-connecting/-valid` 与 compatible 的放大是老行为，没动。`styles.motion.css` 顶部注释、
   `packages/editor/README.md` 的坑同步改了。

   e2e 验收 8 改为读 `getComputedStyle(handle, '::before')`，另加「圆点本身缩放为 1」，以及一条重新量测的断言：
   鼠标停在 voxel 的**输入**圆点上时把它改一个长标题、节点被撑宽（ResizeObserver → `updateNodeInternals`，
   与强制重量等价；输入圆点在左侧不挪，鼠标仍在上面），然后断言它两条边的端点。锚点改为「圆心 ± 半个
   `offsetWidth`」算，不再直接取包围盒的左右缘 —— 否则圆点被缩放时基准和端点一起歪，假绿。

   ```
     ✓ 移到端口：圆点 ::before 计算后的缩放 1.35 > 1
     ✓ 圆点本身没有缩放（量测拿到的盒子不变）
     ✓ 端口的光晕（::before 的 box-shadow）非 none
     ✓ （前提）节点被撑宽了（118 → 177 px），鼠标仍在端口上
     ✓ hover 端口期间重新量测之后，它的两条边端点与锚点最大偏差 0 px ≤ 1
   ```

   **对照**（证明这条断言不是白给的）：在运行中的 app 里注入一条旧写法
   `.node-port:hover .react-flow__handle { --lyflow-handle-scale: 1.35 }`（缩放圆点本身）再跑同一组：

   ```
     ✗ 圆点本身没有缩放（量测拿到的盒子不变） — {"scale":1.35,"ownScale":1.35,…}
     ✗ hover 端口期间重新量测之后，它的两条边端点与锚点最大偏差 1.75 px ≤ 1
         [{"id":"e_8pf5y2","start":0,"end":1.75,…},{"id":"e_bdmp9e","start":0,"end":0,…}]
   ```

   偏的正好是被 hover 的那一端、正好 1.75 px（10 px 圆点 × 0.35 ÷ 2），与分析一致。

2. **实时预览结束时不播 done 闪光**，error 的闪光与抖动照播（plan 的 S2 补了一句）。判据是状态变成 done 那一刻
   `execution.preview` 为真。e2e 验收 7 加了一次 `run({ preview: true })`（换了 seed，保证 voxel 真算）：

   ```
     ✓ （前提）这是一次预览运行，voxel 真算了（done 不是 skipped）
     ✓ 预览运行结束后节点没有 data-flash="done"
   ```

3. **`epoch` 保留**，未改。

4. **关动效时视口动画也为 0**：编辑器自己调的五处 `fitView`（适配视图的快捷键与右键菜单、整理之后、
   进子图、打开缺坐标的文件）时长改由 `lib/motion.ts` 的 `viewportMs()` 取，开时 200 ms、关时 0。编辑器里没有
   调 `setCenter` / `setViewport` / `zoomTo` 的地方。plan 的 A4 补了一句，「不做」里的 fitView 例外改成
   「原有的 fitView 过渡保留，但受 A4 的开关管」。e2e 验收 9 先缩远、再按 Ctrl+Shift+F，读 60 ms 与 400 ms 时的缩放：

   ```
     ✓ （对照）动效开着时适配视图有过渡：60 ms 时缩放还没到终值
     ✓ 关动效时适配视图一步到位：60 ms 时已经是终值（fitView 的 duration 为 0）
   ```

大图性能组这一轮的数：打开 300 节点 126 ms（< 1000）、拖动 57 fps（≥ 30）、事件到渲染 60 ms（< 100）。
