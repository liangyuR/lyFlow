# M7 验收记录 —— 去掉复刻约束，把散文变成校验

逐条对着 `docs/m7-plan.md` §1 走：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
实现由一个 Opus 子代理按计划完成（文档清理与 TS 侧各分出一个子代理并行），基于 main `20cd35e`。
所有命令都在 `LYFLOW_PACKS=gap;dts` 下跑，长输出落盘到 `%TEMP%` 再 grep。

## 门禁

| 命令 | 结果 |
|---|---|
| `$env:LYFLOW_PACKS="gap;dts"; pnpm check > $env:TEMP\m7-check.log 2>&1` | 退出码 0，末行「全链路绿」。core doctest **245/245**（34429 断言）、三份 schema、`cargo test` **130/130**、lyflow-client 单独构建、CLI `--no-default-features`、嵌入 SDK、编辑器单测 **4/4**、前端构建、MCP **45/45** |
| `$env:LYFLOW_PACKS="gap;dts"; pnpm e2e > $env:TEMP\m7-e2e.log 2>&1` | 退出码 0，**424/424 项通过**（M6 是 423，多出的一条是本轮加的顶层 params 往返） |
| `pnpm core:dump`（带 `LYFLOW_PACKS`） | 重新生成 `app/public/manifest.dev.json`：60 个算子，`preconditions` 0 处。该文件被 `app/.gitignore` 忽略，不进 commit |

grep 落盘日志：

```
grep -nE "跳过|未验|FAILED|error|skip" m7-check.log   # 只有三类命中，都不是跳过：
  77:  [doctest] test cases: 245 | 245 passed | 0 failed | 0 skipped
  175: -- Check for working CXX compiler: ... - skipped            （CMake 自己的探测）
  264: ✔ stderr 掺进 stdout 的行被跳过而不是让解析崩掉              （MCP 现有用例的标题）
grep -nE "跳过|未验|✗|FAIL" m7-e2e.log                  # 零命中
```

## §1 逐条

### ✅ 1. 四份约束文档删除、全仓零提及

```
git ls-files docs | grep -E "gap-(integration-plan|acceptance|pack-migration-(plan|acceptance))"   → 空
git grep -nE "gap-(integration-plan|acceptance|pack-migration-(plan|acceptance))"                  → 空
```

用的是文件名主干（不带 `.md`），`gap_ops.h` 里「…偏离 23」这类不带扩展名的引用也一并清掉了。
历史验收记录（`std-pack-acceptance.md` 三处）按 J2 只把文件名换成「当时的 gap 集成验收记录（M7 已删除）」。
`docs/m7-plan.md` 本身**未被跟踪**（引用了这四个文件名，是计划原文），不在 `git grep` 范围内；
如果要提交它，这两条命中是计划的引文，不是链接。

### ✅ 2. 兼容承诺清掉

```
git grep -nE "G8|复刻行为|逐位一致|逐位不变|和以前一样" -- core packs packages bridge crates schema 'docs/*.md' docs/adr/ README.md
```

剩三条，逐条说明：

| 位置 | 归类 | 理由 |
|---|---|---|
| `docs/m5-plan.md:45` | 合法保留 | M5 自己的决定编号 G8（manifest 加 preconditions），与兼容无关；该行已注明「M7 J4 已撤销」 |
| `docs/m5-plan.md:52` | 合法保留 | 「G8–G11 是小改」，同为 M5 编号 |
| `docs/phase-a1-acceptance.md:123` | 历史验收记录（J2 豁免） | 「注入与读盘逐位一致」讲的是确定性，不是兼容承诺 |

另外（不在 grep 范围、与兼容无关）：`examples/embed_inject.cpp:124` 的「逐位」是注入确定性断言。
代码里「逐位不变」「和以前一样」「老图行为不变」一类都改成了现行行为说明，例如 guard 的文档
「带内结果与不加带相同」、band 的选项「合规帧与 free 相同」。

### ✅ 3. `preconditions` 在源码与 schema 零命中

```
git grep -n preconditions -- . ':!docs'   → 空
```

`packages/mcp/dist`、`build-test` 由 `pnpm check` 重新构建过；`app/public/manifest.dev.json` 由 `core:dump` 重新生成（0 处）。
docs 里剩的提及都是「M7 J4 已撤销」的注记或历史验收记录。

### ✅ 4. `pnpm check` 全绿、无跳过 —— 见「门禁」

### ✅ 5. 新增测试

| 要求 | 用例 | 位置 |
|---|---|---|
| J5/J6 `fit_line` band 未接 refLine：`validate` 非零、`bad_param`/`validate`、执行前失败 | `fit_line_band_without_ref_line_fails_at_validate`：`lyflow validate` 退出 1，诊断 `code=bad_param phase=validate nodeId=fit`；`lyflow run` 退出 1，stdout 里**没有** `run_started` 也没有任何 `node_state` | `bridge/src/cli.rs` |
| 同上（core 层） | 「gap.fit_line dirMode=band 没接 refLine：validate 就报 bad_param，节点不执行」：诊断唯一一条；直接 `exec::Run` 时 `fit` 从未进 `running` | `packs/gap/tests/test_import_bundle.cpp` |
| J5 钩子机制本身 | 「validate 钩子：error 进 Validate 诊断且节点不执行，warning 只提醒」（`test.validated`）：error → plan 返回诊断数组；warning 出现在 validate 数组、运行时走 warn 日志 | `core/tests/test_graph_params.cpp` |
| J6 其余挪进 validate 的检查 | 「gap.fit_gap_circles 的参数与连线检查在 validate 里」「gap.datum_window 的 lengthMm 与 gap.gap 的 definition A 在 validate 里查」 | `packs/gap/tests/test_gap_ops.cpp` |
| J7 绑定写入生效值 | 「顶层参数：绑定在展开期写进节点，生效值与 source=graph 由 core 说」；子图实例：「…里面的节点也报 source=graph」 | `core/tests/test_graph_params.cpp` |
| J7 改值只动绑定节点及下游的 cacheKey | core：「换一个值只改被绑定节点及其下游的 cacheKey」（`side` 支路键不变、传等于 default 的值键不变）；CLI：`param_only_moves_the_bound_node_and_its_downstream` | 同上 / `cli.rs` |
| J7 `unknown_bind` / `param_conflict` / `--set` 命中 | core：「unknown_bind、param_conflict、未声明的名字」6 个子用例（节点不存在、参数不存在、节点里又显式写、两个参数绑同一目标、宿主传未声明名字、binds 写法错）；CLI：`set_on_a_bound_param_and_unknown_param_are_usage_errors`（退出码 4，stderr 含 `param_conflict` 与 `--param count=`） | 同上 |
| J8 ABI `params_json` 与 CLI `--param` 一致 | `abi_params_json_and_cli_param_agree`：同一张图，CLI `--param count=1234` 与 `RunSpec.params_json={"count":1234}` 两次运行 g 都是 1234 点、下游 v 点数相同、`run_started.nodes` 的 cacheKey **逐个相等**；ABI 传未声明名字 → `run_finished.error.code=unknown_param` | `cli.rs` |
| J8 `lyflow params` source=graph | `params_reports_the_graph_source_and_which_graph_param`：`source=graph`、`graphParam=count`、`--only graph` | `cli.rs` |
| J8 eval / patch | `eval_takes_graph_params_next_to_sweep_axes`（顶层参数与扫描轴混用）、`patch_param_rewrites_the_default_and_is_idempotent` | `cli.rs` |
| J9a 框变换、datumSide=right 用右侧变换 | 首轮是四角点包围盒；**返工后**改为「gap.business_rois 只搬框中心、宽高不变，datumSide 决定 base ROI 用哪侧变换」，见文末「返工」 | `test_gap_ops.cpp` |
| J9c toward 左/右 → innerEnd 两端；segmentPoints 过大 → segmentApplied=false | 「gap.fit_line 按 toward 截取与取 innerEnd，与点序无关」（两种点序 × 两个 toward）、「…内点不多于 segmentPoints 时不截取，quality 说出来」 | 同上 |
| J9d 带约束路径固定半径 | 「gap.fit_gap_circles 的固定半径在圆心高度带那条路上也生效」（`rightCenterTol>0` always + `rightRadiusFixed` → 半径 = 1.0 mm，model=`circle-center-band`）；另加相机分开回退那条：「…在相机分开的回退里也生效」 | 同上 |
| J9e 不写 signed 出带符号值 | 「gap.flush 默认给带符号垂距，signed=false 才取绝对值」 | 同上 |

跑法：`build\core\bin\lyflow-core-tests.exe --test-case="*顶层参数*,*validate*,*toward*,*business_rois*,*固定半径*,*导入的图*,*带符号*"` → 22/22；
`cargo test`（bridge）130/130，上表的 7 个新用例都在其中（`cargo test -- --list` 核对过名字）。

### ✅ 6. 导入器

仓库内夹具（`test_import_bundle.cpp` 的 `kConfig`）：
- 「导入的图：fit_line 接 toward、不写 side，business_rois 写 datumSide」—— 模板、模型两种 kind 逐节点查。
- 「导入的图：基准线与参考线的 toward 各接同侧的 gap 框」—— `n_fit_base ← gapLeft`、`n_fit_ref ← gapRight`、`n_fit_datum ← 锚框`。
- 「导入的图带顶层参数：gapOffset 绑两处，模型模式的 modelPath 逐个列出节点」—— `gapOffset.binds = [n_gap.offset, n_circles.offset]`，节点上不再写 `offset`；模型模式 `modelPath.binds = [n_infer.modelPath, n_ref.modelPath]`，无通配。
- 「导入的图过 core 的 validate：模板、模型、带 fallback 三种」—— 零 error。

真实配置（`tianmu_0904` 的 12 个测点）额外跑了一遍 CLI：

```
lyflow import <点>/StandardGap.yml --kind StandardGap.yml:template|StandardGap.yml   → 24 张，lyflow validate 24/24 退出 0
同 12 份配置 + setting.yml(enabled true/false) --kind StandardGap.yml:model          → 24 张，lyflow validate 24/24 退出 0
R1 模型+fallback 图：fit_line 4 个、带 side 的 0 个、带 baseSide 的节点 0 个
params = {"gapOffset":{"binds":["n_gap.offset","n_circles.offset"],"default":0.4,...},
          "modelPath":{"binds":["n_infer.modelPath","n_ref.modelPath"],...}}
```

`--param gapOffset=0.4` 与 `=1.4` 跑同一帧 R2：gap 28.540 → 29.540，恰好差 1.0，flush 不动。

### ✅ 7. 编辑器保留顶层 `params`

- 单测 `packages/editor/test/graph-params.test.mjs`（`pnpm --filter @lyflow/editor test`，4/4，已挂进 `scripts/check.ps1`）：
  打开即保存逐字相等（含键序）；改名 / setParam / 移动 / bypass / 标输出 / applyMigrations / 撤销 / 重做之后仍逐字相等；没有 `params` 的图不会凭空多出这个键。
- e2e `scripts/e2e/m3.mjs`：经 **Tauri 后端**保存 → 打开 → 再保存，`params` 原样（「顶层 params 经后端保存、打开、再保存原样保留」✓）。
  这一条是必要的：`bridge/src/graph.rs` 的 `GraphDoc` 原来没有 `params` 字段，serde 会静默丢掉它，本轮加上了。

### ✅ 8. `pnpm e2e` 全绿、无跳过 —— 见「门禁」

`scripts/e2e/gap.mjs` 的两组真实 gap 图（`LYFLOW_GAP_GRAPH` / `LYFLOW_GAP_GRAPH_MODEL`）在未设环境变量时直接 return，
既不计数也不打印（与 M6 一致）；本轮没有设这两个变量，改由上面第 6 条的 CLI 真实数据覆盖。

### ✅ 9. commit —— 两个，见文末

## J6：挪进 validate 的检查

gap 与 dts 两个包的全部 ops 过了一遍（`grep -n "Status::Error("`，gap 81 处、dts 16 处，外加 `inputs.has(` 与参数判断）。

| 算子 | 检查 | 去向 |
|---|---|---|
| `gap.fit_line` | `dirMode≠free` 未接 `refLine` | `validate`（`bad_param`, paramPath=dirMode, port=refLine）；compute 副本删除 |
| `gap.fit_gap_circles` | `leftCenterTol>0` 未接 `refLine` | `validate`；compute 副本删除 |
| 同上 | `rightCenterTol>0` 既没接 `refLineRight` 也没接 `refLine` | `validate` |
| 同上 | `*RadiusMax ≤ *RadiusMin` | `validate`（paramPath=*RadiusMax） |
| 同上 | `*RadiusFixed` 且 `*RadiusValue` 不在 (min, max) | `validate`（paramPath=*RadiusValue） |
| `gap.datum_window` | `lengthMm ≤ 0` | `validate` |
| `gap.gap` | definition A 未接 `baseLine`（原为 Execute 期 `bad_input`） | `validate`（`bad_param`, paramPath=definition, port=baseLine） |
| `gap.load_template` | `dir` 为空 | compute 副本删除：`dir` 是 path 参数，core 的「path 必填」校验早就覆盖 |
| `gap.measure_reference` | `configPath` 为空 | 同上 |

留在 compute 里的（依赖数据或文件系统，不是「只依赖参数与连接」）：`gap.gap` 基准线没有端点、`measure_reference`
配置读不了 / 模型文件不存在 / 模型加载失败、`io` 的文件不存在与读失败、各算子的空云、类型不对、拟合失败等。
**dts**：11 个 op 文件的 Execute 错误全部依赖输入数据（`type_mismatch` / `no_segments` / `bad_input` / `io` / `no_*`），没有只依赖参数的检查，无需挪。
std 包不在 J6 范围，其中只依赖参数的检查（如 `fit.circle_2d` 的半径上下限、`edit.translate_region` 的零法向）原样留在 compute。

## J4：每个包里 preconditions 的去向

「校验」= J5/J6 的 validate 或已有的端口契约/运行期信号；「并入 doc」= op.doc 或参数 doc；「删除」= 描述的正是本轮修掉的行为，或与现有 doc 重复。

| 包 / 算子 | 条数 | 去向 |
|---|---|---|
| gap `align_template` | 3 | 全部并入 op.doc（trustClamped、退化方向锁定、低分不判失败） |
| gap `select_alignment` | 2 | 并入 op.doc |
| gap `fit_line` | 4 | ①side 填反 → **删除**（J9c 改成 toward 必接，这个失败方式不存在了）；②distThresh 过紧 → distThresh 的 doc；③ROI 里是直线的假设 → op.doc；④相对倾角当常数 → dirNominalDeg 的 doc |
| gap `fit_gap_circles` | 6 | ①圆边可见/闭合缝用 notch_width → op.doc；②相机回退读到阴影 → cameraFallback 的 doc；③nominal 填错挑错圆 → nominal 的 doc；④centerTol 只筛候选 → leftCenterTol 的 doc；⑤钉单相机后退回合并云 → leftCamera/rightCamera 的 doc；⑥「minInliers/minArcDeg 不填和以前一样」→ **删除**（两个参数的 doc 本就写了 0=不检查） |
| gap `datum_window` | 4 | ①③并入 op.doc（不含锚框、轴对齐、配最少内点数）；②④删除（窗口语义与轴对齐已写明）；lengthMm≤0 另进 validate |
| gap `overall_roi` | 2 | 并入 op.doc |
| gap `business_rois` | 2 | ①刚体变换吃掉差异 → op.doc；②只变换对角两点 → **删除**（J9a 修掉） |
| gap `selected_point` / `nearest_to_line` | 2 / 2 | 并入 op.doc |
| gap `groove_joint` | 3 | ①③并入 op.doc；②gapDepth 与 doc 重复 → 删除 |
| gap `load_profile_pair` / `to_measurement_frame` / `load_template` | 3 / 2 / 2 | 并入 op.doc |
| gap `flush` | 3 | ①signed=false 默认折叠 → **删除**（J9e 默认 true），signed 的 doc 改写；②方向折算 → scale 的 doc；③基准线方向来自上游 → op.doc |
| gap `gap` | 3 | ①definition A 缺基准线 → **校验**（validate）+ 缺端点仍是运行期 bad_input；②③并入 op.doc |
| gap `corner_vertex` | 3 | ①→ minAngle 的 doc；②与 op.doc 重复 → 删除；③→ scale 的 doc |
| gap `judge` / `measure_reference` | 2 / 3 | 并入 op.doc |
| gap `profile_tensor` | 2 | ①1280 槽 → 已有端口契约 elementCount（运行期信号）+ doc；②通道口径 → op.doc |
| gap `labels_from_logits` | 2 | ①形状 → 已有 shape 契约，行序并入 doc；②只做 argmax → op.doc |
| gap `roi_from_labels` | 3 | ①缺段报 model_roi_failed → doc 已有，删除；②槽号对应 → 已有 elementCount 契约，删除；③backdrop → doc 已有，删除 |
| gap `labels_to_cloud` | 2 | doc 与契约已覆盖，删除 |
| gap `drop_non_finite` | 2 | 并入 op.doc |
| gap `roll_anchored_crop` | 3 | 并入 op.doc（走了哪一支看 `status` Record，运行期信号） |
| gap `notch_width` | 3 | ①③与 doc 重复 → 删除；②缓坡末端不适用 → op.doc |
| gap `point_offset` | 3 | ①②并入 op.doc；③absDx/absDy 折叠 → absDx/absDy 的 doc |
| gap `result_bundle` | 3 | 并入 op.doc |
| std-pointcloud `edit.translate_region` | 4 | ①纯几何选区 → op.doc；②零法向 → normal 的 doc 已写、compute 里的 bad_param 保留；③点数点序不变 → doc 已有，删除；④法向不跟着转 → op.doc |
| dts、std-ml | 0 | 没有 preconditions |

## 偏离与取舍

1. **validate 钩子签名**：`std::vector<lyflow::Issue> (*)(const ParamView&, const std::set<std::string>& connectedInputs)`，`Issue{Severity, Status}` 放在 `status.h`，带 `Issue::error/warning` 两个工厂。phase 由 buildPlan 统一改写成 Validate。钩子只在该节点参数段全部通过时调用（拿默认值占位的参数跑钩子只会派生出假问题）；钩子抛异常记 `internal`。
2. **「阻断 plan」的范围**：钩子的 error 与现有的参数/连线校验同一待遇 —— 节点 `valid=false`、`lyflow_plan` 返回诊断数组、`lyflow validate` 退出 1、CLI `run` 预校验不起跑。core 的 `Run` 本身保持 D5 的节点级语义（其余合法节点照跑、坏节点标 error、从不进 running），没有改成「有 error 就整图不跑」：那会改动编辑器边改边跑的既有行为，超出 J5。
3. **CLI `--param` 的实现**：`load_graph` 把值写成该顶层参数的 `default`（core 的语义本来就是「给值用值，没给用 default」），`run/validate/plan/params/eval/perturb` 因此看到同一个值。C ABI 只有 `lyflow_run_options.params_json`（计划只要求这一处）；`lyflow_validate/plan/effective_params` 没有加带取值的变体，core 内部的 `validateGraphJson/planGraphJson/effectiveParamsJson` 已经接受 `paramsJson`，宿主需要时再暴露。两条路径的一致性由 `abi_params_json_and_cli_param_agree` 钉住。
4. **`--param` 的歧义**：eval/sweep 原有 `--param <节点>.<参数>=<start>:<end>:<steps>`。按 `=` 左边**是否含 `.`** 区分，含点是扫描轴，不含是顶层参数（参数名里没有点）。
5. **退出码**：`--param` 未声明的名字（`unknown_param`）与 `--set` 命中被绑定参数（`param_conflict`）都按「参数错」退出 4，与 `lyflow params` 既有的 unknown_node/unknown_param 同一口径；图里自己写出的 param_conflict 仍是校验失败退出 1。
6. **`patch --param`**：第五个动作（remove → add → rewire → set → param），改顶层参数的 `default` 并落盘，同值 no-op。`diff` 的输出新增 `graphParams` 段（不加的话只改顶层参数的 patch 会被判成「没有语义变化」而不落盘）。
7. **顶层参数的 `type`**：可选，只校验是 manifest 的参数类型名；值由被绑定参数自己的声明规整（错了报在目标节点上）。core 容忍声明里的额外键，schema（TS 子代理写的）是 `additionalProperties: false`，比 core 严。
8. **绑到子图实例**：顶层参数绑子图实例的提升参数时，来源一路传下去，里面的节点报 `source=graph` 而不是 `bound`；绑到子图没声明的外参报 `unknown_bind`（报在实例节点上）。
9. **`fit_line` 的 `segmentPoints: 0`** 定义为「有意不截」，不发 warn；`segmentPoints` 不少于内点数时才发 J9c 要求的 warn。原因：方向基准线（`n_fit_datum`）本来就整条都要，导入器过去写 100000 来达到「不截」，按 J9c 字面每帧都会多一条噪声 warn。导入器现在给它写 0。
10. **`fit_line` 的 `inlier_ends`**：首尾端点也改成沿直线方向投影的两个极值内点（原来取下标首尾），与 J9c「不再依赖点序」一致；`inliers` 输出按下标升序。
11. **J9d 的实现**：约束 RANSAC 在固定半径下改成「两点 + 半径」定圆心（每对两个解），重拟只用 Gauss-Newton 动圆心；相机分开回退一律用配置的固定半径。`separatedFixedFree` 删掉，quality 的 `radiusMode` 在所有路径上都如实报 fixed。
12. **`modelPath` 的 binds** 除了 `ml.onnx_run`（`n_infer`）还列了黑盒对照 `gap.measure_reference`（`n_ref`）：同一个模型文件，不绑的话宿主换模型时 n_ref 悄悄用旧的，正是 J7 要消掉的那类不一致。`gapOffset` 没有「备用分支的同名节点」可绑：备用闭包只备份到基准线/参考点，间隙与圆拟合节点只有一份。
13. **版本号**：改了参数或默认值的 5 个算子只升次版本（business_rois / roi_from_labels / fit_line 1.1.0，fit_gap_circles 1.2.0，flush 1.1.0）。升主版本必须带完整迁移链（`Registry::validate()` 会拒），而计划定了不写 migrations。老图会以 `unknown_param`（side / baseSide）与 `missing_input`（toward）报出来，正是「由导入器重新生成」的信号。
14. **`scripts/check-gap.ps1`**：A/B 三条改为 `LYFLOW_GAP_AB=1` 时才跑、只打印退出码不判门禁（J3）；「导入器与 Python 生成器逐节点等价」一步删除 —— Python 生成器没有跟着 J9/J10 改，两边必然不同。
15. **`scripts/check.ps1`** 加了一步 `pnpm --filter @lyflow/editor test`，让第 7 条的单测进门禁。
16. **没动的兼容行为**：`gap.align_template` 全局粗配时把目标云加两遍（原 G8 注释的对象）不在 J9 清单里，行为保留，注释改写成现状说明。
17. **宿主切换计划**：`docs/phase-b-plan.md`、`docs/gap-inspector-integration-design.md` 以 39 样本 A/B「逐值一致」为门槛，M7 之后必然不成立；但那是宿主仓库的验收（J11），只在两份文档开头加了一段现状注记，没有改写它们的门槛。
18. 顺手修了 `core/README.md` 里 `C:\vcpkg` 被存成控制字符的三处老问题，以及文档子代理引入的 `packs\gap\tools` 路径被转义成制表符的五处。

## 读数变化（首轮，已被「返工」一节取代）

用 main `20cd35e` 构建的 `bridge/target/release/lyflow.exe`（带 gap 包、M7 之前）与本轮的 debug CLI，对 `tianmu_0904` 数据集前 39 个样本各自导入模板路径图、跑同一帧：

- **flush 的符号**：约一半测点（R1、R2、R3、R5、R6、L5）新读数为负。配置里的 `flush.offset` 是按 |d| 标定的，
  现在加在带符号值上，例如 R5 `4.41 → -7.01`（|d|=5.71，offset=-1.3）。这是 J9e 的直接后果，**这些点的 offset 需要重新标定或改写符号约定**。
- **模板路径的框**（J9a 四角包围盒）：gap 读数大多不变，个别变化明显（R3_13 7.90→7.27、R1_14 3.71→3.02、R4_2 5.70→5.77）；
  R4_16 的右圆在更大的框里拟不出来（`circle_fit_failed`，之前 4.07）。flush 也有随框变化的（L4_29 1.04→0.05、L3_8 3.51→4.25）。
- 模型路径 R1（M6 验收用的那帧）：gap 3.783（M6 记 3.784），flush -2.379（M6 记 2.350，符号为 J9e，量级差 0.03 mm 未单独定位）。

## 未验证

- `pnpm check:gap` 的历史对拍（`LYFLOW_GAP_AB=1`）没有跑：已不是门槛，上一节的 39 样本前后对照代替了它。
- `pnpm e2e:http` 没有跑（M7 不涉及 HTTP 传输）。
- e2e 的两组真实 gap 图（需要 `LYFLOW_GAP_GRAPH*`）没有跑，见第 8 条。

## commit

见 `git log`：代码（core / ABI v10 / CLI / 客户端 / 编辑器 / MCP / schema / gap 包 / 测试 / 脚本）一个，文档一个。
`docs/m7-plan.md`、`docs/m8-plan.md` 未提交。

## 返工（验收者打回的两处）

### 1. flush 的符号与拟合方向无关（bug）

- 源头：`packs/gap/ops/gap_ops.h` 新增 `canonicalLineDir`（朝 +x，x 分量为 0 时朝 +y，零向量退成 (1,0)）。
  `fit.cpp` 的 `lineFromCoefficients`、`lineAxis` 都走它，`gap.fit_line` 输出的 `Line2D.dir` 从此确定。
- `fitFixedDirection`（dirMode=fixed/band 钉方向那条路）入口也先统一朝向：法向跟着 dir 翻，而偶数个点的中位取「上中位」，
  不统一的话 refLine 的 dir 取反会让位置差出一个点距 —— 新测试就是在这里第一次失败的（1.0049 vs 0.9962 mm）。
- `gap.flush` 取法线前**自己再统一一次**，约定写死：**参考点在基准线上方（测量帧里 y 更小）为正**，
  基准线竖直时右侧（x 更大）为正。所以接别的包（例如 `fit.line_2d`）拟出的线也不受方向正反影响。op.doc 与 `signed` 的 doc 写实。
- 下游排查（依赖 `Line2D.dir` 符号的地方）：

| 位置 | 结论 |
|---|---|
| `gap.flush` 法线 | 改为按统一朝向取法线（上面） |
| `gap.gap` definition A 的 u | 取自基准线端点，且强制 `u.x ≥ 0`，与 dir 符号无关 |
| `gap.corner_vertex` | u 强制 `u.x ≥ 0`；夹角用「从顶点出发的远端」，已与 dir 符号无关 |
| `centerAboveLine`（圆心高度带） | 只用斜率 dy/dx，与符号无关 |
| `lineAngleDeltaDeg`（band 判据） | 折进 (−90, 90]，与符号无关 |
| 方向钉死的 pin = refLine.dir + nominal | 方向差 180° 是同一条线；位置由 `fitFixedDirection` 统一朝向后与符号无关（上面） |
| `gap.nearest_to_line` | 取垂距绝对值，与符号无关 |
| `gap.datum_window` | 不读 Line2D |
| groove / notch / offset / dts 自己构造的 Line2D | 不进 gap.flush，本轮不动 |

- 新测试（`test_gap_ops.cpp`）：
  - 「gap.flush 的符号只看参考点在哪一侧，基准线方向取反结果不变」：0° / 29° / −40° / 90° 四条基准线，dir 与 −dir 两次读数逐位相等，都是 +0.8 mm（参考点在上方 / 竖直时在右侧）。
  - 「gap.fit_line 输出的方向统一朝 +x，与拟合给的正反无关」：dirMode=fixed，refLine 取 0° 与 180°（即把拟合方向人为取反），两条输出线 dir 相同、`dir.x > 0`，接下游 flush 的读数相同且为正。

**39 样本对照**（模板路径，同一帧，旧 = main `20cd35e` 构建的 `bridge/target/release/lyflow.exe`，新 = 返工后）：

| 样本 | gap 旧 | gap 新 | flush 旧 | flush 新 | 备注 |
|---|---|---|---|---|---|
| R4_2 | 5.703 | 5.700 | 0.565 | 0.713 | |
| R4_3 | 6.097 | 6.088 | 1.082 | 1.038 | |
| R5_4 | 6.040 | 6.143 | 4.411 | **−6.719** | |
| R2_5 | 9.222 | 9.249 | 4.153 | **−6.014** | |
| R3_6 | 10.166 | 9.617 | 2.603 | **−5.203** | 见下 |
| L3_8 | 7.843 | 7.843 | 3.508 | 3.470 | |
| L3_9 | 7.786 | 7.813 | 5.706 | 5.521 | |
| R1_10 | 拟不出 | 拟不出 | 2.374 | **−2.374** | 新旧都 `circle_fit_failed` |
| R5_11 | 6.414 | 6.499 | 3.875 | **−6.475** | |
| R3_13 | 7.896 | 7.906 | 3.565 | **−6.421** | |
| R1_14 | 3.712 | 3.755 | 2.612 | **−2.762** | |
| R4_15 | 5.358 | 5.345 | 0.787 | 0.787 | |
| R4_16 | 4.070 | 3.617 | 0.544 | 0.469 | 首轮四角包围盒时拟不出，返工后拟得出，见下 |
| R4_17 | 5.582 | 5.541 | 0.722 | 0.701 | |
| L1_18 | 4.475 | 4.381 | 0.784 | 0.539 | |
| R1_19 | 0.875 | 1.049 | 2.421 | **−2.399** | |
| R2_22 | 9.430 | 9.430 | 2.968 | **−4.968** | |
| R2_24 | 4.870 | 4.870 | 3.076 | **−5.076** | |
| R1_25 | 3.053 | 3.054 | 2.374 | **−2.423** | |
| R4_26 | 5.399 | 5.401 | 0.908 | 0.947 | |
| R1_27 | 3.510 | 3.510 | 2.466 | **−2.465** | |
| L4_29 | 5.315 | 5.319 | 1.042 | 0.073 | |
| R6_30 | 6.122 | 6.118 | 4.860 | **−5.894** | |
| L3_31 | 7.544 | 7.555 | 5.188 | 5.265 | |
| R4_33 | 5.456 | 5.484 | 1.304 | 1.240 | |
| R4_35 | 5.304 | 5.304 | 1.237 | 1.199 | |
| R1_36 | 3.318 | 3.312 | 2.664 | **−2.406** | |
| L5_37 | 8.241 | 8.236 | 5.309 | **−4.589** | |
| R4_38 | 5.290 | 5.217 | 0.925 | 0.893 | |
| R1_39 | 拟不出 | 拟不出 | 2.144 | **−2.131** | 新旧都 `circle_fit_failed` |

另 9 个样本新旧都在上游失败（`icp_score_low` ×6、`roi_empty` ×3，与本轮改动无关）。完整表在 `%TEMP%\m7r-ab\compare.csv`。

**读数为负的测点：R1、R2、R3、R5、R6、L5**（16 个样本），与首轮是**同一批**，而且每个测点的所有帧同号 —— 不是随机翻转。
为了确认，用旧构建把 39 个样本的 `n_fit_base` / `n_fit_ref` 各跑一遍：**52 条输出线里 dir.x < 0 的 0 条**。
也就是说「方向不统一」这个 bug 是真的（fixed/band 路径与别的包的线都能触发，新测试钉住了），
但在这批数据上没有发作；这些负值是几何上的：参考点确实在基准线**下方**（y 更大）。旁证：框没变的样本上
新旧 |d| 相等，例如 R1（offset 0）2.374 / −2.374、R2_22（offset −1）|d| 都是 3.968。
这几个点的 `flush.offset`（R2 −1、R3 −1.3、R5 −1.3、R6 −0.5、L5 +0.3）是按 |d| 标定的，
现在加在带符号值上 —— **要么重标 offset，要么这些点的客户约定本来就是「参考面低为正」、需要一个翻符号的开关（例如 scale=−1）**。
这是标定 / 约定问题，不是算法问题，本轮不动配置。

### 2. business_rois 只搬框中心（设计修订）

- `geometry.cpp`：框中心按 ICP 变换搬过去，宽高保持配置原值，仍是轴对齐框。op.doc、`packs/gap/README.md`、ADR-0015、roadmap、两个对拍脚本的说明跟着改。
- 测试改为「gap.business_rois 只搬框中心、宽高不变，datumSide 决定 base ROI 用哪侧变换」：右侧转 30° 再平移，
  断言宽高仍是 1 mm × 1 mm、中心等于 (0.5, 0.5) mm 经变换后的位置；datumSide=right 时 flushBase 用右侧变换、flushRef 用左侧。
- 39 样本上（见上表）：
  - **R4_16**：首轮四角包围盒时 `circle_fit_failed`；返工后拟得出，gap 3.617（旧 4.070）。右圆从合并云的普通拟合
    （旧：R 1.108 mm、12 内点）变成了相机分开回退（新：`camera-separated-circle-primary`，R 1.516 mm、9 内点）——
    右 ROI 与旧版只差几个 µm（右侧转角 0.71°），却落到了回退那条路上，这个点位的右圆本身就在边缘。
  - **R1_14**：gap 3.755（旧 3.712，首轮 3.02）。框与旧版差 ≤ 0.02 mm，右圆 R 1.635 → 1.590。
  - **R3_6**：gap 9.617（旧 10.166）。右 ROI 在 5 位小数上与旧版完全相同（转角 −0.02°），但严格开区间裁剪
    少了一个贴边的点（134 → 133），RANSAC 就换了一个圆（R 1.765 → 0.984 mm，内点都是 16）——
    是这个点位右圆拟合本身不稳，不是框的设计问题。
  - 其余测点 gap 与旧版差在 ±0.1 mm 内，大多 ±0.03 mm。

### 返工后的门禁

| 命令 | 结果 |
|---|---|
| `$env:LYFLOW_PACKS="gap;dts"; pnpm check > $env:TEMP\m7r-check.log 2>&1` | 退出码 0，「全链路绿」。core doctest **247/247**（34469 断言，多了两条新用例）、`cargo test` 130/130、编辑器 4/4、MCP 45/45 |
| `$env:LYFLOW_PACKS="gap;dts"; pnpm e2e > $env:TEMP\m7r-e2e.log 2>&1` | 退出码 0，**424/424** |
| grep「跳过/未验」 | check：只有 CMake 探测的「skipped」、doctest 的「0 skipped」、MCP 老用例标题三类，与首轮相同；e2e：零命中 |
