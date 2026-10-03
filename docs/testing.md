# 测试地图

**写新测试之前先查这里。** 要验的行为多半已经有测试：能在已有用例里加一行（表驱动的一行、一个 SUBCASE、同一流程里多断言一句）就别新开用例；能在更低一层测的就别放到 e2e。

2026-09-26 按四路审计做过一次合并精简（提交 test/prune），数字是那之后的：

（2026-09-29 逐项实测过一遍：C++ 默认 / dts / gap;dts 三种构建，Rust 默认与纯平台构建 `LYFLOW_STD_PACKS=0`、`tests/host.rs` 带 dts 全跑，下表的数都是跑出来的，不是推算。2026-10-01 大图阶段之后按同样的跑法再实测一遍，数字已更新；PR #3 第二轮 review 修正后又跑了一遍；2026-10-01 晚的易用性 / 性能 / 代码质量这一轮之后全部重跑，含纯平台构建与安装包；2026-10-02 这一轮的后半段之后又全部重跑一遍；同一天晚上真鼠标 / 真按键那一轮修完之后再跑一遍 `pnpm check`、桌面 / 安装包 / HTTP 三种 e2e（纯平台构建与 dts、gap;dts 的 C++ 那一轮没改、没重跑；[usability-perf-acceptance.md](usability-perf-acceptance.md) 末尾）。）

| 层 | 命令 | 规模 | 跑一遍 |
|---|---|---|---|
| C++ core + 算子包（doctest） | `pnpm core:build`（`pnpm check` 第一步） | 默认 186 例；`LYFLOW_PACKS=dts` 194 例；`LYFLOW_PACKS=gap;dts` 276 例 | 分钟级（含编译） |
| Rust bridge / CLI（`cargo test`） | `pnpm check` 的 Rust 步骤 | lib 154（纯平台构建 95 通过 / 59 ignored）；`tests/host.rs` 13（默认 11 通过 / 2 ignored，要 `LYFLOW_PACKS=dts` 才全跑）；`tests/disk_cache.rs` 2（真起两次 `lyflow` 进程验落盘缓存；纯平台构建 1 通过 / 1 ignored） | < 1 分钟（已编译时） |
| editor 纯逻辑（node:test） | `pnpm --filter @lyflow/editor test` | 136 | 秒级 |
| MCP（node:test） | `pnpm --filter @lyflow/mcp test` | 32 | 秒级 |
| 桌面 app e2e（CDP） | `pnpm e2e`（带 `LYFLOW_PACKS=gap;dts`）；只跑几组用 `--only 模块[:分组],…`（scripts/e2e/README.md） | 907 条断言、104 个分组（精简前 1095） | 已编译时约 3.3 分钟（精简前 4.5）；首次要编 core 与 tauri，另加十几分钟 |
| 浏览器宿主 e2e | `pnpm e2e:http` | 33 条断言（精简前 58） | 几分钟 |

## 放在哪一层

- **算法、数值、执行语义、C ABI 契约** → C++ doctest。gap / dts 的测量数值只在这里钉，改动必须保留数值与容差。
- **bridge 的封送、IPC 命令、CLI 的参数 / 退出码 / 输出形状、配方失配的 Rust 实现** → `cargo test`。与 C++ 同层重复的不要再写（C++ 已经测了语义，Rust 只测封送和命令层）。要标准包算子的测试加 `#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]`，要 dts 包的加 `#[cfg_attr(not(dts_pack), ignore = …)]`，与不带这个属性的不要合并。
- **编辑器 store 动作与 lib 纯函数**（图参数、配方、迁移写回、自动连线、参数面板模型、transform / curve、布局、ROI 标签摆放、执行事件怎么落进节点表）→ `packages/editor/test`。
- **只有真界面才有的**（真鼠标 / 按键、DOM 标记、渲染、系统层行为、跨进程的完整链路）→ e2e。e2e 里**不要**再逐字段复查单测已经测过的 store 数据，只留界面那一半。

## 已有覆盖：按功能查

| 功能 | 主要测试 |
|---|---|
| 执行器、事件 seq、取消、并发（线程预算按整个进程在算的节点数分：`test.budget`）、缓存复用（含落盘缓存：编解码往返、清空内存后命中、指纹隔离、坏文件） | `core/tests/test_executor.cpp`、`test_cache.cpp`、`test_flow.cpp`（并发 8 run、取消 100 次）；跨进程与 `--cache-dir` / `LYFLOW_CACHE_DIR` / `lyflow cache info|clear` 在 `bridge/tests/disk_cache.rs`（落盘开关是进程级的，不放进和别的用例同进程的单元测试） |
| 抢占不阻塞（ADR-0027：立即返回、同一时刻最多一个在算、只留最新请求、排队的被取消 / 起不来时补发 run_finished；重扫库目录与热重载的维护窗口：等被停掉的那个退出、期间的请求只排队、窗口不交错） | 状态机在 `bridge/src/execution/tests.rs`（假 run 可控地「卡住」）；真 app 里重扫库目录不卡主线程、期间的运行排到重扫之后在 e2e `noderun.mjs` 的 `suiteLibraryRescanStalled`；编辑器怎么接（视图按 `resultRunId` 取、带 targets 的等 `run_started` 才换，从没开跑就收场时被抢占那次在算的节点落成已取消）在 `packages/editor/test/execution-store.test.mjs`；工具栏的「↻ 重跑」/「取消中…」（`runControlsOf`、取消中的起落、重跑沿用原来的范围）也在这个文件；真 app 里抢占一个卡住的运行不卡主线程、取消中点重跑在 e2e `noderun.mjs` 的 `suitePreemptStalled`（不理取消的测试算子 `test.stall`，`core/tests/stall_test_op.h`，`LYFLOW_TEST_OPS` 注册） |
| 计划、cacheKey、Run to node / 选中 | `core/tests/test_plan.cpp`、`test_noderun.cpp`（含修订二的 `attachedStale`：挂上一次的旧结果）；e2e `m3.mjs`（Shift+F5）、`noderun.mjs`（右键「运行到此节点」，运行中可点 = 抢占；改上游后下游过期但还能看） |
| 子图内部出错的定位（子图节点上写明内部节点、检查器里逐条写明来源，点它 / 点诊断 / 点工具栏 error / F8 打开到那一层并标红框；F8 / Shift+F8 在出错的节点之间跳） | 路径解析、聚合与跳转顺序在 `packages/editor/test/execution-store.test.mjs`；真界面在 e2e `m4.mjs` 的 `suiteInnerError` |
| 预览区的空态给下一步（没跑过 → 运行到此节点；出错 → 错误原文 + 定位到参数；被上游连带没执行 → 点名出错的节点 + 定位过去） | 找最近的出错上游（只穿过被连带取消的；根因在子图外面时往外一层接着找：`culpritOf`）、子图节点打开到里面那个在 `packages/editor/test/execution-store.test.mjs`（与上一行共用子图夹具）；真界面在 e2e `run.mjs` 的 `suiteBadParam`（出错 / 连带，真鼠标点定位）与 `noderun.mjs` 的 `suiteMenu` 开头（真鼠标点「运行到此节点」） |
| 查找节点（Ctrl+F：整张图连子图里面的一起列、按名字 / id / 算子 / 所在子图模糊找，回车打开到那一层；库算子里面不列） | 列举与排序在 `packages/editor/test/execution-store.test.mjs`（与上一行共用子图夹具）；真界面在 e2e `m4.mjs` 的 `suiteNested` 末尾（顶层直接跳进两层子图） |
| 日志页（只看警告与错误、按节点或内容筛；节点写带层级的名字，点它打开到那一层）；检查器的节点 id（路径 id，点了复制）；撤销 / 重做之后的 toast | 撤销提示在 `packages/editor/test/graph-params-actions.test.mjs`；真界面在 e2e `m4.mjs`：日志页与节点 id 在 `suiteNested`，Ctrl+Z 的 toast 在 `suiteLibrary` |
| 子图、库算子（含展开为内联子图：定义去掉 id、内联后逐位相同；库目录设置：`bridge/src/library_settings.rs` 单测、e2e m4 面板增删 + CLI 同读） | `core/tests/test_subgraph.cpp`；`packages/editor/test/graph-params-actions.test.mjs`（合成 / 解散 / 展开库算子的 store 动作）；e2e `m4.mjs` |
| 多选同一种算子一起改参数（`setParamMany`：每个节点照 `setParam` 路由、一条撤销、并进拖动的外层事务） | `packages/editor/test/graph-params-actions.test.mjs`；真界面（标「不同」、拖一下两个都变、一条撤销）在 e2e `m3.mjs` 的 `suiteEditing` |
| 拖线时的即时挡错（P0 #7 端口类型、#16 环检测、E6 Any 推导；`compatibleTargets` / `compatibleSources` 的置灰表；松在节点身子上接哪个端口的 `dropOnNode`、松在一个具体端口上只判它的 `dropOnPort`、已接着线的输入换来源的 `canConnectReplacing` / `replaceableTargets`；插到连线中间用哪一对端口的 `insertPortsFor`；拖线松在空白处挑了新算子、接它哪个端口的 `pendingPort`）；GraphDoc → React Flow 的映射（连线按实际类型着色、惰性边虚线、引用复用；只挪了位置的节点 data 沿用原对象）| `packages/editor/test/typecheck.test.mjs`；真鼠标拖线在 e2e `m3.mjs`、`m8b.mjs`；长一点的提示按字数停留（警告至少 4 秒）在 `m3.mjs` 的 `suiteDropToSearch` |
| 算子搜索的排序（名字命中优先、短的优先、缩写与中文关键词、说明命中标字段） | `packages/editor/test/search.test.mjs`；弹层与面板的真界面在 e2e `m3.mjs` |
| 图参数（规格、校验、传参） | `core/tests/test_graph_params.cpp`、`test_params.cpp`；`packages/editor/test/graph-params*.test.mjs`；e2e `params_p1.mjs`（参数菜单的复制路径名 / 粘贴值、读剪贴板不弹权限框也在它的 `suiteIncludeTopLevel` 末尾） |
| 配方与四类失配 | 共享夹具 `schema/fixtures/recipes/`：`bridge/src/recipe.rs` 与 `packages/editor/test/recipes.test.mjs` 对着同一份 `expected.json`；e2e `params_p3.mjs` 只验界面、磁盘与对话框 |
| 参数面板（虚拟列表、搜索、chip、14 种控件；打字之后按 Esc 撤回：数字框与文字框在 `suiteAllTypes`、图参数规格在 `suiteSearchFilter`；聚焦的数字框上滚滚轮不改值、数字框里按 ↑ 按参数的步长走一格、没范围的参数按值的量级走（步长怎么取：`stepFor` 在 `param-values.test.mjs`）、下拉框里选完一项就按 Ctrl+Z 撤得掉、取色器拖着选一条撤销也在 `suiteAllTypes`） | `packages/editor/test/param-panel.test.mjs`、`param-values.test.mjs`；e2e `params_p2.mjs` |
| 迁移（含改连线 ADR-0025） | `core/tests/test_cache.cpp`（迁移链）、`bridge/src/patch/tests.rs` / `commands.rs`、`packages/editor/test/migrations.test.mjs` |
| 点云载荷的 rgb、按节点清缓存（C ABI v12） | `core/tests/test_output_view.cpp`、`test_cache.cpp`；`bridge/src/execution/tests.rs` 的 `cloud_payload_carries_rgb_last_and_padded`；`packages/mcp/test/cloud.test.ts`；e2e `m4.mjs`（RGB 着色）、`noderun.mjs`（右键清缓存） |
| 数据类型、Bundle、输出视图 ABI | `core/tests/test_data.cpp`、`test_bundle.cpp`、`test_output_view.cpp`、`test_contract.cpp` |
| 图像数据域（docs/image-plan.md：valueJson 逐通道统计、块均值缩小、`lyflow_output_image` 的级别 / 行切片 / 越界、落盘往返、`shape` 契约用在图像上、C ABI 注入图像带行填充、注入摘要带 normals / rgb） | `core/tests/test_data.cpp`、`test_output_view.cpp`、`test_cache.cpp`、`test_contract.cpp`、`test_flow.cpp`（合成图像的测试算子 `test.make_image` / `test.take_gray` 在 `core/tests/image_test_op.h`，e2e 经 `LYFLOW_TEST_OPS` 同一份）；LYIM 载荷布局 `bridge/src/execution/tests.rs` 的 `encode_image_frame_matches_the_documented_layout`；差异表的 Image 行 `compare-diff.test.mjs`；e2e `peek.mjs` 的图像组（适配级别、切原图分段取齐、悬停读数与合成公式一致） |
| 运行摘要（summary） | `core/tests/test_summary.cpp`；CLI 的 `--summary` 形状在 `bridge/src/cli/tests.rs` |
| 标准算子 / PCD 读写 / ONNX（法线与两个离群点滤波分段并行：与 PCL 原实现逐点相同、与线程数无关、`flipTowardsViewpoint` 关掉不翻，在 `test_std_ops.cpp` 里拿 PCL 的三个类当参照；体素栅格按线程预算并行之后与原来的单线程写法逐字节相同，同一个文件；ASCII PCD 的快读快写与 PCL 1.12 本身逐字节对照、1 / 3 / 8 个线程各一遍 —— 怪文件、怪词、随机词、超过一块的文件、逗号小数点的 locale，在 `test_io_pcd.cpp`） | `packs/std-pointcloud/tests/*`、`packs/std-ml/tests/test_ml_ops.cpp`（模型用例要 `LYFLOW_TEST_ONNX_MODEL`，不设会打出跳过；`[N,6,1280] → [N,8,1280]` 的模型本机在桌面 DTS 文件夹的 `v12s0.onnx`） |
| 主预览的图像模式（画输出还是输入那张图、放大后重跑视角不动、像素框拖动写回参数并可撤销、像素几何叠画；规则本身在 `view-rule.test.mjs`） | e2e `m8b.mjs` 的 `suiteImageMainView` |
| 大图预览（ADR-0028：源头按 2 的幂缩到 4 MP、像素参数 / 几何 / 量测在 compute 两侧换算、`absolute` 绝对尺寸、u16 单通道缩小时 0 不计入、超预算提示按数据域） | 规则与执行器端到端在 `core/tests/test_pixel_scale.cpp`（探针算子 `test.px_probe` 在同一个文件里）；shrinkImage 的 scale 与 D3 在 `test_data.cpp`；点云那一支的提示在 `test_subgraph.cpp` 的预览超预算用例；真实链路（找圆、区域统计预览与正式对得上，经过张量转回来的图带着比例）在 `packs/std-image/tests/test_image_ops.cpp`；主预览按原图尺寸摆放、角标、框与正式结果同一处，预览 ↔ 正式之间视角不动（源头的大图、手选「原图」、比源头小的输出三种）在 e2e `m8b.mjs` 的 `suiteImagePreviewScale` |
| MCP 看图（LYIM 解码、u16 拉伸到 8 位、最近邻缩、PNG 头与 inflate 读回、超过 16 MB 分段取齐；冒烟里真调一次 `view_output_image`） | `packages/mcp/test/cloud.test.ts`、`smoke.test.ts`、`server.test.ts`（工具面 16 个） |
| 图像算子（std-image：adapter 零拷贝与 RGB 顺序、读写往返含中文路径、各算子数值、像素单位的几何、单通道契约；深度图 ↔ 点云的反投影、来回一趟逐像素相同、z 缓冲；图像 → 张量 → ONNX → 图像的推理链路，同样要 `LYFLOW_TEST_ONNX_MODEL`） | `packs/std-image/tests/test_image_ops.cpp`；e2e `peek.mjs` 图像组的最后一段（真 app 里灰度 → Otsu → 区域统计、掩膜边是单通道 u8） |
| dts 面差（与宿主 Python 旧算法的对照、现场轮廓） | `packs/dts/tests/test_dts_ops.cpp`（夹具 `tests/data/*.h`） |
| gap 测量、积木、导入、模型 ROI | `packs/gap/tests/*`；e2e `gap.mjs`、`m8b.mjs`、`m8c.mjs`、`params_p4.mjs`（编辑器 vs CLI 逐位相同） |
| 2D 拖框（文件底图与输入端口底图）、自动连线、片段；加节点记进「最近用过」（搜索弹层与面板空查询时排最前）；节点复制粘贴（id 重映射、内部连线、平移、静音照旧；系统剪贴板里带标记的 JSON 与整张图的 JSON，真按键在 e2e `m3.mjs` 的 `suiteEditing`，Ctrl+V 读 paste 事件、不调 `readText`、复制节点之后又复制了一段字时不粘旧节点、鼠标停在检查器上粘贴落在画布里也在那里；子图里复制、Ctrl+A / Ctrl+M / Ctrl+E 取当前这一层、日志里选着文字时 Ctrl+C / Ctrl+X 归浏览器在 e2e `m4.mjs` 的 `suiteNested`；被图参数绑定的参数在副本上写成有效值在 `graph-params-actions.test.mjs`）；Shift+D / 右键「复制并保留输入」（副本接原件的同一个上游，子图里接上子图入口，一条撤销；删子图里的节点连带摘掉子图入口指着它的那条）在 `graph-params-actions.test.mjs`，真按键与菜单在 e2e `suiteEditing`；右键「复制参数 / 粘贴参数 / 全部恢复默认」（稀疏的展开、只粘同一种算子、被图参数或子图参数提供的不动、各一条撤销、换了一张图子图节点不粘）在 `graph-params-actions.test.mjs`（`lib/paramClipboard.ts`），真右键在 e2e `suiteEditing`；右键「换成别的算子…」（`lib/replace.ts`：参数怎么带过去 `carryParams` 在 `param-values.test.mjs`；线、绑定、子图入口、图输出怎么收拾、子图出口会断就不换、一条撤销在 `graph-params-actions.test.mjs`），真右键与搜索在 e2e `suiteBypassReroute` 末尾；删节点连带删边（P0 #5）；F2 改名（改名框里没改就回车不记撤销）、F 适配选中、框选选上框里的节点（按着 Shift 是追加、不按是换掉；框选之后选中节点的标题与端口不被选区框盖住）、方向键挪节点每下一条撤销、拖其中一个选中的节点整组走且整段一条、撤销掉粘贴之后选中里不留它的 id、真按 Delete 删框选的节点与连线一条撤销、右键按在节点上拖平移且松手不弹菜单、右键菜单 Esc 收起与点菜单外面收起、画布下沿的节点上右键菜单整个在窗口里（怎么摆：`placeMenu` 在 `layout.test.mjs`）、右键菜单的断开全部连线 / 选中同一种算子 / 改名 / 删除与空白处菜单、混选时在检查器里点一种算子收窄、数字框里打着字按 F5 跑的是新值、Tab / Space 在空白处开搜索而在按钮上归按钮、焦点在勾选框上照样 Ctrl+Z、拖完数字框紧接着 Ctrl+Z 撤得掉（e2e `suiteEditing`；打着字按 Ctrl+S 存进文件、图名与节点标题敲完回车一条撤销而 Esc 撤回、工具栏下拉框 Esc 与点外面收起在 `suitePanels`）；一个手势一条撤销（store 的 `batch`：并成一条、并进拖动的事务、cancel 不记）在 `autoconnect.test.mjs`，界面上的四处（Delete、右键插 reroute、拖线松手后搜索选中、拖到连线上插入）各在 e2e `m3.mjs` 的对应分组里补了一句 Ctrl+Z；从算子面板拖到连线上插入在 `m8b.mjs` 的算子面板组；连线右键「插入算子…」与选中一条连线按 Tab 插到线中间、Ctrl+Delete 删除并接通上下游在 `suiteBypassReroute` 末尾（接回去的规则 `healPlan` 与 store 的一条撤销在 `autoconnect.test.mjs`）；对话框开着时按 Delete 不删在 `params_p3.mjs` 的 `suiteCreateSaveReopen` | `packages/editor/test/autoconnect.test.mjs`、`roiframes.test.mjs`；e2e `m8b.mjs`、`m8c.mjs`；「最近用过」的真界面在 e2e `m3.mjs` 的 `suiteDropToSearch` 末尾；拖线松在节点身子上（接唯一能接的端口 / 提示对准）也在那里，线头松在节点身子上改接、线头改接到已接着线的输入（换来源）在 `suiteReconnect` |
| 两节点输出对比（差异表的配对、每种类型的行、容差、全量点数；进入即冻结、换 B 解冻、B 被删自动退出、快捷键；两栏内容合并） | `packages/editor/test/compare-diff.test.mjs`、`compare-store.test.mjs`、`view-rule.test.mjs`；e2e `compare.mjs`（两栏、冻结后重跑只有 A 变、共用相机、右键换 B、A 跟随、拖框互斥、两侧都只有值）。差异表每种类型的数值只在单测里钉 |
| 预览里选点与测距（屏幕空间最近点、看不见的点跳过、同距取近、一组两点、readout 的单位与行） | `packages/editor/test/pick.test.mjs`；快捷键 `M` 不撞 `Ctrl+M` 在 `compare-store.test.mjs` 的快捷键那条；e2e `m3.mjs` 的测量组（真单击选点、拖动不选、重跑标过期、换节点清掉；测量关着时双击设转心、重跑与换到同一坐标系的节点时相机与转心不动也在那里；要不要重新取景的 `sameFrame` 在 `view-rule.test.mjs`）、`compare.mjs` 一句（A、B 两栏各点一次） |
| 点云缓存的并发请求合并（同键只取一次、失败不留占位）与字节预算（法线、颜色也算） | `packages/editor/test/cloud-cache.test.mjs`；e2e `m4.mjs` §2 的「事件到渲染」（拖动中预览运行的延迟中位数；拖动中每次重跑预览都留着上一片云、没有变空） |
| `lyflow eval / sweep / perturb`（值路径、样本集、轴扫描与斜率、`pointFrom` 刀口跟锚点；`--jobs` 同时跑几次：按顺序交出、停在同一行；终端里的进度行） | `bridge/src/eval/tests.rs`、`perturb.rs` 的单元测试（`--jobs` 的调度用假任务钉：`ordered_parallel_*`；进度行原地刷新、按宽度截断、关着时一个字节不写：`the_progress_line_*`；`run` 的那一行数节点：`cli/tests.rs` 的 `the_run_progress_line_*`；没成的那几次在 stderr 上归成一行：`the_failure_digest_*`）；`cli/tests.rs` 的 `eval_crosses_parameter_sets_with_samples`（`--jobs 4` 与一次接一次逐行相同）、`eval_with_jobs_stops_at_the_same_row_as_without`；`bridge/src/cli/tests.rs` 的 `perturb_*` 集成测试（`crop_chain` 小图：固定刀口斜率 > 0、刀口跟锚点挪走后不响应、取不到锚点判失败）；MCP `packages/mcp/test/argv.test.ts` |
| 预览的显示设置（着色、色带、点大小、显示点数落 localStorage、读回时逐项校验；手动着色范围按着色模式分开记）；选看哪个点云输出（预览栏下拉框、检查器「输出」里点一行、连线查看器「在主 3D 视图打开」带上端口；几何节点的底图跟着它接的那个口） | 读回的校验、按模式取范围、启动时读回、`cloudPortsOf` 与底图沿边取端口在 `packages/editor/test/view-rule.test.mjs`；栏上的包围盒尺寸怎么写（`extentText`）在 `pick.test.mjs`；真界面在 e2e `m3.mjs` 的 `suiteViewer`（分组结束时放回原来的存储；提取下标的 selected / rest） |
| 连线查看器 Edge Peek | e2e `peek.mjs`（含 ⤢ 回到全貌、点大小、新窗口沿用主预览的着色）；窗口上限与自动关窗的提示、新窗口带上主预览的着色 / 色带 / 点大小 `packages/editor/test/peek-store.test.mjs`；图像按段取齐（超过 16 MB 分几段要）`packages/editor/test/image-fetch.test.mjs` |
| 按输出类型选视图（主预览的点云 / 值） | `packages/editor/test/view-rule.test.mjs`；e2e `gap.mjs` 的「量测输出」组（`transform.make` 显示值、手动选只对当时的节点有效） |
| 动效、hover、端点对齐 | e2e `motion.mjs`、`noderun.mjs`；300 节点的图上真拖一个节点（先确认中心点露在画布上、拖完确实挪了）与鼠标扫过一片节点时的帧率在 `m4.mjs` 的 `suiteBigGraph` |
| 节点运行按钮；右键「选中上游 / 下游」 | 图结构（智能运行的上游闭包、选中上 / 下游的闭包）在 `packages/editor/test/node-run.test.mjs`；按钮在 e2e `noderun.mjs`；右键菜单的选中在 e2e `m4.mjs` 的 `suiteCompose` 开头 |
| 分栏、拖放配置（dragDropEnabled）；窗口窄了两侧面板让位、画布留够最窄；检查器的排布（参数在端口小节前面、算子说明截两行、端口小节的开合记住） | 怎么让（右栏先缩、到最窄停）、开关偏好的读写在 `packages/editor/test/layout.test.mjs`；e2e `params_p2.mjs` 的 `suiteLayout` 开头（检查器排布，真鼠标点「展开说明」、收起端口小节）；e2e `params_p2.mjs` 的 `suiteLayout`（右侧分栏；窗口真缩到 900、参数面板不出窗口；预览与检查器之间、底部抽屉的上沿上下拖，双击恢复默认）、`m8b.mjs` 的算子面板组 |
| 图结构编辑 `lyflow patch`（七个动作、幂等、改坏不落盘） | `bridge/src/patch/tests.rs` |
| 指标路径（`eval` 写错时列出、`--list-metrics` 正向列出） | `bridge/src/cli/tests.rs` 的 `eval_lists_the_available_paths_when_the_metric_is_wrong`、`bridge/src/eval/tests.rs` 的 `available_paths_*` |
| MCP 工具、argv 拼装、CLI 解析（含逐行回调、请求取消时结束子进程：`cli.test.ts` 用 node 当假 CLI；客户端取消 `run_graph` 时后端那次运行跟着取消：`smoke.test.ts` 里 `test.stall` 睡 20 秒、几秒内收到 cancelled） | `packages/mcp/test/*`（`smoke.test.ts` 是唯一跑通 MCP → CLI 的） |
| HTTP 传输、宿主嵌入（含用户片段、底图点云文件两个端点，图像端点的行切片） | `scripts/e2e/http.mjs` |
| 外部 Rust/Tauri 宿主（`attach`、`lyflow_handler!`、`sceneId` 注入、工作区路径） | `bridge/tests/host.rs`（`MockRuntime` 跑真 IPC） |
| 自动备份（存过盘的写 `<file>~`、没存过盘的写到传输层给的那一处、找回来、换上算没保存、删掉；没有那个口的传输不备份；读坏了的删掉） | `packages/editor/test/autosave.test.mjs`（内存里的假传输）；真的 app data 位置与写盘在 e2e `m3.mjs` 的 `suitePanels` 末尾（harness 跑之前把用户自己的那份挪开、收尾时挪回去） |
| 桌面壳：窗口标题（只有文件名：Windows 的反斜杠路径也拆开）、新建之后空画布列出最近打开的（点一条打开、别处双击照样开搜索）、关窗口与工具栏「新建」前问「保存 / 不保存 / 取消」（有没存的改动才问；真鼠标点编辑器画的那一问：取消不关、不保存文件不动、保存先存盘；关窗口经 devbridge 不点 × 走一遍，监听装没装上、destroy 的权限另查；各个选项的分支与另存为取消了的情况在 `packages/editor/test/autosave.test.mjs`）、主窗口放行剪贴板读取、挡掉 WebView2 的刷新键（Ctrl+R / 搜索面板开着时的 F5 不重新载入）与右键菜单 | e2e `m3.mjs` 的 `suitePanels` 末尾；剪贴板读取在 `params_p1.mjs` 的 `suiteIncludeTopLevel` |

## 共用的夹具与辅助

- **共享夹具**：`schema/fixtures/recipes/`（Rust 与 TS 共用）、`schema/examples/*.lyflow.json`、`examples/param-showcase.lyflow.json`（`test.param_showcase` 覆盖 14 种参数类型，C++ / CLI / e2e 共用）。新增跨语言规则时往共享夹具里加一行，别各写一份。
- **C++**：`core/tests/helpers.h`（`seqIsDense` 等；直接调算子的 `test::OpCall` / `test::StubContext` —— 输入里有 Box2D、Record 这类不好拼图的值时用，`threads` 给线程预算，包测试只用本包算子时传自己的注册表）、`core/tests/test_ops.h`（测试算子，含会抛异常的 `test.throw`）、`param_showcase_op.h`。
- **Rust**：`bridge/src/cli.rs` 的 `#[cfg(test)] pub(crate) mod test_support`（`SharedBuf`、`Ran`、`cli()`），其它模块复用它；cli 自己的测试在 `bridge/src/cli/tests.rs`。
- **e2e**：`scripts/e2e/page.mjs`（`newDoc`、`buildGraph`、`placeAtScreen`、`dragMouse`、`runAndWait`、`mustOk`，参数面板虚拟列表里滚到某一行的 `revealInList`、`clickSelector`、`pickRecipe` 等），`peek.mjs` 的 `openByDoubleClick`，`m8b.mjs` 的 `roiGeometry` / `setCamera` / `waitValidated`。各 params 文件的 `typeIn` / `typeInto` 仍是各写一份（要不要先滚进列表、往哪种控件里打字各不相同），新代码先找现成的。

## 写测试的规矩

1. **先查、后加。** 按上表找到已有的测试；同一判据的新情形加成表驱动的一行或 SUBCASE。
2. **一个事实一条断言。** 同一个对象不要拆成多条 eq；合成一条，出错信息写清是哪一项不对。
3. **前提不算断言。** e2e 里「图跑通了」「菜单点到了」「动作返回 'ok'」用 `mustOk()`：不成立就中断这一组并报原因，但不单独计数。
4. **不许静默跳过。** 缺数据、缺包、缺模型时要么 `report.fail` 并给出补救命令，要么明确打出「跳过」；分组自己 `return` 会让汇总照样「全绿」。跑 e2e 要带 `LYFLOW_PACKS=gap;dts`，否则依赖 gap 的分组会缺席。
5. **修 bug 时加的复现测试要留**，在测试名或注释里写明对应的修复（如「修前 NoSuchOutput」），以后精简时靠它识别。
6. **不测常量、不测框架本身**（TypeScript 类型已保证的、`ok(…, true)`、waitFor 之后必然成立的）。
7. 名字要说它真正测的东西；测不到的路径（比如异常转 internal）宁可补一个测试算子，也别让名字冒充。
