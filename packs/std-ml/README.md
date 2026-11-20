# std-ml — LyFlow 标准算子包：推理与结果解码

正式 SDK 已包含本包。以下原生依赖、构建命令和源码路径用于[引擎仓](https://github.com/liangyuR/lyflow-engine/tree/main/packs/std-ml)。

默认构建启用 Tensor 解码与 ONNX 推理。

| id | 输入 → 输出 | 参数 |
|---|---|---|
| `ml.onnx_run` | `input: Tensor` → `output: Tensor` | `modelPath`、`inputName`、`outputName`、`intraOpThreads`（默认 1）、`modelContract` |
| `image.yolo_decode`、`image.yolox_decode` | 检测 Tensor 与预处理映射 → 原图像素实例 | 置信度、IoU、候选及结果上限 |
| `image.ocr_orient` | 识别输入 Tensor、方向概率 Tensor 与布局 → 方向校正的 Tensor 与布局 | `minConfidence` |
| `image.ocr_ctc_decode` | 识别输出 Tensor 与布局 → 文本和逐行结果 | 字典、轴顺序、输出数值模式与字符过滤 |

`Tensor` 是 core 的端口类型：`shape` + float32 数据，行主序。结果元信息包含形状与 min/max/mean，
完整数值可通过 `lyflow_output_tensor` 按 `offset` / `count` 切片读取；`Indices` 同样通过 `lyflow_output_indices` 读取 int32 下标。
C ABI 返回的视图在释放对应 handle 前有效。编辑器 Transport 的 `getOutputTensor` / `getOutputIndices`
使用二进制载荷（`LYTN` / `LYIX`），格式见 [HTTP 传输契约](../../docs/http-transport.md)。图像预处理和裁图由 `std-image` 提供。
模型输出读取前检查元素类型、形状与元素数；仅支持 float32 稠密张量，允许零元素输出。
其他类型、标量或无效形状返回 `bad_input`，诊断指向 `outputName`。

## 依赖

设置 `LYFLOW_STD_PACKS=0` 和 `LYFLOW_PACKS=std-ml` 可构建纯 Tensor 解码。需要在精简构建中运行模型时，加上 `LYFLOW_MODULES=ml-runtime`。

推理模块使用 onnxruntime 1.32.0（win-x64）。构建前准备一份：

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

## 串行批次与固定识别契约

std-ml0.3.0增加 `batchMode=serial`：沿Tensor首轴逐项调用常驻ONNX会话，固定batch=1模型可处理N项；N=0仍验证模型、名称、dtype与契约，零次推理并返回canonical `{0}` 空Tensor。只支持单float32输入/输出，最多256项和32Mi个float，输出形状/有限值按实际运行结果再次校验，失败不发布半批。原有 `model` 默认模式和通用显式选头行为保留。

`modelContract`新增 `pphumanseg_2023mar`、`ocr-db`、`ocr-direction-2` 与 `ocr-ctc`，均复核声明与实际形状。线程预算在serial模式限制至上下文预算；取消在Run前后与批次边界生效，单次ORT调用内部不可抢占。组合与模型支持矩阵见[P4示例](../../docs/p4-examples.md)。
