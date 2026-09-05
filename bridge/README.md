# bridge —— Rust 桥接层

Tauri 桌面壳。职责：IPC、序列化、文件读写、进程生命周期、事件推流、崩溃隔离。

**不理解算子语义，不改写图结构。** 见 [docs/architecture.md](../docs/architecture.md)。

## C++ 怎么进来的

`build.rs` 用 `cc` crate 直接编译 `core/src/**/*.cpp` 成静态库并链进来，
**不经过 CMake** —— 只装 Rust + MSVC 的机器上 `cargo build` 就能跑通。
`core/CMakeLists.txt` 保留给单独调试 core 用。

新增 `core/src/ops/*.cpp` 会被自动编进来：`build.rs` 递归 glob，并对每个
目录发 `cargo:rerun-if-changed`，所以加文件会触发重编。

## 暴露给前端的 command

| command | 方向 | 说明 |
|---|---|---|
| `get_manifest` | C++ → 前端 | 全量算子描述。进程内缓存，解析一次。 |
| `get_core_info` | C++ → 前端 | core 版本 + 算子/类型数量，给状态栏用。 |
| `save_graph` | 前端 → 磁盘 | 结构校验后写盘，pretty JSON。 |
| `load_graph` | 磁盘 → 前端 | 读盘 + 结构校验。 |

## 启动自检

`run()` 第一件事是调 `lyflow_manifest_problems()`，非空就打印并 `exit(1)`。

契约破了继续跑没有意义 —— 前端会拿到一份自相矛盾的 manifest，然后以各种
离奇的方式失败。算子描述写错是开发期的事，用户不该看到这条路径。
