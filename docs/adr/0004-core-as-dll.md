# ADR-0004：core 编译成 DLL，Rust 运行时加载

- 状态：已采纳
- 日期：2026-09-05

## 背景

M0/M1 的 core 是「零依赖的一堆 .cpp」，`bridge/build.rs` 用 cc crate 直接编译它们，
产物是一个静态库链进 Rust。这在算子只有注册表和 JSON 导出时非常舒服：
`cargo build` 在一台只装了 Rust + MSVC 的机器上就能跑通，不需要 CMake，
「加一个算子 → 前端出现」的循环是秒级的。

M2 要真的跑点云，也就是要接 PCL。PCL 从 vcpkg 装的是 `x64-windows` 动态三元组，
一次 `find_package(PCL)` 拉进来的是十几个 import 库和二十多个运行时 DLL
（pcl_*、boost_*、flann、lz4、qhull、zlib、png…），而且这张清单会随
PCL 版本和 feature 组合而变。

## 决策

1. core 由 **CMake** 构建成一个 **DLL**（`lyflow_core.dll`），只导出 C ABI。
2. Rust 侧不静态链接、不使用 import 库，改用 **libloading 在运行时 `LoadLibrary`**，
   所有调用经一张函数表。
3. `bridge/build.rs` 调 cmake crate 驱动同一份 `core/CMakeLists.txt`，
   再把整个输出目录（含 vcpkg applocal 拷来的依赖 DLL）拷到 cargo 的 target 目录。

## 理由

**用 cc crate 手工链 PCL 是死路。** 要在 build.rs 里复刻 `find_package(PCL)` 的
结果，等于把 vcpkg 的依赖解析用 Rust 重写一遍，而且每次升级 PCL 都要重写。
CMake 已经知道答案，让它回答。

**运行时加载让 M3 的热重载变成「卸载再加载」。** 静态链接的话，
「重编 core 之后不重启就换掉实现」需要重构所有调用点；
运行时加载下，热重载就是 drop 掉那个 `Core` 结构体再建一个 —— 调用点一行不改。
M3 的热重载是开发期最值钱的功能之一（ADR-0003），值得为它现在多写一层函数表。

**DLL 边界只放 C 函数，不放 C++ 类。** 导出 C++ 类等于把 ABI 焊死在编译器版本、
STL 版本和 `_ITERATOR_DEBUG_LEVEL` 上，而热重载恰恰要求两边可以独立重编。

## 代价

- 构建 core 现在需要 CMake + Ninja + vcpkg。这是真实成本，写进 README。
- 多一层函数表和一次 `LoadLibrary` 失败的错误路径要处理（找不到 DLL 时要说人话，
  而不是让 app 白屏）。
- 分发时必须把那二十多个依赖 DLL 一起打包（`tauri.conf.json` 的 `bundle.resources`）。

## 配套的两条硬约束

**D8：core 固定按 RelWithDebInfo 编，不跟 cargo profile 联动。**
Rust 永远用 `/MD`，而 vcpkg 的 debug 库是 `/MDd`。混用两种 CRT 的后果不是链接错误，
是运行时在完全无关的地方崩 —— 跨 CRT 释放内存、两份独立的 errno、两份独立的堆。
让 `cargo build --debug` 去编一个 debug core，就是在给自己埋这类雷。

**D9：exe 内嵌 `activeCodePage=UTF-8` 的 manifest。**
PCL 的文件 IO 走窄字符串，窄字符串怎么解释取决于进程的 ACP。
注意只有 **exe** 的 manifest 决定进程 ACP，给 DLL 贴 manifest 无效 ——
所以 `lyflow-dump-manifest.exe`、`lyflow-core-tests.exe` 和 Tauri 的 exe 都要各贴一份。
详见 `core/src/ops/pcl/pcl_path.h`：光有 manifest 还不够，那里还有一层开机探测兜底。

## 复议条件

如果哪天 core 的依赖回到「只有标准库」，或者项目改为把算子做成插件 DLL
（M5 的方向），这条决定的形态要重新评估 —— 但那时更可能是**更加**依赖运行时加载，
而不是回到静态链接。
