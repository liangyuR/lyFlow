# 验收：内置算子拆成 packs/std-pointcloud，core 零第三方依赖

对着 [std-pack-plan.md](std-pack-plan.md) §4 逐条走。
决定与理由见 [ADR-0014](adr/0014-std-as-pack-core-zero-dep.md)。

环境：Windows 11、VS 18 (MSVC 14.51)、CMake 4.3 + Ninja、vcpkg `C:\vcpkg`（PCL 1.15.1）、
LyFlow `main` @ 442a315 之上。

## 结论

| # | 验收项 | 状态 |
|---|---|---|
| 1 | 默认构建的 manifest 与改前逐字节相同（只多 `pack`） | ✅ 通过 |
| 2 | `LYFLOW_STD_PACKS=0`：2 个算子、DLL 无第三方依赖、core doctest 全过 | ✅ 通过 |
| 3 | 默认 `pnpm check` / `pnpm e2e` / `pnpm e2e:packaged` 全绿 | ✅ 通过 |
| 4 | 带 gap 包：`pnpm check` 全绿；两条 A/B 39/39 | ✅ 通过 |
| 5 | 热重载：改包里算子的 label，app 换代 | ✅ 通过 |
| 6 | 改 core 头文件的增量重编不再编译任何 PCL TU | ⚠️ 纯平台构建通过，**默认构建不通过** |

---

## 复现命令

```powershell
cd D:\project\LyFlow

# 0. 改前的 manifest 存档（在动手之前先做，存到 build/ 之外）
$b = "$env:TEMP\lyflow-std-pack-baseline"; mkdir $b -Force
cmd /c "build\core\bin\lyflow-dump-manifest.exe > `"$b\manifest-before.json`""

# 1. 默认构建 + 逐字节比对
pnpm core:build
cmd /c "build\core\bin\lyflow-dump-manifest.exe > `"$b\manifest-after.json`""
Get-FileHash "$b\manifest-*.json" -Algorithm SHA256

# 2. 纯平台构建（另起一个 build 目录，不动默认那份）
cmake -S core -B build\core-nopacks -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
      -DLYFLOW_OP_PACKS="" -DLYFLOW_STD_PACKS="0"      # 要在 vcvars64 之后
cmake --build build\core-nopacks
build\core-nopacks\bin\lyflow-dump-manifest.exe --check
dumpbin /dependents build\core-nopacks\bin\lyflow_core.dll
build\core-nopacks\bin\lyflow-core-tests.exe

# 3. 全链路
pnpm check; pnpm e2e; pnpm tauri build; pnpm e2e:packaged

# 4. 带 gap 包
$env:LYFLOW_OP_PACKS = "D:\project\xyz-gap-inspector\lyflow"
pnpm check
# 两条 A/B 见 docs/gap-acceptance.md「复现命令（第二部分）」第 2、3 步
```

---

## 1. 默认构建的 manifest 与改前逐字节相同

改前存档取自动手之前的 `build/core/bin/lyflow-dump-manifest.exe`（不带包的 16 算子版本），
用 `cmd /c "… > 文件"` 落盘以避开 PowerShell 的重编码。

```
改前  SHA256 D5FA5D7C591736A88477D85B01254CDD7C5A99019FBDFE725A78411A83B3B4D7   31841 bytes
改后  SHA256 D05ED234BA31C61BAE392242FD496B082C8C032649785646903028E5CF9DAC5B   32373 bytes
```

行级 diff（`difflib.ndiff`）：

```
差异行数 14
Counter({'+       "pack": "std-pointcloud@0.1.0",': 14})
```

**14 行全部是新增的 `pack` 字段，一行都没有删除或修改。** 每个包内算子恰好一行，
`gen.synthetic` 与 `util.reroute` 没有这一项（它们不属于任何包）。
算子顺序、id、版本、参数、分类、文案、端口、capabilities 全部逐字节不变。

顺序之所以能保住，是因为 `registerBuiltinOps` 写成了夹心
（`gen.synthetic` → 标准包 → `util.reroute` → 外部包），见 ADR-0014。
带 gap 包时 37 个算子的顺序也与改前一致：`gap.*` 仍然排在 `util.reroute` 之后。

**通过。**

## 2. `LYFLOW_STD_PACKS=0`：纯平台构建

```
> build\core-nopacks\bin\lyflow-dump-manifest.exe --check
ok: 2 operator(s), 11 port type(s)

> build\core-nopacks\bin\lyflow-dump-manifest.exe | Select-String '"id"'
      "id": "gen.synthetic",
      "id": "util.reroute",
```

`bin/` 目录里的 DLL：

```
lyflow_core.dll        （就这一个）
```

`dumpbin /dependents`：

```
Dump of file build\core-nopacks\bin\lyflow_core.dll
    KERNEL32.dll
    MSVCP140.dll
    VCRUNTIME140.dll
    VCRUNTIME140_1.dll
    api-ms-win-crt-{heap,string,locale,math,stdio,time,runtime,convert,filesystem}-l1-1-0.dll
```

没有 `pcl_*`、没有 `boost_*`、没有 `flann`、没有 `lz4`。
对照默认构建的同一条命令：

```
Dump of file build\core\bin\lyflow_core.dll
    pcl_io.dll  pcl_segmentation.dll  pcl_features.dll
    pcl_search.dll  pcl_filters.dll  pcl_common.dll
    KERNEL32.dll  ADVAPI32.dll  MSVCP140.dll  …
```

core doctest：

```
[doctest] test cases:   74 |   74 passed | 0 failed | 0 skipped
[doctest] assertions: 2747 | 2747 passed | 0 failed |
```

**通过。**

## 3. 默认构建的全链路

`pnpm check`：

```
=== C++ core ===              ok: 16 operator(s), 11 port type(s)
                              [doctest] test cases: 88 | 88 passed | 0 failed
=== manifest vs schema ===    符合 schema/operator-manifest.schema.json
=== execution-event vs schema ===  ok
=== graph-doc vs schema ===   ok
=== Rust bridge ===           53 passed; 0 failed
=== headless CLI（不带 Tauri）===  算子描述自检干净
=== frontend ===              tsc --noEmit && vite build ✓
全链路绿
```

88 个 doctest = core 的 74 个 + 标准包的 14 个（`test_std_ops.cpp` 10 条 +
`test_io_pcd.cpp` 4 条）。

`pnpm e2e`：**304/304 项通过，全绿**。
`pnpm e2e:packaged`：**308/308 项通过，全绿**（`tauri build` 之后）。

**通过。**

> 中途踩到一个与本次改动无关的坑，记在这里省得下次再查一遍：
> `bridge/tauri.conf.json` 的 `bundle.resources` 是 `../build/core/bin/*.dll`，
> 而 gap 包用 `file(COPY ...)` 塞进那个目录的 `onnxruntime.dll` / `yaml-cpp.dll`
> **不会因为下次不带包构建就消失**。带 gap 跑过之后直接 `pnpm tauri build`，
> 安装包里就会多出这三个 DLL，`e2e:packaged` 的「不带算子包时没有 onnxruntime」
> 那一条会响。换模式打包前先把它们从 `build/core/bin` 和
> `bridge/target/release` 删掉。已记进 docs/op-packs.md 与 roadmap 的已知毛刺。

## 4. 带 gap 包

`$env:LYFLOW_OP_PACKS = "D:\project\xyz-gap-inspector\lyflow"; pnpm check`：

```
=== C++ core ===   ok: 37 operator(s), 11 port type(s)
                   [doctest] 全过
…
全链路绿
```

37 = core 2 + std-pointcloud 14 + gap 21。算子顺序与改前完全一致。

gap 包这一侧只改了两处（`lyflow-ops` 分支）：
`LINK` 里加 `lyflow_pcl_support`（不再蹭 core 的 `find_package(PCL)`）、
加一条 `if(NOT TARGET lyflow_pcl_support)` 的 configure 期护栏。
`INCLUDES` 顺序不变（仓库 `src/` 在前、gap 侧 vcpkg 在后），`gap_pch.h` 照旧 ——
`lyflow_op_pack` 现在每个包一个对象库，所以标准包的 `pcl_pch.h` 和它互不干扰。
gap 的 ops 里没有一处 `#include "ops/pcl/..."`，所以没有 include 要改。

**模型 A/B**（`--model v12s0.onnx`，基线 `%TEMP%\lyflow-gap-baseline-model`）：

```
一致 39 / 39；最大 |Δ| = 0.000000 mm
现场值（manifest.csv，容差 0.006 mm）：41 / 41 个数值一致；
最大 |Δ| = 0.00499 mm（KUN10_HXMK2F118TA253680_L4_29 gap）
```

**模板 A/B 回归**（不带 `--model`，基线 `%TEMP%\lyflow-gap-baseline`）：

```
一致 39 / 39；最大 |Δ| = 0.000000 mm
现场值（manifest.csv，容差 0.006 mm）：0 / 31 个数值一致；
最大 |Δ| = 2.75261 mm（KUN10_HXMK2A128TA237796_L3_8 gap）
```

两条都与 [gap-acceptance.md](gap-acceptance.md) §8「最终数字」逐字相同 ——
模板路径的现场值 0/31 是既有结论（现场跑的不是模板路径，见那份文档的 §7），
不是本次改动引入的。

**通过。**

## 5. 热重载

把 `packs/std-pointcloud/ops/filter_voxel_grid.cpp` 的
`op.label = "Voxel Grid";` 改成 `"Voxel Grid (hot)";`，
`scripts/core-watch.ps1` 在后台跑，app 是 `pnpm tauri dev`：

```
core-watch：盯着 D:\project\LyFlow\core、D:\project\LyFlow\packs
[core-watch] 检测到改动，增量构建…
[1/3] Building CXX object …\lyflow_pack_std_pointcloud.dir\…\filter_voxel_grid.cpp.obj
[2/3] Linking CXX shared library bin\lyflow_core.dll
[core-watch] 构建完成，等 app 热重载
```

app 侧（CDP 读 `window.__lyflow`）：

```json
{
  "before": { "label": "Voxel Grid",       "count": 16, "generation": 0 },
  "elapsedSec": "6.1",
  "after":  { "label": "Voxel Grid (hot)", "count": 16, "generation": 1 },
  "consoleErrors": []
}
```

改一个算子的 label → 6.1 秒后 app 换代、面板里的名字变了、没有控制台报错。
只重编了 1 个 TU —— 包的对象库带 PCH，改一个算子不牵连别的。
探针跑完把文件改回去了，`op.label = "Voxel Grid";` 已复原。

**通过。**

## 6. core 头文件改动的增量重编

在 `core/include/lyflow/status.h` 末尾加一行注释，两个 build 目录各跑一次
`cmake --build`，数 `Building CXX` 的行数：

**纯平台构建 `build/core-nopacks`：20 个 TU，没有一个 PCL TU**（也没有 PCL TU 可编）：

```
status.cpp  cloud_io.cpp  dump_manifest.cpp  op_packs.cpp  manifest.cpp
builtin_ops.cpp  operator.cpp  util_reroute.cpp  c_api.cpp  registry.cpp
gen_synthetic.cpp  subgraph.cpp  executor.cpp  library.cpp  graph.cpp  plan.cpp
test_plan.cpp  test_subgraph.cpp  test_cache.cpp  test_executor.cpp
```

**默认构建 `build/core`：标准包的 14 个 TU 全部重编**：

```
register.cpp  filter_statistical_outlier.cpp  util_merge.cpp  filter_radius_outlier.cpp
io_load_pcd.cpp  io_save_pcd.cpp  filter_crop_box.cpp  segment_ransac_plane.cpp
transform_apply.cpp  transform_make.cpp  filter_passthrough.cpp
segment_extract_indices.cpp  filter_voxel_grid.cpp  features_normals.cpp
filter_random_sample.cpp   （外加 core 自己的 16 个 TU）
```

**这一条在默认构建下不通过，如实记下来。**

原因很直接：包里每个算子都 `#include "ops.h"` → `lyflow/registry.h` →
`lyflow/manifest.h` → `lyflow/status.h`。拆包不改变这条 include 链，
所以碰 core 的公共头一定会牵连所有算子的 TU —— 拆包之前也是这样（那时是
`lyflow_core_pcl` 的 6 个 PCL TU + 8 个手写算子 TU，同一批文件）。
**这一条没有变好，也没有变坏。**

零依赖真正买到的是另外两件事，它们都验到了：

- 不装 PCL 也能构建、测试、跑 core 本身（第 2 条）。这条路上「改 core 头文件」
  确实一个 PCL TU 都不编，因为一个都不存在。
- 包的 PCH 现在归包自己。改 `pcl_pch.h` 只影响标准包，改 `gap_pch.h` 只影响 gap 包。

要让默认构建也满足「改 core 头文件不重编包」，得在 core 与包之间插一层
**版本稳定的算子 ABI**，那正是 ADR-0013 权衡后排除掉的方案 1。
不为这一条把它请回来。已记进 [roadmap.md](roadmap.md) 的已知毛刺。

**未通过（默认构建）／通过（`LYFLOW_STD_PACKS=0`）。**

---

## 计划没覆盖、自己决定的地方

1. **注册顺序做成夹心。** 计划 §1 的 S5 要求 manifest 逐字节相同，而 ADR-0013
   说「包排在内置之后」。两者不能同时成立 —— 拆走的 14 个算子原本夹在
   `gen.synthetic` 和 `util.reroute` 中间。选择保 S5：
   `gen.synthetic → 标准包 → util.reroute → 外部包`，并把
   `registerOpPacks` 拆成 `registerStdPacks` / `registerExternalPacks` 两个生成函数。
   副作用是外部包（gap）的注册位置也一字不动，A/B 因此完全没有顺序扰动。

2. **`pack` 字段是字符串 `名字@版本`**，不是嵌套对象。理由：manifest 是逐行
   diff 的对象，一行一个字段最省事；前端不解释它，将来要结构化再走 schema 版本。
   `lyflow_op_pack` 因此多了一个可选的 `VERSION`。core 自带的两个算子这一项为空，
   `fieldIfSet` 直接不输出 —— 这样「没有 pack 就是平台自带」是一条可读的规则。

3. **包名的连字符。** 计划 §2 写的是 `NAME std-pointcloud`，但包名要进 C++
   命名空间。生成注册入口时过一遍 `string(MAKE_C_IDENTIFIER)`：目录与 `pack`
   字段仍是 `std-pointcloud`，命名空间是 `lyflow::packs::std_pointcloud`。

4. **每包一个对象库。** 计划 §3 允许「放开单 PCH 限制」或「gap 复用标准包的 PCH」。
   选了前者：`lyflow_op_pack` 现在给每个包建一个 `lyflow_pack_<ident>` OBJECT 库，
   PCH / DEFINES / OPTIONS / INCLUDES 都是包自己的。后者做不到 —— gap 的 PCH 里有
   领域头（`GapUtils.hpp`、`DetectionTypes.hpp`），标准包不该认识它们。

5. **写盘钩子 `lyflow/cloud_io.h`。** 计划没提 `lyflow_output_save`。它在 core 的
   C ABI 里，实现却是 `ops/pcl/io_save_pcd.cpp` 的 `saveCloudToFile`。
   做成函数指针钩子：标准包在 `registerPackOps` 里 `setCloudWriter`，
   没有包装写盘实现时返回 `unsupported`。把 PCD/PLY 的读写抄进 core 等于把 PCL 拉回来。

6. **`Ticker` 升到公共头。** 它原本在 `core/src/ops/ops.h`（`lyflow::ops` 命名空间），
   而包要用它。搬进 `lyflow/operator.h` 的 `lyflow` 命名空间 —— 算子都写在
   `lyflow::ops` 或 `lyflow::packs::*` 下，非限定的 `Ticker` 照样找得到，
   所有算子源文件一个字没改。

7. **core 的测试怎么摆脱包**（计划 S6 说「用 `gen.synthetic` 与 `test.*`」，
   但仓库里原本没有 `test.*` 这套东西）。做法：
   - 新增 `core/tests/test_ops.h`，注册 9 个只在测试进程里存在的算子
     （`test.thin` 抽稀、`test.merge2` 双输入、`test.split` 按下标切、
     `test.half_indices` 产 Indices、`test.sink` 纯副作用、`test.migrated` 带迁移链，
     外加原先散在各文件里的 `test.block` / `test.any_pass` / `test.sleep`）。
     它们不进 manifest —— `lyflow-dump-manifest` 不链测试代码。
   - `test_executor` / `test_cache` / `test_plan` / `test_subgraph` 里凡是「测执行器
     而恰好用了点云算子」的用例，换成对应的 `test.*`，断言一条没删。
   - 真正测算法的用例随算子搬进 `packs/std-pointcloud/tests/test_std_ops.cpp`
     （完整 pipeline、体素键越界、体素 nearest 的取消、`util.merge` 的空云通道、
     `crop_box` 的可选输入、`io.load_pcd` 的空 path、`filter.random_sample` 的迁移链）
     与 `test_io_pcd.cpp`（中文路径 PCD 往返，原样搬）。
   - `test_plan.cpp` 里「15 个算子都在」那条拆成两半：core 那份只断言
     `gen.synthetic` / `util.reroute` 且 `pack` 为空；14 个算子的清单进包的测试，
     顺带断言每个都带 `pack == "std-pointcloud@0.1.0"`。

8. **空串按「没设」处理。** `-DLYFLOW_STD_PACKS=""` 与
   `$env:LYFLOW_STD_PACKS=""` 都不该悄悄变成「关掉标准包」（PowerShell 里
   `$env:X=""` 留下的是空串而不是删除，第一遍 `pnpm check` 就栽在这上面 ——
   bridge 拿到 2 个算子的 DLL，24 个 cargo 测试全红）。CMake 与 `build.rs` 两侧
   都把空串当默认值。

9. **`core/src/packs/placeholder.cpp` 删掉了。** 它存在的理由是「不带包时
   `lyflow_core_packs` 是空对象库会 LNK4221」。现在没有共享的包对象库了，
   没有包就一个 `lyflow_pack_*` 目标都不建。

10. **`set_property(CMAKE_CONFIGURE_DEPENDS <包的 cmake>)` 去掉了。**
    `include()` 本来就会把它登记成 configure 依赖，再显式登记一遍会让 ninja 报
    `is defined as an output multiple times`（标准包的路径经 `packs/*` glob 拿到，
    与 `include()` 规范化后的那份撞车）。包里新增 `.cpp` 靠包自己的
    `file(GLOB CONFIGURE_DEPENDS)`，行为不变。

11. **`app/src/types/manifest.ts` 加了一行 `pack?: string`。** 计划说「前端忽略即可」，
    但三层契约的 TS 侧不写出来就是个隐形字段。只加类型，没有任何代码读它。

## 没做的

- 没有动任何算子的实现、id、参数、文案。`git diff -M` 里 14 个 `.cpp` 全部是
  纯重命名（`R`），只有 6 个 PCL 算子 + adapter + pcl_path 有内容改动，
  且都只是 include 路径（`ops/pcl/xxx.h` → `lyflow_pcl/xxx.h`、`ops/ops.h` → `ops.h`）。
- 没有改 `scripts/e2e`、CLI 测试、schema 样例、图生成器 —— S5 成立的直接证据。
- `cargo test` 的已知并发不稳定（gap-acceptance.md 偏离第 8 条）本轮没有复现，
  也没有去动它。
