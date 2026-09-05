# M4 验收记录 —— 能扩展

逐条对着 [m4-plan.md](m4-plan.md) §6 与 §1–§4 走一遍：**怎么跑 + 实际输出摘要 + 通过/未通过/未验证**。
与计划不同的地方全部记在最后的「偏离与决策」里。

复现用的四条命令（Windows，MSVC + vcpkg 的 PCL，vcpkg 在 `C:\vcpkg`）：

```powershell
pnpm check          # C++ 编译 + 自检 + doctest → schema 校验 → cargo test → CLI → 前端 build
pnpm e2e            # CDP 驱动真实 app（自己起 tauri dev，跑完自己收尾）
pnpm tauri build    # 打包；产物在 bridge\target\release\
pnpm e2e:packaged   # 同一套断言，跑打包产物在一个干净目录里的拷贝
```

单独跑某一层：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-core.ps1   # C++ + doctest
.\build\core\bin\lyflow-core-tests.exe                            # 只跑 doctest
cd bridge; cargo test                                             # Rust + CLI
cd bridge; cargo build --bin lyflow --no-default-features         # CLI 不依赖 Tauri 的证据
pnpm cli:build                                                    # release 版 CLI
powershell -ExecutionPolicy Bypass -File scripts\headless-demo.ps1  # §6 第二条：无界面演示
```

---

## §6 —— M4 完成的定义

### ✅ 1–4 各自验收自动化

| 章节 | 自动化在哪 | 结果 |
|---|---|---|
| §1 子图 | `core/tests/test_subgraph.cpp`（12 个 TEST_CASE）+ `scripts/e2e/m4.mjs` 的八个分组 | 通过 |
| §2 live preview | `test_subgraph.cpp` 的两个 preview TEST_CASE + `bridge/src/cli.rs` 的 `preview_decimates_the_source` + `m4.mjs` 的「§2 live preview」 | 通过 |
| §3 headless CLI | `bridge/src/cli.rs` 的 `mod tests`（20 个）+ `scripts/headless-demo.ps1` | 通过 |
| §4 大图性能 | `m4.mjs` 的「§4 大图性能」 | 通过 |

CLI 没有界面，所以它的验收在 `cargo test` 而不是 CDP 里 —— 这一条记进了「偏离与决策」第 3 条。

### ✅ 一条真实任务用「库算子 + CLI」跑在无 GUI 的机器上

**怎么跑**：`powershell -ExecutionPolicy Bypass -File scripts\headless-demo.ps1`
（需要先 `pnpm cli:build`）。脚本自己造一个临时库目录、写一个库算子、写一张用它的图，
然后只用 `lyflow.exe` 走完 manifest / validate / plan / run / dump / sweep / diff 与退出码。

**实际输出摘要**（本机，release 版 CLI）：

```
=== manifest --check：库算子已注册 ===
  lib.denoise -> Library/Cleanup，1 个参数
=== validate：图是合法的 ===
[]
校验通过
=== plan：库算子展开成了路径式节点 ===
  n_gen           81553e00eaf7b8b35410db5db1568d14
  n_clean/voxel   609a01331a57434093dc225027b67b98
  n_clean/sor     70050eb953a9fe6ef8510639a174b9f6
=== run：JSON Lines 事件流 ===
run ok in 175 ms
  n_gen          200000 点
  n_clean/voxel   76471 点
  n_clean/sor     72668 点
=== dump：把结果写成 PCD ===
  ...\cleaned.pcd  1162876 字节
=== sweep：扫 5 组 leafSize，源头只加载一次 ===
  5 组，其中 4 组复用了源头的结果
  n_clean.leafSize,n_clean/sor:cloud.elementCount
  0.005,143224
  0.01125,61788
  0.017499999999999998,29042
  0.02375,16359
  0.03,10685
=== diff：只挪了坐标的两份图没有语义差异 ===
=== 退出码 ===
  坏参数 -> exit 1（应当是 1）
无界面演示全程跑通
```

**「无 GUI 的机器」这一条如实说明**：脚本本身**没有启动任何窗口**，也没有加载 Tauri —— CLI 是
`--no-default-features` 构建的，Tauri 一行都没编进去（`pnpm check` 里有这条门禁）。
但它跑在**本机**，一台装了 MSVC、vcpkg 与 WebView2 的开发机上。
「同一份产物在一台干净的无 GUI 机器上能跑」与 M2/M3 一样**未验证**：手上没有干净机器。
真要验，需要的是 `lyflow.exe` + `bridge\target\release\` 下的全部 DLL（PCL/boost 那几十个），
拷进一个空目录，设 `LYFLOW_LIBRARY_DIRS` 指向库文件所在目录即可 —— 这正是
`pnpm e2e:packaged` 的 `stagePackagedApp` 做的事（它现在也会把 `lyflow.exe` 一起拷进去）。

### ✅ 交互清单 P2 #31 #32 #33 #34 #36 #37 标完成

见 [interaction-checklist.md](interaction-checklist.md) 的 P2 表。#32（分组框）**以子图取代**，
理由记在「偏离与决策」第 1 条。#35（并排对比）与 #38（协作）仍然没做，前者留 M5。

---

## §1 子图 / 复合算子

### ✅ 把演示 pipeline 的中间几个节点合成子图，运行结果与合成前完全一致

**怎么跑**：`pnpm e2e` 的「§1 子图：合成之后结果一模一样，事件里是路径式 id」分组。
五节点直链 `gen → crop → voxel → sor → tail`，跑一次记下末端点数，
把中间三个合成子图，再跑一次。

**实际输出摘要**：

```
✓ 合成前跑通                    ✓ 末端有点数
✓ composeSubgraph 返回了新节点   ✓ 顶层剩下三个节点
✓ 顶层节点是 gen / 子图 / tail   ✓ 子图收了三个节点
✓ 跨边界的入边变成了一个输入端口   ✓ 跨边界的出边变成了一个输出端口
✓ 子图节点的 op 是 sub: 引用
✓ 计划里是展开后的路径 id        ✓ 子图节点本身不在计划里
✓ 合成后跑通
✓ 末端点数与合成前完全一致        ✓ 子图内部的体素点数也一致
✓ 子图内部全部命中缓存（合成不改变 cacheKey 以外的东西）
✓ 子图节点带子图角标   ✓ data-subgraph 指向定义
✓ 状态聚合成一个（全 done/skipped → done/skipped）   ✓ 角标显示内部进度 3/3
```

「末端点数与合成前完全一致」是最硬的那一条：合成之后子图内部**全部命中缓存**，
说明展开出来的 cacheKey 与合成前逐个相同 —— 合成是一次纯粹的结构重排。

C++ 侧对应 `test_subgraph.cpp` 的「子图展开：结果与平图逐点一致」：
平图与子图版本的 `elementCount` 逐个相等，且计划里没有 `s` 只有 `s/v`、`s/p`。

**结论：通过。**

### ✅ 改一个提升参数只重算子图内受影响的节点

**怎么跑**：`pnpm e2e` 的「§1 提升参数」分组。走**真实的参数右键菜单**提升，
退出子图后在外层表单上改值，再对比 `plan_graph` 的 `cached`。

**实际输出摘要**：

```
✓ 右键菜单里提升成功              ✓ 绑定指到内部节点的那个参数
✓ 内参标注了提升来源              ✓ 内参变成只读
✓ 外层节点的表单上有这个参数       ✓ 外层可编辑
✓ 源头仍然命中缓存                ✓ 子图里的裁剪也命中缓存
✓ 被改的体素要重算                ✓ 它的下游也要重算
✓ 改完还能跑通                    ✓ 源头是 skipped     ✓ 体素是 done
```

C++ 侧对应「子图参数：外参覆盖内参，一个外参可绑多个内参」：
同一个 `leaf` 绑到两级体素上，两级跑出来的点数相同（第二级什么都没再减掉）。

**结论：通过。**

### ✅ 保存到库后新建图从面板拖出即用

**怎么跑**：`pnpm e2e` 的「§1 库算子」分组。走真实的右键菜单 →「保存到库…」→ 对话框 → 保存。

**实际输出摘要**：

```
✓ 右键 → 保存到库 → 对话框走通
✓ 库里至少有一个算子              ✓ 库文件写到了 app data 下的 library/
✓ manifest 里出现了 lib.<id>      ✓ 分类挂在 Library/ 下
✓ 节点面板里也有它（manifest 驱动，前端零改动）
✓ 库算子在新图里跑通              ✓ 库算子展开成了内部节点   ✓ 内部节点都有结果
✓ 收尾：库文件已删除
```

C++ 侧对应「库目录：`*.lyflow-op.json` 注册成 `lib.<id>`，和内置算子无差别」：
扫描后 `Registry::find("lib.clean")` 拿得到，`category == "Library/Cleanup"`，
整份注册表自检干净，用它的图跑出 `s/v` 是 `done`。

**结论：通过。**

### ✅ 嵌套两层子图的 cacheKey 稳定（重跑全 skipped）

**怎么跑**：`pnpm e2e` 的「§1 嵌套两层」分组 + doctest「子图嵌套两层：cacheKey 稳定，重跑全 skipped」。

**实际输出摘要**：

```
✓ 两层都合成出来了     ✓ 计划里出现两层路径（n_x/n_y/n_voxel）
✓ 第一次跑通           ✓ 第二次跑通
✓ 重跑全部 skipped     ✓ 两次编译出来的 cacheKey 一模一样
✓ 能进到第二层         ✓ 第二层的事件前缀是两段   ✓ 连按两次 Esc 回到顶层
```

doctest 里那条更严格：两次 `run_started.nodes` 的整段 JSON（id + cacheKey + level）逐字相等。

**结论：通过。**

### ✅ 递归引用被拒

**怎么跑**：doctest「子图递归引用被拒」+ `pnpm e2e` 的「§1 递归引用被拒」。
界面上造不出递归，所以 e2e 走的是「打开一份手改过的文件」这条真实路径。

**实际输出摘要**：`prepareGraph` 返回 false，诊断里有 `recursive_subgraph`；
整条运行路径的结果是 `run_finished: error`，不是崩溃。
CDP：`validate_graph` 报 `recursive_subgraph`，诊断挂在 `n_a/inner`（展开时真正出问题的那个节点）。

**结论：通过。**（诊断的 nodeId 是路径而不是顶层节点，见「偏离与决策」第 5 条。）

### ✅ 导航、状态聚合、3D 视图

**怎么跑**：`pnpm e2e` 的「§1 子图导航」分组。

```
✓ 双击子图节点进去了     ✓ 当前层级是子图的三个节点   ✓ 画布上渲染的就是内部节点
✓ 事件前缀是路径         ✓ 面包屑显示深度 1           ✓ 面包屑里有子图名
✓ 内部节点显示自己的执行状态
✓ 子图里选中体素能看到点云
✓ Esc 退回顶层           ✓ 顶层又是三个节点           ✓ 点面包屑的「顶层」也能退出
```

### ✅ 解散子图与快捷键

```
── §1 解散子图：内容内联回来，提升的参数落回内参
✓ 内联出两个节点   ✓ 顶层回到四个节点   ✓ 子图定义已经删掉了
✓ 提升参数的值落回了内参（外层改成 [0.07,0.07,0.07] → 内参就是它）
✓ 解散之后照样跑通

── §1 快捷键：Ctrl+G 合成、Ctrl+Shift+G 解散
✓ Ctrl+G 之后顶层剩三个节点   ✓ 多了一份子图定义
✓ Ctrl+Shift+G 之后回到四个节点   ✓ 子图定义也一并清掉
✓ 撤销回到子图状态   ✓ 再撤销回到四个节点
```

另有 doctest 覆盖「子图节点静音 → 整棵子树透传」「Run to node 的目标可以是子图节点」
「子图的未知参数会被报出来」「合成出来的 OperatorDesc 通得过注册表自检」。

---

## §2 Live preview

### ✅ 拖滑块时 3D 视图跟手（事件到渲染 < 100 ms），松手后正式结果替换预览

**怎么跑**：`pnpm e2e` 的「§2 live preview」分组。页面里挂一个 `MutationObserver` 盯着
`.viewer[data-run]`（**这片云属于哪一次运行**），`window.__lyflow.runMarks` 记下每次运行结束的
`performance.now()`，两者相减就是「事件到渲染」。拖的是 `filter.random_sample.keepRatio` 的
真滑块，用 CDP 的 Input 域发真鼠标事件。

**实际输出摘要**：

```
✓ 找到 keepRatio 的滑块
✓ 拖动触发了预览运行
✓ 事件到渲染 61 ms < 100 ms
✓ 松手后自动补了一次正式运行（preview 标记已经落回 false）
✓ 正式运行的源头是全量点数（400000）
✓ preview run 里源头抽到了 2 万点
✓ 预览的结果进了独立命名空间（正式那份的条目没被顶掉）
✓ 正式重跑仍然全部命中缓存
```

**61 ms 是 `tauri dev` 下的数字**，也就是 **debug 构建的 Rust**。这一轮为它做了一处真优化：
`encode_cloud` 原先逐个 `f32::to_le_bytes` 拼字节，几十万次未内联的调用在 debug 下就是几十毫秒
（改之前量到的是 90 ms）；改成整段按字节拷（小端机器上 f32 的内存布局就是要的字节序）。
release 构建（`pnpm e2e:packaged`）的数字见下面「打包与干净目录」。

### ✅ 预览期间正式缓存无新增条目

`cacheKey` 混入 `preview:<maxPoints>`，两组键互不命中。断言的是「预览之后正式重跑**仍然全部
skipped**」—— 如果预览污染了正式的键，这一条立刻会红。

**结论：通过。**

C++ 侧另有两条 doctest：「preview 模式：源头抽稀，且不污染正式缓存」（50 万点的源在
preview 下报 2 万点，正式跑仍然报 50 万点，两边的 cacheKey 不同）、
「preview 超预算会发一条 warn 日志」。

---

## §3 Headless CLI

### ✅ CI 脚本用 `lyflow run` 跑演示 pipeline 并断言退出码与最终点数

**怎么跑**：`cd bridge; cargo test`（20 个 CLI 测试）+ `scripts\headless-demo.ps1`。

**实际输出摘要**（`cargo test --quiet`）：`53 passed; 0 failed`（M3 是 32）。CLI 那部分覆盖：

| 测试 | 断言的东西 |
|---|---|
| `manifest_check_is_clean` / `manifest_dumps_the_whole_bundle` | `--check` 退出 0 且 problems 为空；manifest 压成一行 JSON |
| `validate_accepts_a_good_graph_and_rejects_a_bad_one` | 干净的图输出 `[]` 退出 0；坏参数退出 **1** 且 `paramPath == "leafSize"` |
| `plan_reports_cache_keys` | 每节点 32 位 cacheKey 与 level |
| `run_streams_execution_events_as_json_lines` | 首条 `run_started`、末条 `run_finished: ok`、**seq 逐条连续**、`schemaVersion == 1` |
| `run_set_overrides_a_param` | `--set g.pointCount=1234` → 事件里 `elementCount == 1234` |
| `run_to_node_prunes_the_downstream` | `--to g` 的计划里只有 `g` |
| `run_exits_one_on_validation_failure` | 退出 **1**，输出的是诊断不是事件 |
| `run_exits_two_when_a_node_fails` | 读不到的文件 → 退出 **2**，`run_finished: error` |
| `unknown_subcommand_and_option_are_usage_errors` | 退出 **4** |
| `dump_writes_the_output_to_disk` | PCD 真的写出来且 > 1 KB，末行是 `dump_written` |
| `sweep_reuses_the_upstream_across_the_grid` | 5 组里 **4 组** 的 `skipped` 含源节点；指标单调下降；CSV 6 行 |
| `sweep_broadcasts_a_scalar_onto_a_vector_param` | 扫 `leafSize`（vec3f）时一个数广播到三个分量 |
| `diff_ignores_ui_and_catches_params` | 只挪坐标 → `empty: true`；改参数/加节点 → 精确列出 |
| `diff_merges_defaults_before_comparing` | 显式写一个等于默认值的参数不算变化（稀疏存储） |
| `migrate_reports_and_optionally_writes` | 不带 `--write` 一个字节都不改；带了之后再跑没有迁移 |
| `subgraph_expands_into_path_ids` | CLI 看见的 plan 是 `["g", "s/v"]` |
| `preview_decimates_the_source` | `--preview --preview-points 2000` → 源头报 2000 |
| `set_rejects_an_unknown_node` | 退出 1 并说「图里没有节点」 |

`scripts\headless-demo.ps1` 的输出见 §6 那一节。演示 pipeline 的最终点数是
`n_clean/sor 72668 点`，退出码 0。

**结论：通过。**

### ✅ `sweep` 5 组 leafSize 只加载文件一次

`sweep_reuses_the_upstream_across_the_grid` 断言 5 行里恰好 **4 行**的 `skipped` 含源节点；
`headless-demo.ps1` 用真库算子重跑了一遍，同样是「5 组，其中 4 组复用了源头的结果」。

### ✅ `diff` 对只移动节点的两份图输出为空

`diff_ignores_ui_and_catches_params` 断言 `empty: true`；
`headless-demo.ps1` 用 `--json` 再验一遍。

### ✅ CLI 不依赖 Tauri

`pnpm check` 里的「headless CLI（不带 Tauri）」这一步跑
`cargo build --bin lyflow --no-default-features`：整棵依赖树里没有 tauri。
这是 F6 唯一靠得住的证据 —— 光看代码说「我没 import 它」是不够的。

---

## §4 大图性能

**怎么跑**：`pnpm e2e` 的「§4 大图性能」分组。fixture 是 30 条链 × 10 个节点的合成图，
每条链里穿插 4 个 `util.merge`，它们的第二个输入从链头引一条边过来（制造扇出）。

**实际输出摘要**：

```
✓ 搭出 300 节点 / 390 边
✓ 打开 300 节点的图用了 89 ms < 1000 ms
✓ 只渲染了视野里的节点（onlyRenderVisibleElements）
✓ 拖动时的帧率 189 fps ≥ 30
✓ 40 个节点的运行：160 次状态变化合并成 3 次 store 更新
```

帧率是页面里挂一个 rAF 采样器、同时用 CDP 的 Input 域**真的拖一个节点**量出来的中位数。
合成 `MouseEvent` 骗不过 React Flow 的 d3-drag（它要读 `event.view.document`）——
第一版就是这么写的，结果控制台里刷出一串 `Cannot read properties of null`。

「160 次状态变化合并成 3 次 store 更新」是 §4 那条「16 ms 内批量应用一次」的直接证据。

**结论：通过。**

---

## 全链路门禁与 CDP 验收

### ✅ `pnpm check` 全链路绿

```
=== C++ core ===          ok: 16 operator(s), 5 port type(s)
                          [doctest] test cases: 77 | 77 passed | 0 failed
                          [doctest] assertions: 2771 | 2771 passed | 0 failed
=== manifest vs schema ===        ok: 16 operator(s), 5 port type(s) 符合 schema
=== execution-event vs schema === ok: 16 条符合 schema
=== graph-doc vs schema ===       ok: 4 node(s), 3 edge(s) 符合 schema
=== Rust bridge ===       test result: ok. 53 passed; 0 failed
=== headless CLI（不带 Tauri） === 算子描述自检干净
=== frontend ===          tsc --noEmit && vite build ✓
全链路绿
```

doctest 从 M3 的 65 例涨到 **77 例**（新增 `test_subgraph.cpp` 的 12 例），
cargo 从 32 项涨到 **53 项**（CLI 20 项 + 法线载荷 1 项）。
schema 样例也跟着补了：图样例里加了一份带提升参数的子图定义，
事件样例里加了 preview 模式的 `run_started` 与路径式 `nodeId`。

### ✅ `pnpm e2e` 全绿

M2 分组 + M3 分组 + M4 分组一起跑，**286/286 全绿**（M3 是 188 项）。
M4 新增的分组：子图合成/导航/提升/解散/快捷键/嵌套/递归/库算子、live preview、大图性能，
以及「M3 尾巴」那一组。

### ✅ 打包与干净目录：`pnpm tauri build` + `pnpm e2e:packaged`

```
$ pnpm tauri build
    Finished `release` profile [optimized] target(s) in 2m 44s
       Built application at: D:\project\LyFlowridge	arget
elease\lyflow-app.exe
    Finished 2 bundles at:
        ...undle\msi\LyFlow_0.1.0_x64_en-US.msi
        ...undle
sis\LyFlow_0.1.0_x64-setup.exe
```

产物路径：

| 东西 | 路径 |
|---|---|
| 桌面壳 | `bridge	arget
elease\lyflow-app.exe`（9.8 MB，GUI 子系统） |
| headless CLI | `bridge	arget
elease\lyflow.exe`（623 KB，控制台子系统，不含 Tauri） |
| 安装包 | `bridge	arget
eleaseundle\msi\LyFlow_0.1.0_x64_en-US.msi` |
| | `bridge	arget
eleaseundle
sis\LyFlow_0.1.0_x64-setup.exe` |

`pnpm e2e:packaged`：把 exe（两个都拷）+ 同目录的 20 个 DLL 复刻进一个干净目录再跑同一套断言，
**289/289 全绿**（比 dev 那轮多四条：打包产物启动、core 版本可读、内置算子 ≥ 16、CLI 也随包）。

release 构建下的性能数字（同一批断言，比 dev 那轮更快）：

```
✓ 事件到渲染 53 ms < 100 ms          （dev 是 61 ms）
✓ 打开 300 节点的图用了 46 ms < 1000 ms（dev 是 89 ms）
✓ 拖动时的帧率 189 fps ≥ 30
✓ 40 个节点的运行：160 次状态变化合并成 3 次 store 更新
```

**这不等于「在一台干净机器上验过」**：这台机器有 MSVC 与 vcpkg，漏打包的 DLL 仍可能被系统
从别处找到。与 M2/M3 一样如实标为**未验证**。

---

## M3 留下的三个尾巴

### ✅ (a) `core-watch` 对重命名式写入不触发

**问题**：`sed -i`（以及编辑器的原子保存）是「写一个临时文件再改名」。
`FileSystemWatcher.WaitForChanged` 是**同步**的 —— 它只在被调用的那一刻才注册监听，
两次调用之间发生的事件直接丢掉。临时文件的 `Created` 事件吃掉了一次 `WaitForChanged`，
紧接着的 `Renamed` 就落在了两次调用的缝里。

**修法**：事件监视降级成「快一点的信号源」，真正的判据换成**源码指纹**
（所有关心的文件的 `LastWriteTimeUtc` 之和 + 个数），每 2 s 轮询一次比对。
指纹对「字节是怎么写进去的」完全不敏感。同时把 `CreationTime` 也加进 `NotifyFilter`。

**怎么验**：把 core-watch 的检测部分单独抽出来跑（不构建，检测到就打印一行），
对着一个临时目录做一次 `sed -i`：

```
# 旧逻辑
PROBE-READY
PROBE-EVENT Created sub\sed0OIcDV      ← 只看见临时文件，改名那一下丢了
PROBE-DONE                              ← 16 秒里再没有任何反应

# 新逻辑
PROBE-READY
PROBE-BUILD 03:40:54.331                ← sed 之后 ~2.3 s（轮询间隔）触发
PROBE-DONE
```

**结论：通过。** 代价是最坏情况下比原地写入慢 2 s，以及每 2 s 遍历一次 `core/`
（一百多个文件，可以忽略）。

### ✅ (b) PNG 导出走保存对话框

**修法与计划不同**：没有装 `tauri-plugin-fs`，而是加了一个**只做一件事**的 command
`write_file_bytes(path, contents)`。导出流程变成
「`plugin-dialog` 的 `save()` 选路径 → `write_file_bytes` 写字节」。
理由记在「偏离与决策」第 2 条。

**怎么验**：`pnpm e2e` 的「M3 尾巴」分组里，CDP 调 `transport.writeFileBytes` 往中文路径写
8 个字节的 PNG 头，Node 侧读回来逐字节比对。原生保存对话框 CDP 驱动不了，
所以**「点开对话框」那一下没有自动化覆盖**；文件写出这一半是覆盖了的。

### ✅ (c) `lyflow_cloud_view` 加 normals 通道

**修法**：`CloudPreview` / `lyflow_cloud_view` / 二进制载荷 / `decodeCloud` 四处一起加。
载荷布局变成 `头 | xyz | [intensity] | [normals]`，可选通道按 `flags` 的位序排
（bit0 = intensity，bit1 = normals），加通道只要在两端各加一个位。

**怎么验**：
- `cargo test cloud_payload_carries_normals_when_the_op_produces_them`：
  `gen.synthetic` 的输出 `has_normals() == false`，接一个 `features.normals` 之后是 true，
  载荷长度是 `16 + 24 + 12n + 4n + 12n`，法线段的第一个 float 与 `view.normals()[0]` 相等。
- `pnpm e2e` 的「M3 尾巴」分组：源头的点云上「法线」选项是禁用的且写着「（无）」，
  换到 `features.normals` 的输出之后选项可用、文字不再带「无」，
  切过去之后 `.viewer[data-shading] == "normal"`。

**结论：通过。** 法线着色用分量绝对值当 RGB（色带对法线没有意义，所以走单独一条路）。

---

## 契约与文档

| 交付物 | 状态 |
|---|---|
| [ADR-0010 subgraph-by-expansion](adr/0010-subgraph-by-expansion.md) | ✅ |
| [ADR-0011 preview-as-decimated-run](adr/0011-preview-as-decimated-run.md) | ✅ |
| [ADR-0012 headless-cli](adr/0012-headless-cli.md) | ✅ |
| `schema/graph-doc.schema.json`：`subgraphs` 完整结构（subgraph / subInput / subOutput / subParam）、`op` 说明 `sub:` 与 `lib.` | ✅ 图样例已带一份子图并通过校验 |
| `schema/execution-event.schema.json`：`run_started.mode` / `previewMaxPoints`、`nodeId` 的路径式说明、error code 加 `recursive_subgraph` | ✅ 事件样例 16 条通过校验 |
| `docs/graph-doc.md`：新增「子图」一节 | ✅ |
| `docs/operator-manifest.md`：新增「子图与库算子也是算子」 | ✅ |
| `docs/interaction-checklist.md`：P2 #31 #32 #33 #34 #36 #37 标完成 | ✅ |
| `docs/roadmap.md`：M4 勾选 + 新契约 + 抓到的 bug + 已知毛刺 | ✅ |
| `README.md`：命令行用法、库目录位置、状态改成 M4 | ✅ |
| `core/README.md`：子图与库算子、live preview | ✅ |
| `bridge/README.md`：两个 bin、库算子目录、新 command、载荷布局 | ✅ |
| `app/README.md`：层级（子图）、大图性能 | ✅ |
| `scripts/e2e/README.md`：m4.mjs 与「CLI 为什么不在这里」 | ✅ |

### 注释收敛（CLAUDE.md 第二条）

用与 M3 同一份扫描器（一段连续的行注释 > 2 行，或跨 3 行以上的 `/* */` 块算违规；
扫 `core/ bridge/src bridge/build.rs app/src scripts/ schema/` 的
`.cpp .h .hpp .rs .ts .tsx .ps1 .mjs .js .py .css` 外加所有 `CMakeLists.txt`）：

```
TOTAL 0 in 0 files
```

新写的五处超长注释（GraphCanvas / execution.ts / graph.ts / c_api.h / core-watch.ps1 的文件头）
在提交前压回了两行。

---

## 这一轮抓到的真 bug

1. **`RunLog::ofKind()` 返回临时 vector，range-for 里活不过初始化那一句。**
   `for (const Json& n : log.ofKind("run_started").front()["nodes"])` —— C++17 的 range-for
   只延长**最终**那个临时量的寿命，中间那个 vector 在循环开始前就析构了。
   症状极具迷惑性：`elementCountOf()` 读同一批事件全对，`planIds()` 却拿到一个空列表。
   修法是先落地成局部变量。（helpers.h 里原有的 `runStatus()` 恰好是对的 —— 它存了局部变量。）

2. **`RunHandle` 一 drop，结果仓的索引就没了。** CLI 的 `dump` 先 `execute()` 再
   `output_save()`，而 `execute` 返回时句柄已经析构 → `lyflow_run_free` → `freeRun` →
   「结果仓里没有 v.cloud」。修法是把句柄挂在 `RunResult` 上活到调用方用完。
   这条契约 `bridge/README.md` 早就写着，但只有真的分两步用才会踩到。

3. **两个 bin 让 `cargo run` 不知道该跑哪个。** `tauri dev` 跑的是不带 `--bin` 的
   `cargo run`，加了 CLI 之后直接 `error: could not determine which binary to run`，
   退出码 101。`pnpm e2e` 于是卡在「等 CDP target」上十五分钟才超时 ——
   而 `tail -80` 把 tauri 的输出全缓冲住了，什么都看不见。
   修法是 `default-run = "lyflow-app"`。

4. **事件合并把 `run_started` 播下的 idle 占位吃掉了。** M3 的 devbridge 从 store 快照推
   状态流水账，M4 加了 16 ms 合并窗口之后，`idle → pending` 这一跳在同一个窗口里被抹平。
   十条「状态序列」断言同时变红。修法是流水账改成从**事件**记，并在 `run_started` 时
   为计划里的每个节点补一条 idle。

5. **把 float 绑到 vec3f 的内参上，报的是 `bad_param` 而不是「类型不匹配」。**
   写第一版 doctest 时把 `leaf`（float）绑到 `leafSize`（vec3f）上，
   诊断是「数组长度应当是 3」—— 对，但要顺着 `coerceParam` 才看得懂。
   子图的参数声明是**数据**，不是代码，所以展开阶段没法在绑定时就查类型：
   内参的类型只有 manifest 知道，而合成 OperatorDesc 时并不知道它会被绑到哪儿。
   现状是「内参那一侧照常报错，paramPath 指向内参名」，记进「偏离与决策」第 6 条。

6. **`tauri build` 把 CLI 改名盖掉了桌面壳。** tauri-cli 按**cargo 包名**去找刚编出来的
   exe，再把它改名成 `mainBinaryName`。包名是 `lyflow`，而 `lyflow.exe` 正是 CLI ——
   于是 `target
elease\lyflow-app.exe` 变成了一份 CLI 的拷贝，`lyflow.exe` 直接消失。
   `pnpm e2e:packaged` 的症状是「启动了 lyflow-app.exe，然后终端里打出一屏 CLI 用法，
   接着 CDP 等十五分钟超时」。修法是把**包名**也改成 `lyflow-app`，
   那次改名就变成了 no-op。这条只在打包路径上暴露，`pnpm e2e`（走 `cargo run`）永远碰不到。

另有一条不算 bug 但值得记：`encode_cloud` 逐个 `to_le_bytes` 在 debug 构建里是几十万次
未内联调用，占了「事件到渲染」延迟的一大半。改成整段按字节拷之后从 90 ms 降到 61 ms。

---

## 偏离与决策

计划没覆盖、或与计划不同的地方，依据 F1–F7 与 architecture.md 的职责边界自行决定，逐条记在这里。

1. **P2 #32「节点分组框」以子图取代，没有单独做一个纯 UI 的框。**
   m4-plan §1.1 写着「`groups` 同时启用（P2 #32）：纯 UI 的框，schema 早已预留」。
   实际做下来，子图节点本身就是「一个可折叠的框」，而且它有语义：能提升参数、
   能整体静音、能存成库算子、能进去编辑。再叠一个纯 UI 的框会变成两套心智
   （「这几个节点是一组」到底是哪一种组？），而它带来的唯一新能力是「框住但不改变执行」——
   那正是选中多个节点已经能做的事。`groups` 字段仍然在 schema 里留着，没有实现。

2. **PNG 导出没有装 `tauri-plugin-fs`，改成一个窄口的 `write_file_bytes` command。**
   计划写的是「装 tauri-plugin-fs（**或等价方案**）」。装插件要开一整片 ACL
   （`fs:allow-write-file` 加 scope），而前端真正需要的只有「把这些字节写到用户刚在
   保存对话框里选的那个路径」。一个 command 是六行代码、零新依赖、权限面最小。
   CLI 写文件走的是 Rust 的 `std::fs`，本来就不需要插件。

3. **CLI 的验收在 `cargo test` 而不是 CDP。** 计划的验收方式一栏没有明说 CLI 走哪条。
   CDP 是用来驱动**界面**的，而 CLI 没有界面；把它塞进 e2e 只会让每条断言都要先起一个
   Tauri 窗口。实现因此放在 `src/cli.rs`（lib 里）而不是 bin 里 —— bin 只有三行。
   另外补了 `scripts/headless-demo.ps1` 做端到端的真实演示（§6 第二条）。

4. **桌面壳的 exe 改名成 `lyflow-app.exe`，cargo 包名也跟着改成 `lyflow-app`。**
   包名推导出的 bin 名（`lyflow`）让给了 CLI —— m4-plan §3 明写「`lyflow run …`」，
   改 CLI 的名字会让文档里所有命令都变形。连带改动：`tauri.conf.json` 的 `mainBinaryName`、
   `Cargo.toml` 的 `default-run`（`tauri dev` 跑的是不带 `--bin` 的 `cargo run`）、
   以及**包名**（`tauri build` 按包名找 exe 再改名，见「抓到的真 bug」第 6 条）、
   `scripts/e2e/harness.mjs` 一处。产品名、窗口标题、安装包名仍然是 LyFlow。

5. **子图内部的诊断挂在路径 id 上，不聚合到子图节点。** F2 只说「前端按路径前缀聚合到
   当前层级的节点上」，没说诊断怎么办。现状：`node_state` 的状态确实聚合了（子图节点会变红），
   但**诊断文本**留在内部节点上，要进去才看得到具体是哪个参数。
   把内部诊断复制一份到子图节点上会让「一次看到所有红框」变成「同一条错误看到两遍」，
   而 `paramPath` 指的是内参名，在外层表单上根本没有对应的输入框。

6. **提升参数不做类型校验。** 子图的参数声明是数据（一份 JSON），绑定关系也是数据；
   展开时把外参的值原样写进内参，类型对不对由**内参那一侧**的常规校验来判
   （诊断的 `paramPath` 是内参名，`nodeId` 是内部节点的路径）。
   在展开阶段查类型需要合成 OperatorDesc 时就知道「这个参数会被绑到哪个算子的哪个参数」，
   那是把 manifest 的知识搬进展开器 —— 违反「谁掌握信息谁做决策」。
   界面上从内参右键提升时，外参的类型是**从内参抄过来的**，所以正常路径不会错。

7. **静音一个子图节点 = 静音它整棵子树。** F1 只说「`bypass` 的子图节点整体透传」。
   实现是展开时把 `bypass` 沿路径传下去，每个内部节点各自按 E5 的规则透传。
   对线性子图这正是期望行为；对内部有分叉的子图，规则与单个节点被静音时完全一致
   （每个输出取第一个类型兼容且已连线的输入）。备选是「把外部边直接短接」，
   那要在展开器里重新实现一遍 E5 的类型匹配，而且 cacheKey 会变得没法解释。

8. **`Run to node` 的目标按路径前缀匹配。** 计划没提子图节点怎么当目标。
   给一个子图节点的 id 等于给它展开后的全部内部节点 —— 否则「右键子图 → 运行到此节点」
   会报 `unknown_node`（那个 id 在展开后的图里根本不存在）。

9. **`sweep` 的 `--param` 支持把一个数广播到 vecNf 的全部分量。**
   m4-plan §3 的验收原话是「`sweep` 5 组 leafSize」，而 `leafSize` 是 `vec3f`。
   分量数从「图里当前写的值 → manifest 的默认值 → 子图定义里的默认值」依次问，
   都问不到就当标量。不这么做的话那条验收根本写不出来。

10. **`diff` 默认输出给人看，`--json` 才是机器格式。** 与「stdout 是 JSON Lines」不一致，
    但 `--json` 这个 flag 在计划里就写着，说明当初也是这么想的。差异不算错误，
    所以 `diff` 永远退出 0。

11. **`lyflow manifest`（不带 `--check`）把 core 那份缩进过的 JSON 压成一行。**
    否则 stdout 就不是 JSON Lines 了。第一版直接透传，`cargo test` 立刻红。

12. **`save_as_library` 拒绝内部还有 `sub:` 的子图。** 库文件必须自包含 ——
    它被别的文档加载时，那份文档里没有内层子图的定义。错误信息里写了出路
    （「先把内层也保存到库，或者解散它」）。

13. **库算子不能「展开为内联子图」。** m4-plan §1.4 列了这一项。定义在库文件里，
    前端手上只有合成出来的 OperatorDesc（端口 + 参数），没有内部拓扑。
    右键菜单里那一项留着，点了会明说这件事而不是静默无反应。
    真要做，需要一个「把库定义整份取回来」的 command —— 那是 M5 的事。

14. **`Ctrl+G` 从「整理布局」改成「合成子图」，整理布局挪到 `Ctrl+L`。**
    m4-plan §1.4 明写「选中若干节点 → Ctrl+G『合成子图』」，而 graph-doc.md 里
    Ctrl+G 原本是整理布局。Blender / ComfyUI 的 Ctrl+G 都是分组，跟着惯例走。
    `docs/graph-doc.md` 与 `lib/keymap.ts`（唯一那张键表）已同步，
    `scripts/e2e/m3.mjs` 里那条断言改按 Ctrl+L。

15. **`Esc` 在子图里先退一层，没有运行时才取消运行。** 计划说「Esc/面包屑退出」，
    而 Esc 在 M2 起就是「取消运行」。判据：正在跑就取消，没在跑且在子图里就退一层。
    两件事在时间上几乎不会重叠。

16. **preview 只跑到选中节点，没选中节点时拖参数不触发预览。**
    F5 说「目标 = 选中节点」。没有选中节点时 target 是空的，那等于全图预览 ——
    在一张大图上拖参数会变成连续的全图运行。宁可不触发。

17. **`node_state` / `node_progress` 批量落库，`run_started` / `run_finished` 立刻 flush。**
    §4 只说「16 ms 内到达的 `node_state` 批量应用一次」。两端立刻 flush 是必要的：
    验收脚本（和用户）都靠「等运行结束再读状态」，晚 16 ms 落库会让最后几条状态丢在窗口里。
    另外状态流水账改成从事件记而不是从 store 快照推，见上面「抓到的真 bug」第 4 条。

18. **`onlyRenderVisibleElements` 超过 80 个节点才开。** §4 只说「开」。
    小图下全量渲染的手感更好 —— 开了之后平移会有一帧空窗，而三五十个节点的图根本不需要它。

19. **库目录的额外路径走环境变量 `LYFLOW_LIBRARY_DIRS`（分号分隔），没有做设置界面。**
    m4-plan §1.3 说「`library_dirs` 来自 app data 下 `library/` 和设置里的额外目录」。
    项目还没有「设置」这个东西，为一个字段造一整套设置存储不划算；
    环境变量对 CI 与 CLI 也更顺手。app 与 CLI 读的是同一份列表。

20. **`Registry` 加了 `setLibraryOperators`，库算子排在内置算子之后。**
    调用它会让已有的 `OperatorDesc*` 失效，所以约定与热重载完全一致：
    调用前必须放掉所有 `RunHandle`。`refresh_library` 与库目录 watcher 都先 `drop_all()`。

21. **`lyflow_output_save` 是新加的 C ABI，不是让 Rust 自己拼 PCD。**
    CLI 的 `dump` 要写盘，而写盘格式的知识只有 `ops/pcl/io_save_pcd.cpp` 有。
    备选是从 `lyflow_output_cloud` 拿 xyz+intensity 自己拼 —— 那会漏掉 rgb，
    而且同一件事会有两份实现。

22. **`Data` 的 rgb 通道仍然不进二进制载荷。** 这一轮只加了 normals（M3 的尾巴 c 点名要的）。
    rgb 是 `uint8` 三通道，加进去要改载荷布局与 `flags` 的第三个位；
    目前没有算子产出 rgb，加了也没东西可验。

23. **300 节点的基准是合成图**（30 条链 × 10 个节点，其中 4 个是 `util.merge`），
    不是一条真实 pipeline。真实的点云 pipeline 到不了 300 个节点，
    而这个 fixture 的目的就是把渲染压到极限。边数 390（计划说 400）。

24. **`pnpm e2e:packaged` 的干净目录里现在也拷了 `lyflow.exe`。**
    §6 第二条要的是「库算子 + CLI 跑在无 GUI 的机器上」，那份产物里得有 CLI。
