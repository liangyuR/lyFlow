# std-ml — LyFlow 标准算子包：推理

版本 0.2.0，默认启用。全仓库唯一一处 onnxruntime 依赖（ADR-0015）。

| id | 输入 → 输出 | 参数 |
|---|---|---|
| `ml.onnx_run` | `input: Tensor` → `output: Tensor` | `modelPath`、`inputName`、`outputName`、`intraOpThreads`（默认 1）、`modelContract` |

`Tensor` 是 core 的端口类型：`shape` + float32 数据，行主序。结果元信息包含形状与 min/max/mean，
完整数值可通过 `lyflow_output_tensor` 按 `offset` / `count` 切片读取；`Indices` 同样通过 `lyflow_output_indices` 读取 int32 下标。
C ABI 返回的视图在释放对应 handle 前有效。编辑器 Transport 的 `getOutputTensor` / `getOutputIndices`
使用二进制载荷（`LYTN` / `LYIX`），格式见 [HTTP 传输契约](../../docs/http-transport.md)。构造张量、解读张量都是领域的事
（比如 `gap.profile_tensor` 与 `gap.labels_from_logits`），本包只负责「跑一次前向」。
模型输出读取前检查元素类型、形状与元素数；仅支持 float32 稠密张量，允许零元素输出。
其他类型、标量或无效形状返回 `bad_input`，诊断指向 `outputName`。

## 依赖

onnxruntime 1.19.2（win-x64）。构建前准备一份：

```powershell
pwsh -ExecutionPolicy Bypass -File scripts/fetch-onnxruntime.ps1
```

它优先从本机的 `xyz-gap-inspector/3rdparty/` 复制，否则从 GitHub release 下载，
落在 `third_party/onnxruntime/`（已 gitignore）。已有一份时设
`LYFLOW_ONNXRUNTIME_ROOT` 指过去也行。缺失时 CMake 直接 FATAL 并打印这条命令。

`onnxruntime.dll` 与 `onnxruntime_providers_shared.dll` 由本包的 cmake 拷进
core 的 `bin/`，`bridge/build.rs` 整目录搬走它。

## 确定性

`intraOpThreads` 默认 1，同时用作 inter-op 线程数：归约顺序固定，
同一份输入在同一台机器上逐位可复现 —— 调参时前后两次读数的对比因此才有意义。

## 会话缓存

按「规范化绝对模型路径 + 文件大小/mtime + 线程数」缓存 `Ort::Session`，
最多保留 8 个最近使用的会话。改模型文件会自动重载，成功加载新指纹后移出该路径的旧指纹缓存；
同一文件的不同线程配置共用这 8 个名额，避免线程预算切换时反复重建。
`externalKey` 也是同一个文件指纹，所以换模型会让下游缓存失效。

缓存淘汰只释放缓存持有的引用，正在推理的调用会持有会话到结束；并发调用较多时，
活跃会话总数可以暂时超过 8。缓存上限按会话个数计算，模型大小与 ONNX Runtime 内存池会影响实际内存占用。
会话构建仍串行执行以避免重复加载，慢构建不会阻塞已有会话命中；资源回收在缓存条目锁外完成。

每个会话复用一份 float32 输入缓冲和 CPU `MemoryInfo`。形状稳定或输入元素数不超过已分配容量时，
输入缓冲复用已有内存，减少每次推理的分配与释放；源张量仍复制到独立缓冲，保持只读。
缓冲保留该会话见过的最大容量，随会话释放，实际常驻内存需同时计入这部分容量。

同会话的输入缓冲调整、输入复制、ORT 张量构造、推理和输出复制由同一把互斥锁保护，
输入与输出的 ORT 句柄在解锁前销毁。即使模型输出引用输入缓冲，下次推理也不会改写已经交付的张量。
不同会话各有独立缓冲与互斥锁；线程数仍由显式 `intraOpThreads` 决定。

## 显式模型契约

`modelContract=generic` 保持既有按名称选择输入/输出的通用行为。`yolox-nano-0.1.1rc0` 要求唯一 float32 输入 `[1,3,416,416]` 与输出 `[1,3549,85]`。`yolo-detection` 要求唯一 float32 静态输入 `[1,3,H,W]`（H/W是32的倍数、32..2048），输出为 `[1,4+C,N]`（1..512类、8/16/32步长锚点数）或 `[1,N,6]`（1..4096条端到端检测）。额外IO、动态尺寸及不兼容元素类型均拒绝，不静默丢弃其他输出。模型色序、归一化和类别语义由对应图像预处理/解码节点明确约束。

真实 YOLOv8/11/26 与 YOLOX 的外部模型准备和许可见 [检测示例](../../examples/p1-detection-README.md)。这不改变通用 ONNX 算子的默认契约。
