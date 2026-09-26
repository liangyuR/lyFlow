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
2. 发 `cargo:rustc-env=LYFLOW_CORE_BIN=<build>/bin`，并把该目录里的
   `.dll/.exe/.pdb` 拷到 `target/<profile>/` 和它的 `deps/`，给 `tauri build`
   与打包用。
3. 给 exe 贴 `lyflow-app.manifest`（D9：`activeCodePage=UTF-8`）。

**不发任何 `rustc-link-lib`。** DLL 在运行时才加载：**开发构建**
（`debug_assertions`）从**本配置自己的** `LYFLOW_CORE_BIN` 加载，其余情况从 exe
同目录（不搜 PATH，也不看当前工作目录 —— 前者会加载到无关的同名 DLL，
后者在双击启动时根本不是安装目录）。

为什么开发构建不用 exe 同目录：cargo 会按 feature 集把同一个 crate 建好几遍
（`lyflow-app` 带 `desktop`、lib 的 test、CLI `--no-default-features`），
每一份有自己的 `OUT_DIR` 和自己的 `lyflow_core.dll`，而 `target/<profile>/`
是**共用**的 —— 谁最后跑过 `build.rs` 谁的 DLL 就留在那里。带 `LYFLOW_OP_PACKS`
构建的 app 因此会加载到不带包那次留下的 DLL，表现是算子凭空少一半。
拷贝是 `build.rs` 的副作用，只在这个配置的 `build.rs` 重跑时才发生，
所以「重新构建一次」并不能纠正它。

Windows 上用 `LOAD_WITH_ALTERED_SEARCH_PATH` 加载：PCL、`yaml-cpp` 这些依赖
DLL 跟着从**核心 DLL 自己的目录**解析，而不是 exe 目录。

新增 `core/src/**/*.cpp` 会被自动编进来：`build.rs` 递归 glob 源文件与目录，
并对每一项发 `cargo:rerun-if-changed`，所以加文件会触发重编。

### 需要什么

CMake + Ninja + vcpkg（PCL）。Ninja 通常不在 PATH 上，`build.rs` 会依次尝试
`LYFLOW_NINJA` 环境变量 → PATH → VS 自带的那份（cmake.exe 旁边的 `../Ninja/`）
→ vswhere。都找不到就退回 CMake 默认生成器，慢但能用。

### 三个踩过的坑

- **别 `canonicalize()` core 的路径。** Windows 上它返回 `\\?\D:\...`，
  CMake 的 `file(GLOB)` 在这种扩展长度路径下一个文件都匹配不到，
  最后报的是「No SOURCES given to target」，离真正的原因十万八千里。
- **`WindowsAttributes::app_manifest` 是整份替换，不是合并**
  （tauri-build 2.6.3 的 `res.set_manifest`）。所以 `lyflow-app.manifest` 里
  必须自带 Tauri 原来的 Common-Controls v6 依赖，否则文件对话框会掉回旧样式，
  而且不报错。
- **窗口必须 `"dragDropEnabled": false`**（`tauri.conf.json`）。缺省是开的：Tauri 在 Windows 上给窗口
  注册自己的拖放目标，WebView2 里的 HTML5 拖放整个被吞掉 —— 算子面板拖不进画布，也不报错。e2e 的拖入用的是
  合成的 DragEvent，看不见这一层；`m8b.mjs` 里有一条断言盯着这个配置。app 自己不收系统文件拖放，关掉没有代价。

## 暴露给前端的 command

| command | 方向 | 说明 |
|---|---|---|
| `get_manifest` | C++ → 前端 | 全量算子描述。按**代数**缓存解析结果，热重载后自动作废。 |
| `get_core_info` | C++ → 前端 | core 版本、算子/类型数量、热重载代数，给状态栏用。 |
| `save_graph` | 前端 → 磁盘 | 结构校验后写盘，pretty JSON。 |
| `load_graph` | 磁盘 → 前端 | 读盘 + 结构校验，返回 `{ doc, migrations }`（ADR-0008）。 |
| `validate_graph` | 前端 → C++ | 权威校验，返回**全部**诊断（D5）。 |
| `plan_graph` | 前端 → C++ | 编译但不执行，报每节点的 cacheKey 与是否已缓存（ADR-0007）。 |
| `clear_cache` / `cache_stats` | 前端 → C++ | 结果仓的清空与统计，给菜单和状态栏用。`clear_cache` 是**进程级**的，别的 run 的结果也会没；只想让一次运行不吃缓存用 `no_reuse`（CLI `--no-cache`）。 |
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
还有 `isolate: string[] | null`（C ABI v11，[docs/node-run-plan.md](../docs/node-run-plan.md)）：
只重算这几个节点，上游只取缓存，缺结果时 core 在开跑前以 `upstream_not_ready` 整次失败。
以及 `force: string[] | null`（修订一 V1）：这些节点跳过缓存强制重算。两者都经 `RunManager::start` 的
`StartOptions` 原样交给 `RunSpec::isolate` / `RunSpec::force`，抢占规则不变。

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

### 二进制张量与下标（v8）

[ADR-0019](../docs/adr/0019-output-tensor-and-indices-over-abi.md)。布局（小端），
逐字节的表在 [docs/http-transport.md](../docs/http-transport.md)：

```
u32 magic 'LYTN' | u32 rank | u32 flags | u32 count
u64 offset | u64 total | i64 shape[rank] | f32 data[count]

u32 magic 'LYIX' | u32 count | u32 total | u32 flags
u64 sourceCloudId | i32 values[count]
```

`shape` 永远是**完整**形状，不随 `offset/count` 变 —— 前端要靠它算下一片在哪。
两处的头长度（32 与 24）都让数组落在自然对齐上，前端直接开
`BigInt64Array` / `Float32Array` / `Int32Array` 视图。

切片是这两条路控制数据量的唯一手段：张量不抽稀（抽稀过的图像是**另一张图**）。
`count` 在 command 里被 clamp 到 4 194 304（16 MB），**`count = 0`（取到末尾）同样被
clamp**，否则一个一亿元素的张量会一次性 400 MB 过 IPC。上限是「一次 IPC 该多大」
的产品判断，属于桥接层，core 自己不设限。

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

## 顶层图参数（`--param`）

`run` / `validate` / `plan` / `params` / `eval` / `patch` 都认 `--param <名字>=<json>`，
给图顶层 `params` 里声明的参数取值（[graph-doc.md](../docs/graph-doc.md)「顶层图参数」）。
值先按 JSON 解析，解析不了当字符串，与 `--set` 同一条规则；与 C ABI 的 `params_json` 结果一致。

```bash
lyflow run g.lyflow.json --param gapOffset=0.12 --param modelPath=D:/models/v12s0.onnx --summary
```

- 图没声明的名字 → `unknown_param`，退出码 4。
- `--set` 命中被顶层参数绑定的节点参数 → 报错，退出码 4，提示改用 `--param`。
- `lyflow params` 里这类参数的 `source` 是 `graph`，另带 `graphParam: "<名字>"`。
- `eval` 另有 `--param <node>.<param>=<start>:<end>:<steps>` 扫描轴：按 `=` 左边**是否含 `.`**
  区分，含 `.` 是扫描轴，不含是顶层参数。

## 配方（`--recipe` 与 `lyflow recipes`，param-recipe P4）

配方是顶层图参数的一组取值，存在图旁的 `<图名>.recipes/<名字>.lyflow-recipe.json`（格式与四类失配见
[docs/recipe.md](../docs/recipe.md)）。实现在 `src/recipe.rs`：读文件、目录约定、specDigest、四类失配与修复建议 ——
编辑器的 `packages/editor/src/lib/recipes.ts` 是另一份，两边对着 `schema/fixtures/recipes/` 的同一组夹具断言（摘要逐字节、
每一条的类别 / 参数 / 建议 / 文案），`cargo test recipe` 就是这组断言。数字与 JSON 文本按 ECMAScript 的格式写（`js_number` /
`js_json`），SHA-256 用 `sha2`。

```bash
lyflow run      g.lyflow.json --recipe g.recipes/车型A.lyflow-recipe.json --param cutMax=2 --summary
lyflow validate g.lyflow.json --recipe g.recipes/坏.lyflow-recipe.json        # 失配 → 退出码 4，stderr 逐条列出
lyflow recipes  g.lyflow.json --json                                          # 列目录：值个数、失配、默认配方
```

- `run` / `validate` / `plan` / `params` / `eval` / `patch` 都认（`dump` / `sweep` / `perturb` 经同一个 `load_graph`，也认；
  `migrate` 拒绝）。叠加顺序：基础 → `--recipe` → `--param`；配方的值与 `--param` 一样写成图参数的 `default` 再交给 core。
- `eval`：配方作用于所有样本；`--params` 的参数组里不含 `.` 的键写图参数，夹在配方与 `--param` 之间。
- `patch --recipe`：把配方的值写回基础（落盘），动作顺序 remove → add → rewire → set → recipe → param。
- 失配 ①–③：`recipe_mismatch:`，退出码 4，一个节点都不跑、什么都不写；④ 只在 stderr 提示。读不出配方是 `bad_recipe:`（4）。
- `lyflow recipes`：只读，不需要 core；给 `--recipe` 时每行带合成好的 `params`（MCP 的 `run_graph` 拿它交给后端）。

## 批量评估（`lyflow eval`）

`src/eval.rs`。一条命令把「一组样本 × 一组参数 → 任意标量指标 → 内建统计」跑完，
不用再写解析器、批跑器和统计脚本（[ADR-0020](../docs/adr/0020-eval-and-perturb-as-cli.md)）。

```
lyflow eval <graph> <样本集>
                    [--params <paramsets.json>] [--param <node>.<param>=<start>:<end>:<steps>]...
                    [--param <名字>=<json>]...
                    --metric <path> [--metric <path>]...
                    [--holdout <tag>=<value>] [--group-by <tag>]
                    [--csv <out.csv>] [--base-dir <dir>] [--parallel <n>] [--no-cache] [--set ...]

<样本集> 三选一（perturb 共用同一组）：
    --samples <samples.jsonl>
    --samples-glob <pat> --bind <node>.<param>
    --samples-dir <root> --bind-pair <node>.<pA>,<node>.<pB> --pattern <globA>,<globB>
                  [--sample-subdir <name>] [--sort-by name|mtime] [--split-half <tagKey>]
另有 [--samples-jsonl-out <path>]：把生成的样本集写出来，可核对可复用
```

```bash
# 51 帧同一张图，指标是图级命名输出 gap，后半段留出
lyflow eval 4.lyflow.json --samples kun10-p4.jsonl --metric outputs.gap --holdout half=b

# 扫两个 distThresh，顺便看看 bundle 里的点数
lyflow eval 4.lyflow.json --samples kun10-p4.jsonl \
  --param n_fit_l.distThresh=0.2:0.8:4 --param n_fit_r.distThresh=0.2:0.8:4 \
  --metric outputs.gap --metric outputs.bundle.point_counts.input_primary --csv p4.csv

# glob 一步生成样本：每个文件一个样本，绝对路径写进 --bind 指的参数
lyflow eval load.lyflow.json --samples-glob "clouds/*.pcd" --bind r.path \
  --metric nodes.r.elementCount

# 目录模式：一帧一个目录，两个 glob 配成双相机一帧，按时间前后各半打 tag
lyflow eval 4.lyflow.json --samples-dir kun10/sensor --sample-subdir 4 \
  --bind-pair n_load.primaryFile,n_load.secondaryFile \
  --pattern "*Master*.pcd,*Slave*.pcd" --split-half half \
  --set n_load.source=files --metric outputs.gap --holdout half=b
```

- **指标是值路径**：`outputs.<名字>[.a.b]`、`nodes.<节点>.<端口>[.a.b]`、
  `nodes.<节点>.durationMs|elementCount|byteSize`、`run.durationMs`。
  Measurement 自动拆包（`outputs.gap` 直接是个数），Record 的 `data` 一层透明，bool 按 0/1。
  拼错是 `EXIT_USAGE`，stderr 会把这张图上所有可用的标量路径列出来 —— 第一次总会拼错。
- **样本**每行 `{ id, set: { "<node>.<param>": <json> }, tags? }`。`scene` 字段留给将来的注入，
  这一版遇到就报用法错。覆盖顺序：全局 `--set` → 参数组 → 样本的 `set`。
- **目录模式**（`--samples-dir`）：`<root>` 下每个直接子目录是一帧，样本 id 取帧目录名；
  `--sample-subdir` 再往下一层。每个 glob 在一帧里要**恰好匹配到一个**文件，
  0 个或多个是 `EXIT_USAGE` 并报出是哪一帧。`--sort-by name`（默认）先读帧目录名里的
  `dd-MM-yyyy-HH-mm-ss` 时间戳，有一帧读不出就整体退回字典序并在 stderr 说一句；
  `--split-half` 排序后前一半 `a`、后一半 `b`（奇数时前半多一个），配 `--holdout <key>=b`。
- stdout 每行一个 `eval_row`，末尾每个（参数组 × 指标）一行 `eval_summary`
  （`n / ok / failCodes / mean / std / min / max / p2p`；`std` 是样本标准差，`n<2` 给 `null`）。
- 样本之间**顺序跑**，`--parallel` 是传给 core 的节点并行度，与 `run` 同义。缓存默认开。
- `sweep` 现在是这套引擎上的一层壳，只负责轴展开与 `sweep_row` 的老形状；
  它的 `--metric nodeId:port.field` 老写法两个子命令都还认。

## 改图结构（`lyflow patch`）

`src/patch.rs`。四个动作、顺序定死、幂等、每步之后过形状校验，最后过 core 的 `validate`，
任一步不过就整体不写（[ADR-0023](../docs/adr/0023-patch-as-idempotent-structural-edit.md)）。

```
lyflow patch <graph> [--remove-node <id|glob>]... [--add-node <json>]... [--rewire <from>=<to>]...
                     [--set <node>.<param>=<json>]... [--param <名字>=<json>]...
                     [--dry-run] [-o <out>] [--json] [--base-dir <dir>]
```

```bash
# 先看差异：输出与 lyflow diff 逐字相同
lyflow patch 4.lyflow.json --remove-node 'b_*' --rewire n_fb_line:out=n_fit_base:line --dry-run

# 真写一份到别的路径，回执走 --json
lyflow patch 4.lyflow.json --rewire n_fb_line:out=n_fit_base:line -o short.lyflow.json --json
```

- **顺序是 remove → add → rewire → set**，与命令行上的先后无关 —— 同一组动作换个写法得到同一张图。
  「先改接线再删节点」要写成两条命令，中间那一步本来就该被看一眼。
- `--remove-node` 删节点连带它的所有边；glob（`*` / `?`，大小写不敏感）只对 id。
  **图级 `outputs` 还指着的节点不给删**：报错、整体不写、退出 1。
- `--add-node '{"id":…,"op":…,"params":…?,"ui":…?}'`：id 撞了报错；不认识的字段报错（拼错不静默）；
  没给 `ui` 就放在图里现有坐标的右下角外面一格。没有 `--connect`，所以新节点要么是不需要输入的源算子，
  要么配 `--rewire` 把已有的边挪过去。
- `--rewire n_fb_line:out=n_fit_base:line`：所有从左端口出发的边改为从右端口出发。
  只改边，不动图级 `outputs`（左端口上挂着图输出时 stderr 说一句）。两端的节点不存在是错，不是 no-op。
- `--set` 与 `run --set` 同义（解析不出 JSON 就当字符串）。
- **幂等**：删不存在的 id、左端口没有出边的 rewire、同值的 set 都是 no-op，走 stderr 与 `--json` 的
  `noops[]`。同一条命令跑两遍，第二遍 `applied` 全空、`diff.empty` 为 true；全是 no-op 的**原地**
  覆写干脆不落盘（免得白白动 mtime），给了 `-o` 就照写。
- `--dry-run` 不写文件，stdout 是 `lyflow diff` 的那份渲染（`cli::diff_docs` / `cli::render_diff`
  两个子命令共用一份实现）。`--json` 时同一份差异在 `patch_result.diff` 里。
- `-o` 省略时原地覆写。落盘先写同目录的临时文件再改名。
- `--json` 一行：

  ```jsonc
  {"kind":"patch_result",
   "applied":{"removed":["b_alt"],"added":[],"rewired":["a:cloud=g:cloud"],"set":["a.leafSize"]},
   "noops":[{"action":"set","spec":"a.leafSize=[0.05,0.05,0.05]","reason":"same_value","message":"…"}],
   "wrote":"g.lyflow.json",     // --dry-run 或「全 no-op 的原地覆写」时是 null
   "diff":{ /* 与 lyflow diff --json 同一个对象 */ }}
  ```

- 退出码：0 成功；1 = 动作对不上这张图（没有那个节点、id 撞了、图输出还指着被删节点）
  或者改完之后不合法（诊断照旧一行 JSON 打在 stdout）；4 = 用法错（一个动作都没给、写法不对）。

## 库算子目录

`commands::library_dirs` 给出扫描列表：app data 下的 `library/`，
外加环境变量 `LYFLOW_LIBRARY_DIRS`（分号分隔）里的额外目录。
`watcher::spawn_library` 盯着它们，`*.lyflow-op.json` 变了就重扫并推一条
`manifest-updated` —— 与热重载同一条通路，前端零改动。

重扫会重建注册表，所以必须先 `RunManager::stop_active()`：正在跑的 run 握着 `OperatorDesc` 指针。
上一次跑完的那个**留着** —— 重扫不换 DLL，它在结果仓里的数据照样有效；以前这里用的是 `drop_all()`，
存库后 400 ms watcher 再扫一遍时放掉了刚跑完的 run，界面显示「完成」、按它的 runId 却取不到输出。
`drop_all()` 只给热重载用（换 DLL，要连结果仓一起清空，ADR-0009）。
watcher 扫之前先比一遍库文件的（路径, 大小, 修改时间）：存库、手动重扫已经扫过的那次写盘不再扫第二遍。

## 启动自检

`run()` 第一件事是调 `lyflow_manifest_problems()`，非空就打印并 `exit(1)`。
DLL 加载不上也走这条路，错误信息里会写清楚 `lyflow_core.dll` 该在哪 ——
白屏加一句「加载失败」是最难排查的形态。

契约破了继续跑没有意义：前端会拿到一份自相矛盾的 manifest，然后以各种
离奇的方式失败。算子描述写错是开发期的事，用户不该看到这条路径。
