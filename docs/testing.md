# 测试地图

[文档首页](README.md) · [环境准备](getting-started.md) · [Windows 发包](deployment/packaging.md)

本页说明当前验证入口和覆盖边界，不记录某次修订的通过结论。测试数量、跳过项、耗时及产物身份，以对应提交的实际输出为准。

## 选择验证入口

从仓库根目录使用 PowerShell 7 执行；依赖准备见 [快速开始](getting-started.md)。原生检查按 `LYFLOW_STD_PACKS`、`LYFLOW_PACKS` 选择的包组合执行，不能用纯平台结果替代标准包或领域包验收。

| 要验证的内容 | 命令 | 范围与前提 |
|---|---|---|
| 完整开发门禁 | `pnpm check` | core、manifest、schema、Rust bridge/客户端、CLI、C++ SDK、前端与 MCP。 |
| 选择门禁步骤 | `pwsh -NoProfile -File scripts/check.ps1 -Steps frontend,mcp` | 可选步骤为 core、manifest、schema、bridge、client、cli、sdk、frontend、mcp；不给 `-Steps` 则全部运行。 |
| C++ core 与算子包 | `pnpm core:build` | 编译并运行 doctest；算法数值、执行语义和 C ABI 契约的主要入口。 |
| Tensor 元信息微基准 | `pwsh -NoProfile -File scripts/bench-tensor-metadata.ps1` | 对不可变张量首次与重复读取 `valueJson()` 计时，不代表整图耗时。 |
| 编辑器纯逻辑 | `pnpm --filter @lyflow/editor test` | 当前按 25 个文件、96 个功能契约用例组织，包含工作区与 PythonNode；不启动桌面壳。 |
| 编辑器类型 | `pnpm --filter @lyflow/editor typecheck` | 同时检查 React 18 与 React 19 类型契约。 |
| 共享可视化 | `pnpm --filter @lyflow/visualization test` | 图像、点云等共享可视化逻辑。 |
| Python SDK 与 worker | `python -B -m unittest discover -s python/tests -v` | Python 3.11 及以上与 NumPy；检查节点发现、数据交换和 worker 行为。 |
| MCP 单元测试 | `pnpm --filter @lyflow/mcp test:unit` | 不含需要 CLI 的集成冒烟；完整入口为 `pnpm --filter @lyflow/mcp test`。 |
| glue / gap 门禁 | `pnpm check:glue` / `pnpm check:gap` | 运行相应包的专项检查；真实数据和历史对拍需额外配置，不能把人造帧结果当现场结论。 |
| 桌面交互 | `pnpm e2e` | CDP 驱动真实 Tauri/WebView2；完整领域场景需带 `gap;dts`。 |
| 桌面构建产物 | `pnpm e2e:packaged` | 同一套桌面交互断言验证构建产物，不等于运行库/SDK 发包验证。 |
| 浏览器宿主 | `pnpm e2e:http` | HTTP 传输、结果取数与 React 宿主链路。 |
| Windows 发包 | `pnpm run package` | 固定标准包、solution-glue、solution-gap、solution-dts，生成运行库/SDK ZIP、编辑器 tarball 和正式运行库 NuGet 包；详见 [发包契约](deployment/packaging.md)。 |

桌面完整验收及按分组复验：

```powershell
$env:LYFLOW_PACKS = 'gap;dts'
pnpm e2e
pnpm e2e --only m3:suitePanels,params_p4
```

选择规则、夹具及原生窗口驱动见 [E2E 脚本说明](../scripts/e2e/README.md)。切换包组合后要重新构建 core；已有 DLL 不会因为环境变量改变而自动拥有新算子。

## CI 的实际范围

[ci.yml](../.github/workflows/ci.yml) 在 PR、main push 和手动运行时执行以下七个 job（并行），只做格式检查与单元测试，目标是普通 PR 总耗时 5 分钟内。`fast` 是汇总检查，实际编译与单测由 `frontend`、`client` 执行。

| job | runner | 内容 |
|---|---|---|
| `quality` | ubuntu-24.04 | actionlint 1.7.12 检查 workflow；Prettier 3.6.2 检查前端、MCP 和配置；clang-format 20.1.8 检查跟踪的 C/C++ 源码。 |
| `client` | windows-2022 | 运行库 NuGet 契约测试、bridge/客户端/运行库探针的 Rust 格式检查、独立 Rust 客户端编译与单测。 |
| `frontend` | windows-2022 | 编辑器 React 18/19 类型、桌面与 React 宿主构建、编辑器及共享可视化单测、MCP 类型/构建/单测。 |
| `fast` | ubuntu-24.04 | 要求 `frontend` 与 `client` 都成功，不重复运行其测试。 |
| `native` | windows-2022 | 纯平台 + dts 的 CMake 编译及 CTest；Python 3.12 与 NumPy 2.2.6 的 SDK/worker 测试；M2 轻量 SDK 安装与公共核消费。 |
| `rust` | windows-2022 | 自行编译同组合的 `lyflow_core`（不含测试），再做 Rust 桌面/CLI 编译、lib/bins 单测及 `python_nodes`、`profiles`、`host` 集成测试。 |
| `vision` | ubuntu-24.04 | 无 PCL/ORT 的值、几何、图像、度量、Tensor 解码与涂胶场景包编译、算子自检及 CTest。 |

`native`、`rust` 与 `vision` 用 sccache 缓存编译结果，因此关闭了预编译头；源文件必须自己包含所需头文件，不能依赖 PCH。

CTest 的一个条目执行该构建组合的全部 doctest，条目数不是单元测试例数。Rust 使用 `LYFLOW_PREBUILT_CORE_DIR` 复用本 job 编出的 core。纯平台 + dts 不依赖标准包的 vcpkg 端口，需要标准包的既有 Rust 用例会标记 ignored；这不证明标准算子包已验证。

日常 CI 不编译依赖 PCL/ORT 的组合（std-pointcloud、std-ml 推理、solution-gap），也不执行 P1/P4 真实模型链、SDK 安装消费、CLI 交付契约、整套桌面/浏览器 e2e、完整真实数据评估或正式发包。改动这些包或交付链时，合并前须在本地运行对应入口（如 `pnpm check:gap`、`pnpm check:glue`，或带全部包与模型测试选项的 `scripts/build-core.ps1`），并在 PR 描述中记录结果。完整 `pnpm check` 与各专项入口仍应按改动范围运行。

失败诊断上传到 Actions artifact，保留 7 天：前端、MCP、Rust 单测日志，以及 native 的 CTest JUnit、`Testing/Temporary/*.log` 和 CMake 配置日志。格式和编译错误也可直接查看失败步骤输出。

[Windows release](../.github/workflows/release-windows.yml) 仅在 `v*` tag 或手动触发时运行，日常 CI 不代替发布门禁。运行库版本、ABI、源码修订、资产检查和下游消费边界以 [发包说明](deployment/packaging.md) 为准。LyFlow 的软件检查也不等于 GlueSight 或其他宿主的现场验收。

## 测试归属与现有入口

优先在已经拥有该功能契约的用例里增加表项或断言，不为同一判据在不同层重复建测试。数值判据放在能覆盖它的最低一层（内核 → 包 → 场景）；发布、SDK 与 e2e 层只验结构、身份绑定、覆盖生效与跨接口一致，不再断言内点数、槽号或读数。

| 行为 | 主要位置 |
|---|---|
| 公共内核数值（几何含二维折线重采样、度量、Profile 拟合与尺寸） | `kernels/tests/`（`lyflow-kernel-tests`）；不在包测试或发布契约里重复这些判据。 |
| 算法数值、端口数据、C ABI、执行计划、缓存、取消与子图 | `core/tests/`、各 `packs/*/tests/`。领域数值测试保留原判据和容差。 |
| 图像响应、法向候选与颜色模型 | `packs/std-image/tests/test_line_scan.cpp`、`packs/std-image/tests/test_color_model.cpp`。 |
| 标准算子组合与旧图拆分 | `packs/std-pointcloud/tests/test_p2_height_rules.cpp`、`test_p2_advanced_fit.cpp`、`test_p5_primitive_crop.cpp` 及 `packs/std-image/tests/test_p5_inspection.cpp`。节点拆分公共规则在 `core/tests/test_graph_migration.cpp`。 |
| 涂胶搜索、示教、灰度/彩色测量与飞拍场景 | `solutions/glue/tests/`，涵盖随包彩色、灰度和飞拍图。 |
| 宿主注入（整节点与输入注入、只给部分输出、注入进缓存键） | `core/tests/test_flow.cpp`；Profile 的 C ABI 注入在 `core/tests/test_profile_c_api.cpp`。 |
| Gap/DTS 场景、导入器及退役诊断 | `solutions/gap/tests/`、`solutions/dts/tests/`；StandardGap 导入图由宿主注入两路 Profile 运行在 `solutions/gap/tests/test_import_bundle.cpp`。 |
| 锚框窗口、重叠高度观测和公差比较 | 几何计算在 `kernels/tests/test_geometry.cpp`，窗口空间检查在 `packs/std-geometry/tests/test_geometry.cpp`，观测在 `packs/std-profile/tests/test_compare_overlap.cpp`，比较在 `packs/std-metrology/tests/test_metrology.cpp`。 |
| 安装后的 CLI/SDK 交付契约（EXACT 版本、链接与依赖闭包、覆盖生效与还原、身份绑定、CLI 与嵌入宿主一致） | `tests/release/*_contract.py`、`tests/release/p5_sdk_consumer/`、`bridge/tests/p6_delivery_contract.py`；产品版本取自 `scripts/release-common.ps1` 的 `Get-ReleaseVersion` 等单一来源，不在测试里写死。对应数值判据在 `core/tests/integration/p4_recognition.cpp`、`p5_depth_geometry.cpp` 及各包测试。 |
| 算子分包清单（字段、Python 示例 ID、与 manifest 的 ID 及包归属一致） | `scripts/tests/test_operator_package_audit.py` 测检查脚本本身；CI 的 `quality` 检查清单，`native`、`vision` 用各自 manifest 以 `--subset` 核对。 |
| Rust 封送、IPC、命令行选项/退出码、宿主及跨进程缓存 | `bridge/src/*/tests.rs`、`bridge/tests/`；客户端在 `crates/lyflow-client/`。 |
| 图动作、撤销、端口兼容、自动连线、参数、配方与搜索 | `packages/editor/test/graph-params*.test.mjs`、`typecheck.test.mjs`、`param-values.test.mjs`、`recipes.test.mjs`、`search.test.mjs`。 |
| 标签快照、异步操作归属、宿主文档通知及备份恢复 | `packages/editor/test/flow-tabs.test.mjs`、`flow-backup.test.mjs`、`autosave.test.mjs`。 |
| 执行事件、正式判定、结果显示与预览 | `packages/editor/test/execution-store.test.mjs`、`view-rule.test.mjs`。 |
| Python 发现、数据校验、异常与 worker；引擎及宿主接入 | `python/tests/`、`core/tests/test_python.cpp`、`bridge/tests/python_nodes.rs`、编辑器 Python 场景。 |
| 真鼠标、键盘、弹窗、焦点、窗口行为与渲染 | `scripts/e2e/`；共享动作位于 `page.mjs`，CDP 连接位于 `cdp.mjs`。 |
| 图像、点云等取数与展示的跨端契约 | `packages/visualization/`、`packages/mcp/` 及 [MCP 可视化说明](mcp-visualization.md)。 |

跨语言规则优先扩充 `schema/fixtures/recipes/`、`schema/examples/` 等共享夹具；全类型参数示例位于 `examples/param-showcase.lyflow.json`。C++ 辅助在 `core/tests/helpers.h`，CLI 测试辅助由 `bridge/src/cli.rs` 的 `test_support` 提供。

## 结果应如何解释

- 同时记录源码修订、包组合、依赖/数据前提、执行入口和失败或跳过项。旧提交的通过记录不能代替当前代码验证。
- 缺包、缺模型、缺数据要明确报失败或说明跳过，不能让分组提前返回后仍显示全部通过。
- e2e 的启动、等待及必要前提用 `mustOk`；行为断言要说明真正验证的结果，避免只断言元素存在。
- 现场精度、标定单位、客户数据效果及产品节拍，需要对应宿主和数据的独立验证。基准、模拟结果或历史算法对拍不能自动转成现场结论。

## P1 三维与真实模型门禁

CI 的 `vision` 在 Linux 运行无PCL的值/几何/位姿/图像包。三维点云/配准和真实YOLOv8、YOLO11、YOLO26、YOLOX检测回归不在 CI 中运行，须在本地使用发布组合同一固定PCL 1.15.1与OpenCV/ORT依赖执行，不降级PCL版本或静默跳过重依赖。

本地执行 `python scripts/fetch-p1-model-assets.py` 并按[示例说明](../examples/p1-detection-README.md)导出三种YOLO模型，配置 `-DLYFLOW_P1_MODEL_TESTS=ON`、`LYFLOW_P1_ASSETS=<资产目录>`。此选项默认关闭以保留离线开发；显式开启后缺模型或图片即失败。原有推理夹具另由 `tests/release/make_fixtures.py --output-dir build/p1-fixtures` 生成，测试时设置 `LYFLOW_TEST_ONNX_MODEL` 指向 `channels-6-to-8.onnx`。不使用“未配置模型”的日志代替真实推理验证。

检测门禁比较执行器、C ABI和动态C++ SDK输出，对公开人像、猫图和空背景验证类别与有效空集合；点云/位姿有完整组合图与失败门限回归。公开数据验证不等于客户模型质量或现场节拍验收。

若本地或发布构建在编译前报固定依赖缺包，先比对失败诊断中的 `environment.json` 和 `*abi_info.txt`。Windows runner 更新可能替换相同 MSVC toolset 目录中的编译器文件，改变 vcpkg 的实际编译器身份及整张依赖图 ABI；固定目录名不能证明既有二进制缓存仍适用。此时在可信 main 上运行现有 `Windows release` 手动入口，设置 `rebuild_deps=true`、`dependencies_only=true`，仅预热依赖并空目录验证 NuGet 恢复，再重跑失败的构建。不要降低PCL版本或跳过P1测试。


## P2 标定、形状与稳定依赖门禁

P3永久回归位于std-image的test_p3_markers/workspace/shape_matching.cpp，跨接口组合位于core/tests/integration/p3_calibration.cpp与p3_shape.cpp。形状用真实大于4MP的源图触发Preview，检查ROI→Box2D→模型/图像尺度和原始坐标；手工设置Image.scale的直接compute测试不能替代该调度链。Marker与工作空间分别检查相机K/D、米制帧、双向体积及原生精度。

默认交付依赖来源以[稳定版本审计](dependency-stable-audit.md)为准。OpenCV5.1的Windows和LinuxCI使用同一固定源码与最小profile；Windows发布仍锁定14.44工具链。源码校验、受管安装记录及licenses全部通过后才能跳过编译，命中缓存名称本身不代表合格。Rust/C++加载契约用真实依赖WINMM的独立DLL检查System32解析，运行库的其他DLL仍不能从CWD/PATH补齐。

## P2 分割、OCR、识码与标注门禁

`-DLYFLOW_P4_MODEL_TESTS=ON`要求std-image和std-ml同时启用。先运行 `python scripts/fetch-p4-model-assets.py --output-dir build/p4-assets`，设置 `LYFLOW_P4_ASSETS` 为该目录；显式开启后模型/字典/图像缺失即失败，无新增跳过。本地 Windows 全量构建运行公开PPHumanSeg、PP-OCRv6检测/识别及配套方向模型和P1四模型检测；CI 的 Linux job 验证无PCL图像链及ZXing私有C++20静态后端。

永久回归覆盖逐ROI掩膜复用的0/1/2次实际Run、unknown/类别冲突、中文多行/倒置/旋转OCR、合法零字节QR、增强前后反馈、原生重叠/孔洞标注、同尺寸文件更新和真实路径链接逃逸。准备脚本的离线、坏缓存、源归档和路径边界独立执行；来源和许可见[P4资产](p4-assets.md)。跨接口测试核对执行器、C ABI和动态C++ SDK的完整原生Tensor/图像，不以只检查manifest替代实际运行。

## P2 原生帧、Python与结果交付门禁

`core/tests/test_frames.cpp`覆盖所有原生载荷、来源身份与缓存独立所有权、坏归档/预算、FIFO容量/顺序/重置/取消。深度缺陷和截面的中间记录覆盖来源云、截面云重绑及加载后再次保存。`core/tests/test_python.cpp`继续通过真实worker验证新直接几何/Tensor端口、版本错误与恢复。`python -B -m unittest discover -s python/tests -v`运行SDK侧测试。

安装后的新CLI与C++17消费者不在 CI 中运行，改动交付链时在本地执行：`python bridge/tests/p6_delivery_contract.py --cli <CLI> --core <DLL> --output-dir <目录>`验证真实大数组逐字节往返、只读/重算、时间筛选、会话、文件日志及loopback ACK。发布入口也执行同一安装后契约。编辑器功能测试总数保持100以下，无新增跳过；查看器浏览器证据与样例见[P6指南](p6-examples.md)。
