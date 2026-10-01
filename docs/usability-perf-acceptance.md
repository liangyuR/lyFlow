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
| 复制粘贴走系统剪贴板 | 跨窗口、重开之后都粘得进来，整张图的 JSON 也认；静音的粘出来还是静音的 | `autoconnect.test.mjs`；e2e `m3.mjs` 的 `suiteEditing` |
| 撤销 / 重做的提示 | toast 说撤掉的是哪一步 | `graph-params-actions.test.mjs`；e2e `m4.mjs` 的 `suiteLibrary` |
| F 适配选中 | 把视图对准选中的节点（Ctrl+Shift+F 仍是全图） | e2e `m3.mjs` 的 `suiteEditing` |
| 日志页 | 只看警告与错误、按节点或内容筛；节点写带层级的名字，点它打开到那一层 | e2e `m4.mjs` 的 `suiteNested` |
| 检查器的节点 id | 路径 id（`--to`、`--set` 认的那个），点了复制 | 同上 |
| 空画布 | 写着从哪开始（搜算子、打开、拖片段） | e2e `m3.mjs` 的 `suiteEditing` |
| 警告样式 | 三条 CSS 选择器被批量改名改坏（`.toast--lyflow-warn` 等），警告 toast、「非确定性」标签、warn 日志一直没上色 | e2e `noderun.mjs` 的 `suiteIsolateOnly` 查计算出来的颜色 |
| 大图 | 节点运行按钮的上游闭包与计划查询改为按身份缓存（原来每个按钮每次 store 更新都整图扫一遍） | `node-run.test.mjs` 与原实现逐个对照；300 节点打开 ~310 → ~240 ms（profile） |

## 宿主、CLI 与 MCP

| 项 | 做了什么 | 验收在哪 |
|---|---|---|
| 重扫库目录不卡界面 | 在执行管理器的维护窗口里做：等被停掉的那个退出、期间的运行只排队；热重载同一个窗口 | `bridge/src/execution.rs` 的状态机单测；e2e `noderun.mjs` 的 `suiteLibraryRescanStalled`（重扫期间 IPC 6 ms，修前 2.9 s） |
| 安装包 | 里面的 core 就是这次构建编出来的那一份；release 不开热重载 | `pnpm e2e:packaged` 开头的「安装包（干净目录）」一节 |
| `eval / sweep / perturb --jobs` | 同时跑几次，行的顺序与内容不变，停也停在同一行；Ctrl+C 一次取消全部 | `eval.rs` 的 `ordered_parallel_*`；`cli.rs` 的两条对照 |
| 终端进度行 | eval / sweep / perturb 与 run：stdout 重定向、stderr 在终端上时原地刷新一行，管道里一个字节不多 | `eval.rs` 的 `the_progress_line_*`、`cli.rs` 的 `the_run_progress_line_*` |
| 失败的原因写在 stderr | run / validate 列出诊断与出错节点；eval / perturb 把没成的几次按原因归成一行 | `cli.rs` 写法错那张表；`eval.rs` 的 `the_failure_digest_*` |
| 数字选项写错是用法错 | `--parallel 4x` 以前悄悄当成 0 | `cli.rs` 写法错那张表 |
| MCP | eval / perturb 的 `jobs`；客户端取消时 CLI 子进程跟着结束、`run_graph` 替它发 cancel；eval / perturb 逐行发进度通知 | `packages/mcp/test/cli.test.ts`、`smoke.test.ts`（去掉接线时冒烟会失败） |
| `pnpm e2e --only` | 按模块 / 分组挑着跑 | `scripts/e2e/README.md` |

## 算子性能（输出逐位不变）

| 算子 | 200 万点上 | 做法 | 验收 |
|---|---|---|---|
| `filter.statistical_outlier` | 13.7 → 4.8 s | 近邻搜索按线程预算分段并行，均值方差照原顺序单线程累加 | 与 PCL 原实现逐点对照（dense 与掺 NaN 两份云、1 / 3 / 8 线程） |
| `features.normals` | 12.4 → 5.2 s | 同上；顺手修了 flipTowardsViewpoint 关掉也照翻 | 同上 |
| `filter.radius_outlier` | 7.3 → 3.4 s | 同上；非 dense 云上只要够数的邻居 | 同上 |
| `filter.voxel_grid`（最近点） | 2.0 → 0.95 s | 开放寻址的查找表 | 输出逐字节对照 |
| `image.region_stats`（不接掩膜） | 35 → 2–5 ms | 直接算整图 | 有无掩膜两条路对照 |
| `filter.random_sample`（留一半） | 158 → 80–120 ms | 留得多时用标记表代替 `std::sort` 排回原顺序 | 与原实现（同种子的部分 Fisher-Yates + 排序）逐点对照 |
| 线程预算 | — | 按整个进程里在算的节点数分（跨运行），几次运行同时算时不超订 | `test_executor.cpp` 的 `test.budget`；`--jobs 4` 时与按运行分的 A/B 吞吐无差别 |

`--jobs` 的实际收益取决于图：全是单线程算子的图（生成 + 体素）12 个样本 2.12 → 1.2 s，统计离群点这种自己就吃满核的
1.3–1.5×（4 核 8 线程，jobs = 4）。

## 代码质量

- clippy 清零（bridge、lyflow-client），MSRV 写准；gap 包全量构建的 5 条编译警告修掉。
- 包测试里七份 `NullContext + Call` 统一成 `test::OpCall`；参数面板虚拟列表的 e2e 帮手收进 `page.mjs`；删掉五个没人调的导出。
- e2e：工具栏宽度那组不再跟着 KUN10 数据一起失败（自己搭状态）；live preview 那组先预热预览缓存（量的才是「跟手」）；
  m4 的第一组起了名字（`--only` 挑得到）。

## 已知、没修的

- **新会话里第一次拖大图慢。** 刚启动、搭或载入 300 节点的图之后的第一次拖动只有 20 fps 上下（开发构建与安装包都是），
  再载入一遍之后是 60 fps；与这一轮无关（aad654c 上一模一样）。跟踪下来主线程的时间主要在 Chromium 的分层
  （`Layerize` / `PaintArtifactCompositor::Update`，第一次 580 ms、第二次 330 ms，同一段拖动），合成层只有 11 个，
  不是层太多；节点几乎全在视野里（看全图时 280 / 300 都渲染）。e2e 的「拖动 ≥ 30 fps」在全量里跑在别的组后面，
  所以过；`--only m4:suiteBigGraph` 单独跑会挂在这一条上，那是这个现象，不是回归。
- **真实数据没验。** KUN10 / 天幕数据补齐之前，`params_p4` 的验收 26 照旧是「未验」那一条失败。

## 最后一轮验证（2026-10-02）

| 层 | 结果 |
|---|---|
| `pnpm check`（C++ 构建与 doctest、三份契约对 schema、cargo test、CLI、嵌入 SDK、前端 typecheck + build、MCP） | 通过 |
| C++ doctest | 默认 181、`LYFLOW_PACKS=dts` 189、`gap;dts` 271，全过 |
| Rust | lib 154（纯平台构建 95 通过 / 59 ignored）；`tests/host.rs` 11 通过 / 2 ignored；`tests/disk_cache.rs` 2 |
| 编辑器 node:test / MCP | 94 / 32 |
| 桌面 e2e（`LYFLOW_PACKS=gap;dts`） | 793 / 794，104 个分组；唯一的失败是 KUN10 数据不在（验收 26 的「未验」） |
| `pnpm e2e:http` | 33 / 33 |
