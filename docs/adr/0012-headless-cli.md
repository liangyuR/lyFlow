# ADR-0012：headless CLI 是 bridge 的第二个 bin，输出 JSON Lines

日期：2026-09-06（M4）　状态：已采纳

## 背景

M4 的产出定义是「一条真实任务能用『库算子 + CLI』跑在无 GUI 的机器上」。
需要一个既能在 CI 里跑、又保证与桌面**完全同一条路径**的入口。

「完全同一条路径」不是修辞：如果 CLI 自己写一遍图的加载与校验，那么它和桌面
迟早会在某个边界情况上给出不同的答案，而那时没人知道该信哪一个。

## 决定

**CLI 是 `bridge` crate 的第二个 bin，与 app 共用 `core_ffi` 与 `graph.rs`，
并且不依赖 Tauri（用 cargo feature 隔离）。stdout 是 JSON Lines。**

```toml
[[bin]] name = "lyflow-app"  path = "src/main.rs"       required-features = ["desktop"]
[[bin]] name = "lyflow"      path = "src/bin/lyflow.rs"

[features]
default = ["desktop"]
desktop = ["dep:tauri", "dep:tauri-build", "dep:tauri-plugin-dialog"]
```

`cargo build --bin lyflow --no-default-features` 因此完全不碰 Tauri —— 这一条进了
`pnpm check`，是这条边界唯一靠得住的证据。

**输出格式**：`run` / `dump` / `sweep` 的 stdout 是 JSON Lines，`run` 那一份就是
`ExecutionEvent` 原样，一行一条，seq 连续。`validate` / `plan` / `migrate` / `manifest`
各输出一行 JSON。人看的东西一律走 stderr。

**退出码**：0 成功，1 校验失败，2 执行失败，3 被取消（Ctrl+C），4 参数错。
`run` 会先单独 validate 一次 —— 校验失败与执行失败是两个不同的退出码，
混在一次 run 里分不开。

**实现放在 `src/cli.rs`（lib 里），bin 只有三行。** 这样 `cargo test` 能覆盖
每个子命令的退出码与输出形状；把逻辑写在 bin 里的话它们就只能靠人手跑。

## 后果

**app 的 exe 改名成 `lyflow-app.exe`。** `src/main.rs` 自动推导出的 bin 名是包名
`lyflow`，会和 CLI 撞名。改 CLI 的名字会让文档里所有 `lyflow run …` 都变成
`lyflow-cli run …`，那是更贵的一边。`tauri.conf.json` 加了 `mainBinaryName`，
`scripts/e2e/harness.mjs` 跟着改了一处。产品名（安装包、窗口标题）仍然是 LyFlow。

**dump 要让 core 写盘。** C ABI 加了 `lyflow_output_save`，实现直接复用
`ops/pcl/io_save_pcd.cpp` 里那份写盘代码。备选是让 Rust 从
`lyflow_output_cloud` 拿到 xyz+intensity 自己拼 PCD —— 那会漏掉 rgb，
而且写盘格式的知识会出现在两个地方。

**Ctrl+C 用 `SetConsoleCtrlHandler`，不引依赖。** 十五行 `extern "system"`，
只在 Windows 下编译。处理器里拿到当前 `RunHandle` 调 `cancel()` 并返回 1，
让主线程把 `run_finished(cancelled)` 发完再退出 —— 直接被杀掉的话，
JSON Lines 会断在半条事件上。

**`sweep` 是同进程的笛卡尔积循环。** 这正是它值得存在的理由：进程内的结果仓让上游
只算一次。五组 leafSize 的扫描里，源节点在后四次都是 `skipped`，
这条断言写进了 `cargo test`。

**`diff` 默认输出给人看，`--json` 才是机器格式。** 这一条与「stdout 是 JSON Lines」
不一致，是有意的：`diff` 的主要用户是站在终端前的人，而 `--json` 这个 flag 在
m4-plan 里就写着，说明当初也是这么想的。差异不算错误，所以 `diff` 永远退出 0。

**`--set` 的路径是 `nodeId.param`，值是 JSON 字面量**，与参数右键的「复制路径名」
配套（那一条从 M3 起就在，注释里写着「给 M4 的 CLI 用」）。值解析不出 JSON 时当成
字符串 —— `--set n.path=cloud.pcd` 是最常用的一条，不该逼人写引号里的引号。
