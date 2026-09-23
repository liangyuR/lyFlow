# M7 实施计划 —— 去掉复刻约束，把散文变成校验

目标：**gap 包不再以「与原算法逐位一致」为正确性标准；算子的约束要么是加载期校验，要么是运行期信号，
不再以散文形式存在。** 这是 M8「高层测点算子，让人也能建图」的前提：人能建图的前提是参数少、互不牵连，
而现在那些牵连大多是复刻约束留下的。

来源：风挡 KUN10 两天调参的评审（09-21/22，874 台车），以及 2026-09-23 对评审逐条核对代码的结论。

**老图不迁移。** 现场图由导入器重新生成；本里程碑内算子的参数、端口、默认值可以破坏性修改，不写 migrations。

---

## 0. 定死的决定

| # | 决定 | 一句话理由 |
|---|---|---|
| J1 | **删除四份约束文档**：`docs/gap-integration-plan.md`、`docs/gap-acceptance.md`、`docs/gap-pack-migration-plan.md`、`docs/gap-pack-migration-acceptance.md`。其余文档、README、ADR、代码注释里指向它们的链接，以及「G8」「复刻行为而不是修正」「与基线逐位一致」「老图逐位不变」「free 时和以前一样」这类**兼容承诺**全部删除或改写成现在的行为说明 | 约束已经被放弃，留着的文字会继续误导读者和 Agent |
| J2 | 历史验收记录（`m*-acceptance.md`、`phase-a*-acceptance.md`）只修死链，正文不改；**计划、ADR、README、代码注释**里的现行规则按 J1 清掉 | 验收记录是当时的事实，计划和注释是现行规则 |
| J3 | `packs/gap/tools/` 下的 A/B 对拍脚本保留，但**不再是验收门槛**；`packs/gap/README.md` 里相应段落改成「历史对拍工具，行为已有意偏离基线」 | 用户明确选择不删对拍工具 |
| J4 | **manifest 删除 `preconditions` 字段**：`OperatorDesc`、registry 序列化、`schema/operator-manifest.schema.json`、schema 样例、`app/public/manifest.dev.json`（用 `core:dump` 重新生成）、编辑器类型与 Inspector、MCP 的类型/工具/服务端与其测试、相关文档（`operator-manifest.md`、`mcp.md`、`agent-tuning.md`、`op-packs.md`、`roadmap.md`）。**所有包**里的 `op.preconditions = {...}` 删掉，每条逐一处理：能判定的 → J5 校验或 J9 运行期信号；仍有用的用法说明 → 压缩进 `op.doc` 或对应参数的 `doc`；描述的正是本里程碑要修掉的怪行为 → 直接删 | 散文约束运行期一条都不检查，这是评审的核心结论 |
| J5 | **算子加载期校验钩子**：`OperatorDesc` 新增可选 `ValidateFn validate`。签名固定为纯函数：输入 = 解析后的参数视图（含默认值与绑定值）+ 已连接的输入端口名集合；输出 = 若干条 Status，各带 severity（error / warning）。`buildPlan` 在参数解析与连边之后对每个节点调用；error 进 `Phase::Validate` 诊断并阻断 plan，warning 走已有的 warn 日志通道并出现在 `lyflow validate` 输出里。**不给它任何数据**（点云、上游输出） | 加载期只能查静态的东西；给数据就成了第二个 compute |
| J6 | gap 包里**只依赖参数与连接关系**的 Execute 期 `bad_param` 检查全部挪进 `validate`，Execute 里不再保留副本。至少包括：`fit_line` dirMode≠free 未接 refLine；`fit_gap_circles` 配了 `*CenterTol` 却没接对应参考线、半径上下限倒置、固定半径不在上下限之间；`datum_window` lengthMm≤0。子代理须把 gap 与 dts 包所有 ops 过一遍，挪完的清单写进验收文档 | 同一个错误两处判断，迟早不一致 |
| J7 | **顶层图参数**：GraphDoc 顶层可选 `"params": { "<名字>": { "type"?, "default", "binds": ["<节点>.<参数>", ...], "doc"? } }`，语义与子图 `params[].binds` 完全相同，解析复用同一段代码。绑定在展开期写进节点参数，因此缓存键、`plan`、`lyflow params` 自动正确。规则：① bind 指向不存在的节点/参数 → validate error `unknown_bind`；② 被绑定的参数若在节点里又显式写了值 → validate error `param_conflict`（一处定义）；③ `--set` 命中被绑定的参数 → 报错并提示改用 `--param`；④ 同一目标被两个顶层参数绑定 → `param_conflict` | 「本来就是全局」的值要有唯一定义处，而不是宿主改 JSON 或人手同步 |
| J8 | 顶层参数的**运行期取值**：CLI `run`/`validate`/`plan`/`params`/`eval`/`patch` 加 `--param <名字>=<json>`；C ABI `lyflow_run_options` 加 `params_json`（一个 JSON 对象，名字→值），**ABI 版本 +1**；`client.hpp` 与 `crates/lyflow-client` 各暴露一个设置方法；`lyflow params` 的 source 增加 `graph`（来自顶层参数），并带上是哪个顶层参数；未声明的名字 → `unknown_param`。编辑器只需**原样保留**顶层 `params`（类型 + 读写往返），编辑界面留给 M8 | 宿主从此传值而不是改 JSON；校验的图就是跑的图 |
| J9 | **gap 算子修正**（均为破坏性修改，不留旧参数）：<br>a. `gap.business_rois`：`baseSide` 改名 `datumSide`，文档写实：「基准件在缝的哪一侧。决定 base ROI 用哪一侧的 ICP 变换；不改变哪个框是 flushBase」。实现去掉「先换槽位再换端口名」的两次互换，直接按侧选变换。**只变换框中心、保持原宽高**，取代只变换对角两点（验收返工修订：初版的四角包围盒会随转角撑大框，R4_16 右圆因此拟不出来）<br>a'. 所有 `Line2D` 输出方向统一朝 +x（竖直时朝 +y），`gap.flush` 带符号时「参考点在基准线上方为正」（验收返工发现：方向不统一时符号随拟合方向随机翻转）<br>b. `gap.roi_from_labels`（模型路径出四框的那个算子）：删掉 `baseSide`，它两次互换相互抵消，是空参数<br>c. `gap.fit_line`：**删掉 `side` 参数**，新增必接输入 `toward: Box2D`（缝那一侧的 ROI，用其中心）。`innerEnd` = 内点中沿直线方向投影最靠近 `toward` 中心的那一个；截取改为按同一投影保留最靠近 `toward` 的 `segmentPoints` 个内点，不再依赖点序。quality 加 `segmentApplied: bool`，未截取时发一条 warn 日志<br>d. `gap.fit_gap_circles`：`*RadiusFixed` 在**所有路径**都生效（约束带路径 `circleFitConstrained` 接收固定半径；相机分开回退不再「强制自由半径」）；`selectClosestNominal` 的文档去掉「强制自由半径」<br>e. `gap.flush`：`signed` 默认改为 `true` | 逐条对应评审问题表里核实属实的部分 |
| J10 | **导入器**（`import_standard_gap`）跟着 J9 重写相应节点：`fit_line` 接 `toward`（基准线/参考线接同侧的 gap 框，方向基准线接它的锚框），不再写 `side`；`business_rois` 写 `datumSide`。并生成顶层参数：`gapOffset` 绑定 `n_gap.offset` 与 `n_circles.offset`（有备用分支时一并绑定），模型模式下 `modelPath` 显式列出每个 `ml.onnx_run` 节点。**不使用按算子类型的通配绑定** | 通配等于把「宿主硬编码算子 id」搬进图里，新算子照样静默漏掉 |
| J11 | **不做**：宿主仓库（`lyflow_measurer` 的 JSON 手术、plan 失败没进 errors）；高层测点算子与顶层参数的编辑界面（M8）；`gap.datum_window.side`（导入器里存在显式覆盖的配置，是否总等于「背离缝」未核实）；`minArcDeg` 默认值（缺跨工位数据）；`ExecContext` 任何改动 | 范围收在 LyFlow 仓库内、证据充分的部分 |

---

## 1. 验收（全部可机器断言，子代理逐条写进 `docs/m7-acceptance.md`）

1. `git ls-files docs` 里不再有 J1 的四个文件名；`git grep -n` 这四个文件名在全仓库（不含 `.git`、构建产物目录）零命中。
2. `git grep -nE "G8|复刻行为|逐位一致|逐位不变|和以前一样"` 在 `core/ packs/ packages/ bridge/ crates/ schema/ docs/*.md docs/adr/ README.md` 中，除 J2 允许的历史验收记录外零命中。若确有与兼容无关的合法用法（例如缓存确定性测试里的「逐位」），逐条列在验收文档里说明。
3. `git grep -n preconditions` 在源码与 schema 中零命中（构建产物 `dist/`、`build-test/` 须重新构建后再查，或说明其为忽略的产物）。
4. `LYFLOW_PACKS=gap;dts` 下 `pnpm check` 全绿，输出落盘后 grep 无「跳过 / 未验」。
5. 新增 doctest / cargo test，至少覆盖：
   - J5/J6：`fit_line` dirMode=band 未接 refLine 的图，`lyflow validate` 非零退出，诊断 code=`bad_param`、phase=validate；并断言执行前就失败（没有任何 node_started 事件）。
   - J7：绑定写入生效值；改 `--param` 只改变被绑定节点及其下游的 cacheKey；`unknown_bind`、`param_conflict`、`--set` 命中绑定参数，各一条失败用例。
   - J8：C ABI `params_json` 与 CLI `--param` 结果一致；`lyflow params` 显示 source=`graph`。
   - J9a：构造带旋转的对齐变换，断言输出框是四角点变换后的包围盒；`datumSide=right` 时 flushBase 用右侧变换。
   - J9c：同一片云，`toward` 放在左/右两侧时 `innerEnd` 分别落在对应端；`segmentPoints` 大于内点数时 `segmentApplied=false`。
   - J9d：配 `rightCenterTol>0`（always）与 `rightRadiusFixed=true` 时，输出半径等于固定值。
   - J9e：不写 `signed` 的 `gap.flush` 输出带符号值。
6. 导入器：对仓库内现有 StandardGap 测试夹具重新导入，生成的图 `lyflow validate` 通过；图里没有 `side`（`fit_line`）与 `baseSide`（`business_rois`、`roi_from_labels`）；顶层有 `gapOffset`，模型模式有 `modelPath`，且其 binds 逐个列出节点。
7. 编辑器：带顶层 `params` 的图打开再保存，`params` 原样保留（单测或 e2e 均可）。
8. `LYFLOW_PACKS=gap;dts` 下 `pnpm e2e` 全绿，输出落盘后 grep 无「跳过 / 未验」。
9. 一个风格与仓库一致的 commit（或按 J 分组的少量 commit），不 push。
