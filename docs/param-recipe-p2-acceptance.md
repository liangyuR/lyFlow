# 参数配方 P2（参数面板与全部类型控件）验收记录

对应 [param-recipe-plan.md](param-recipe-plan.md) 的 P2（P2.1–P2.10，验收 9–16）。2026-09-24，Windows 11，WebView2，
`LYFLOW_PACKS=gap;dts`。截图：[params-p2-panel.png](params-p2-panel.png)（面板打开、与画布并排、选中「全类型示例」节点）、
[params-p2-types.png](params-p2-types.png)（面板最大化，transform 与 curve 控件），由 `scripts/e2e/record-params-p2.mjs` 在真实 app 里截。

- `pnpm check`：退出码 0，「全链路绿」。doctest **301/301**（新文件 `core/tests/test_param_values.cpp` 6 个用例：showcase 的
  14 种类型与注册表自检、manifest 导出 curve 默认值是对象 / flags 选项是整数、curve 值的合法性、transform 长度、curve 进 cacheKey 与键顺序
  无关且原样进 compute、`evaluateCurve`），`cargo test`（bridge）**143/143**，manifest 对 schema 两遍（71 个算子；带
  `LYFLOW_TEST_OPS=1` 的 72 个，含 transform / curve 默认值），graph-doc 样例多了 `examples/param-showcase.lyflow.json`，
  `@lyflow/editor` 单测 **43/43**（新增 `param-values.test.mjs` 8 条、`param-panel.test.mjs` 6 条），两个前端构建，MCP 45/45。
- `pnpm e2e`：退出码 0，**908/908**，其中新分组 `scripts/e2e/params_p2.mjs` **121 项**（验收 9：20、10：37、11：9、12：24、13：13、
  14：9、15：9）。完整输出落盘后 grep「未验」「跳过」「FAIL」「✗」「中断」均为 0 行；gap 的张量组、M8b / M8c 分组都真的跑了，M8b 验收 7 没有卡住。
- `pnpm e2e:http`：`LYFLOW_E2E_HEADLESS=1` 下退出码 0，**51/51**。
- （2026-09-26 起 e2e 断言做过合并精简，这里的日志与条数是当时的快照；见 test/prune 精简提交）
- 以上三项都是最后一次代码改动之后跑的。

| # | 验收项 | 结果 |
|---|---|---|
| 9 | 开关（按钮 / Ctrl+Shift+P）、拖宽、最大化与还原、宽度记忆；打开时 Inspector 不显示；画布选中 ↔ 面板定位双向 | ✅ 通过 |
| 10 | 14 种类型的控件都能改值，doc 里值与类型正确；transform / curve 改 → 存盘 → 重开不变 | ✅ 通过 |
| 11 | advanced 默认折叠；visibleWhen / enabledWhen 随另一个参数即时生效 | ✅ 通过 |
| 12 | 搜索与每个过滤 chip 的结果（期望值对着 doc 独立算） | ✅ 通过 |
| 13 | 子图定义行有共享标记，改它两个实例的有效值都变；库算子内部只读 | ✅ 通过 |
| 14 | ROI 缩略图 → 2D 拖框视图，拖动后参数变化，一次撤销还原 | ✅ 通过 |
| 15 | 1000 参数的图：打开 < 300 ms、滚动 ≥ 50 fps、输入到画布状态 < 100 ms | ✅ 通过 |
| 16 | 不回归：`pnpm check`、带包的 `pnpm e2e`、headless `pnpm e2e:http` | ✅ 通过 |

## 9. 面板形态（P2.1、P2.2）

- 工具栏「参数」按钮（`param-panel-toggle`，`aria-pressed`）与 `Ctrl+Shift+P`（键表 `paramPanel`，`?` 面板里自动出现）开关；
  状态在 ui store 的 `paramPanel`（纯 UI，不进 doc 不进撤销）。开着时右侧那一列换成面板，`Inspector` 不渲染（断言
  `.app__inspector` 不在 DOM 里）。3D 视图仍在这一列顶上，收成一条「3D 预览」标题栏，点开或 ROI 行「拖框」时展开；
  Viewer3D 始终是同一个实例，切换时不重建 WebGL 场景。
- 拖宽：真鼠标拖分栏把手 120 px，宽度变化 ±6 px 内；松手时记进 `localStorage["lyflow.paramPanel.width"]`，关上再开还是这个宽。
  面板与 Inspector 各记各的宽度（面板默认 640，Inspector 380）。
- 最大化（「□」）：画布与算子面板收起、面板占满工作区；还原后画布回来、面板回到记住的宽度。画布只被压成 0 宽而不卸载，
  节点尺寸与端口量测都还在（motion-plan A5 的顾虑）。
- 画布 ↔ 面板：画布上真鼠标点节点 → 面板滚到那一节并高亮（`data-focused`，在列表视口里）；面板里点节点标题 → 画布选中它并
  `setCenter` 居中（中心偏差 ≤ 12 px 断言）。定义里的节点先进到那一层再选中。
- 三个页签：按节点 / 配方矩阵 / 配方管理；后两个是占位（写明 P3 提供什么）。

## 10. 14 种类型的控件（P2.6）

`test.param_showcase` 的一个节点接在 gen 后面，全部经面板里的真实 DOM 事件改（数字框打字失焦、下拉 change、复选框 / chip click、
取色器 input，curve 的点用真鼠标拖）：

| 类型 | 参数 | 改成 | doc 里 |
|---|---|---|---|
| bool | enabled | 点复选框 | `false` |
| int | iterations | 42 | `42`（整数） |
| float | gain | 1.5 | `1.5` |
| vec2f / vec3f / vec4f | offset / scale / weights | 某一分量 | `[0,0.25]` / `[2,1,1]` / `[0.25,0.25,0.25,0.5]` |
| enum | mode | 精确 | `"precise"` |
| flags | features | 点「颜色」 | `7`（3 \| 4） |
| string / text / path | tag / note / exportPath | 文本 | 原样字符串（path 行先打开 Write File 才露出来） |
| color | tint、overlay（带 alpha） | `#ff8000`、A = 0.25 | `[1, 0.502, 0]`、`[1, 0.5, 0.1, 0.25]` |
| transform | pose | T x = 0.5，R z = 90°，再切 4×4 改 m[11] = 0.2 | 16 个数，行主序，摘要「T[0.5, 0, 0.2] R[0, 0, 90]°」 |
| curve | response、falloff | 真鼠标拖点、列表改 y、「+ 控制点」、换线性 | `{points, interp}`，x 在 0–1 严格递增 |

然后 F5：运行成功，core 的 echo（`test.param_showcase` 把收到的参数原样回显成 Record）里的 pose / response 与 doc 逐字相同 ——
core 接受每一种值。往返：经 Tauri 存盘 → `loadGraph` 读回 → `loadDoc`，pose / response / falloff 与存盘前深相等，面板里的
transform 摘要与曲线点数不变。

## 11. 折叠与联动（P2.3）

- advanced 组（「高级」）默认收起、里面的行不挂；点标题展开。搜索或过滤生效时一律展开（命中的行不能藏在收起的组里）。
- visibleWhen：Mode = 快速时 Custom Factor 不在，改成自定义立刻出现，改回又藏；Write File 控制 Export Path。
- enabledWhen：关掉 Enabled → Tolerance 置灰（`data-enabled="0"`、输入框 disabled）；`ne` 条件：Mode = 快速时 Precision 置灰，改成精确可编辑。

## 12. 搜索与过滤（P2.5）

图：gen + show（改了 tag / mode / iterations = 5000 越界）+ 体素滤波（改了 leafSize、改了标题），再在面板里点 show.gain 的空心书签纳入配方。
期望值在 Node 这边照 `lib/paramPanel.ts` 写明的判据对着 doc + manifest + 校验诊断独立算：

- chip 计数（全部 / 已改动 / 配方 / 诊断）逐个相等；「全部」（advanced 展开后）与「诊断」下把虚拟列表从头滚到尾收齐的行键 = 期望集合
  （2026-09-26 起「已改动」「配方」两个 chip 不再单独比列表，由计数与下面的「类型 + 已改动」兜着）；
- 搜索「参数全类型 iter」（多词都要中）「LEAF」（大小写不敏感）的结果，以及搜索时「全部」计数跟着变
  （2026-09-26 起从五个词减到这两个，单词的「gain」「体素」「demo-tag」走的是同一条判据）；
- 类型 vec3f + 已改动，「类型 ▾」下拉里的计数叠着当前 chip；
- 诊断（P2.8）：越界的 iterations 行下贴着消息；图参数 gain 的 default 设成 50（越过它自己的 max 10）后，诊断贴在「图参数」分组那一行下。

判据：行的全集 = 图参数 + 当前层每个节点的**可见**参数 + 子图实例展开进定义的节点（按实例各算一份）；已改动 = 与算子默认不同
（行首蓝点；图参数行比第一个绑定目标的默认值）；配方 = 图参数本身与由它提供的行；诊断 = 带 paramPath 的 error / warning
（编辑期校验 + 上次运行的错误）；搜索词按空白切开，每个都要出现在「参数名 label 节点标题 展开后的节点 id 值的文本」里。

## 13. 子图定义与库算子（P2.3、P2.4）

打开 `examples/param-showcase.lyflow.json`（两个「预处理」实例共享 `sg_pre`）：实例下面有「子图定义 · 2 个实例共享」一栏（默认收起），
展开后定义里的节点标题旁同样的徽标，行上 `data-shared="2"`。在实例 A 下面把定义里 voxel 的 leafSize x 改成 0.03：doc 里改的是
`subgraphs.sg_pre`；实例 B 下面同一行显示 0.03；plan 里两个实例内部 voxel 的 cacheKey 都变；再跑两个实例点数相同、与改之前不同。
库算子：把子图存成 `lib.e2e_p2_*` 放进画布 —— 那一节标「库算子 · 内部只读」，没有「子图定义」可展开，也没有任何内部行。

## 14. ROI（P2.7）

ROI 行有一块 72×44 的缩略图：同组可见的框都画、这一个高亮；3D 视图取过这个节点的云之后按底图的 XY 范围画（`data-from-cloud="1"`），
之前按几个框的并集。点「拖框」（或缩略图）：面板上方的视图展开、相机切 2D（相机模式挪进了 ui store）、选中该节点、切到这个框所在的组；
真鼠标拖框身向右，xMin / xMax 都变大、宽度不变，面板那一行的数跟着变，撤销记录「拖动 ROI」；一次 Ctrl+Z 回到原值。

## 15. 性能（P2.9）

脚本生成 50 个 `test.param_showcase`（每个 22 个参数、20 个可见、值各不相同）= 1000 个可见参数，写文件、经 `loadGraph` 打开：

| 指标 | 阈值 | 实测（最后一次 `pnpm e2e`） |
|---|---|---|
| 从 toggle 到第一批行画出来（多等一帧） | < 300 ms | **35 ms** |
| 真鼠标滚轮一路滚下去的帧率（中位） | ≥ 50 fps | **57 fps** |
| 数字框打字失焦 → graph store 的 doc 更新并画完一帧 | < 100 ms | **13 ms** |

「全部」chip 的计数 = 1000（与脚本那边数出来的可见参数数相等）。

- 虚拟化：`components/VirtualList.tsx`，按类型估行高、ResizeObserver 量真值、视口上方行高变化时补 scrollTop；只挂视口附近的行
  （断言挂着的 < 120、列表总项 > 500）。
- 帧率：rAF 采样（中位帧间隔），真鼠标滚轮 60 下、每下 160 px 一路往下滚。
- 输入：挂着的一个 float 行里打字失焦，量到 graph store 的 doc 换了并画完一帧。

## 16. 不回归

见页首。第一遍完整 `pnpm e2e` 是 907/908：ROI 分组「视图取过云之后缩略图按底图范围画」读得太早（首次取云要经 IPC），
改成等视图显示出那片云再读；排查时顺带发现下面「P2 之外发现的问题」那一条，把 ROI 分组挪到库算子分组前面（修复后已挪回）。改完之后
`pnpm check`、`pnpm e2e`（908/908）、`pnpm e2e:http`（51/51）按顺序重跑，都绿；截图在那之后另起 app 截。

## 取舍与计划没覆盖的决定

1. **transform 与 curve 的值格式**。manifest 对 transform 只有 core 一侧「16 个数」的校验、对 curve 完全没有约定（core 原样存字符串、
   不校验），所以 P2 定下来并写进 [operator-manifest.md](operator-manifest.md)「transform 与 curve 的值」与 schema
   （`$defs.transformValue` / `$defs.curveValue`，`paramSpec` 里按 type 约束 default）：
   - transform = 16 个数的 4×4 **行主序**矩阵（与 core 的 `Transform` 数据类型同一布局），编辑器拆成平移 + 欧拉角（度，内旋 X→Y→Z，
     R = Rz·Ry·Rx，与 `transform.make` 同一约定）；非刚体只给矩阵视图。
   - curve = `{"points": [[x, y], …], "interp": "linear" | "smooth"}`：至少两点、x ∈ [0, 1] 严格递增、y 有限且受 min/max 约束，
     只有这两个键；smooth 是 Fritsch–Carlson 单调三次。core 的 `coerceParam` 按这套判据查（不合法报 `bad_param`），注册表自检查
     默认值，manifest 导出时默认值还原成对象；新增 `lyflow::checkCurveValue` / `lyflow::evaluateCurve`（`lyflow/manifest.h`）。
     x 固定 0–1 是我的选择：曲线参数的通常用法是「归一化输入 → 输出」，定义域也放开的话限位、画布范围都得多一套字段。
2. **测试算子怎么「只在测试 / e2e 构建里注册」**：`test.param_showcase` 放在 `core/tests/param_showcase_op.h`（与 `test_ops.h` 同处）。
   doctest 经 `ensureTestOps()` 注册；e2e 跑的是真的 app（`tauri dev`），所以 core 编进了 `core/tests/e2e/register_e2e_ops.cpp`，
   **运行时**看环境变量 `LYFLOW_TEST_OPS=1` 才注册（`harness.mjs` 起 app 时设）。没有做成编译开关：e2e 与日常开发用的是同一份
   dev 构建，编译开关会让两者来回重编 core。没设变量时注册表、manifest 与之前逐字节相同（`lyflow-dump-manifest --check` 仍是 71 个）。
   `examples/param-showcase.lyflow.json` 因此只在带这个变量的 app / CLI 里能跑。
3. **P1 的 Inspector 图参数简表**：面板开着时 Inspector 整个不渲染，由面板的「图参数」分组取代；面板关着时简表**保留** —— 不开面板也
   看得见、改得动、删得掉，P1 的 e2e 也靠它。改规格（label、限位、单位、group、名字）只在面板里有。
4. **advanced 默认折叠只做在面板里**，Inspector 不动：Inspector 里 locate_template 的模板槽组（大多是 advanced）是 L20 的手风琴，
   与 2D 视图的切换条联动；收进 `<details>` 的输入框失焦不触发提交，Inspector 这边的既有交互与 e2e 都要跟着重验，超出 P2。面板关着时 Inspector 仍是老样子。
5. **子图的展开按实例各列一份**：同一个定义在两个实例下的绑定链不同（K2 的提升链是按实例绑的），所以「子图定义」一栏挂在每个实例下面，
   行的全集与计数也按实例算（两个实例的定义行各算一次）。面板列的是当前层级（`ui.path`）；在定义里的行上编辑经
   `setParam(…, at)` 作用到那一层（store 动作多了可选的 `at`，`promoteToGraphParam` / `promoteParam` / `unpromoteParam` 同）；
   点定义里的节点标题或「拖框」会进到那一层。
6. **书签**：空心 = 点一下纳入配方（`promoteToGraphParam`，子图里是整条提升链）；实心 = 已纳入，点一下滚到「图参数」分组的那一行
   （解除绑定在那一行和右键菜单里，不放在一个一碰就改图的图标上）。
7. **顺手修了三处老问题**：① React Flow 点选节点时只改内部的 nodeLookup、不发 store 更新，`onSelectionChange` 要等下一次更新才来 ——
   不带移动的一次点击（触控板轻点、CDP 真鼠标）选中慢一拍，面板的「画布选中 → 定位」跟着慢。画布在 `onNodesChange` /
   `onEdgesChange` 里把 select 变更直接写进 ui store。② `lib/files.ts` 的 `baseName` 只认 `/`，Windows 路径在工具栏上显示成整条；
   工具栏加了「参数」按钮后放不下，顺手改成两种分隔符都认，并让工具栏不折行。③ manifest 导出 flags 的选项值写成了字符串（`"4"`），
   编辑器按位与永远是 0 —— 之前没有任何 C++ 算子用 flags 所以没暴露；导出改成整数，编辑器也兼容字符串。
8. **面板里向量排成一行**（Inspector 里仍是一列），最大化时控件列封顶 720 px：一千个参数时行越矮越好。

## P2 之外发现的问题

- 「重扫库目录之后第一次运行取不到结果」已修复，见 commit `fix(bridge): 重扫库目录只停活跃的 run`（ROI 分组已排回库算子分组之后）。
