# 测试地图

**写新测试之前先查这里。** 要验的行为多半已经有测试：能在已有用例里加一行（表驱动的一行、一个 SUBCASE、同一流程里多断言一句）就别新开用例；能在更低一层测的就别放到 e2e。

2026-09-26 按四路审计做过一次合并精简（提交 test/prune），数字是那之后的：

| 层 | 命令 | 规模 | 跑一遍 |
|---|---|---|---|
| C++ core + 算子包（doctest） | `pnpm core:build`（`pnpm check` 第一步） | 默认 151 例；`LYFLOW_PACKS=gap;dts` 232 例 | 分钟级（含编译） |
| Rust bridge / CLI（`cargo test`） | `pnpm check` 的 Rust 步骤 | 134；纯平台构建 79 通过 / 55 ignored | < 1 分钟（已编译时） |
| editor 纯逻辑（node:test） | `pnpm --filter @lyflow/editor test` | 60 | 秒级 |
| MCP（node:test） | `pnpm --filter @lyflow/mcp test` | 27 | 秒级 |
| 桌面 app e2e（CDP） | `pnpm e2e`（带 `LYFLOW_PACKS=gap;dts`） | 707 条断言、92 个分组（精简前 1095） | 已编译时约 3.3 分钟（精简前 4.5）；首次要编 core 与 tauri，另加十几分钟 |
| 浏览器宿主 e2e | `pnpm e2e:http` | 30 条断言（精简前 58） | 几分钟 |

## 放在哪一层

- **算法、数值、执行语义、C ABI 契约** → C++ doctest。gap / dts 的测量数值只在这里钉，改动必须保留数值与容差。
- **bridge 的封送、IPC 命令、CLI 的参数 / 退出码 / 输出形状、配方失配的 Rust 实现** → `cargo test`。与 C++ 同层重复的不要再写（C++ 已经测了语义，Rust 只测封送和命令层）。要标准包算子的测试加 `#[cfg_attr(std_packs_off, ignore = "纯平台构建没有标准包")]`，与不带这个属性的不要合并。
- **编辑器 store 动作与 lib 纯函数**（图参数、配方、迁移写回、自动连线、参数面板模型、transform / curve、布局、ROI 标签摆放）→ `packages/editor/test`。
- **只有真界面才有的**（真鼠标 / 按键、DOM 标记、渲染、系统层行为、跨进程的完整链路）→ e2e。e2e 里**不要**再逐字段复查单测已经测过的 store 数据，只留界面那一半。

## 已有覆盖：按功能查

| 功能 | 主要测试 |
|---|---|
| 执行器、事件 seq、取消、并发、缓存复用 | `core/tests/test_executor.cpp`、`test_cache.cpp`、`test_flow.cpp`（并发 8 run、取消 100 次） |
| 计划、cacheKey、Run to node / 选中 | `core/tests/test_plan.cpp`、`test_noderun.cpp`；e2e `m3.mjs`（Shift+F5）、`noderun.mjs`（右键「运行到此节点」） |
| 子图、库算子 | `core/tests/test_subgraph.cpp`；e2e `m4.mjs` |
| 图参数（规格、校验、传参） | `core/tests/test_graph_params.cpp`、`test_params.cpp`；`packages/editor/test/graph-params*.test.mjs`；e2e `params_p1.mjs` |
| 配方与四类失配 | 共享夹具 `schema/fixtures/recipes/`：`bridge/src/recipe.rs` 与 `packages/editor/test/recipes.test.mjs` 对着同一份 `expected.json`；e2e `params_p3.mjs` 只验界面、磁盘与对话框 |
| 参数面板（虚拟列表、搜索、chip、14 种控件） | `packages/editor/test/param-panel.test.mjs`、`param-values.test.mjs`；e2e `params_p2.mjs` |
| 迁移（含改连线 ADR-0025） | `core/tests/test_cache.cpp`（迁移链）、`bridge/src/patch.rs` / `commands.rs`、`packages/editor/test/migrations.test.mjs` |
| 数据类型、Bundle、输出视图 ABI | `core/tests/test_data.cpp`、`test_bundle.cpp`、`test_output_view.cpp`、`test_contract.cpp` |
| 运行摘要（summary） | `core/tests/test_summary.cpp`；CLI 的 `--summary` 形状在 `bridge/src/cli.rs` |
| 标准算子 / PCD 读写 / ONNX | `packs/std-pointcloud/tests/*`、`packs/std-ml/tests/test_ml_ops.cpp`（模型用例要 `LYFLOW_TEST_ONNX_MODEL`，不设会打出跳过） |
| gap 测量、积木、导入、模型 ROI | `packs/gap/tests/*`；e2e `gap.mjs`、`m8b.mjs`、`m8c.mjs`、`params_p4.mjs`（编辑器 vs CLI 逐位相同） |
| 2D 拖框、自动连线、片段 | `packages/editor/test/autoconnect.test.mjs`、`roiframes.test.mjs`；e2e `m8b.mjs`、`m8c.mjs` |
| 连线查看器 Edge Peek | e2e `peek.mjs` |
| 动效、hover、端点对齐 | e2e `motion.mjs`、`noderun.mjs` |
| 节点运行按钮 | e2e `noderun.mjs` |
| 分栏、拖放配置（dragDropEnabled） | e2e `params_p2.mjs`（右侧分栏）、`m8b.mjs` 的算子面板组 |
| MCP 工具、argv 拼装、CLI 解析 | `packages/mcp/test/*`（`smoke.test.ts` 是唯一跑通 MCP → CLI 的） |
| HTTP 传输、宿主嵌入 | `scripts/e2e/http.mjs` |

## 共用的夹具与辅助

- **共享夹具**：`schema/fixtures/recipes/`（Rust 与 TS 共用）、`schema/examples/*.lyflow.json`、`examples/param-showcase.lyflow.json`（`test.param_showcase` 覆盖 14 种参数类型，C++ / CLI / e2e 共用）。新增跨语言规则时往共享夹具里加一行，别各写一份。
- **C++**：`core/tests/helpers.h`（`seqIsDense` 等）、`core/tests/test_ops.h`（测试算子，含会抛异常的 `test.throw`）、`param_showcase_op.h`。
- **Rust**：`bridge/src/cli.rs` 的 `#[cfg(test)] pub(crate) mod test_support`（`SharedBuf`、`Ran`、`cli()`），其它模块复用它。
- **e2e**：`scripts/e2e/page.mjs`（`newDoc`、`buildGraph`、`placeAtScreen`、`dragMouse`、`runAndWait`、`mustOk` 等），`peek.mjs` 的 `openByDoubleClick`，`m8b.mjs` 的 `roiGeometry` / `setCamera` / `waitValidated`；各 params 文件里的 `reveal`、`typeIn`、`pickRecipe` 目前有重复拷贝，新代码先找现成的。

## 写测试的规矩

1. **先查、后加。** 按上表找到已有的测试；同一判据的新情形加成表驱动的一行或 SUBCASE。
2. **一个事实一条断言。** 同一个对象不要拆成多条 eq；合成一条，出错信息写清是哪一项不对。
3. **前提不算断言。** e2e 里「图跑通了」「菜单点到了」「动作返回 'ok'」用 `mustOk()`：不成立就中断这一组并报原因，但不单独计数。
4. **不许静默跳过。** 缺数据、缺包、缺模型时要么 `report.fail` 并给出补救命令，要么明确打出「跳过」；分组自己 `return` 会让汇总照样「全绿」。跑 e2e 要带 `LYFLOW_PACKS=gap;dts`，否则依赖 gap 的分组会缺席。
5. **修 bug 时加的复现测试要留**，在测试名或注释里写明对应的修复（如「修前 NoSuchOutput」），以后精简时靠它识别。
6. **不测常量、不测框架本身**（TypeScript 类型已保证的、`ok(…, true)`、waitFor 之后必然成立的）。
7. 名字要说它真正测的东西；测不到的路径（比如异常转 internal）宁可补一个测试算子，也别让名字冒充。
