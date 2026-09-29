# 结果缓存落盘 —— 实施计划（方案 B：CLI 专用、默认关）

> 状态：**已实施**（2026-09-29，D1–D3）。调研见 [disk-cache-research.md](disk-cache-research.md)。

## 0. 要解决的问题

Agent / MCP 的调参循环每次起一个新的 `lyflow` 进程（ADR-0021），内存缓存随进程消失：读 PCD、预处理、ONNX 推理
每一轮都从头算。`eval / sweep / perturb` 在一个进程内已经复用缓存，跨进程没有。

## 1. 定死的边界

| # | 决定 | 理由 |
|---|---|---|
| K1 | **只给 CLI**（`run / eval / sweep / perturb`），编辑器不开 | 桌面端是常驻进程，内存缓存够用；编辑器跨会话的需求没人提 |
| K2 | **默认关**，显式 `--cache-dir <dir>` 才开；没有默认目录 | 落盘是有代价的（磁盘、过期风险），必须是调用方知情的选择 |
| K3 | **目录按构建指纹分**：`<dir>/<指纹>/`，指纹变了整个目录作废 | 调研 §4 的过期问题：cacheKey 只看算子版本，改了 compute 没升版本就会读到旧结果。指纹一变全部重算，粗但一定对 |
| K4 | **缓存判定仍归 C++**（ADR-0007）：磁盘层做在 `ResultStore` 里，内存未命中时查盘、算完落盘；`lyflow_plan` 的 `cached` 预测同样查盘 | CLI 不自己编排「导入 / 导出」—— 那是第二份缓存判定，迟早和执行器漂开 |

## 2. 做法

### 2.1 C ABI（v14）

```c
// 设置落盘缓存目录（dir 为 NULL / 空串 = 关）。fingerprint 由宿主给：同一指纹下的结果才复用。
// 返回空串 = 成功，否则一句人话的原因（目录建不出来等）。
LYFLOW_API char* lyflow_cache_set_dir(const char* dir, const char* fingerprint);
```

### 2.2 core

- `ResultStore` 多一个可选的磁盘层：`reuse` / `peek` 在内存里没找齐端口时，查 `<dir>/<指纹>/<cacheKey 前 2 位>/<cacheKey>.lfc`；
  读到就放进内存层再按原路径命中。执行器写入（`put`）之后，**这个节点的全部端口都能序列化**且**耗时 ≥ 阈值**时整节点落盘一个文件。
- 文件格式 `.lfc`：魔数 + 格式版本 + 端口表（名字、类型、长度）+ 各端口的二进制块。写临时文件再原子改名（多个进程并发写同一目录不会读到半个文件）；
  读到坏文件当未命中并删掉。
- `PointCloud::id` 读回时重新分配。
- 不落盘：非确定性算子（它们的 cacheKey 带 runId，本来就不命中）、preview 命名空间、Error。

### 2.3 bridge / CLI

- `--cache-dir <dir>`（`run / eval / sweep / perturb` 共用），或环境变量 `LYFLOW_CACHE_DIR`（给 MCP 用，见 §5）。
- 指纹由 Rust 算（`core_ffi` 已经知道加载的是哪个 DLL）：core DLL 文件内容的 sha256 + 同目录其它 DLL 的「名字 / 大小 / 修改时间」+ ABI 号 + 格式版本。
- `lyflow cache info|clear --cache-dir <dir>`：看占用、清掉（含别的指纹的旧目录）。v1 不做自动淘汰。

### 2.4 测试

- core doctest：落盘后清掉内存再跑，命中且输出逐位相同（点云含 intensity / normals / rgb、张量）；指纹换了不命中；坏文件当未命中；
  不可序列化的端口整节点不落盘；plan 的 `cached` 预测查盘。
- `cargo test`：`--cache-dir` 两个进程先后跑同一张图，第二次上游节点 skipped；换指纹不命中；`cache info / clear`。
- 不进 e2e（编辑器不涉及）。

## 3. 约 3–4 天，分三片

| 片 | 内容 |
|---|---|
| D1 | core：`.lfc` 编解码 + ResultStore 磁盘层 + `lyflow_cache_set_dir`（ABI v14）+ doctest |
| D2 | bridge：指纹、`--cache-dir` / `LYFLOW_CACHE_DIR`、`lyflow cache info|clear` + cargo test |
| D3 | 文档：ADR-0007 修订、agent-tuning / mcp.md、testing.md |

## 4. 明确不做

编辑器落盘；按算子声明 `persistable` + 细粒度构建指纹（调研里的方案 A）；自动淘汰 / 配额；跨机器共享缓存目录。

## 5. 已确认（2026-09-29，均按建议）

1. **哪些类型能落盘**：
   - (a) 只做点云与张量（覆盖读盘、滤波、ONNX 推理这几类「贵的上游」），其余类型的节点照旧重算；
   - (b) 再加 Record / Measurement / 2D 几何 / Plane / Transform（都是定长结构或 JSON，多约 1 天），gap 的中间节点也能跳过。
   - 建议 (a)：贵的都在点云与张量上，Record 这类节点本来就快。
2. **落盘阈值**：节点耗时 ≥ 20 ms 才落盘（比它快的，读盘不一定比重算快）。建议就用 20 ms，不给用户调。
3. **MCP 怎么打开**：MCP server 读环境变量 `LYFLOW_CACHE_DIR`，有就给每次起的 CLI 带上 `--cache-dir`（配置一次，工具参数不变）。建议这样做。
4. **依赖 DLL 的指纹**：onnxruntime、PCL 这些 DLL 只取「名字 / 大小 / 修改时间」，不读全文（全文哈希每次起进程要多花上百毫秒）。建议这样做。
