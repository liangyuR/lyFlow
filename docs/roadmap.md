# Roadmap

里程碑按「能演示什么」划分，不按「写完哪个模块」划分。

## M0 — 契约先行 ✅

**目标：三层之间的接口定死，各自可以独立开工。**

- [x] `OperatorManifest` JSON Schema 定稿
- [x] `GraphDoc` JSON Schema 定稿
- [x] `ExecutionEvent` 格式定稿
- [x] C++ 侧：算子注册表 + manifest 导出 + 自检，注册 3 个算子（load_pcd / voxel_grid / passthrough）
- [x] Rust 侧：Tauri 壳，FFI 链入 core，manifest 转发到前端，GraphDoc 结构校验 + 落盘
- [x] 前端：读 manifest 渲染出节点面板 + 算子详情（还不能连线）

产出：一份能跑通的「C++ 加算子 → 前端节点面板自动出现」的链路。这条链路通了，后面加算子就是无痛的。

**验收记录**：在 `core/src/ops/` 新增一个 `.cpp` 并在 `builtin_ops.cpp` 加一行调用后重启，
新算子出现在面板里，且树上自动长出了它所属的新分类分支 —— 前端源码零改动。
门禁 `pnpm check` 覆盖：C++ 编译 + 算子自检 → manifest 对着 schema 校验 →
Rust 15 个测试（含 FFI、中文穿越边界、GraphDoc round-trip）→ 前端 strict typecheck + build。

**已知毛刺**（M3 已解决）：`tauri dev` 只 watch `bridge/`，改了 `core/` 的 C++ 不会自动重编。
M3 起 `pnpm dev` 会同时起 `scripts/core-watch.ps1`，改 C++ 存盘即热重载
（[ADR-0009](adr/0009-hot-reload-by-copy.md)）。

## M1 — 能编辑 ✅

**目标：能画出一张合法的图并存盘。**

- [x] 交互清单 P0 第 1–13 项
- [x] GraphDoc ↔ React Flow 映射层
- [x] 语义化 change 动作 + 撤销重做
- [x] 端口类型校验 + 类型着色

产出：能编辑、能存、能重新打开的编辑器。还不能跑。

**从后续里程碑提前做掉的两项**（成本极低，且直接服务于「画出一张*合法*的图」）：

- P0 #16 环检测 —— 原计划 M2。连线时即时拒绝成环，几十行，`lib/typecheck.ts`
- P1 #18 连线中途松手弹搜索面板 —— 数据通路已具备（`SearchPopup.pendingFrom`）

**验收方式**：不写前端单元测试，改为通过 CDP 驱动真实运行的 Tauri app。
两套脚本共 60 项断言，全绿：

- 状态层 33 项：建图 / 类型校验 / 稀疏参数 / 撤销重做 / 拖动合并 / 复制粘贴 /
  删除清边 / 存读往返 / Rust 拒绝坏图 / 控制台无报错
- UI 层 27 项：真实鼠标键盘事件走完整链路 —— 双击唤起搜索、输入过滤、回车落节点、
  manifest 驱动的控件（vec3f 三分量 + 锁、enum 下拉、滑块、勾选框）、参数联动、
  Ctrl+Z/Y、输入框内按键不误触画布快捷键

**这轮验证抓到的三个真 bug**（都不是单元测试能发现的）：

1. React Flow 底层 d3-zoom 的双击缩放调 `stopImmediatePropagation`，把双击事件
   拦死在冒泡到搜索面板处理器之前 —— 真实用户双击只会放大。已关掉 `zoomOnDoubleClick`
2. MiniMap 一个节点都不画：它判断「节点有没有尺寸」看的是传入的对象，而尺寸只经由
   `onNodesChange` 的 dimensions 事件回传，被我们按 ADR-0002 丢弃了。
   修法是把量测尺寸放进画布组件的旁路缓存、映射时合并，GraphDoc 保持干净
3. 选中状态会流回画布再回调 `onSelectionChange`，无条件写新 Set 会造成渲染死循环。
   `setSelection` 加了幂等比对

## M2 — 能跑 ✅

**目标：点运行，C++ 真的执行，前端看到状态。**

详细实施计划见 [m2-plan.md](m2-plan.md)：定死的决定、C ABI v2、执行器、结果仓与二进制 IPC、15 个算子、验收定义。
逐条验收记录见 [m2-acceptance.md](m2-acceptance.md)：`pnpm check` 全链路绿、
`pnpm e2e` 64/64、`pnpm e2e:packaged`（打包产物在干净目录里）67/67。
唯一没验成的是「同一份产物在一台**干净机器**上能跑」—— 手上没有干净机器，如实标为未验证。

- [x] C++ 侧：GraphDoc 校验 + 编译成执行计划 + 顺序执行
- [x] `ExecutionEvent` 流式推送
- [x] 交互清单 P0 第 14–16 项（状态高亮、错误定位、环检测）
- [x] 3D 视图：选中节点看其输出点云
- [x] 算子扩到 10–15 个，覆盖一条真实 pipeline

产出：**第一条端到端可演示的流程。** 这是项目的分水岭。

**三条形态上的变化**（都在 m2-plan.md §0 里定死，M3+ 只填功能不改形态）：

- core 从「cc crate 编的静态库」变成 **CMake 构建的 DLL + libloading 运行时加载**
  （[ADR-0004](adr/0004-core-as-dll.md)）。PCL 是 vcpkg 动态三元组，
  几十个 DLL 手工链是死路；顺带把 M3 的热重载降级成「drop 再建一个」。
- PCL 关在 `src/ops/pcl/` 里，`include/` 下零 PCL 头（[ADR-0005](adr/0005-pcl-boundary.md)）。
  代价是进出各拷一次 xyz，换来「加一个手写算子」仍然是秒级反馈。
- 结果留在 C++ 的**内容寻址结果仓**，点云走二进制 IPC
  （[ADR-0006](adr/0006-result-store-binary-ipc.md)）。M3 开缓存复用只是
  「不再删 + 加 LRU 预算」，键的定义不用动。

**这轮验证抓到的两个真 bug**（同样不是单元测试能发现的）：

1. 3D 视图切换节点时，上一次取点云的请求被作废，而 `setLoading(false)` 写在
   `if (!cancelled)` 里面 —— 于是切到一个**没有点云输出**的节点（不发新请求）
   就永远停在「正在取点云…」。修法是 `finally` 里无条件放下 loading。
2. 验收脚本连跑两次运行时，按下 F5 的那一刻 `runStatus` 还是上一次的 `ok`，
   只等「不是 running」会立刻返回上一次的快照。症状极具迷惑性：
   画布上明明是红的，断言拿到的却是 `done`。修法是先记下 `runId` 再等它变。

全绿之后又做了一轮独立代码审查，改掉 17 处缺陷 —— 全是**静默出错**那一类：
体素栅格把相距十公里的点折叠进同一个体素（下标截成 21 位，绕回来了）、
3D 视图对同一片云发起一串重复的 IPC、畸形 PCD 读到缓冲区外面、
`Run::work()` 没有顶层 try/catch（异常直接 terminate，而 `c_api.h` 承诺过
「异常绝不跨 ABI」）、`unsafe impl Sync` 的理由是假的。逐条见
[m2-acceptance.md](m2-acceptance.md) 的「代码审查抓到的缺陷」。
教训是：全绿只说明测到的路径是对的，说明不了没测到的路径。

**已知毛刺**：`pnpm check` 会把 core 编两遍（`build-core.ps1` 一遍产出两个 exe 与测试，
`bridge/build.rs` 一遍产出 DLL），两个构建树各自增量，冷启动时要多等一次。
合并成一个需要让两边传完全相同的编译选项，而 cmake crate 会强行注入自己的一套 ——
为此把两边焊死不划算。

## M3 — 能用 ✅

**目标：自己愿意天天用它，而不是回去改代码。** 详细计划见 [m3-plan.md](m3-plan.md)，
验收见 [m3-acceptance.md](m3-acceptance.md)。

核心轨（C++/Rust）：
- [x] 缓存复用 + `skipped`；`lyflow_plan` 给前端精确 stale 与「将重算 N 个节点」
- [x] 并行执行（依赖计数驱动，`ExecContext::threadBudget()` 供算子内部分配线程）
- [x] bypass 执行语义 + Reroute + `Any` 类型推导
- [x] 算子迁移表与 aliases，迁移以诊断形式回写 GraphDoc（headless 同路径）
- [x] 算子热重载（运行时重新加载 core DLL）
- [x] 参数联动 `visibleWhen` / `enabledWhen`

编辑轨（前端）：
- [x] 交互清单 P1 全部（#17–#30）
- [x] 快捷键单表驱动 + 面板；自动布局；最近文件 / 备份恢复；日志与诊断抽屉
- [x] 3D 视图着色模式 / 钉住 / 导出

**三份新契约**：[ADR-0007](adr/0007-cache-authority.md)（缓存判定只在 C++）、
[ADR-0008](adr/0008-migration-as-diagnostic.md)（迁移走诊断通道）、
[ADR-0009](adr/0009-hot-reload-by-copy.md)（热重载靠复制 DLL）。
C ABI 升到 v4：加了 `lyflow_plan` / `lyflow_cache_clear` / `lyflow_cache_stats`，
`lyflow_run_options` 多了 `max_parallel` 与 `cache_budget_bytes`。

**这一轮 CDP 抓到的真 bug**：Shift+F5 被 F5 抢先匹配（键表的 Shift 判定太松）、
重连端点被上游节点盖住时按下去变成拖节点、缓存复用让 M2 的「上游仍然 done」断言
变成 skipped、`onReconnectEnd` 的回调签名各版本不一致（改成自己记一笔而不是猜参数）。
逐条见 [m3-acceptance.md](m3-acceptance.md)。

**已知毛刺**：
- 结果仓不再随 run 结束而缩小，常驻内存约等于 LRU 预算（默认 min(8 GB, 物理内存 40%)）。
  状态栏显示占用，抽屉里可以清空 —— 但**没有**按图/按节点的细粒度淘汰。
- 热重载会清空缓存并取消正在跑的 run。这是 E4 定死的取舍，不是遗漏。
- `filter.random_sample` 升到了 2.0.0（`count`/`ratio` → `keepCount`/`keepRatio`）。
  M2 以前存的图打开时会提示迁移一次，保存后不再提示。
- 对齐参考线只画「与另一个节点的边缘/中心对齐」，不画等距分布线。
- 「一位未参与开发者 10 分钟完成任务」与「连续使用一天」两条人工验收**未执行**。

产出：可以交给同组其他人用的版本。

## M4 — 能扩展 ✅

**目标：图成为可复用、可脚本化、可交互探索的资产。** 详细计划见 [m4-plan.md](m4-plan.md)，
验收见 [m4-acceptance.md](m4-acceptance.md)。

- [x] 子图 / 复合算子：C++ compile 期展开成平图，路径式节点 id，参数提升，库算子目录
- [x] Live preview：源头抽稀的 preview run，独立缓存命名空间
- [x] headless CLI `lyflow`：run / validate / plan / migrate / manifest / dump / sweep / diff，JSON Lines 事件流
- [x] 参数扫描（P2 #34）、图 diff（#36）、大图性能（#37）；分组框（#32）以子图取代

产出：一条真实任务能用「库算子 + CLI」跑在无 GUI 的机器上。

**三份新契约**：[ADR-0010](adr/0010-subgraph-by-expansion.md)（子图靠编译期展开）、
[ADR-0011](adr/0011-preview-as-decimated-run.md)（preview 是抽稀过的普通 run）、
[ADR-0012](adr/0012-headless-cli.md)（CLI 是第二个 bin，JSON Lines）。
C ABI 升到 v5：加了 `lyflow_set_library_dirs` / `lyflow_library_count` / `lyflow_output_save`，
`lyflow_run_options` 多了 `mode` / `preview_max_points` / `preview_budget_ms`，
`lyflow_cloud_view` 多了 normals 通道。

**这一轮抓到的真 bug**：`ofKind()` 返回的临时 vector 在 range-for 里活不过
初始化那一句（C++17 的经典坑，症状是断言看到一个空列表而事件本身是对的）、
`RunHandle` 一 drop 结果仓的索引就没了导致 `dump` 拿不到刚跑完的结果、
两个 bin 让 `cargo run` 不知道该跑哪个（`tauri dev` 直接 101 退出）、
**`tauri build` 按 cargo 包名找 exe 再改名，把 CLI 盖到了桌面壳头上**（只在打包路径上暴露）、
事件合并窗口把 `run_started` 播下的 idle 占位吃掉了、
把 float 绑到 vec3f 的内参上产生的是 `bad_param` 而不是显式的类型错误。
逐条见 [m4-acceptance.md](m4-acceptance.md)。

**已知毛刺**：
- 子图内部节点的诊断挂在路径 id 上，顶层只看得到「这个子图红了」，
  要进去才知道是哪个内参（ADR-0010 的代价一）。
- 库算子不能「展开为内联子图」：定义在库文件里，前端手上只有合成出来的 OperatorDesc。
  右键那一项会明说这一点。
- `save_as_library` 拒绝嵌套了 `sub:` 的子图 —— 库文件必须自包含。
- preview 只跑到选中节点。没选中节点时拖参数不会触发预览。
- 300 节点的基准是合成图（30 条链 × 10 个 reroute），不是真实 pipeline。

## M4 之后 — core 零第三方依赖

内置的 14 个点云算子整体搬进仓库内的标准算子包 `packs/std-pointcloud/`，
走的是和 gap 包同一套 `lyflow_op_pack` 机制。core 只剩 `gen.synthetic` 与
`util.reroute`，不再 `find_package(PCL)`。用户视角零变化：默认构建导出的 manifest
与拆包前逐字节相同，只多了每个包内算子的一个 `pack` 字段。
见 [ADR-0014](adr/0014-std-as-pack-core-zero-dep.md) 与
[std-pack-acceptance.md](std-pack-acceptance.md)。

顺带放开了两条 ADR-0013 的限制：**每包一个 PCH**（拆包前全局只能有一份，
标准包和 gap 包没法共存）、**包名可以带连字符**（进命名空间前过一遍
`MAKE_C_IDENTIFIER`）。

**已知毛刺**：
- **默认构建里，改一个 core 头文件仍然会重编包的 14 个 TU**（它们 include
  `lyflow/registry.h` → `manifest.h` → `status.h`）。零依赖买到的是「可以不装 PCL
  地开发 core」（`LYFLOW_STD_PACKS=0` 那条路），不是「装了 PCL 也不重编」。
  后者要一层稳定的算子 ABI，那正是 ADR-0013 排除掉的方案。
- 注册顺序被 manifest 的字节兼容性钉住了：`registerBuiltinOps` 是
  `gen.synthetic → 标准包 → util.reroute → 外部包` 的夹心。
  「包永远排在内置之后」这句话不再成立。
- 包用 `file(COPY ...)` 往 `bin/` 里放的 DLL（gap 包的 `onnxruntime.dll`）
  不会因为下次不带包构建就消失。换模式打包前要自己删。
- `lyflow_output_save` / CLI `lyflow dump` 依赖标准包装进来的写盘钩子。
  纯平台构建里它们返回 `unsupported` —— 这是设计，但错误信息是运行期才看到的。

## M5 — 外延（只列方向，动工前再写计划）

- 第二种数据域 **Image**：`Data::Kind::Image`、2D 视图、OpenCV 算子按 PCL 同样的边界规则接入。
  这是对「数据模型是否通用」的真正检验，也是项目名里「Vision Flow」的兑现
- 第三方算子插件 DLL：`lyflow_plugin_init(Registry*)`，同工具链约束
- 缓存落盘（`externalKey` 机制已留口子）
- 两节点输出并排对比（P2 #35）

不计划：协作 / 多人编辑（P2 #38）。

## 已从后续里程碑提前到 M2 的接口决定

这些在 M2 就按终态定下，后面只填功能不改形态（详见 m2-plan.md §0）：
运行时加载的 core DLL（→ M3 热重载 ✅）、cacheKey 内容寻址的结果仓（→ M3 缓存 ✅）、
`run_started.nodes[].cacheKey`（→ M3 stale ✅）、GraphDoc `bypass` 字段（→ M3 静音 ✅）、
Plan 的 level（→ M3 并行 ✅）、`targets`（→ M3 Run to node ✅、M4 CLI `--to`）、
抢占式 run（→ M4 live preview ✅）、`externalKey`（→ 缓存落盘）、
参数右键的「复制路径名」（→ M4 CLI 的 `--set` ✅）、
GraphDoc 的 `subgraphs` 字段与 `op` 命名空间（→ M4 子图 ✅）。

## 优先级判断依据

遇到"先做 A 还是先做 B"时，按这个顺序问：

1. 它是否阻塞了「加一个算子」的成本？→ 优先。这个成本会被乘上未来所有算子的数量。
2. 它是否阻塞了端到端演示？→ 优先。没有 M2 之前，所有前端打磨都是在赌。
3. 它是否在交互清单的 P0？→ 优先。
4. 其余按被卡住的实际频率排。
