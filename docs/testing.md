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
| 编辑器纯逻辑 | `pnpm --filter @lyflow/editor test` | 当前按 24 个文件、94 个功能契约用例组织，包含工作区与 PythonNode；不启动桌面壳。 |
| 编辑器类型 | `pnpm --filter @lyflow/editor typecheck` | 同时检查 React 18 与 React 19 类型契约。 |
| 共享可视化 | `pnpm --filter @lyflow/visualization test` | 图像、点云等共享可视化逻辑。 |
| Python SDK 与 worker | `python -B -m unittest discover -s python/tests -v` | Python 3.11 及以上与 NumPy；检查节点发现、数据交换和 worker 行为。 |
| MCP 单元测试 | `pnpm --filter @lyflow/mcp test:unit` | 不含需要 CLI 的集成冒烟；完整入口为 `pnpm --filter @lyflow/mcp test`。 |
| glue / gap 门禁 | `pnpm check:glue` / `pnpm check:gap` | 运行相应包的专项检查；真实数据和历史对拍需额外配置，不能把人造帧结果当现场结论。 |
| 桌面交互 | `pnpm e2e` | CDP 驱动真实 Tauri/WebView2；完整领域场景需带 `gap;dts`。 |
| 桌面构建产物 | `pnpm e2e:packaged` | 同一套桌面交互断言验证构建产物，不等于运行库/SDK 发包验证。 |
| 浏览器宿主 | `pnpm e2e:http` | HTTP 传输、结果取数与 React 宿主链路。 |
| Windows 发包 | `pnpm run package` | 固定标准包、glue、gap，生成运行库/SDK ZIP、编辑器 tarball 和正式运行库 NuGet 包；详见 [发包契约](deployment/packaging.md)。 |

桌面完整验收及按分组复验：

```powershell
$env:LYFLOW_PACKS = 'gap;dts'
pnpm e2e
pnpm e2e --only m3:suitePanels,params_p4
```

选择规则、夹具及原生窗口驱动见 [E2E 脚本说明](../scripts/e2e/README.md)。切换包组合后要重新构建 core；已有 DLL 不会因为环境变量改变而自动拥有新算子。

## CI 的实际范围

[ci.yml](../.github/workflows/ci.yml) 在 PR、main push 和手动运行时执行以下五个 job。`fast` 是汇总检查，实际编译与单测由 `frontend`、`client` 执行。

| job | runner | 内容 |
|---|---|---|
| `quality` | ubuntu-24.04 | actionlint 1.7.12 检查 workflow；Prettier 3.6.2 检查前端、MCP 和配置；clang-format 20.1.8 检查跟踪的 C/C++ 源码。 |
| `client` | windows-2022 | 运行库 NuGet 契约测试、bridge/客户端/运行库探针的 Rust 格式检查、独立 Rust 客户端编译与单测。 |
| `frontend` | windows-2022 | 编辑器 React 18/19 类型、桌面与 React 宿主构建、编辑器及共享可视化单测、MCP 类型/构建/单测。 |
| `fast` | ubuntu-24.04 | 要求 `frontend` 与 `client` 都成功，不重复运行其测试。 |
| `native` | windows-2022 | 纯平台 + dts 的 CMake 编译及 CTest；Python 3.12 与 NumPy 2.2.6 的 SDK/worker 测试；Rust 桌面/CLI 编译、lib/bins 单测及 `python_nodes` CLI/嵌入集成测试。 |

CTest 的一个条目执行该构建组合的全部 doctest，条目数不是单元测试例数。Rust 使用 `LYFLOW_PREBUILT_CORE_DIR` 复用同一 job 的 core。纯平台 + dts 不依赖标准包的 vcpkg 端口，需要标准包的既有 Rust 用例会标记 ignored；这不证明标准算子包已验证。

日常 CI 不执行整套桌面/浏览器 e2e、全部 SDK 安装消费、所有 feature/包组合、完整真实数据评估或正式发包。完整 `pnpm check` 与各专项入口仍应按改动范围运行。

失败诊断上传到 Actions artifact，保留 7 天：前端、MCP、Rust 单测日志，以及 native 的 CTest JUnit、`Testing/Temporary/*.log` 和 CMake 配置日志。格式和编译错误也可直接查看失败步骤输出。

[Windows release](../.github/workflows/release-windows.yml) 仅在 `v*` tag 或手动触发时运行，日常 CI 不代替发布门禁。运行库版本、ABI、源码修订、资产检查和下游消费边界以 [发包说明](deployment/packaging.md) 为准。LyFlow 的软件检查也不等于 GlueSight 或其他宿主的现场验收。

## 测试归属与现有入口

优先在已经拥有该功能契约的用例里增加表项或断言，不为同一判据在不同层重复建测试。

| 行为 | 主要位置 |
|---|---|
| 算法数值、端口数据、C ABI、执行计划、缓存、取消与子图 | `core/tests/`、各 `packs/*/tests/`。领域数值测试保留原判据和容差。 |
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

CI 的 `vision` 在 Linux 运行无PCL的值/几何/位姿/图像包；`vision-native` 在 Windows 使用发布组合同一固定PCL 1.15.1与OpenCV/ORT依赖，运行三维点云/配准和真实YOLOv8、YOLO11、YOLO26、YOLOX检测回归。该job只读恢复依赖缓存，不降级PCL版本或静默跳过重依赖。

本地执行 `python scripts/fetch-p1-model-assets.py` 并按[示例说明](../examples/p1-detection-README.md)导出三种YOLO模型，配置 `-DLYFLOW_P1_MODEL_TESTS=ON`、`LYFLOW_P1_ASSETS=<资产目录>`。此选项默认关闭以保留离线开发；显式开启后缺模型或图片即失败。原有推理夹具另由 `tests/release/make_fixtures.py --output-dir build/p1-fixtures` 生成，测试时设置 `LYFLOW_TEST_ONNX_MODEL` 指向 `channels-6-to-8.onnx`。不使用“未配置模型”的日志代替真实推理验证。

检测门禁比较执行器、C ABI和动态C++ SDK输出，对公开人像、猫图和空背景验证类别与有效空集合；点云/位姿有完整组合图与失败门限回归。公开数据验证不等于客户模型质量或现场节拍验收。
