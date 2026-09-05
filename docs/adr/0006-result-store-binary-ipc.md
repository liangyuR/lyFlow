# ADR-0006：结果留在 C++ 的内容寻址结果仓，点云走二进制 IPC

- 状态：已采纳
- 日期：2026-09-05

## 背景

M2 的 3D 视图要做到「选中节点 → 看见它的输出点云」。数据现在在 C++ 的堆上，
要送到前端的 `THREE.BufferGeometry` 里。同时 M3 要做缓存复用与 `skipped` 状态。

摆在面前的三条路：

1. 结果随 `ExecutionEvent` 一起 JSON 化推给前端
2. 结果由 Rust 侧持有（执行结束时从 C++ 拷出来）
3. 结果留在 C++，前端按需拉

## 决策

**结果留在 C++ 的 `ResultStore`，两层结构：**

```
内容寻址层   cacheKey ──► Data        真正持有数据，同键只有一份
索引层       runId ──► (nodeId, port) ──► cacheKey
```

**点云走二进制 IPC**，不经 JSON：
`get_output_cloud` 返回 `tauri::ipc::Response` 的裸字节，布局是

```
u32 magic 'LYPC' | u32 pointCount | u32 totalPoints | u32 flags
f32 bounds[6] | f32 xyz[3n] | [f32 intensity[n]]
```

前端 `new Float32Array(buffer, offset, len)` 零拷贝进 attribute。

## 理由

**JSON 化点云走不通，这不是「慢一点」而是「不可行」。**
一百万个点的坐标写成 JSON 数组大约 30MB 文本，前端还要整段 `JSON.parse`
再逐个 `push` 进 `Float32Array`。这条路在第一个真实数据集上就会死。
二进制那条路上，同样一百万点是 12MB 字节、一次 `new Float32Array` 的视图构造。

**结果不能放 Rust 侧。** 那意味着每次运行结束把所有中间结果从 C++ 拷一份到 Rust，
拷贝量等于整条 pipeline 的中间结果总和 —— 而其中绝大多数用户根本不会去看。
更要命的是 M3 的缓存复用需要 C++ 在**编译期**就知道「这个 cacheKey 有没有结果」，
结果在 Rust 侧的话这个问题答不了。
architecture.md 的分工写得很清楚：中间结果缓存与复用是 C++ 的职责。

**为什么从第一天就按 cacheKey 内容寻址，而不是简单的 `runId+node+port → Data`。**
M2 用不上复用，`freeRun` 时顺带把无人引用的 Data 删掉，行为和直接按 runId 存
一模一样。但 M3 打开缓存复用时，两者的差距是「删掉两行 + 加一个 LRU 字节预算」
和「重写这一层加它的所有调用点」。
键的定义（算子 id、版本、规范化参数、按端口名排序的上游键、`externalKey`）
也在 M2 就定死 —— 否则 M3 一改定义，此前算出来的键全部作废，等于白算一个里程碑。

**`externalKey` 钩子。** 路径没变但文件被覆盖了，缓存也必须失效。
IO 算子返回 `size:mtime`。M2 只是把它揉进 cacheKey，M3 才靠它真正判断命中，
但接口现在就留好，免得那时候要动所有 IO 算子。

## 内存上界

结果仓本身没有上界，上界由**谁持有 run** 决定：Rust 的 `RunManager` 最多同时持有
两个 `RunHandle`（正在跑的 + 上一次完成的），`RunHandle` 析构就 `lyflow_run_free`，
`freeRun` 清索引并删掉无人引用的 Data。所以峰值是两次运行的中间结果。
M3 加 LRU 字节预算之后，这个上界会变成一个显式的数字。

## 代价

- 前端要写一段手工的二进制解析。约二十行，且有 magic 校验；
  比起 JSON 的方案，多出来的复杂度全在一处，不会扩散。
- `lyflow_cloud_view` 是一个「借出去的指针 + 释放句柄」的 C ABI，
  Rust 侧必须用 RAII 包住（`Drop` 调 `lyflow_cloud_view_free`），漏一次就是内存泄漏。
- 抽样在 C++ 侧做（等步长，不是随机 —— 随机的话拖动 maxPoints 滑块时画面会闪，
  用户会以为数据在变）。`bounds` 用**全量**点云算，抽稀不该让 fit-to-bounds 漏掉边角。

## 复议条件

如果将来要把结果直接送进 GPU（共享内存 / DXGI 句柄），这套二进制布局是
自然的中间形态，不需要推翻。真正会触发复议的是「结果需要跨进程共享」，
比如 headless 与 GUI 分成两个进程 —— 那时结果仓要长出一层共享内存后端。
