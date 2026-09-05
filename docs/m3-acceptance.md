# M3 验收记录

对着 [m3-plan.md](m3-plan.md) 的 §4 与 §1.1–1.6，写清「怎么跑 + 实际输出摘要 + 结论」。

**结论只有三种**：通过 / 未通过 / 未验证。「未验证」是如实标注，不是委婉的通过 ——
§4 的后两条（「一位未参与开发者 10 分钟完成任务」「连续使用一天」）是人工项，
本轮**确实没有执行**，标成未验证。

环境：Windows 11 Pro 26200，MSVC（VS Community），vcpkg `C:\vcpkg`
（PCL 1.15.1，`x64-windows` 动态三元组），Node 24，pnpm 11.1.3。

---

## 复现命令

```powershell
# 1. 全链路门禁：C++ 编译 + 算子自检 + core 测试
#    → 三份契约样例对 schema 校验 → cargo test → 前端 strict typecheck + build
pnpm check

# 2. core 的 doctest（pnpm check 里已经跑过，单独跑用这个）
powershell -File scripts/build-core.ps1          # 构建 + 自检 + 测试
.\build\core\bin\lyflow-core-tests.exe           # 只跑测试
.\build\core\bin\lyflow-dump-manifest.exe --check

# 只跑 M3 新增的那一批
.\build\core\bin\lyflow-core-tests.exe -ts="test_cache.cpp"

# 3. Rust（含热重载换代那一轮）
cd bridge; cargo test
cd bridge; cargo test hot_reload_swaps_in_a_fresh_generation -- --nocapture

# 4. CDP 端到端验收（自己起 tauri dev，跑完自己收尾）
pnpm e2e

# 5. 注释规约（CLAUDE.md 的第二条）：全仓没有超过两行的注释块
#    扫描器见下方「注释收敛」一节
```

调试验收脚本本身时不必每次重编：另开一个窗口跑

```powershell
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=9222 --remote-allow-origins=*"
pnpm tauri dev
```

然后 `$env:LYFLOW_E2E_ATTACH="1"; pnpm e2e`。

### 热重载的手工复现（CDP 验不到，见 1.5）

```powershell
pnpm dev              # 同时起 core-watch 与 tauri dev
```

1. 等窗口起来，状态栏右侧应当显示「热重载 · 第 0 代」。
2. 画一张图（比如 `gen.synthetic → filter.voxel_grid`），随便调几个参数，
   **不要保存**，按几次 Ctrl+Z / Ctrl+Y 让撤销栈里有东西。
3. 不关窗口，编辑 `core/src/ops/util_reroute.cpp`，把 `op.label` 从 `"Reroute"`
   改成 `"Reroute 2"`，存盘。
4. 终端里 core-watch 打印「检测到改动，增量构建…」→「构建完成，等 app 热重载」；
   app 的终端打印「热重载：第 1 代已就绪，16 个算子」。
5. **5 秒内**：右上角弹出「core 已热重载（16 个算子）」，节点面板里的 Reroute
   变成 Reroute 2，状态栏变成「热重载 · 第 1 代」。
   **画布上的图、参数值、撤销栈原样保留**，标题栏的 `*` 也还在。
6. 反向验一次失败路径：把那个文件改成编译不过（比如删掉一个分号），存盘。
   core-watch 打印「构建失败，app 继续用上一代」，app 那边**什么都不发生** ——
   DLL 没被覆盖，watcher 收不到事件。
   若要验自检失败的路径，把 `op.category` 改成空字符串（能编过、自检不过），
   app 会弹「core 热重载失败，仍在用第 1 代：…」，界面上的算子仍是上一代。
7. 关掉 app 再起一次，终端第一行会打印「清理了 N 个上次留下的热重载 DLL」。

---

## §4 的四条

### ✅ 1. 1.1–1.6 各自的验收全部自动化（core 测试 / cargo test / CDP）

逐项见下面的「§1 核心轨逐条」。汇总：

| 项 | core doctest | cargo test | CDP |
|---|---|---|---|
| 1.1 缓存复用 | 6 个 TEST_CASE | `plan_graph_*` / `cache_stats_*` | 「1.1 缓存复用」20 条 |
| 1.2 并行执行 | 4 个 TEST_CASE（含随机取消 100 次） | — | 「1.2 并行执行」11 条 |
| 1.3 bypass / reroute / Any | 5 个 TEST_CASE | — | 「1.3 / P1 #24 #25」15 条 |
| 1.4 迁移 | 5 个 TEST_CASE | `load_graph_returns_migrations_*` | 「1.4 迁移」14 条 |
| 1.5 热重载 | — | `hot_reload_swaps_in_a_fresh_generation` | 「1.5 热重载」5 条（装置）＋手工复现 |
| 1.6 参数联动 | 2 个 TEST_CASE | — | 「1.6 参数联动」4 条 |

**结论：通过。** 唯一没能进自动化的是「热重载真的换了一代**新编译的**代码」——
CDP 没法在一次会话里重编 C++，那一步是手工复现（步骤见上）；
换代机制本身（复制 → 加载 → 自检 → 换 `Arc<Core>` → 新代能编译图）由
`cargo test` 的 `hot_reload_swaps_in_a_fresh_generation` 覆盖。

### ✅ 2. CDP：P1 #17–#30 每项至少一条断言

`pnpm e2e` 的 M3 分组（`scripts/e2e/m3.mjs`）与项号的对应：

| # | 分组 | 代表断言 |
|---|---|---|
| 17 | P1 #17：连线端口吸附 | 落点偏 16 个画布像素仍然吸附上了 |
| 18 | P1 #18：连线中途松手 → 搜索面板 | 面板记住了是从哪个端口拖出来的 / 新节点自动接上了 |
| 19 | P1 #19：拖离输入端连线 | 拖离输入端后改接到了另一个端口 / 改接是一条撤销记录 |
| 20 | P1 #20：拖线时的端口兼容性可视化 | 兼容端口被标成 compatible / 不兼容端口真的被压暗了 |
| 21 | P1 #21：拖节点到连线上自动插入 | 拖到连线上自动插入到中间 / 原来那条边没了 |
| 22 | P1 #22 + E8 | 拖动后位置吸到了 8 px 网格上 / dagre 把三个节点排成了从左到右 |
| 23 | 1.1 缓存复用 | 改完之后 stale 精确到那两个节点 / 上游那个没有被连坐 |
| 24 | 1.3 / P1 #24 #25 | 双击连线插入了一个 reroute / reroute 的端口颜色随源类型变化 |
| 25 | 1.3 + P1 #25 #26 #28 #29 | 静音写进了 doc 的 bypass 字段 / Ctrl+E 折叠了节点 / 双击标题改名 |
| 26 | P1 #25 #26 #28 #29 | 参数行右键弹出菜单 / 重置为默认把稀疏键删掉了 |
| 27 | P1 #27 + E7 | 目标就是选中的那个节点 / 下游没进计划 |
| 28 | P1 #25 #26 #28 #29 | 水平拖动改了数值 / 整段拖动只记一条撤销 |
| 29 | P1 #25 #26 #28 #29 | ? 打开了快捷键面板 / 面板从键表生成 |
| 30 | P1 #30 / §2.6 | 切到高度着色 / 钉住后选别的节点视图不切换 |

**实际输出**（`pnpm e2e`，24 个分组）：

```
── 中文路径：保存图 + 写 PCD                                      5
── 演示 pipeline：读 PCD → 裁剪 → 降采样 → 去噪 → 平面 → 分离 → 存盘  19
── validate_graph / get_output_info                                6
── 坏参数：leafSize = 0                                           16
── 运行中按 Esc 取消                                              6
── 改参数 → 结果标为过时                                          5
── Run to node（只跑上游闭包）                                     6
── 1.1 缓存复用：重跑全 skipped、改中间节点只重算下游               20
── 1.2 并行执行：菱形图跑通、事件不重不漏                          11
── 1.3 / P1 #24 #25：静音透传、reroute 串联与类型推导              15
── 1.4 迁移：v1 的图打开即改写、可撤销、再打开不再提示              14
── 1.5 热重载：开发期装置已就位                                     5
── 1.6 参数联动：visibleWhen 隐藏的参数不渲染也不校验必填            4
── P1 #17：连线端口吸附                                            2
── P1 #20：拖线时的端口兼容性可视化                                4
── P1 #19：拖离输入端连线，另一端跟着鼠标                          4
── P1 #21：拖节点到连线上自动插入                                  2
── P1 #18：连线中途松手 → 搜索面板 → 自动接上                      4
── P1 #22 + E8：网格吸附、对齐参考线、dagre 自动布局                7
── P1 #25 #26 #28 #29：折叠重命名、参数右键、拖动改值、快捷键面板   11
── §2.5：日志/诊断抽屉、最近文件、备份恢复、窗口标题                 9
── P1 #30 / §2.6：着色模式、色带与范围、钉住、导出                  9
── P1 #27 + E7：Shift+F5 跑到选中节点                              3
── 控制台                                                          1

188/188 项通过，全绿
```

**结论：通过。**

### ⚠️ 3. 一位没参与开发的同事，一句话任务，10 分钟内完成

**未验证。** 这是人工项，本轮没有执行 —— 没有第二个人参与。
交付时应当照 [interaction-checklist.md](interaction-checklist.md) 的「验收方式」做一次，
卡住的点全部记回那张清单。

### ⚠️ 4. 连续使用一天（真实数据、真实任务）没有回去改代码

**未验证。** 同上，人工项，需要真实数据和真实任务，本轮没有条件执行。

---

## §1 核心轨逐条

### ✅ 1.1 缓存复用

> 计划的验收：原图重跑全部 `skipped`，run 总耗时 < 50 ms；改中间节点参数只重算其下游；
> `plan_graph` 的预测集合与实际 `skipped` 集合完全一致。

**怎么跑**：`.\build\core\bin\lyflow-core-tests.exe`（`test_cache.cpp` 的前六个 TEST_CASE）
＋ `pnpm e2e` 的「1.1 缓存复用」分组。

**实际输出摘要**：

- doctest「原图重跑：全部 skipped，总耗时 < 50 ms」：三个节点全部 `skipped`，
  `stats.cached == true`，`run_finished.durationMs < 50`。
- doctest「lyflow_plan 的预测集合与实际 skipped 集合完全一致」：
  冷启动时 3 个节点 `cached=false`、cacheKey 都是 32 位十六进制；跑一次之后
  `predicted == statesOf(again, "skipped")`，两个集合逐元素相等。
- doctest「改中间节点参数：只重算它和它的下游」：`g` 是 `skipped`，`v`/`p` 是 `done`。
- doctest「LRU 字节预算」：预算压到 1/3 后 `entries` 下降、`bytes <= budgetBytes`、
  `evictions > 0`；`setBudget(0)` 回到默认值。
- doctest「纯副作用算子不参与缓存」：删掉输出文件后重跑，`g` 是 `skipped`、
  `w`（`io.save_pcd`）是 `done`，**文件又写出来了**。
- CDP：「工具栏提示将重算 3 个节点」→ 跑一次 →「工具栏改口说全部命中缓存」→
  「原图重跑全部 skipped」「重跑总耗时 < 50 ms」；改中间参数后
  「改完之后 stale 精确到那两个节点」「画布上恰好两个节点是虚线框」
  「上游那个没有被连坐」；抽屉里「缓存条目数 > 0」→ 点「清空缓存」→「条目归零」。

**结论：通过。**

### ✅ 1.2 并行执行

> 计划的验收：菱形图（1 源 → 4 支 → 1 汇）每支 sleep 200 ms 的测试算子，
> 墙钟 < 500 ms；事件 `seq` 无重复无空洞；随机 cancel 100 次无死锁无泄漏。

**怎么跑**：`.\build\core\bin\lyflow-core-tests.exe` 的四个并行 TEST_CASE。

**实际输出摘要**：

- 「菱形图：四支各睡 200 ms，并行墙钟 < 500 ms」：`run_finished.durationMs < 500`，
  `run_started.maxParallel >= 2`。
- 「同一张图串行跑要慢得多」：同一张图 `maxParallel=1` 时
  `durationMs >= 4 × 120 ms` —— 这条是用来证明上一条的快不是假的。
- 「并行下事件 seq 无重复无空洞」：`seq` 集合无重复且从 0 连续；
  四支全部 `done`。
- 「随机取消 100 次」：100 轮，每轮在 0–40 ms 的随机时刻取消。
  `join` 每次都返回（死锁的表现就是它永远回不来，整个测试进程会停在那里），
  `runStatus ∈ {cancelled, ok}`，**没有任何节点报 error**（取消不是失败），
  `seq` 每轮都连续；100 轮之后结果仓 `entries < 200`。

**结论：通过。**

关于「PCL 的 OpenMP 线程数配合」：接口做完了（`ExecContext::threadBudget()` =
`max(1, cores / maxParallel)`，`core/README.md` 的「并行」一节写了用法），
但**当前 16 个内置算子里没有一个走 OpenMP**（`features_normals` 用的是
`pcl::NormalEstimation` 而不是 `NormalEstimationOMP`），所以没有可配置的对象。
为了「验收看起来完整」而把它改成 OMP 版本是本末倒置 —— 那是算法选型，不是 M3 的事。
详见下面「偏离与决策」第 6 条。

### ✅ 1.3 bypass 与 reroute

> 计划的验收：bypass 一个体素节点，下游拿到的是原始点数；
> reroute 串两级后端口颜色随源变化，接不兼容类型被拒。

**怎么跑**：doctest 的五个 TEST_CASE ＋ `pnpm e2e` 的「1.3 / P1 #24 #25」分组。

**实际输出摘要**：

- doctest「bypass：静音的体素节点让下游拿到原始点数」：`v` 是 `skipped` 且
  `stats.bypassed == true`（没有 `cached`），`v` 与 `p` 的 `elementCount`
  都等于源头 `g` 的。
- doctest「bypass 改变 cacheKey」：静音跑一次 → 取消静音再跑，`g` 是 `skipped`
  而 `v` 是 `done`，且点数比静音时少 —— 说明没有拿到静音时的结果。
- doctest「bypass 找不到类型兼容的源」：静音 `segment.ransac_plane` 之后，
  下游 `extract_indices` 报 `bypassed_no_source`，`portName == "indices"`。
- doctest「Any 推导：reroute 串两级」：两个 reroute 的 `inputTypes["in"]` 与
  `outputTypes["out"]` 都被推成 `PointCloud`；把推导出的 `PointCloud`
  接到 `Indices` 端口上产出 `type_mismatch`。
- doctest「孤立的 reroute 推不出类型也不报错」：只有 `missing_input`，没有 `type_mismatch`。
- CDP：Ctrl+M →「静音写进了 doc 的 bypass 字段」「画布上的节点标了静音」
  「静音进了撤销栈」；跑一次 →「下游拿到的是原始点数」；Ctrl+Z 撤销回去；
  双击连线 →「双击连线插入了一个 reroute」「原来那条边被拆成了两条」
  「reroute 的端口颜色随源类型变化」（比对 `getComputedStyle` 的 backgroundColor）
  「接不兼容类型被拒」。

**结论：通过。**

### ✅ 1.4 迁移与别名

> 计划的验收：fixture 里一份 `filter.voxel_grid@1.0.0` 的图，把算子改到 2.0.0
> 并注册迁移，打开后参数已换名、可撤销、保存后再打开不再提示。

**偏离**：fixture 用的是 **`filter.random_sample`** 而不是 `filter.voxel_grid`，
理由见「偏离与决策」第 8 条（前者的 `count`/`ratio` 确实该改名，而 `voxel_grid`
的 `leafSize` 没有值得破坏兼容性的理由；`voxel_grid` 还是演示 pipeline 的一环，
为验收去动它会牵连一串测试）。**版本号留在 2.0.0**，manifest、文档、schema 样例已同步。

**怎么跑**：doctest 的五个迁移 TEST_CASE ＋ `cargo test load_graph_returns_migrations_for_an_old_document`
＋ `pnpm e2e` 的「1.4 迁移」分组。

**实际输出摘要**：

- doctest「v1 的 random_sample 图产出 migration 诊断」：诊断里
  `kind == "migration"`、`severity == "warning"`、`opVersion == "2.0.0"`、
  `params.keepCount == 123`、**没有** `params.count`、`notes` 非空；
  整份诊断里没有任何 `severity == "error"`。
- doctest「迁移是在内存里生效的」：一张 `opVersion: "1.0.0"` 带 `count: 100`
  的图**直接跑得通**，`elementCount == 100` —— 不必先存一次盘。
- doctest「迁移之后再存再开：不再产出迁移诊断」。
- doctest「别名重定向也是一次迁移」：`test.old_name` → `test.renamed`，
  诊断 `kind == "migration"`、`op == "test.renamed"`。
- doctest「注册表自检：迁移链断档会被挡在启动时」：3.0.0 只给了 `Migration{1,…}`
  时报「missing migration from major 2」；补上 `Migration{2,…}` 后 `validate()` 为空。
- cargo test：`load_graph` 回的 `migrations` 有一条，`params.keepCount == 250`，
  且 **`loaded.doc.nodes[1].params["count"] == 250`** —— 桥接层没有改图。
- CDP：`load_graph` 拿到迁移 → `applyMigrations` → doc 里换成 `keepCount`、
  `opVersion` 升到 2.0.0、置 dirty、撤销记录是「迁移 1 个节点」；
  Ctrl+Z 回到旧参数；重做 → 保存 → 再打开「不再提示迁移」；
  迁移后的图跑得通且抽样点数就是迁移过来的值。

**结论：通过。**

### ✅ 1.5 热重载（开发期）

> 计划的验收：`pnpm dev` 下新增一个算子 cpp 并加注册行，5 s 内节点面板出现新算子，
> 画布上的图与撤销栈原样保留。

**怎么跑**：
自动化部分 `cd bridge; cargo test hot_reload_swaps_in_a_fresh_generation`
＋ `pnpm e2e` 的「1.5 热重载」分组；
「真的换了一份新编译的代码」那一步是**手工复现**，步骤写在本文开头。

**实际输出摘要**：

- cargo test：`reload_from(dll_path())` 之后代数 +1、`lyflow_core.gen1.dll`
  真的生成在 exe 目录里、`core()` 返回的 `Arc` 与旧的不是同一个、
  新一代自检干净、算子数不变、并且**新一代能编译图**（`plan()` 给出 32 位 cacheKey）。
- cargo test：`generation_dll_names_are_distinct_and_cleanup_is_safe` ——
  代号不重复，没有可删的东西时 `cleanup_old_generations()` 不 panic。
- CDP：`get_core_info().hotReload == true`、启动时 `generation == 0`、
  状态栏显示「热重载 · 第 0 代」且 `data-generation` 与 core 一致、
  算子数 ≥ 16（认得 M3 新加的 `util.reroute`）。
- 手工复现（本轮实际跑过，用一段 CDP 脚本盯着 `pnpm dev` 起的实例观察）：

  ```
  before: {"operatorCount":16,"generation":0,"hotReload":true} label = Reroute
  graph before: {"nodes":2,"edges":1,"dirty":true,"undoDepth":5,"leaf":[0.03,0.03,0.03]}
  touching core/src/ops/util_reroute.cpp
  after : {"operatorCount":16,"generation":1,"hotReload":true} label = Reroute 2
  graph after : {"nodes":2,"edges":1,"dirty":true,"undoDepth":5,"leaf":[0.03,0.03,0.03],"generation":1}
  statusbar: {"text":"热重载 · 第 1 代","gen":"1"}
  ```

  改一行 `op.label` 存盘 → core-watch 增量构建 → 代数 0→1、面板里的名字变了，
  而**节点数、连线数、dirty 标记、撤销栈深度（5）、参数值全部原样**。
- 换代之后照常能跑（把那行改回去触发了第 2 代，在第 2 代上跑一张三节点图）：

  ```
  generation: 2
  run after reload: ok {gen:"done/9999", reroute:"done/9999", voxel:"done/9157"}
  ```

  这一条值得单独验：换代时 `RunManager::drop_all()` 把两个 `RunHandle` 都丢掉了，
  结果仓也清空了，「还能不能跑」不是显然的。

**结论：通过**（自动化覆盖换代机制，「新代码生效」由手工复现覆盖）。

### ✅ 1.6 参数联动

> 计划：`visibleWhen` / `enabledWhen` 在 `params.ts` 求值，`ParamControls` 隐藏/禁用。
> C++ validate 对隐藏参数仍校验形态但**不校验必填**。

前端部分 M1 已实现，M3 只核对 C++ 侧的规则。

**怎么跑**：doctest 的两个 TEST_CASE ＋ `pnpm e2e` 的「1.6 参数联动」分组。

**实际输出摘要**：

- doctest「隐藏的 path 参数不校验必填」：`io.save_pcd` 的 `path` 是可见必填项，
  空着照样报 `bad_param`。
- doctest「被 visibleWhen 藏起来的参数只查形态不查必填」：一个 `source=identity`
  时隐藏 `path` 的测试算子，藏着时诊断为空、露出来时报「还没有选择文件」、
  **藏着但填了个数字仍然报 `bad_param`**（形态照查）。
- CDP：`filter.random_sample` 的 `mode=count` 时只渲染 `keepCount`、
  换成 `ratio` 时只渲染 `keepRatio`；隐藏参数没有把图判成非法；
  `filter.passthrough` 选了 `intensity` 时 `keepOrganized` 的复选框 `disabled`。

**结论：通过。**

---

## 契约与文档

| 交付物 | 状态 |
|---|---|
| [ADR-0007 cache-authority](adr/0007-cache-authority.md) | ✅ |
| [ADR-0008 migration-as-diagnostic](adr/0008-migration-as-diagnostic.md) | ✅ |
| [ADR-0009 hot-reload-by-copy](adr/0009-hot-reload-by-copy.md) | ✅ |
| `schema/execution-event.schema.json`：`stats.cached` / `stats.bypassed`、`run_started.maxParallel`、`nodes[].bypass`、error code 加 `migration` / `bypassed_no_source` | ✅ 样例已同步并通过校验（12 条） |
| `schema/graph-doc.schema.json`：`bypass` 的描述改成 M3 的实际语义 | ✅ |
| `docs/interaction-checklist.md`：P1 #17–#30 标完成 | ✅ |
| `docs/roadmap.md`：M3 勾选 + 已知毛刺 | ✅ |
| `core/README.md`：并行、缓存、算子改版本 | ✅ |
| `bridge/README.md`：新 command、热重载 | ✅ |
| `README.md`：`core-watch` 与 `pnpm dev` 的联动 | ✅ |

### 注释收敛（CLAUDE.md 第二条）

单独一个 commit（`chore: 注释收敛到两行以内`）。全仓源文件里超过两行的注释块
从 188 处（外加 `core/CMakeLists.txt` 的 7 处，原先没计入）降到 **0**，
长篇的「为什么」迁到了 `app/README.md`、`core/README.md`、`bridge/README.md`、
`schema/README.md` 与新建的 `scripts/e2e/README.md`。

复核方式（扫描器脚本，扫 `core/ bridge/src bridge/build.rs app/src scripts/ schema/`
的 `.cpp .h .hpp .rs .ts .tsx .ps1 .mjs .js .py .css`，外加所有 `CMakeLists.txt`）：

```python
# 一段连续的注释行 > 2 行，或者一个跨 3 行以上的 /* */ 块，都算违规
```

实际输出：`TOTAL 0 in 0 files`。

---

## 偏离与决策

计划没覆盖、或与计划不同的地方，依据 E1–E8 与 architecture.md 的职责边界自行决定，逐条记在这里。

1. **C ABI 升到 v4，`lyflow_run_options` 加了两个字段**（`max_parallel`、
   `cache_budget_bytes`）。m3-plan 开头说「不改任何 M2 接口的形态」——
   在结构体**末尾追加**字段是往里填功能，不是改形态；两侧同时构建，没有跨版本兼容问题。
   `maxParallel` 是 §1.2 明确要求的 run 选项，`cache_budget_bytes` 是 §1.1 的「run 选项可覆盖」。

2. **诊断数组里每一项都带 `kind`**，取值 `"diagnostic"` | `"migration"`。
   计划只说迁移诊断长什么样。让前端靠「有没有 `op` 字段」去猜自己拿到的是什么，
   是那种能工作五年然后在某天加了个字段时突然坏掉的设计。多一个字段，判据就是显式的。

3. **`lyflow_plan` 校验失败时返回诊断数组**（照计划），两种数组由 `kind` /
   `cacheKey` 区分。Rust 侧的 `plan_graph` 原样转发，前端 transport 里做一次
   「每一项都有 `cacheKey` 才当成计划」的判定，判不出来就当作「暂时没有计划」。
   一张连不通的图，「哪些节点会重算」本来就没有答案。

4. **`manifest.h` 现在 `#include <nlohmann/json.hpp>`。** `MigrateFn` 的签名是
   `json(const json&)`（照计划），而 `manifest.h` 被每个算子 TU 间接包含，
   全量构建因此慢几秒。备选是让迁移函数收发 JSON **文本**，但那要求每个算子作者
   自己 parse/dump —— 把成本转嫁给了更常发生的那一侧。

5. **`Registry` 的构造函数改成 public。** 为了让测试能建一个隔离的注册表
   （验「迁移链断档」和「隐藏参数」需要往里塞坏算子）。
   备选是往全局注册表里塞再想办法收拾，那会让测试之间产生顺序依赖。
   `Registry::instance()` / `ensureRegistry()` 仍然是进程内那一份的唯一入口。

6. **OpenMP：只铺了接口，没有改算子。** 计划说「PCL 内部 OpenMP 线程数设为
   `cores / maxParallel`」。实际情况是当前 16 个内置算子**没有一个走 OpenMP**
   （`features_normals` 用的是 `pcl::NormalEstimation`，不是 `NormalEstimationOMP`），
   所以没有可配置的对象。做法是把预算通过 `ExecContext::threadBudget()` 传下去、
   在 `core/README.md` 写清「自己开线程时问它」，而**不是**为了让验收项看起来完整
   去把算法换成 OMP 版本 —— 那是算法选型，会改变结果的数值特性，不该混进 M3。

7. **拖节点到连线上的命中判据**（P1 #21）：计划写的是「距离 < 12 px」，
   实现是「**连线穿过节点矩形**，或中心距离 ≤ 12」。
   12 px 是**画布坐标**，画布缩到 50% 时只剩 6 个屏幕像素，实际够不着；
   而「把节点拖到线上」这个手势的字面意思就是节点盖住了线。
   误触风险由另外两个条件挡着：该节点必须**一条边都没有**，
   而且必须**有且仅有一对**兼容端口。

8. **迁移 fixture 用 `filter.random_sample` 而不是 `filter.voxel_grid`，
   版本留在 2.0.0。** 理由：`count` / `ratio` 读不出来是「保留」还是「丢弃」，
   改名成 `keepCount` / `keepRatio` 是个本来就该做的改动；
   而 `voxel_grid` 的 `leafSize` 没有值得破坏兼容性的理由，它还是演示 pipeline
   的一环，为验收去动它会牵连一串测试和文档。
   一条只在测试里走过的迁移路径，第一次真用的时候一定是坏的 —— 所以让它是真的。
   已同步：manifest（`--check` 干净）、`core/README.md`、`docs/operator-manifest.md`、
   `docs/roadmap.md` 的「已知毛刺」。**M2 以前存的图打开时会提示迁移一次，保存后不再提示。**

9. **`Any` 推导的粒度：一个节点的全部 `Any` 端口共用一个类型变量。**
   E6 只说「沿 `Any` 端口传播到定点」，没说粒度。共用一个变量正好是
   `util.reroute` 的语义，也是最容易解释的一种；需要多个互相独立的 `Any` 端口的算子
   应当拆开。这条写进了 `docs/operator-manifest.md`。

10. **PNG 导出走浏览器下载而不是 Tauri 的保存对话框。** 项目没装 `tauri-plugin-fs`，
    对话框返回的路径没有东西能写进去。`<a download>` 在 WebView2 和浏览器模式下都工作，
    代价是文件落在下载目录而不是用户选的位置。装 fs 插件是 M4 的事（CLI 也要写文件）。

11. **3D 视图的「法线着色」在界面上是禁用的。** 二进制点云载荷（ADR-0006）
    目前只带 `xyz` 与 `intensity`，没有法线通道。选项留着并写明原因，
    比悄悄不提供更诚实 —— 真要支持得先改 `lyflow_cloud_view` 的布局。

12. **两个新的 CSS 文件**（`styles.viewer.css` / `styles.params.css`，
    由各自的组件 import）。原因是这两块由并行的工作流独立完成，
    共用一个 `styles.editor.css` 会互相覆盖。其余 M3 的样式仍在 `styles.editor.css`。

13. **测试隔离：`test::Session` 默认清空结果仓；Rust 测试的图按 seed 参数化。**
    缓存是进程级的，不隔离的话「第二个用同一张图的测试」会拿到 `skipped`
    而不是 `done` —— 那是**真实行为**，但会让断言测的是运行顺序。
    要验缓存的测试显式用 `runGraphCached` / `keepCache=true`。

14. **M2 的两条 e2e 断言从 `done` 放宽成 `done | skipped`**
    （「Run to node」和「坏参数」分组里的上游节点）。这不是把测试改绿：
    缓存复用之后上游本来就该是 `skipped`，断言的原意是「它跑通了」。

15. **迁移的 e2e 走 `transport.loadGraph` + `applyMigrations` 而不是文件对话框。**
    CDP 驱动不了原生对话框。覆盖的是「读盘 → 拿到迁移 → 写回 doc → 撤销 →
    保存 → 再打开不再提示」这整条，只有「点开文件选择器」这一下没走。

16. **`pnpm dev` 改成 `node scripts/dev.mjs`**（同时起 core-watch 与 tauri dev），
    老行为保留为 `pnpm dev:app`，只要监视是 `pnpm core:watch`。
    没有引 `concurrently`：一个依赖换四十行 spawn 不划算，而且退出时要
    `taskkill /T` 整棵树才不留孤儿，这段逻辑本来就得自己写。

17. **对齐参考线只画「与另一个节点的边缘/中心对齐」**，不画等距分布线。
    计划只要求前者。已记进 roadmap 的「已知毛刺」。

18. **参数粘贴会把数值 clamp 到 `min`/`max`**（形态不符仍然拒绝并 toast）。
    与手输时 `NumberInput.commit` 的行为一致；写一个越界值进去只会在下一次运行时报错。

---

## CDP 抓到的真 bug

这一轮 `pnpm e2e` 抓到的、单元测试发现不了的问题。前三条是产品代码的 bug，
后面几条是验收脚本自己的 —— 一并记下来，因为它们会咬下一个写 CDP 断言的人。

**产品代码：**

1. **Shift+F5 被 F5 抢先匹配。** 键表的 `matchOne` 原来只在写明 `Shift+` 时才要求
   shift 按下，于是遍历到 `F5` 那一条就先命中了，「运行到选中节点」永远退化成「运行全图」。
   Shift 必须**精确**匹配；唯一的例外是 `?` 这种本来就要按 Shift 才打得出的符号。
2. **`onReconnectEnd` 的第四个参数形态不确定。** 原本按「第四个参数是 `connectionState`，
   读它的 `isValid`」写，拿到 `undefined` 就当作无效 →
   **把一条刚刚重连成功的边直接删掉**。改成在 `onReconnect` 里自己记一笔，
   `onReconnectEnd` 只看那一笔。
3. **拖节点到连线上的容差不可用。** 12 px 是画布坐标，缩到 50% 只剩 6 个屏幕像素。
   见「偏离与决策」第 7 条。
**验收脚本（写 CDP 断言的人都会踩）：**

4. **搜索面板的全屏 backdrop 会毒化下一组。** 上一组的拖拽失败留下一个没关的面板，
   下一组的鼠标事件全打在 backdrop 上，症状是「面板记住的端口不是我刚拖的那个」。
   `newDoc()` 现在每次都先 `closeSearch()` + `endConnection()`。
5. **React Flow 的 `nodeDragThreshold` 吞掉第一段位移。** 拖拽越长丢得越多，
   落点永远差一截。`dragMouse` 现在先发一个 2 px 的「唤醒」移动。
6. **`viewport` 的 transform 是相对画布容器的，不是相对窗口的。**
   摆位时多加一次 `canvas.left`，节点被推出画布 —— 而 DOM 查询照样找得到它，
   于是拖拽落在了右侧 3D 面板上。
7. **`fitView` 之后缩放会大于 1**，两个节点并排就塞不进画布。
   拖拽类的分组现在先 `normalizeZoom` 把缩放压到 0.6–0.8 再摆位。
8. **重连端点会被上游节点盖住。** 它在端口外侧 `reconnectRadius` 处；
   两个节点离得近时那个位置正好在上游节点的方块上，一按下去变成拖节点。
9. **缓存让 C++ / Rust 测试产生顺序依赖。** 见「偏离与决策」第 13 条。
10. **Windows 的定时器粒度让「睡 200 ms」的测试算子睡了 600 ms。**
    原来把总时长切成 40 段 5 ms 累加，而每次 `sleep_for(5ms)` 实际约 15 ms。
    改成掐总的 deadline。并行验收差点因此假红。
