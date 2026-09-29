# 结果缓存落盘 —— 调研记录

> 状态：**调研记录；按方案 B 已实施**（2026-09-29，见 [disk-cache-plan.md](disk-cache-plan.md)）。C 档「结果缓存落盘」。
> 文件行号是写这份记录时的位置，之后可能漂移；以函数 / 字段名为准。

## 1. cacheKey 现在怎么算

`core/src/exec/plan.cpp` 的 cacheKey 计算（XXH3-128，`core/src/exec/hash.h`），依次喂进哈希：

- `cacheNamespace`：预览与正式分开（ADR-0011）
- `op->id`、`op->version`
- bypass 标记
- `canonicalParamsJson`（键排序）
- `providedDigest`：注入数据的哈希（`executor.cpp` 里算）
- `op->externalKey(ParamView(params, baseDir))`
- 上游按端口名排序，每个上游加 port、fromPort 与上游的 cacheKey
- 非确定性算子再加 `runId` —— 它们永远不命中

**没进键的**：算子实现的哈希 / DLL 或包的 build id（`OperatorDesc::pack` 里有「名字@版本」，没用上）；依赖库版本（PCL、onnxruntime）；键格式版本号。

`externalKey` 目前的实现都是文件的 `size:mtime`：`io_load_pcd.cpp`、ONNX 算子只对 `modelPath` 取文件戳（`ml_onnx_run.cpp`），gap / dts 包里另有 7 处（例如 `packs/gap/ops/blocks.cpp`）。`baseDir` 只用来解析路径，本身不进键。

## 2. ResultStore（`core/src/exec/result_store.h`）

- 两层：内容层 cacheKey → Data；索引层 runId → node → port。
- `put(replace)`、`reuse`（全部端口都在才算命中）、`peek`（只探测、不计数）、`freeRun`（只丢索引）。
- `evict` 跳过被 pin 的键；`setBudget` 默认 min(8 GB, 物理内存 40%)；`CachePin` RAII。
- 文档里对落盘的说法：
  - ADR-0006：`externalKey` 就是给「路径没变、文件被覆盖」留的钩子。
  - ADR-0007 的复议条件：做落盘时 `cached` 的判定要多一层「磁盘上有没有」，LRU 变两级；`evict` 不动预览命名空间。
  - m3-plan、m4-plan、roadmap 都把落盘列为「以后再议」。

## 3. 要序列化的数据类型（`core/include/lyflow/data.h`）

PointCloud（xyz / intensity / normals / rgb）、Indices、Transform、Plane、Box2D、Line2D、Circle2D、Point2D、
Measurement、Tensor、Record（本质是 JSON）、Bundle；Error 不该落盘。

**坑**：`PointCloud::id` 只在进程内唯一，`Indices::sourceCloudId` 靠它对账 —— 读回来时要重新分配 id、同步改写 Indices。

已有的序列化都不够用：
- 点云写盘 `cloud_io.h` 是挂钩，core 不认格式，实现由标准包注册，只有写没有读，入口 `lyflow_output_save`。
- `valueJson()` 是给人看的 JSON，点云与 Indices 返回空串。
- 张量与下标没有落盘格式（`http-transport.md`、ADR-0019）。

## 4. 热重载与过期（ADR-0009）

- 换的是整个 `lyflow_core.dll`：复制成 `gen<N>` 再加载（`bridge/src/core_ffi.rs`）。包编进 core，没有单独的包 DLL。
- 换代前先 `core.cache_clear()`（`bridge/src/watcher.rs`，顺序写在 ADR-0009）。
- generation 只在 bridge 计数、只用于 manifest 失效，不进 cacheKey。

**过期问题的准确说法**：内存缓存正确，靠的是「换代就清仓」加「旧 DLL 里的对象活不过卸载」。磁盘缓存跨进程、跨构建都还在，
而 cacheKey 只看 `op.version`：改了 compute 却没升版本（开发期常态）、升级 PCL / onnxruntime、编译器改了浮点行为，
新进程都会读到旧实现算的结果，而且没有提示。次要风险：mtime 精度、复制文件时 mtime 被保留；`externalKey` 只覆盖作者声明的文件，
算子隐式读的配置不在键里。

## 5. 跨进程复用的需求

- `lyflow eval / sweep / perturb` 在一个进程内默认走缓存（ADR-0020）。
- 但 MCP / Agent 的调参循环每次起一个新 CLI 进程（`packages/mcp/src/tools.ts`、ADR-0021），读 PCD、预处理、ONNX 推理每次从头算。
- ONNX Session 只在进程内缓存（`ml_onnx_run.cpp`）；`--no-cache` 已有（`bridge/src/cli.rs`）。

## 6. 两个方案（待讨论）

**A. 按算子 opt-in「可持久化」+ 键里加构建指纹**（5–8 天，测试与 e2e 占一半）
- `Capabilities` 加 `persistable`（默认 false），只给 IO 读取、ONNX 这类贵又确定的算子打开。
- 落盘键 = cacheKey + 构建指纹（core DLL 文件哈希或链接时注入的 build id）+ `pack@version` + 依赖库版本 + 键格式版本。
- ResultStore 内存层之下加磁盘层（get / reuse / peek）；`data.cpp` 逐类型二进制编解码，处理 cloud id 重映射；C ABI 加缓存目录入口。

**B. 只在 CLI 里做磁盘缓存，默认关**（3–4 天）
- `--cache-dir <dir>` 开启；目录名带 core DLL 的文件哈希（Rust 侧 `core_ffi` 算），DLL 一变整个目录失效 —— 从根上避开实现过期。
- 只存点云与 Tensor，Indices 带着源云才存；编辑器不受影响，ADR-0007 的判定不变。
- 改 `bridge/src/cli.rs`、`eval.rs`、`perturb.rs`；core 暴露 `lyflow_cache_export/import(cacheKey, path)`，或复用写盘挂钩加一个读回挂钩。
- 代价：只对 CLI / Agent 有用；粒度是整个 DLL（开发期每次重编全失效，但一定正确）。

调研时的倾向：先 B（拿到 eval / MCP 的收益），A 留给编辑器跨会话的需求。两种都要给 ADR-0007 补修订。

**待定**：做不做、做哪个；缓存目录默认放哪（倾向不设默认，必须显式 `--cache-dir`）。
