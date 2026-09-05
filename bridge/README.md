# bridge —— Rust 桥接层

Tauri 桌面壳。职责：IPC、序列化、文件读写、进程生命周期、事件推流、崩溃隔离。

**不理解算子语义，不改写图结构。** 见 [docs/architecture.md](../docs/architecture.md)。

## C++ 怎么进来的

M2 起改成了 **CMake 构建的 DLL + libloading 运行时加载**
（[ADR-0004](../docs/adr/0004-core-as-dll.md)）。M0/M1 那套「cc crate 直接编
`core/src/**/*.cpp` 成静态库」在接入 PCL 之后走不通了：PCL 是 vcpkg 的
`x64-windows` 动态三元组，一次 `find_package` 带进来十几个 import 库和二十多个
运行时 DLL，而且清单会随版本变 —— 在 build.rs 里手工复刻 vcpkg 的依赖解析
是死路。CMake 已经知道答案，让它回答。

`build.rs` 做三件事：

1. 用 cmake crate 驱动 `core/CMakeLists.txt`，只构建 `lyflow_core` 这一个目标
   （两个 exe 归 `scripts/build-core.ps1` 管）。固定 `RelWithDebInfo`（D8）。
2. 把 `<build>/bin/` 里的 `.dll/.exe/.pdb` 拷到 `target/<profile>/` 和它的
   `deps/` —— 前者给 `tauri dev`、`tauri build`，后者给 `cargo test`
   （测试可执行文件跑在 `deps/` 里，`LoadLibrary` 只看它自己那个目录）。
3. 给 exe 贴 `lyflow-app.manifest`（D9：`activeCodePage=UTF-8`）。

**不发任何 `rustc-link-lib`。** DLL 在运行时才加载，从 exe 同目录
（不搜 PATH，也不看当前工作目录 —— 前者会加载到无关的同名 DLL，
后者在双击启动时根本不是安装目录）。

新增 `core/src/**/*.cpp` 会被自动编进来：`build.rs` 递归 glob 源文件与目录，
并对每一项发 `cargo:rerun-if-changed`，所以加文件会触发重编。

### 需要什么

CMake + Ninja + vcpkg（PCL）。Ninja 通常不在 PATH 上，`build.rs` 会依次尝试
`LYFLOW_NINJA` 环境变量 → PATH → VS 自带的那份（cmake.exe 旁边的 `../Ninja/`）
→ vswhere。都找不到就退回 CMake 默认生成器，慢但能用。

### 两个踩过的坑

- **别 `canonicalize()` core 的路径。** Windows 上它返回 `\\?\D:\...`，
  CMake 的 `file(GLOB)` 在这种扩展长度路径下一个文件都匹配不到，
  最后报的是「No SOURCES given to target」，离真正的原因十万八千里。
- **`WindowsAttributes::app_manifest` 是整份替换，不是合并**
  （tauri-build 2.6.3 的 `res.set_manifest`）。所以 `lyflow-app.manifest` 里
  必须自带 Tauri 原来的 Common-Controls v6 依赖，否则文件对话框会掉回旧样式，
  而且不报错。

## 暴露给前端的 command

| command | 方向 | 说明 |
|---|---|---|
| `get_manifest` | C++ → 前端 | 全量算子描述。按**代数**缓存解析结果，热重载后自动作废。 |
| `get_core_info` | C++ → 前端 | core 版本、算子/类型数量、热重载代数，给状态栏用。 |
| `save_graph` | 前端 → 磁盘 | 结构校验后写盘，pretty JSON。 |
| `load_graph` | 磁盘 → 前端 | 读盘 + 结构校验，返回 `{ doc, migrations }`（ADR-0008）。 |
| `validate_graph` | 前端 → C++ | 权威校验，返回**全部**诊断（D5）。 |
| `plan_graph` | 前端 → C++ | 编译但不执行，报每节点的 cacheKey 与是否已缓存（ADR-0007）。 |
| `clear_cache` / `cache_stats` | 前端 → C++ | 结果仓的清空与统计，给菜单和状态栏用。 |
| `run_graph` | 前端 → C++ | 启动一次运行，立刻返回 runId。 |
| `cancel_run` | 前端 → C++ | 协作式取消。id 对不上就无操作。 |
| `get_output_info` | C++ → 前端 | 某节点全部输出的 type/elementCount。 |
| `get_output_cloud` | C++ → 前端 | **二进制**点云（见下）。 |
| `get_recent_files` / `push_recent_file` | 磁盘 ↔ 前端 | 最近文件，存在 Tauri 的 app data 里，最多 10 条。 |
| `write_backup` / `backup_status` / `read_backup` / `discard_backup` | 磁盘 ↔ 前端 | `<file>~` 自动备份与崩溃恢复。 |
| `write_file_bytes` | 前端 → 磁盘 | 写一段二进制到用户选的路径。3D 视图导出 PNG 走它。 |
| `get_library_status` / `refresh_library` | C++ ↔ 前端 | 库算子目录的状态与重扫（ADR-0010）。 |
| `save_as_library` | 前端 → 磁盘 | 把 doc 里的一个子图写成 `<id>.lyflow-op.json`。 |

`run_graph` 多了 `mode` / `previewMaxPoints` / `previewBudgetMs`
（[ADR-0011](../docs/adr/0011-preview-as-decimated-run.md)）。`mode: "preview"` 时
源算子的输出先抽稀，结果进独立的缓存命名空间。

三条事件流：`execution-event`（符合
[`schema/execution-event.schema.json`](../schema/execution-event.schema.json)）、
`manifest-updated` 与 `core-reload-failed`（热重载，见下）。

`load_graph` 返回的是 `{ doc, migrations }` 而不是裸 `doc`：**桥接层不改图**。
它只是替 C++ 把 `kind === "migration"` 的诊断挑出来交给前端，
写回 GraphDoc 是 `applyMigrations` 的事（[ADR-0008](../docs/adr/0008-migration-as-diagnostic.md)）。

## 热重载（开发期）

`watcher.rs` 盯着 `build/core/bin/lyflow_core.dll`（`scripts/core-watch.ps1` 的产物）。
变了就走一轮换代，**顺序不能变**（[ADR-0009](../docs/adr/0009-hot-reload-by-copy.md)）：

```
取消活跃 run → RunManager::drop_all() → 清空结果仓
  → 复制成 lyflow_core.gen<N>.dll → 加载 → 自检 → 换掉 Arc<Core>
```

- **必须先 `drop_all()`。** `RunHandle` 持有 `Arc<Core>`；不放掉的话旧 DLL 只是
  「被顶下去」而不是被卸载，两代同时活着、两个结果仓，症状会非常离奇。
- **必须复制。** Windows 锁住已加载的 DLL，不复制的话第一次热重载之后 CMake
  就再也构建不了了。旧代的 gen 文件删不掉是常态，清理放在下次启动。
- 自检（`manifest_problems` 为空 + manifest 可解析）不过就保留旧代，
  emit `core-reload-failed`。半坏的一代比旧的一代难查十倍。
- `watch_source()` 靠 `env!("CARGO_MANIFEST_DIR")` 推路径，安装包里不存在 →
  watcher 不启动，`get_core_info().hotReload` 是 false。

### 运行的生命周期（`execution.rs`）

D3：同一时刻一个活跃 run，**新 run 抢占旧 run**。抢占是同步的 ——
`run_graph` 返回新 runId 时，旧 run 的 `run_finished(cancelled)` 一定已经发出去了，
前端「丢弃过期 runId 的事件」那条规则才不会漏掉它。代价是旧 run 卡在不可取消的
PCL 算子里时，这个 command 会等它跑完。

内存上界也在这里：最多同时持有**两份** run 的结果（正在跑的 + 上一次完成的，
用户可能正在 3D 视图里看它）。第三个出现时最老的 `RunHandle` 被 drop，
`lyflow_run_free` 顺手把它在结果仓里的东西全删掉。

`RunHandle` 的 `Drop` 严格按 cancel → join → free 的顺序走，
最后才释放回调的 `user` 指针 —— C ABI 保证 join 返回后不再回调，
提前释放就是 use-after-free。

### 二进制点云

[ADR-0006](../docs/adr/0006-result-store-binary-ipc.md)。布局（小端）：

```
u32 magic 'LYPC' | u32 pointCount | u32 totalPoints | u32 flags
f32 bounds[6] | f32 xyz[3n] | [f32 intensity[n]] | [f32 normals[3n]]
```

可选通道按 `flags` 的位序依次排在坐标后面：bit0 = intensity，bit1 = normals。
前端按同样的顺序算偏移，所以加通道只要在两端各加一个位。

一百万点走 JSON 是 30MB 文本加一次全量解析；走这条是 12MB 字节加一次
`new Float32Array(buffer, offset, len)`。magic 不是装饰：IPC 上游出错时返回的
可能是一段错误文本，没有它前端会把那段文本当坐标画出来。

## 两个 bin

| bin | 说明 |
|---|---|
| `lyflow-app` | Tauri 桌面壳，需要 `desktop` feature（默认开）。`tauri.conf.json` 的 `mainBinaryName`。 |
| `lyflow` | headless CLI，**不依赖 Tauri**（[ADR-0012](../docs/adr/0012-headless-cli.md)）。 |

`cargo build --bin lyflow --no-default-features` 是这条边界唯一靠得住的证据，
它进了 `pnpm check`。实现在 `src/cli.rs`（lib 里）而不是 bin 里，
这样每个子命令的退出码与输出形状都能被 `cargo test` 覆盖。

包名那个 bin 名（`lyflow`）让给了 CLI，所以桌面壳改叫 `lyflow-app`。连带的两处：

- `default-run = "lyflow-app"` —— `tauri dev` 跑的是不带 `--bin` 的 `cargo run`，
  两个 bin 会让它报「could not determine which binary to run」。
- **cargo 包名也改成了 `lyflow-app`** —— `tauri build` 按**包名**去找刚编出来的 exe，
  再改名成 `mainBinaryName`。包名叫 `lyflow` 的话，它会把 CLI 改名盖到桌面壳头上，
  而且只在打包路径上暴露（`pnpm e2e` 走 `cargo run`，永远碰不到）。

## 库算子目录

`commands::library_dirs` 给出扫描列表：app data 下的 `library/`，
外加环境变量 `LYFLOW_LIBRARY_DIRS`（分号分隔）里的额外目录。
`watcher::spawn_library` 盯着它们，`*.lyflow-op.json` 变了就重扫并推一条
`manifest-updated` —— 与热重载同一条通路，前端零改动。

重扫会重建注册表，所以必须先 `RunManager::drop_all()`，理由与热重载完全一样。

## 启动自检

`run()` 第一件事是调 `lyflow_manifest_problems()`，非空就打印并 `exit(1)`。
DLL 加载不上也走这条路，错误信息里会写清楚 `lyflow_core.dll` 该在哪 ——
白屏加一句「加载失败」是最难排查的形态。

契约破了继续跑没有意义：前端会拿到一份自相矛盾的 manifest，然后以各种
离奇的方式失败。算子描述写错是开发期的事，用户不该看到这条路径。
