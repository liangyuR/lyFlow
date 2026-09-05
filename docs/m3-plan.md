# M3 实施计划 —— 能用

目标不变：**自己愿意天天用它，而不是回去改代码。** 交付标准是能交给同组其他人。

前提：M2 按 [m2-plan.md](m2-plan.md) 落地，即 core 是运行时加载的 DLL、结果仓已按 cacheKey 内容寻址、
`run_started` 已带每节点 cacheKey、GraphDoc 已有 `bypass` 字段。M3 不改任何 M2 接口的形态，只往里填功能。

M3 分两条并行的轨：**核心轨**（C++/Rust，让执行变快变省）和**编辑轨**（前端，交互清单 P1）。
两轨只在 stale 标记和 bypass/reroute 两处交汇。

---

## 0. 定死的决定

| # | 决定 | 一句话理由 |
|---|---|---|
| E1 | 缓存复用的判定**只在 C++**；前端的 stale 标记来自 `lyflow_plan` 返回的 cacheKey 对比，不自己推 | 前端算不出 IO 算子的 externalKey，自己推一定会错 |
| E2 | 并行执行以 **Plan 的依赖计数**驱动，不用 level 同步屏障 | 屏障让一层里最慢的节点拖住全部；依赖计数天然是最优调度 |
| E3 | 迁移在 C++ 的 validate 阶段产出**迁移诊断**，由前端以语义化动作写回 GraphDoc | C++ 不拥有文档；前端不拥有迁移逻辑（headless 走不到）；诊断是两者之间唯一干净的通道 |
| E4 | 热重载 = Rust 丢掉 `Core` 函数表再加载一份 **复制出来的** DLL；缓存随之清空，接受 | Windows 锁住已加载的 DLL 文件，不复制 CMake 就写不回去；缓存里的 Data 是 DLL 里的对象，跨代不可持有 |
| E5 | bypass 进 GraphDoc 节点字段，reroute 是一个 `Any` 类型的普通算子 | bypass 改变执行结果所以是文档语义；reroute 不需要特殊节点类型，`Any` 已经存在 |
| E6 | `Any` 端口的实际类型由**连线推导**，C++ validate 和前端 typecheck 各实现一份同样的定点迭代 | 两边本来就各有一份类型检查（architecture.md 的「体验 vs 正确性」），不新增分工 |
| E7 | 快捷键只有**一张表**，处理器和快捷键面板都从它生成 | 两处维护必然漂移 |
| E8 | 自动布局用 `@dagrejs/dagre`，只在「文档缺 ui」和用户主动点「整理」时触发 | 脚本生成的图必须能打开（graph-doc.md 的承诺），但布局永远不自动覆盖用户摆好的位置 |

E1、E3、E4 各写一份 ADR（0007 cache-authority、0008 migration-as-diagnostic、0009 hot-reload-by-copy）。

---

## 1. 核心轨

### 1.1 缓存复用

- 结果仓：`run_free` 不再顺带删 Data；加 **LRU 字节预算**（默认 min(8 GB, 物理内存 40%)，run 选项可覆盖），正在运行的 run 引用的键 pin 住。
- 执行器：节点 cacheKey 命中 → 直接把仓里的 `shared_ptr<const T>` 挂到输出，发 `node_state: skipped` + stats，不调 compute。
- C ABI 新增：
  ```c
  char* lyflow_plan(const char* graph_json, const char* base_dir, const char* const* targets, size_t n);
        /* 每节点 { nodeId, cacheKey, cached, level, upstreamMissing } 的 JSON 数组；校验失败返回诊断 */
  void  lyflow_cache_clear(void);
  char* lyflow_cache_stats(void);      /* { entries, bytes, budgetBytes, hits, misses } */
  ```
- Rust：`plan_graph` / `clear_cache` / `cache_stats` command。
- 前端：doc 每次 change 后 debounce 150 ms 调 `plan_graph`，与上次 `run_started.nodes` 的 cacheKey 对比：
  变了且 `cached=false` → 节点标 **stale**（虚线框，P1 #23）；Toolbar 显示「将重算 N 个节点」。
  状态栏显示缓存占用，菜单里「清空缓存」。

**验收**：原图重跑全部 `skipped`，run 总耗时 < 50 ms；改中间节点参数只重算其下游；`plan_graph` 的预测集合与实际 `skipped` 集合完全一致。

### 1.2 并行执行

- Plan 每节点记入度；线程池大小 `maxParallel`（run 选项，默认 `min(4, cores)`），就绪队列按 level 再按节点在拓扑序中的位置排。
- `EventSink` 加锁，`seq` 仍全局单调。取消：所有 worker 检查同一个 `atomic<bool>`。
- PCL 内部 OpenMP 线程数设为 `cores / maxParallel`，避免超订。
- 结果仓 pin 逻辑对并发安全。

**验收**：菱形图（1 源 → 4 支 → 1 汇）每支 sleep 200 ms 的测试算子，墙钟 < 500 ms；事件 `seq` 无重复无空洞；随机 cancel 100 次无死锁无泄漏。

### 1.3 bypass 与 reroute

- 执行器：`bypass=true` 的节点不调 compute。每个输出端口取**第一个类型兼容且已连线的输入**直接透传；找不到 → 该输出为空，下游报 `missing_input`，code `bypassed_no_source`。事件里 `state: skipped`，stats 带 `bypassed: true`（schema 加字段）。
- 前端：节点右键「静音」，Ctrl+M 切换；bypass 节点整体半透明加斜纹。这是 change 动作，进撤销栈。
- `util.reroute`：`Any → Any` 的算子，compute 就是透传。
- `Any` 推导：validate 里把已连边的具体类型沿 `Any` 端口传播到定点；仍为 `Any` 的端口不报错。前端 `typecheck.ts` 同样实现，用于 reroute 的端口着色和连线拒绝。

**验收**：bypass 一个体素节点，下游拿到的是原始点数；reroute 串两级后端口颜色随源变化，接不兼容类型被拒。

### 1.4 迁移与别名

- `OperatorDesc` 加 `migrations: vector<{ int fromMajor; MigrateFn }>`，`MigrateFn = json(const json& params)`；`Registry::validate` 检查链条覆盖 `1..currentMajor-1` 无断档。
- validate 阶段：`aliases` 命中 → 重定向；`opVersion.major < current` → 依次套用迁移；产出诊断
  `{ kind: "migration", nodeId, op(新), opVersion(新), params(新), notes[] }`。执行器在内存里用迁移后的值继续跑。
- 次版本变化且默认值有差 → `log warn`「保存于 v1.1.0，`leafSize` 默认值已变更」（graph-doc.md 承诺的提示）。
- Rust `load_graph` 返回 `{ doc, migrations }`（内部先调 `lyflow_validate`）。
- 前端：打开时把 `migrations` 通过 `applyMigrations` 动作写进 doc（一条撤销记录、置 dirty），toast「已迁移 3 个节点」；运行时的迁移诊断同样处理。

**验收**：fixture 里一份 `filter.voxel_grid@1.0.0` 的图，把算子改到 2.0.0 并注册迁移，打开后参数已换名、可撤销、保存后再打开不再提示。

### 1.5 热重载（开发期）

- Rust `CoreWatcher`（`notify` crate）监视 `build/core/bin/lyflow_core.dll`。变化后：取消活跃 run → drop 所有 `RunHandle` 和 `Core` → 把新 DLL 复制成 `lyflow_core.gen<N>.dll` 放到 exe 目录 → `libloading` 加载 → 自检 → 取 manifest → emit `manifest-updated`。自检失败则保留旧代并 emit `core-reload-failed` 附问题列表。
- 前端：收到 `manifest-updated` 替换 manifest store，当前 doc 不动；对 doc 重新 `validate_graph`，算子消失的节点显示「算子缺失」态（保留在 doc 里，可删可等）。
- `scripts/core-watch.ps1`：监视 `core/` 变更后 `cmake --build`。`pnpm dev` 同时起它。
- 依赖 DLL（PCL 等）不参与热重载。

**验收**：`pnpm dev` 下新增一个算子 cpp 并加注册行，5 s 内节点面板出现新算子，画布上的图与撤销栈原样保留。

### 1.6 参数联动

`visibleWhen` / `enabledWhen` 在 `params.ts` 求值，`ParamControls` 隐藏/禁用。C++ validate 对隐藏参数仍校验形态但**不校验必填**。

---

## 2. 编辑轨（交互清单 P1）

按实现共用的基础设施分组，而不是按清单序号。

### 2.1 连线手感（#17 #19 #20 #21）

- 吸附：`connectionRadius` 调到 24 px，落点放大保留。
- 拖离输入端：`edgesReconnectable` + `onReconnect`；拖到空白处松手 → 断开并弹搜索面板（复用 #18 通路）。
- 拖线中的兼容性可视化：`ui store` 记 `pendingFrom`，每个端口按 `canConnect` 结果加 `compatible / incompatible` class。
- 拖节点到连线上：拖动结束时对所选单节点做边命中测试（距离 < 12 px），有且仅有一对兼容端口时插入；否则不动。

### 2.2 布局（#22 + 自动布局）

- 网格吸附 `snapGrid=[8,8]`，Shift 临时关闭。
- 对齐参考线：拖动中与其他节点边缘/中心差 < 4 px 时画线并吸附，自绘 overlay。
- 自动布局：dagre LR 分层；文档缺 `ui.position` 时打开即布局；菜单「整理布局」对选中子集或整图。

### 2.3 节点操作（#24 #25 #26）

- reroute 见 1.3；双击连线中点插入 reroute。
- 折叠（只显示标题与已连端口）、重命名（双击标题）、bypass 见 1.3。
- 参数右键：重置默认 / 复制值 / 粘贴值 / 复制路径名（给 CLI `--set` 用）。

### 2.4 输入与快捷键（#28 #29）

- 数字框拖动改值：水平拖 = step，Shift ×10，Alt ÷10；拖动期间走 `beginTransaction`，松手一条撤销记录（M1 的事务机制已备）。
- 快捷键表 `lib/keymap.ts`：`{ id, keys, label, scope, when }`；`useShortcuts` 和 `?` 面板都从它渲染。默认表：F5 运行、Shift+F5 到此节点、Esc 取消、Ctrl+M 静音、Ctrl+G 整理、Ctrl+Shift+F 适配视图、Ctrl+Z/Y、Del、Ctrl+C/V/X/D、Tab/Space 搜索、`?` 面板。

### 2.5 面板与文件

- 底部抽屉：日志（M2 store 已有）、诊断列表（点击定位到节点/参数）、缓存统计。
- 最近文件（最多 10 个，存 Tauri app data）、关闭/打开时未保存提示、每 30 s 写 `<file>~` 备份、启动发现备份比正文新时提示恢复。
- 窗口标题 `文件名 *`。

### 2.6 3D 视图补齐

- 着色：intensity / 高度 / 法线 / 单色；色带与范围可调。
- 视图钉住：钉住某节点后选中其他节点不切换。
- 导出当前视图 PNG。

---

## 3. 实现顺序

核心轨与编辑轨可以交错，但各自内部按下面顺序。每步 `pnpm check` 绿。

核心轨：1.1 缓存 → 1.2 并行 → 1.3 bypass/reroute → 1.4 迁移 → 1.5 热重载 → 1.6 联动。
编辑轨：2.4 快捷键表（其他项都要挂键）→ 2.1 连线 → 2.3 节点操作 → 2.2 布局 → 2.5 面板文件 → 2.6 视图。

---

## 4. 验收（M3 完成的定义）

- [ ] 1.1–1.6 各自的验收全部自动化（core 测试 / cargo test / CDP）
- [ ] CDP：P1 #17–#30 每项至少一条断言
- [ ] 一位没参与开发的同事，拿到一句话任务「把这个 pcd 降采样后去平面，看结果」，不讲解，10 分钟内完成；卡住的点全部记入交互清单
- [ ] 连续使用一天（真实数据、真实任务）没有回去改代码；改了就说明缺算子或缺交互，记录并补

---

## 5. 明确不做

- 子图、live preview、CLI（M4）
- 缓存落盘（跨进程持久化）。结果仓是内存的；需要时 M4 之后再议，externalKey 机制已为它留好口子
- 跨节点的输出对比视图（P2 #35）
