# 易用性、算子性能与代码质量一轮 —— 验收记录

> 2026-10-01 – 10-02，分支 `improve/usability-perf-quality`。没有事先的计划文档：这一轮是在 PR #3 合并之后
> 逐项找「用着别扭、算得慢、写得不干净」的地方。每一项都有测试钉住（单测或 e2e），性能项写的是同一台机器
> （i5-1135G7，4 核 8 线程的笔记本）上的实测数字，不是推算。逐条的来龙去脉在各自的 commit message 里。

## 编辑器

| 项 | 做了什么 | 验收在哪 |
|---|---|---|
| 子图内部出错的定位 | 子图节点上写明是哪个内部节点出的错；点它、点诊断、点工具栏的「error N」都打开到那一层、参数上标红框；检查器里每条错误前写着来源 | `execution-store.test.mjs`；e2e `m4.mjs` 的 `suiteInnerError` |
| F8 / Shift+F8 | 在出错的节点之间跳（根因在前，上游连带的不算） | 同上 |
| Ctrl+F 查找节点 | 整张图连子图里面的节点一起找（名字 / id / 算子 / 所在子图），回车打开到那一层 | `execution-store.test.mjs`；e2e `m4.mjs` 的 `suiteNested` |
| 多选一起改参数 | 同一种算子多选时检查器给一张表单：值不同的标「不同」，改一次写进每个节点，一条撤销 | `graph-params-actions.test.mjs`；e2e `m3.mjs` 的 `suiteEditing` |
| 右键「选中上游 / 下游」 | 沿连线把整条链选上，再合成、静音、整理、复制 | `node-run.test.mjs`；e2e `m4.mjs` 的 `suiteCompose` |
| 最近用过的算子 | 算子搜索空查询时排最前、标「最近」；面板顶上一组 | `autoconnect.test.mjs`；e2e `m3.mjs` 的 `suiteDropToSearch` |
| 复制粘贴走系统剪贴板 | 跨窗口、重开之后都粘得进来，整张图的 JSON 也认；静音的粘出来还是静音的。Ctrl+V 读 paste 事件里的内容：`readText` 在 WebView2 里会弹「想要查看剪贴板」的框（第一版就是这么写的，真按 Ctrl+V 会弹框，e2e 里因为打了桩没看出来） | `autoconnect.test.mjs`；e2e `m3.mjs` 的 `suiteEditing`（断言没调 `readText`） |
| 没存过盘的图也自动备份 | 30 秒一次写到 app data 里（`untitled.lyflow.json~`），崩溃、断电、被强杀之后下次开 app 问要不要恢复；存了盘、新建或打开别的图之后删掉。从自动备份恢复出来的图（两种都是）算没保存 —— 以前恢复 `<file>~` 之后标成已保存 | `autosave.test.mjs`；e2e `m3.mjs` 的 `suitePanels`（真的 app data、真的写盘；harness 不动用户自己的那份） |
| 关窗口前问一句 | 有没存的改动（图或配方）时点 × 先问「确定关闭吗」，取消就不关；以前直接关，没存过盘的图连备份都没有（M3 计划里写了、没做） | e2e `m3.mjs` 的 `suitePanels`；真给窗口发 WM_CLOSE 看过一次（有改动时弹框、窗口不关，没改动时关掉） |
| 参数菜单「粘贴值」不弹框 | 桌面壳自己开主窗口（`tauri.conf.json` 里 `create: false`），放行剪贴板读取 | e2e `params_p1.mjs` 的 `suiteIncludeTopLevel`（真 `readText` 不挂住、粘贴值写进参数） |
| 子图里复制 / 剪切 / 全选 / 静音 / 折叠 | 取当前这一层的节点。以前取顶层：复制拿不到，剪切删了本层的节点、剪贴板里却是错的；Ctrl+A 选上的是这一层没有的 id；Ctrl+M / Ctrl+E 永远是「打开」，再按一次取消不了 | e2e `m4.mjs` 的 `suiteNested`（去掉修复时这几条失败） |
| 选着文字时的 Ctrl+C / Ctrl+X | 刚在日志、诊断里选了一段文字（最近一次按鼠标不在画布上）：归浏览器，复制的是那段文字。以前复制的是选中的节点，Ctrl+X 还把节点删了 | e2e `m4.mjs` 的 `suiteNested`（去掉修复时失败：节点进了剪贴板、被剪掉） |
| 复制带绑定的节点 | 被图参数绑定的参数在副本上写成此刻的有效值（Ctrl+C 与 Ctrl+D；以前回到算子默认值，副本的行为悄悄变了） | `graph-params-actions.test.mjs` |
| 撤销 / 重做的提示 | toast 说撤掉的是哪一步 | `graph-params-actions.test.mjs`；e2e `m4.mjs` 的 `suiteLibrary` |
| F 适配选中 | 把视图对准选中的节点（Ctrl+Shift+F 仍是全图） | e2e `m3.mjs` 的 `suiteEditing` |
| 框选 | 以前框选只选上先碰到的那一个、可以整体拖的选区不出来，控制台报 Maximum update depth exceeded：`onSelectionChange` 报的是 React Flow 自己那份慢一拍的选中，与 `onNodesChange` / `onEdgesChange` 来回改连线的选中。现在选中只认 select 变更；这一层已经没了的 id（撤销掉一次粘贴之类，以前是那个回调顺带剪的）另外剪掉 | e2e `m3.mjs` 的 `suiteEditing`（换回原来的回调时框选那条失败、控制台报错；不剪时撤销粘贴那条失败） |
| 挪节点都进撤销栈 | 方向键挪选中的节点每按一下一条，拖框选出来的选区整段一条。以前两种都直接写进图、撤销栈里没有，Ctrl+Z 撤掉的是上一步 | 同上 |
| 一个手势一条撤销 | Delete 删框选的节点与连线、右键插入 reroute、拖线松手后在搜索里选一个（加节点 + 接线）、拖节点到连线上插入：以前都记成两条，Ctrl+Z 一次只撤回一半（边还断着、reroute 孤零零地留着、节点还插在线上）。store 加了 `batch`：里面的动作并成一条，拖动中就并进拖动那一条，`cancel()` 撤回不记 | `autoconnect.test.mjs` 的 batch 那条；e2e `m3.mjs` 的 `suiteEditing`、`suiteBypassReroute`、`suiteInsertOnEdge`、`suiteDropToSearch`（Ctrl+Z 一次全回来、撤销栈回到之前；batch 不合并时这四条失败） |
| 右键拖动平移 | 中键与右键平移，React Flow 只在空白处接右键：按在节点 / 连线上拖不动，松手时（Windows 上右键菜单在松开时才弹）指针在哪个节点 / 连线上就弹哪个的菜单。现在按在节点 / 连线上也平移，右键拖过几像素之后的那次右键菜单吞掉；不挪的右键单击照常 | e2e `m3.mjs` 的 `suiteEditing`（去掉时失败：没平移、弹了菜单） |
| 打着字按 F5 / Ctrl+S | 数字框、文字框失焦才提交：以前打了 1234 没失焦就按 F5，跑的还是 1000；Ctrl+S 在输入框里干脆不响应。现在这四个键（F5、Shift+F5、Ctrl+S、Ctrl+Shift+S）先让那个框提交（失焦的 onBlur 是同步的）、再把焦点放回去，然后才跑 / 存 | e2e `m3.mjs`：F5 在 `suiteEditing`（查这次运行的点数）、Ctrl+S 在 `suitePanels`（查文件内容）；去掉时两条都失败 |
| 输入框里按 Esc | 打字之后按 Esc 撤回、不提交：数字框、参数的文字框、图参数的规格（配方矩阵的单元格用的是同一套控件）。以前都把打进去的提交了，还记一条撤销 —— Esc 先 setText 再 blur，同一个事件里 onBlur 拿到的还是这一帧打进去的字 | e2e `params_p2.mjs` 的 `suiteAllTypes`、`suiteSearchFilter`（真按键；换回原来的写法时三条都失败） |
| 对话框开着时按 Delete | 删除只走键表（对话框开着时快捷键一个都不响），React Flow 自己的 `deleteKeyCode` 关掉：以前对话框开着、焦点不在它里面时按 Delete，画布上选中的节点就被删了 | e2e `params_p3.mjs` 的 `suiteCreateSaveReopen`（打开 deleteKeyCode 时失败） |
| 日志页 | 只看警告与错误、按节点或内容筛；节点写带层级的名字，点它打开到那一层 | e2e `m4.mjs` 的 `suiteNested` |
| 检查器的节点 id | 路径 id（`--to`、`--set` 认的那个），点了复制 | 同上 |
| 空画布 | 写着从哪开始（搜算子、打开、拖片段） | e2e `m3.mjs` 的 `suiteEditing` |
| 警告样式 | 三条 CSS 选择器被批量改名改坏（`.toast--lyflow-warn` 等），警告 toast、「非确定性」标签、warn 日志一直没上色 | e2e `noderun.mjs` 的 `suiteIsolateOnly` 查计算出来的颜色 |
| 大图上的 hover | 鼠标扫过 300 节点的图 14 → 33 fps（p90 帧 170–256 → 40–55 ms）；从一个 hover 着的节点开始拖 25 → 41 fps。节点 hover 时淡化不相关的边原来是每条边自己 `is-dimmed`（opacity）：hover 一换几百条边一起重渲染、各开一个合成效果节点再全部重新分层。改成画布上一个 `data-node-hover` + CSS 用 stroke-opacity 淡化，只有相关的几条边重渲染 | e2e `m4.mjs` 的 `suiteBigGraph`（扫过 ≥ 20 fps）、`motion.mjs` 的 hover 组（淡化看计算出来的 stroke-opacity） |
| 大图 | 节点运行按钮的上游闭包与计划查询改为按身份缓存（原来每个按钮每次 store 更新都整图扫一遍）；映射层每条边不再线性找节点 | `node-run.test.mjs` 与原实现逐个对照、`typecheck.test.mjs` 的映射那条；300 节点打开 ~310 → ~240 ms（profile） |

## 宿主、CLI 与 MCP

| 项 | 做了什么 | 验收在哪 |
|---|---|---|
| 重扫库目录不卡界面 | 在执行管理器的维护窗口里做：等被停掉的那个退出、期间的运行只排队；热重载同一个窗口 | `bridge/src/execution/tests.rs` 的状态机单测；e2e `noderun.mjs` 的 `suiteLibraryRescanStalled`（重扫期间 IPC 6 ms，修前 2.9 s） |
| 安装包 | 里面的 core 就是这次构建编出来的那一份；release 不开热重载 | `pnpm e2e:packaged` 开头的「安装包（干净目录）」一节 |
| `eval / sweep / perturb --jobs` | 同时跑几次，行的顺序与内容不变，停也停在同一行；Ctrl+C 一次取消全部 | `eval/tests.rs` 的 `ordered_parallel_*`；`cli/tests.rs` 的两条对照 |
| 终端进度行 | eval / sweep / perturb 与 run：stdout 重定向、stderr 在终端上时原地刷新一行，管道里一个字节不多 | `eval/tests.rs` 的 `the_progress_line_*`、`cli/tests.rs` 的 `the_run_progress_line_*` |
| 失败的原因写在 stderr | run / validate / dump 列出诊断与出错节点（dump 的目标节点写错以前只说「运行 error」）；eval / perturb 把没成的几次按原因归成一行 | `cli/tests.rs` 写法错那张表；`eval/tests.rs` 的 `the_failure_digest_*` |
| 选项写错是用法错 | `--parallel 4x` 以前悄悄当成 0；`dump --format asci` 以前悄悄写成 binary | `cli/tests.rs` 写法错那张表 |
| MCP | eval / perturb 的 `jobs`；客户端取消时 CLI 子进程跟着结束、`run_graph` 替它发 cancel；eval / perturb 逐行发进度通知 | `packages/mcp/test/cli.test.ts`、`smoke.test.ts`（去掉接线时冒烟会失败） |
| `pnpm e2e --only` | 按模块 / 分组挑着跑 | `scripts/e2e/README.md` |

## 算子性能（输出逐位不变）

| 算子 | 200 万点上 | 做法 | 验收 |
|---|---|---|---|
| `filter.statistical_outlier` | 13.7 → 4.8 s | 近邻搜索按线程预算分段并行，均值方差照原顺序单线程累加 | 与 PCL 原实现逐点对照（dense 与掺 NaN 两份云、1 / 3 / 8 线程） |
| `features.normals` | 12.4 → 5.2 s | 同上；顺手修了 flipTowardsViewpoint 关掉也照翻 | 同上 |
| `filter.radius_outlier` | 7.3 → 3.4 s | 同上；非 dense 云上只要够数的邻居 | 同上 |
| `filter.voxel_grid`（最近点） | 2.0 → 0.95 s | 开放寻址的查找表 | 输出逐字节对照 |
| `filter.voxel_grid`（按线程预算并行） | 叶 0.01（13 万个体素）0.30–0.37 → 0.11 s；叶 0.002（159 万个体素）0.84–0.91 → 0.33–0.45 s，最近点 1.24 → 0.52–0.67 s（同一进程里 1 个与 8 个线程先后量） | 体素按下标的哈希分给几个线程，先并行地给每个点定下归哪一份，各份按点的顺序只管自己的体素（每个体素的累加与最近点的取舍还是点的顺序），最后按体素的第一个点归并回原来的先来后到 | 与改并行之前的单线程写法逐字节相同（质心 / 最近点、全部通道与 NaN、两档叶大小、minPts 1 / 3、1 / 3 / 8 个线程）；把归并改成直接拼接，3 / 8 个线程那两档立刻挂 |
| `image.region_stats`（不接掩膜） | 35 → 2–5 ms | 直接算整图 | 有无掩膜两条路对照 |
| `filter.random_sample`（留一半） | 158 → 80–120 ms | 留得多时用标记表代替 `std::sort` 排回原顺序 | 与原实现（同种子的部分 Fisher-Yates + 排序）逐点对照 |
| `io.load_pcd` / `io.save_pcd`（ASCII） | 读 5.1 → 0.56 s、写 6.2 → 0.23 s | 正文换成 from_chars / to_chars（Clinger 快路径），按线程预算分块并行；头与两种二进制照旧交给 PCL；照抄 PCL 1.12 不清流状态的怪癖（第一个数之后都是 atof 再截成 float —— 所以「流坏掉之后」的行与读到哪了无关，才分得开） | 与 PCL 本身逐字节对照，1 / 3 / 8 个线程各一遍（读写两头、怪文件怪词、随机词、> 8 MB、逗号小数点）；写出的文件与 PCL 写的 `cmp` 相同；五处故意改坏（三处转换、并行读的起点、并行写的顺序）都被抓到 |
| `image.to_tensor` / `tensor.to_image`（1200 万像素 RGB） | 230–360 → 94–117 ms / 76 ms 不变 | 原来每个值调一次不内联的 `Image::at`（按位深 switch）；改成按位深走裸指针，逐值算术一字不改；f32 交错的读回直接 memcpy | 与改写前的逐值参照逐位相同（三种位深 × 1/3/4 通道 × 四种布局、带 nan / inf 的张量） |
| `io.load_image`（36 MB 的 PPM） | 415–637 → 180–234 ms | 文件原来经 `istreambuf_iterator` 逐字节读、边读边扩容，改成量好大小一次读完 | 字节一样，解码结果自然一样 |
| 落盘缓存命中（`--cache-dir`，200 万点） | 一次运行 790 → 205 ms（点云 460 → 130 ms） | 缓存文件读法同上 | 新旧两版 CLI 先后量同一个缓存目录 |
| 线程预算 | — | 按整个进程里在算的节点数分（跨运行），几次运行同时算时不超订 | `test_executor.cpp` 的 `test.budget`；`--jobs 4` 时与按运行分的 A/B 吞吐无差别 |

`--jobs` 的实际收益取决于图：全是单线程算子的图（生成 + 体素）12 个样本 2.12 → 1.2 s，统计离群点这种自己就吃满核的
1.3–1.5×（4 核 8 线程，jobs = 4）。

## 代码质量

- clippy 清零（bridge、lyflow-client），MSRV 写准；gap 包全量构建的 5 条编译警告修掉。
- 原来只有 e2e 间接碰到的核心逻辑补了单测：拖线挡错（类型、成环、Any 推导、置灰表）、算子搜索排序、GraphDoc → React Flow 映射、节点复制粘贴、删节点连带删边。编辑器的 node:test 从 86 到 102。
- `bridge/src/cli.rs` 的测试搬进 `cli/tests.rs`（原文件四千多行、一半是测试，搬的时候逐行对照过）；`execution.rs`（六成是测试）、`eval.rs`、`patch.rs` 同样各自搬进 `<模块>/tests.rs`（1714 → 663、2784 → 2110、1002 → 583 行，逐行对照：只差 4 格缩进，跨行字符串的续行原样不动）；包测试里七份 `NullContext + Call` 统一成 `test::OpCall`；参数面板虚拟列表的 e2e 帮手收进 `page.mjs`；删掉五个没人调的导出。
- e2e：工具栏宽度那组不再跟着 KUN10 数据一起失败（自己搭状态）；live preview 那组先预热预览缓存（量的才是「跟手」）；
  m4 的第一组起了名字（`--only` 挑得到）。

## 已知、没修的

- ~~新会话里第一次拖大图慢~~（2026-10-02 查清、已修）。原来的判断是错的：e2e 拖的是 DOM 里第一个节点，它的中心点在画布上沿外、
  被工具栏盖着 —— 按下去按在工具栏上，节点根本没动，量到的是「鼠标按着扫过一片节点」的 hover。真去拖一个露在外面的节点，
  第一次拖本来就有 40–50 fps。慢的是 hover：每进出一个节点几百条边一起开关 opacity（见上面「大图上的 hover」），已修；
  e2e 现在先确认拖到了、再量帧率，另加一条扫过的帧率。
- **真实数据没验。** KUN10 / 天幕数据补齐之前，`params_p4` 的验收 26 照旧是「未验」那一条失败。

## 最后一轮验证（2026-10-02，剪贴板、关窗口、PCD / 读文件提速之后又跑了一遍）

| 层 | 结果 |
|---|---|
| `pnpm check`（C++ 构建与 doctest、三份契约对 schema、cargo test、CLI、嵌入 SDK、前端 typecheck + build、MCP） | 通过。嵌入 SDK 消费方工程那一步第一次在临时目录里探测编译器就失败（ninja 找不到 `CMakeFiles\rules.ninja`），换一个干净目录重跑编译、`embed_minimal` 都过；其余各步一次过 |
| C++ doctest | 默认 185、`LYFLOW_PACKS=dts` 193、`gap;dts` 275，全过 |
| Rust | lib 154；`tests/host.rs` 11 通过 / 2 ignored；`tests/disk_cache.rs` 2；clippy 无告警（纯平台构建这一轮没重跑） |
| 编辑器 node:test / MCP | 102 / 32 |
| 桌面 e2e（`LYFLOW_PACKS=gap;dts`） | 808 / 809，104 个分组；唯一的失败是 KUN10 数据不在（验收 26 的「未验」）。之后加了 hover、拖动帧率、没存过盘的图的自动备份几条，再跑一遍是 815 / 816，跑完 app data 里没有留下验收的备份 |
| 安装包 e2e（`pnpm e2e:packaged`，同样的包） | 815 / 816，唯一的失败同上 —— 主窗口改成代码里开（放行剪贴板读取）之后，安装包照样起得来 |
| `pnpm e2e:http` | 33 / 33 |
