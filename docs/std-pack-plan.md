# 内置算子拆成标准算子包 —— LyFlow = 平台 + 标准包

目标：core 成为零第三方依赖的纯平台；现在内置的点云算子整体搬进仓库内的标准算子包
`packs/std-pointcloud/`，与外部领域包（gap）走同一套 `lyflow_op_pack` 机制。用户视角零变化：
默认构建里算子 id、参数、分类、manifest 一字不差。

## 1. 定死的决定

| # | 决定 | 理由 |
|---|---|---|
| S1 | core 只保留 `gen.synthetic`（自测与演示数据源）和 `util.reroute`（编辑器语义，P1 #24）。其余 14 个算子进 `packs/std-pointcloud/` | 合成点云是测试基础设施；reroute 是平台特性不是算法 |
| S2 | core **零 PCL 链接**，不只是零 PCL 头。PCL 的 find_package、链接、PCH、`adapter.*`、`pcl_path.*` 全部归标准包 | D2 的完成态；改一行 core 头文件不再触发 PCL 相关重编 |
| S3 | 标准包对外提供一个 INTERFACE 目标 `lyflow_pcl_support`（include 目录 + PCL 链接 + PCH 头），其他需要 PCL 的包链接它，不各自 find_package | gap 包现在 `#include "ops/pcl/adapter.h"`，搬家后从这里拿；一份 PCL 配置 |
| S4 | 仓库内的包默认启用：CMake 选项 `LYFLOW_STD_PACKS`（默认 ON）自动加入 `packs/*`；`LYFLOW_OP_PACKS` 仍是外部包列表，追加在后。环境变量 `LYFLOW_STD_PACKS=0` 关闭 | 默认构建开箱可用；纯平台构建可验证 |
| S5 | 算子 id、版本、参数、分类、文案全部不变；`manifest` 逐字节相同 | 老图、e2e、CLI 脚本零改动 |
| S6 | 包内算子的 doctest 随包走（`TEST_SOURCES`）；core 的测试只测执行器、类型、缓存、子图，用 `gen.synthetic` 与 `test.*` 测试算子 | 核心测试不依赖任何包 |
| S7 | 包有自己的版本号与 README，manifest 里每个算子带 `pack` 字段（包名 + 版本） | 为后续按包发版留口子；前端不解释它 |

ADR-0014：std-as-pack-core-zero-dep。

## 2. 目录

```
packs/std-pointcloud/
  lyflow_op_pack.cmake        NAME std-pointcloud, SOURCES ops/*.cpp, TEST_SOURCES tests/*.cpp,
                              PCH pcl_pch.h, 定义 lyflow_pcl_support INTERFACE 目标
  include/lyflow_pcl/adapter.h, pcl_path.h, pcl_pch.h   （从 core/src/ops/pcl/ 迁来，命名空间不变）
  ops/*.cpp                   14 个算子 + register.cpp（registerPackOps）
  tests/*.cpp                 从 core/tests 迁出的算子测试（体素、IO、PCD 往返、滤波等）
  README.md                   算子表 + 版本
core/src/ops/                 只剩 gen_synthetic.cpp、util_reroute.cpp、ops.h
core/CMakeLists.txt           不再 find_package(PCL)；不再有 ops/pcl/
```

## 3. 需要同步的地方

- `bridge/build.rs`、`scripts/build-core.ps1`、`scripts/check.ps1`、`scripts/core-watch.ps1`：透传 `LYFLOW_STD_PACKS`；core-watch 同时监视 `packs/`。
- vcpkg applocal：PCL 的 DLL 现在由标准包的目标带出，确认 `bin/` 里 DLL 集合不变；`LYFLOW_STD_PACKS=0` 时 `bin/` 里没有 `pcl_*.dll`。
- xyz-gap-inspector 的 `lyflow/lyflow_op_pack.cmake` 与 ops：include 改为 `lyflow_pcl/adapter.h`，链接 `lyflow_pcl_support`，PCH 复用（`lyflow_op_pack` 的单 PCH 限制要么放开为「每包一个 PCH」，要么 gap 包直接用 `lyflow_pcl_support` 带的 PCH）。提交到 `lyflow-ops` 分支。
- 文档：`docs/op-packs.md`（默认包与外部包）、`docs/architecture.md`（职责表加「标准包」一行）、`core/README.md`（零依赖、加算子的流程改为「进 std 包或建新包」）、`docs/roadmap.md` 已知毛刺。
- `scripts/e2e`、CLI 测试、schema 样例：不该需要改；改了就说明 S5 破了。

## 4. 验收

- [ ] 默认构建：`lyflow-dump-manifest` 输出与改前**逐字节相同**（改前先 dump 一份存档比对；`pack` 字段若加入则只允许这一处差异）
- [ ] `LYFLOW_STD_PACKS=0`：manifest 只有 `gen.synthetic`、`util.reroute`；`lyflow_core.dll` 的导入表没有任何 `pcl_*`、`boost_*`、`flann`、`lz4`（`dumpbin /dependents`）；core doctest 全过
- [ ] 默认 `pnpm check` 全绿；`pnpm e2e` 全绿；`pnpm e2e:packaged` 全绿
- [ ] 带 gap 包：`pnpm check` 全绿；模板与模型两条 A/B 39/39
- [ ] 热重载：改 `packs/std-pointcloud/ops/filter_voxel_grid.cpp` 的 label，app 换代
- [ ] core 头文件改动的增量重编不再编译任何 PCL TU（改 `core/include/lyflow/status.h` 加一行注释，观察 ninja 重编列表）

## 5. 不做

- 运行时插件 DLL、按包独立发版、Image 域包。
