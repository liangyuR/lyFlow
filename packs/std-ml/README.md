# std-ml — LyFlow 标准算子包：推理

版本 0.1.0，默认启用。全仓库唯一一处 onnxruntime 依赖（ADR-0015）。

| id | 输入 → 输出 | 参数 |
|---|---|---|
| `ml.onnx_run` | `input: Tensor` → `output: Tensor` | `modelPath`、`inputName`、`outputName`、`intraOpThreads`（默认 1） |

`Tensor` 是 core 的端口类型：`shape` + float32 数据，行主序。它不进二进制 IPC ——
Inspector 只看得到形状与 min/max/mean。构造张量、解读张量都是领域的事
（比如 `gap.profile_tensor` 与 `gap.labels_from_logits`），本包只负责「跑一次前向」。

## 依赖

onnxruntime 1.19.2（win-x64）。构建前准备一份：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/fetch-onnxruntime.ps1
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

按「模型路径 + 文件大小/mtime + 线程数」缓存 `Ort::Session`，
改模型文件会自动重载。`externalKey` 也是同一个指纹，所以换模型会让下游缓存失效。
