# M2 验收记录

对着 [m2-plan.md §11](m2-plan.md) 的每一条，写清「怎么跑 + 实际输出摘要 + 结论」。

**结论只有三种**：通过 / 未通过 / 未验证。「未验证」是如实标注，不是委婉的通过 ——
§11 的每一条都通过了，但第 8 条有半句延伸（「干净**机器**」而不是「干净**目录**」）
本次确实没条件验，那半句单独标成未验证，理由写在 8 那一节里。

环境：Windows 11 Pro 26200，MSVC 14.51（VS 18 Community），vcpkg `C:\vcpkg`
（PCL 1.15.1，`x64-windows` 动态三元组），Node 24.16，pnpm 11.1.3。

---

## 复现命令

```powershell
# 1. 全链路门禁：C++ 编译 + 算子自检 + core 测试
#    → 三份契约样例对 schema 校验 → cargo test → 前端 strict typecheck + build
pnpm check

# 2. core 的 doctest（pnpm check 里已经跑过，单独跑用这个）
powershell -File scripts/build-core.ps1          # 构建 + 自检 + 测试
.\build\core\bin\lyflow-core-tests.exe           # 只跑测试
.\build\core\bin\lyflow-dump-manifest.exe --check

# 3. Rust
cd bridge; cargo test

# 4. CDP 端到端验收（自己起 tauri dev，跑完自己收尾）
pnpm e2e

# 4b. 同一套断言，但跑的是打包产物在一个干净目录里的拷贝
pnpm tauri build          # 先出产物
pnpm e2e:packaged

# 5. 安装包
pnpm tauri build
# 产物：bridge/target/release/lyflow.exe（+ 20 个运行时 DLL）
#       bridge/target/release/bundle/nsis/LyFlow_0.1.0_x64-setup.exe
#       bridge/target/release/bundle/msi/LyFlow_0.1.0_x64_en-US.msi
```

调试验收脚本本身时不必每次重编：另开一个窗口跑

```powershell
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=9222"
pnpm tauri dev
```

然后 `$env:LYFLOW_E2E_ATTACH="1"; pnpm e2e`。

---

## 逐条验收

### ✅ 1. 15 个算子 manifest 自检干净，schema 校验过

**怎么跑**：`pnpm check` 的前两步，或者手动
`.\build\core\bin\lyflow-dump-manifest.exe --check`。

**实际输出**：

```
ok: 15 operator(s), 5 port type(s)
ok: 15 operator(s), 5 port type(s) 符合 …\schema\operator-manifest.schema.json
```

15 个算子：`gen.synthetic`、`io.load_pcd`、`io.save_pcd`、`filter.passthrough`、
`filter.voxel_grid`、`filter.crop_box`、`filter.random_sample`、
`filter.statistical_outlier`、`filter.radius_outlier`、`features.normals`、
`segment.ransac_plane`、`segment.extract_indices`、`transform.make`、
`transform.apply`、`util.merge`。

5 个端口类型：`Any`、`PointCloud`、`Indices`、`Transform`、`Plane`。
`PointCloudXYZI` 已按 D10 删除，`core_ffi` 里有一条断言守着它不回来。

**结论：通过。**

### ✅ 2. core 测试：环、诊断全量、默认值、取消、上游失败传播、select 通道保留、中文路径 PCD 往返

**怎么跑**：`.\build\core\bin\lyflow-core-tests.exe`

**实际输出**：

```
[doctest] test cases:  43 |  43 passed | 0 failed | 0 skipped
[doctest] assertions: 265 | 265 passed | 0 failed |
[doctest] Status: SUCCESS!
```

计划里点名的每一项对应的用例：

| 计划里的要求 | 用例 |
|---|---|
| 拓扑与环 | `拓扑序：上游一定排在下游之前`、`环：环上每个节点各一条 cycle 诊断，整图级失败` |
| 诊断全量返回且 paramPath 正确 | `D5：一次返回全部诊断，不在第一个错误处早退`、`paramPath 精确指向出错的参数框` |
| 默认值合并 | `默认值合并：没写的参数用 manifest 的默认值` |
| 类型兼容矩阵 | `类型兼容矩阵`（相等 / 不等 / 端口不存在 / 必填未连 / 可选未连五种） |
| 取消在第 k 个节点生效 | `取消：在第 k 个节点生效，join 在 1 秒内返回` |
| 上游失败传播 | `上游失败：下游标 cancelled + upstream_failed，旁支照常执行` |
| ResultStore 抽样点数与 bounds | `结果仓：抽样点数、总数与 bounds` |
| select 保留全部通道 | `select 保留全部通道` 等四条 |
| 中文路径下 load/save PCD | `test_io_pcd.cpp` 五条（binary / ascii / binary_compressed / 中文文件名 / 文件不存在） |

**结论：通过。**

一点说明：取消那条用了一个只在测试里注册的 `test.block` 算子（阻塞到收到取消为止）。
用「跑一张很大的图然后立刻取消」是不可靠的 —— 机器快一点整张图就跑完了，
测试变成偶发失败，而偶发失败的测试等于没有测试。

### ✅ 3. Rust 测试：事件顺序与 seq、坏参数 paramPath、取消 1s 内 join、二进制输出头部字段正确

**怎么跑**：`cd bridge; cargo test`

**实际输出**：`test result: ok. 26 passed; 0 failed`

对应用例：

| 计划里的要求 | 用例 |
|---|---|
| 事件顺序与 seq | `two_node_run_emits_events_in_order_with_dense_seq`（断言 `g:pending → g:running → g:done → v:pending → …` 与 `seq` 从 0 起严格连续） |
| 坏参数 paramPath | `bad_param_marks_the_exact_input_box`（`errors[0].paramPath == "leafSize"`，下游 `cancelled` + `upstream_failed`） |
| 取消 1s 内 join | `cancel_stops_a_heavy_run_and_joins_within_a_second`（20 节点 × 300 万点） |
| 二进制输出头部字段 | `output_cloud_binary_header_is_correct`（magic、pointCount、totalPoints、flags、bounds 的 min<max、总长度与头部自洽） |
| 中文路径往返 | `chinese_path_save_then_load_roundtrip` |

**结论：通过。**

这些测试不经 Tauri 的 `AppHandle`：`trampoline` 除了 `app.emit` 之外没有别的逻辑，
而起一个真实 AppHandle 会把它们变成需要窗口系统的集成测试。真正值得测的那一整条
—— libloading 加载 DLL → C ABI 启动 run → 工作线程回调 → 事件 JSON →
cancel/join/free 的生命周期 —— 全都覆盖到了。

### ✅ 4. CDP：搭演示 pipeline → F5 → 节点依次变色 → run_finished ok → 选中每个节点 3D 视图点数 > 0

**怎么跑**：`pnpm e2e`（分组「演示 pipeline」）

pipeline：`io.load_pcd → filter.crop_box → filter.voxel_grid →
filter.statistical_outlier → segment.ransac_plane → segment.extract_indices →
io.save_pcd`，图与 PCD 都在一个名字带中文和空格的临时目录里。

**实际输出**：

```
── 演示 pipeline：读 PCD → 裁剪 → 降采样 → 去噪 → 平面 → 分离 → 存盘
  ✓ 运行状态 ok
  ✓ load/crop/voxel/sor/plane/split/out 节点 done      （7 条）
  ✓ load 的状态序列        ["idle","pending","running","done"]
  ✓ out 的状态序列         ["idle","pending","running","done"]
  ✓ 上游 done 早于下游 running
  ✓ voxel 的点数少于 crop
  ✓ 选中 load/crop/voxel/sor/split → 3D 视图点数 > 0    （5 条）
  ✓ 选中 ransac_plane → 视图提示无点云输出
  ✓ 下游 PCD 写到中文目录
```

「依次变色」验的是两件事：每个节点都走完 `idle → pending → running → done`
（`idle` 是 `run_started` 播下的占位，让用户先看到这次要跑哪些节点），
以及上游的 `done` 一定排在下游的 `running` 之前。

「点数 > 0」读的是 3D 视图自己渲染出来的计数，并确认 `<canvas>` 真的在。
`segment.ransac_plane` 的输出是 Indices + Plane，没有点云端口，
所以单独验它给出的是「该节点无点云输出」而不是一片空白。

**结论：通过。**

### ✅ 5. CDP：把 voxel leafSize 改成 0 → 运行 → 对应输入框红框，其他节点照常执行，下游标 cancelled

**怎么跑**：`pnpm e2e`（分组「坏参数」）

**实际输出**：

```
── 坏参数：leafSize = 0
  ✓ 运行状态 error
  ✓ voxel 标 error
  ✓ 错误定位到 leafSize
  ✓ 上游 load 仍然 done
  ✓ 上游 crop 仍然 done
  ✓ 下游 sor / plane / split / out 标 cancelled       （4 条）
  ✓ 下游的原因是 upstream_failed
  ✓ leafSize 输入框标红
  ✓ 错误消息贴在控件下方
  ✓ 其它参数没有被连坐
  ✓ 检查器顶部列出该节点全部诊断
  ✓ 画布上 voxel 节点是 error
  ✓ 画布上 sor 节点是 cancelled
```

**结论：通过。**

### ✅ 6. CDP：运行中 Esc → run_finished cancelled，UI 无残留 running

**怎么跑**：`pnpm e2e`（分组「运行中按 Esc 取消」）。图是 300 万点 + 12 个体素栅格。

**实际输出**：

```
── 运行中按 Esc 取消
  ✓ 运行状态 cancelled
  ✓ 状态里没有残留的 running
  ✓ 画布上没有 running 的节点
  ✓ 画布上有 cancelled 的节点
  ✓ 取消按钮已经变灰
  ✓ 工具栏摘要显示 cancelled
```

Esc 是通过 CDP 的 `Input.dispatchKeyEvent` 真的按下去的，走完整的快捷键链路。

**结论：通过。**

### ✅ 7. CDP：运行后改任一参数 → 全部节点标 stale

**怎么跑**：`pnpm e2e`（分组「改参数 → 结果标为过时」）

**实际输出**：

```
── 改参数 → 结果标为过时
  ✓ 先跑一次 ok
  ✓ 刚跑完不是过时状态
  ✓ store 标记为过时
  ✓ 全部节点都变虚          （.node.is-stale 的数量 == 节点总数）
  ✓ 工具栏显示「已过时」
```

**结论：通过。**

### ✅ 8. 安装包（`tauri build`）在干净目录能启动并跑通演示 pipeline（DLL 随包）

**怎么跑**：

```powershell
pnpm tauri build
pnpm e2e:packaged
```

`--packaged` 模式把 `bridge/target/release/` 里的 `lyflow.exe` 和全部 `.dll`
拷进 `%TEMP%\lyflow 安装目录 <pid>\`（一个空目录），从那里启动，
再跑一遍与 `pnpm e2e` **完全相同**的断言。拷的东西正是两个安装包往
`$INSTDIR` 放的东西 —— NSIS 的 `SetOutPath $INSTDIR`、WiX 的 `INSTALLDIR`。

**实际输出**：

```
干净安装目录：C:\Users\…\Temp\lyflow 安装目录 27356（20 个 DLL 随包）
启动已打包的 …\lyflow.exe（CDP 端口 9222）

── 安装包（干净目录）
  ✓ 打包产物启动成功并加载到了 core
  ✓ core 版本可读
  ✓ 15 个算子
…（后面是与 pnpm e2e 相同的 64 项）

67/67 项通过，全绿
```

**DLL 随包**：`tauri build` 成功产出

- `bridge/target/release/bundle/nsis/LyFlow_0.1.0_x64-setup.exe`
- `bridge/target/release/bundle/msi/LyFlow_0.1.0_x64_en-US.msi`

两个安装包各自带着同样的 20 个文件（从生成的 `installer.nsi` 与 `main.wxs`
里逐个数出来的）：

```
lyflow_core.dll
pcl_common  pcl_features  pcl_filters  pcl_io  pcl_io_ply  pcl_kdtree
pcl_ml  pcl_octree  pcl_sample_consensus  pcl_search  pcl_segmentation
boost_filesystem-vc145-mt-x64-1_92   boost_iostreams-vc145-mt-x64-1_92
bz2  liblzma  libpng16  lz4  z  zstd
```

计划里标注「需要验证」的假设一因此成立：`bundle.resources` 的目标 `"./"`
在 Windows 上落到 exe 同目录，与 `core_ffi::dll_path()` 的「exe 同目录」约定一致。
结论已回写进 m2-plan.md §1。

**结论：通过。**

**仍未验证的那一半**：这台机器装着 MSVC、vcpkg 和 PCL 的全部运行时，
所以「干净**目录**」不等于「干净**机器**」—— 一个漏打包的 DLL 仍可能被系统
从 `C:\vcpkg\installed\x64-windows\bin` 或 MSVC 的目录里悄悄找到。
具体的已知风险是 VC++ 运行时（`vcruntime140*.dll` / `msvcp140*.dll`）：
它不在 vcpkg applocal 的清单里，干净机器上要么装 VC++ Redistributable，
要么把它一起打包。这一条要等到真的有一台干净 Windows 时才能确认，
本次**未验证**，不算在上面那句「通过」里。

### ✅ 9. 中文目录下保存图 + 读 PCD + 存 PCD

三个层次都验了：

| 层次 | 怎么跑 | 结果 |
|---|---|---|
| C++ | `lyflow-core-tests.exe` 的 `test_io_pcd.cpp` | 5 条全过：中文目录 + 中文文件名的 binary / ascii / binary_compressed 往返，以及「文件不存在」报 `io` + `paramPath=path` |
| Rust | `cargo test` 的 `chinese_path_save_then_load_roundtrip` | 过 |
| 端到端 | `pnpm e2e`，整场验收的工作目录就是 `%TEMP%\lyflow 验收 中文目录 <pid>` | 过 |

`pnpm e2e` 的输出：

```
中文工作目录：C:\Users\…\Temp\lyflow 验收 中文目录 43604
── 中文路径：保存图 + 写 PCD
  ✓ 图存进中文目录            演示 流程.lyflow.json
  ✓ 运行状态 ok
  ✓ 中文文件名的 PCD 写出来了   合成 点云.pcd
  ✓ PCD 非空
  ✓ 生成节点报出了点数
```

演示 pipeline 分组接着从这个中文目录读回那个 PCD、再往里写 `去平面 结果.pcd`，
路径全部用**相对路径 + baseDir**，也就是真实使用时的形态。

**结论：通过。**

### ✅ 附加：Run to node（交互清单 P1 #27）

§11 没有单列，但数据通路是 M2 做的，顺手验了：

```
── Run to node（只跑上游闭包）
  ✓ 右键弹出菜单
  ✓ 运行状态 ok
  ✓ 目标节点被记录
  ✓ 上游 gen 执行了
  ✓ 目标 voxel 执行了
  ✓ 下游 tail 根本没进计划
```

### ✅ 附加：`validate_graph` / `get_output_info` 两条 command

这两条 UI 目前没有调用点 —— `validate_graph` 是给 M3 的「边编辑边标红」留的，
`get_output_info` 是给日志抽屉留的。没有调用点的接口最容易悄悄坏掉，所以
`pnpm e2e` 直接过一遍真实的 IPC：

```
── validate_graph / get_output_info
  ✓ 干净的图没有诊断
  ✓ 坏参数被 validate_graph 逮到
  ✓ 诊断带 severity
  ✓ 诊断的 phase 是 validate
  ✓ extract_indices 报出两个输出端口
  ✓ 输出信息带类型与元素数
```

### ✅ 附加：跑完全程没有控制台报错

`pnpm e2e` 全程监听 `Runtime.consoleAPICalled(error)` 与 `Runtime.exceptionThrown`，
一条都不允许有。

---

## 汇总

```
pnpm check        C++ 编译 + 自检 + 43 个 doctest 用例（265 断言）
                  + 3 份契约样例对 schema 校验
                  + 26 个 Rust 测试
                  + 前端 strict typecheck + build          → 全链路绿
pnpm e2e          64/64 项通过，全绿
pnpm tauri build  NSIS + MSI 产出成功，各带 20 个运行时 DLL
pnpm e2e:packaged 67/67 项通过，全绿（打包产物在干净目录里跑同一套断言）
```

| §11 验收项 | 结论 |
|---|---|
| 15 个算子 manifest 自检干净，schema 校验过 | 通过 |
| core 测试（环 / 诊断全量 / 默认值 / 取消 / 上游失败 / select / 中文 PCD） | 通过 |
| Rust 测试（事件顺序与 seq / paramPath / 取消 1s / 二进制头部） | 通过 |
| CDP：演示 pipeline → F5 → 依次变色 → ok → 3D 点数 > 0 | 通过 |
| CDP：leafSize=0 → 红框 + 旁支照跑 + 下游 cancelled | 通过 |
| CDP：运行中 Esc → cancelled，无残留 running | 通过 |
| CDP：改参数 → 全部节点 stale | 通过 |
| 安装包构建 + DLL 随包 + 在干净**目录**跑通演示 pipeline | 通过 |
| 同一份产物在干净**机器**（无 MSVC / vcpkg / VC++ 运行时）上能跑 | **未验证**（手上没有干净机器；已知风险是 VC++ Redistributable） |
| 中文目录：保存图 + 读 PCD + 存 PCD | 通过 |

---

## 代码审查抓到的缺陷

M2 的实现全绿之后又做了一轮独立的代码审查（资源与生命周期、越界与溢出、
状态竞态、契约一致性）。下面这些是**已修**的，每条都补了回归测试或
在验收脚本里有对应断言。它们的共同点是**静默出错** —— 没有崩溃、没有告警，
只是结果悄悄地不对，或者界面悄悄地卡住。

### 高

1. **体素栅格会把相距很远的点折叠进同一个体素。**
   `core/src/ops/filter_voxel_grid.cpp` 曾把三个体素下标各截成 21 位塞进一个
   `uint64` 当键。注释写着「叶大小 1mm 时覆盖 ±1000 米」，实际是
   `±2^20 × leafSize` —— 默认叶大小只有 ±10 km，最小叶大小只有 ±104 m。
   越界的点绕回来和另一个体素撞在一起，于是远处的点被拿去和原点的点求质心。
   一次室外扫描就够越界，而且不报错。
   改成用完整的三个 `int64` 做键（碰撞交给 `unordered_map` 自己解决），
   并且在下标真的装不进 `int64` 时报 `bad_param`（此前那里是 UB）。
   回归测试：`体素栅格：相距很远的点不会被折叠进同一个体素`、
   `体素栅格：坐标相对叶大小过大时报错而不是静默算错`。

2. **3D 视图会对同一片点云发起一串重复的 IPC 请求。**
   取云的 effect 依赖了整张 `execNodes` Map，而那张 Map 每来一条事件就是一个
   新引用 —— 包括每 50ms 一条的 `node_progress`。选中一个刚跑完的节点、而别的
   节点还在跑时：请求发出去 → 下一条进度事件让 effect 重跑 → 旧请求作废、
   发新请求，循环。两百万点时是一队几十兆的往返。改成只订阅**这一个节点的
   状态字符串**。

3. **拖动点大小滑块会重建整个几何体。**
   `pointSize` 同时出现在「重建几何体」和「只改材质」两个 effect 的依赖里，
   后者的注释写着「点大小单独更新，不用重建几何体」，但前者让它形同虚设。
   两百万点时每个 `input` 事件要重新分配 24MB 颜色数组、重扫两百万个点、
   再传一次 GPU，拖动全程界面卡死。改成从 ref 读初值，`pointSize` 不进依赖。

4. **畸形 PCD 会读到缓冲区外面。**
   `adapter::fromBlob` 只查了 `data.size() >= n * point_step`，没查单个字段的
   `offset + width` 是否落在 `point_step` 之内；`readPacked` 还无条件读 4 字节。
   一个 `FIELDS x y z rgb / SIZE 4 4 4 1` 的头 PCL 解析得下来，`point_step` 是 13，
   最后一个点的 rgb 会越过 `data` 末尾三个字节。改成按声明宽度读，
   并在 `findField` 里把越界的字段当作不存在。

5. **`representative = nearest` 的第二趟扫描不响应取消。**
   而抢占式运行是**同步**等 `join` 的（`RunManager::start`），所以这一趟不响应
   取消等于前端整整卡住这一趟的时间，同时 `capabilities.cancellable = true`
   是一句谎话。补上轮询。回归测试：`体素栅格 nearest 模式也响应取消`。

6. **连点两次运行可能让界面永远停在 running。**
   两次 `run_graph` 走不同的 Tauri 工作线程，**回复顺序不保证等于运行开始的
   顺序**。如果先开始的那次回复后到，`beginRun` 会认下一个已经被取消的 runId，
   真正在跑的那次的事件全部落进 `orphans` 再也无人认领。加了一个本地运行序号，
   让后发的调用赢 —— 与 C++ 侧「后开始的 run 抢占先开始的」一致。

7. **StrictMode 下事件监听器注册了两份。**
   `subscribeExecutionEvents` 守的是 `unlisten !== null`，而它在 `await` 之后才
   赋值，于是 StrictMode 的挂载→卸载→再挂载两次都看到 `null`。后果是第一个
   `unlisten` 被覆盖再也调不到（泄漏），每条事件被 `apply` 两遍，控制台刷满
   「seq 不连续」。守卫改成那个 Promise 本身。

### 中

8. **`Run::work()` 没有顶层 `try/catch`。**
   只有 `compute` 被兜住了；`parseGraph`、`buildPlan`（含算子作者提供的
   `externalKey` 钩子）、事件序列化、结果仓写入都在一个 `std::thread` 的函数体里
   裸奔。任何逃出去的异常直接 `std::terminate`，整个 app 无声无息地没了 ——
   而 `c_api.h` 开头写着「异常绝不跨 ABI」，别的入口都做到了，就这里是缺口。

9. **`unsafe impl Sync for RunHandle` 的理由是假的。**
   注释说「core 侧各自加了锁」，但当时 C++ 的 `Run::join` 读写的是一个裸 `bool`。
   两边同时 join 是数据竞争加 UB。给 C++ 侧补了一把 mutex（C ABI 不能把正确性
   押在「调用方一定加了锁」上 —— M4 的 headless CLI 就是另一个调用方），
   并把那段注释改成逐项说明。

10. **和空点云合并会把另一侧的通道全部丢掉。**
    `hasIntensity()` 就是 `!intensity.empty()`，所以一片 0 个点的云在通道求交里
    永远是「什么都没有」。上游一个恰好裁空的 `crop_box` 会让 `util.merge` 丢掉
    另一侧完好的强度与颜色，还倒打一耙 log 出「只有一侧带 intensity 通道」。
    回归测试：`util.merge：空点云不该把另一侧的通道带走`。

11. **点云缓存的上限比注释说的大八倍，而且不是 LRU。**
    按条数封顶（8 条）而注释按「一份两百万点 24MB」估算，但 `maxPoints` 最大到
    8M，一条就是 96MB + 32MB，八条能攒到 1GB。改成按字节预算（256MB），
    并且切换运行时丢掉旧 runId 的条目。顺带修了 LRU：`Map.set` 命中已有键时
    不调整顺序，所以你来回切着看的那片云恰恰是最先被淘汰的。

12. **`Any` 端口是一把对准 `asCloud()` 的枪。**
    执行器只校验端口**声明的**类型，没校验实际流过来的 `Data::Kind`，
    而 `kindFromTypeName("Any")` 返回 `None`，输出契约检查也不约束它。
    M2 里没有算子声明 `Any` 端口，所以现在打不响；但类型表明确写着它是给
    Reroute 用的，M3 一加 Reroute，`transform.make → reroute → voxel_grid`
    就会让算子里那句 `*inputs.get("cloud").asCloud()` 对着 nullptr 解引用 ——
    在 MSVC 上那是 SEH，`catch (...)` 兜不住。在调 compute 之前补了一次 Kind 检查。
    回归测试用一个只在测试里注册的 `test.any_pass` 算子把这条路走通。

13. **3D 视图每次挂载漏一个 WebGL 上下文。**
    `renderer.dispose()` 不释放上下文（那是 `forceContextLoss`），
    `GridHelper` / `AxesHelper` 的 geometry 与 material 也没释放。
    StrictMode 的双挂载、HMR、将来的面板开合各漏一个，浏览器攒够十几个之后
    开始逐出最老的，表现是视图突然变全黑而且不报错。

### 低

- `io.load_pcd` 里 `blob.width * blob.height` 是 `uint32 × uint32`，
  65536×65536 正好回绕成 0，于是「文件里没有 x/y/z」被当成「文件是空的」
  静默通过。改成先转 `uint64`。
- `adapter::toBlob` 的 `row_step = point_step * width` 同样溢出 `uint32`
  （28 字节步长下 1.53 亿点），而 `data.assign(row_step, 0)` 用的正是它 ——
  底下的写循环会冲出堆缓冲区。改成先在 `uint64` 里算，放不下就抛。
- `jsonNumber` 对非有限值会写出裸的 `nan` / `inf`，那不是合法 JSON。
  查下来这条路今天是断的（nlohmann 在**解析期**就拒绝 `1e400`，
  报 `out_of_range.406`），但序列化侧仍然补上了 `null`，
  并加了一条测试把「路是断的」钉住：换 JSON 库或放宽解析策略时它会立刻响。
- `GraphNode`（TS）少了 `bypass` 字段，而 schema / Rust / C++ 三侧都有。
  任何重建节点对象而不是就地改的路径（复制粘贴、将来的迁移写回）会把它丢掉。

### 审查确认没问题的部分

`RunManager` 的锁与句柄所有权、`RunHandle` / `CloudView` / `Core` 的生命周期、
`ResultStore` 的线程安全、`graph.cpp` / `plan.cpp` 的下标与迭代器安全、
manifest 三侧契约的字段一致性、手写算子的数学（`transform.make` 的
Rz·Ry·Rx composition、`crop_box` 的仿射求逆、`random_sample` 的部分
Fisher-Yates 边界、`data.cpp` 对空云/单点/NaN 的处理）。

---

## 偏离与决策

计划没覆盖、实现时自己拍板的地方。判断依据是 §0 的十条决定和
architecture.md 的职责边界。

### 1. PCL 的 CMake 目标名两种都认

vcpkg 装的 PCL 1.15.1 用的是上游 `PCLConfig.cmake`，造出来的目标叫
`pcl_common` / `pcl_io`，**不是**计划里写的 `PCL::common`。
硬写一种就等着在别人机器上炸，所以 `CMakeLists.txt` 里两种都探，
都没有时退回经典的 `${PCL_LIBRARIES}` + `${PCL_INCLUDE_DIRS}`。

### 2. `bridge/build.rs` 只构建 `lyflow_core` 一个目标

计划里的 `build_target("all")` 会把 `lyflow-dump-manifest` 和 `lyflow-core-tests`
在 cargo 侧再编一遍，而它们归 `scripts/build-core.ps1` 管。
确认过 vcpkg 的 applocal 对 `SHARED_LIBRARY` 目标同样生效
（`vcpkg.cmake` 的 `add_library` 覆写里有 `IS_LIBRARY_SHARED STREQUAL "SHARED_LIBRARY"`），
所以只编 DLL 也拿得到那堆依赖 DLL。

代价见 roadmap 的「已知毛刺」：`pnpm check` 会把 core 编两遍。

### 3. `/EHsc` 必须显式写进编译选项

cmake crate 会整个替换 `CMAKE_CXX_FLAGS`，把 CMake 默认带的 `/EHsc` 顶掉。
后果是 MSVC 静默进入「异常关闭」模式，而 `c_api.cpp` 的每个入口都依赖 try/catch
（绝不让异常跨 ABI）。第一次跑 `cargo build` 时是 doctest 先炸出来的
——「Exceptions are disabled!」。

### 4. Ninja 的查找顺序

cargo 不像开发者的命令行那样跑过 vcvars，PATH 上通常没有 ninja。
`build.rs` 依次尝试 `LYFLOW_NINJA` → PATH → VS 自带的那份（cmake.exe 旁边的
`../Ninja/`）→ vswhere，与 `scripts/build-core.ps1` 找的是同一个，
免得出现「命令行能编、cargo 编不了」。

### 5. 前端不丢弃「还没认领」的执行事件

计划 §8 写的是「`runId !== current` 丢弃」。直接照做会漏事件：
C++ 的 `lyflow_run_start` 是**先起线程再返回句柄**，事件完全可能在
`run_graph` 这个 IPC 调用返回之前就到了前端，那一刻 store 里还没有 runId。
一张小图可能整场运行都跑完了，界面上什么都没发生。

所以改成：认不出 runId 的事件先进 `orphans`（有上限），`beginRun` 时按 runId 认领；
过期 run 的事件自然没人认领，下一次 `beginRun` 清掉。「丢弃过期事件」的语义没变。

### 6. `window.__lyflow` 在正式构建里也在

`app/src/lib/devbridge.ts` 把四个 store、transport 和一份状态变迁流水账挂到
`window` 上，验收脚本靠它读状态。它在 release 构建里也存在 —— 这是有意的：
只在 dev 里有的话，就没法用同一套脚本去验证**安装包**能不能跑通，
而那恰恰是最容易出问题的一条。它只读、不含业务逻辑，应用代码一律不 import 它。

### 7. 「stale」是整图的，不是逐节点的

计划 §8 的 `stale: boolean` 就是整图粒度。实现上订阅 graph store 的 `doc` 引用变化
（每个语义化动作都用 immer 产出新 doc），比在每个 change 动作里手动打标可靠 ——
后者一定会漏掉将来新加的动作。精确到「哪几个节点真的失效」要靠
`run_started.nodes[].cacheKey`，那是 M3。

### 8. `io.save_pcd` 的 `binary_compressed` 走 `PCDWriter::writeBinaryCompressed`

`pcl::io::savePCDFile` 的 `binary_mode=true` 给的是**非压缩**二进制。
压缩要走 `PCDWriter` 的专门入口。

### 9. `segment.ransac_plane` 找不到平面时报 `bad_input` 而不是给空结果

给空 Indices + 退化 Plane 的话，下游会拿到一个语义上无意义的平面继续算，
错误会在离现场很远的地方冒出来。直接报错并把红框标到 `distanceThreshold` 上，
消息里写清楚该往哪个方向调。

### 10. `features.normals` 把邻域不足的点的法线置零并 log warn

PCL 在邻域不足时填 NaN。NaN 流到下游，体素栅格一做平均整片法线就全成了 NaN，
再往下就是 3D 视图黑屏。置零 + 一条 warn 至少是可见的。

### 11. 验收脚本里的 `runAndWait` 先记 runId 再等

连跑两次运行时，按下 F5 的那一刻 `runStatus` 还是上一次的 `ok`，
只等「不是 running」会立刻返回上一次的快照。这个坑第一次跑验收就踩到了，
症状极具迷惑性：画布上明明是红的，断言拿到的却是 `done`。

---

## 这轮抓到的两个真 bug

延续 M1 的记录方式 —— 只记那些**单元测试发现不了**的（代码审查抓到的那 17 条
另见上面「代码审查抓到的缺陷」；那些是读代码读出来的，不是跑出来的）。

1. **3D 视图切换节点时会卡在「正在取点云…」。**
   切换节点会作废上一次的取云请求，而 `setLoading(false)` 写在 `if (!cancelled)`
   里面。切到一个没有点云输出的节点（不发新请求）时，loading 就永远放不下来。
   表现是空态文案永远停在「正在取点云…」，而且只在「先看一个有云的节点、
   再快速切到 ransac_plane」这条路径上出现。
   修法是 `finally` 里无条件 `setLoading(false)`，只有数据写入才看 `cancelled`。
   同时把「云 / 状态 / 它属于哪个节点」合成一个 `display` 状态一起更新 ——
   拆成三个 `useState` 会留下「标题是新节点、点云还是旧节点」的中间态。

2. **验收脚本对着上一次运行的快照做断言。**
   见上面「偏离与决策 11」。这个 bug 在脚本里，但它暴露的是一个真实的
   接口性质：`runStatus` 单独不足以判断「这一次运行结束了」，必须配合 `runId`。
   前端自己的事件过滤也依赖同一个性质，所以值得记下来。
