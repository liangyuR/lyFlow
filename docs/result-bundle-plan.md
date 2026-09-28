# gap `result_bundle` 三处缺口 —— 调研与实施计划

> 状态：**「不依赖业务」的那一版已实施**（2026-09-29）；依赖业务定义的部分记在 §4，等阶段 B。
>
> 实施时多出的一处：core 的迁移诊断会把被顶层图参数绑定的参数（图参数注入的值）也写进 `params`，写回节点就是
> param_conflict —— 以前被迁移的节点上恰好没有被绑定的参数。`n_bundle.graphSha256` 被绑定之后 result_bundle v1 → v2
> 的迁移写回撞上了它；改成迁移诊断里去掉被绑定的键（`core/src/exec/plan.cpp`，`test_cache.cpp` 迁移链那条加了一段）。
> result_bundle 的 v1 → v2 迁移也把 bundle 原来的 alignment 输入同时接给新插的 make_roi_set，与细粒度图的接法一致。
> 行号是写这份计划时的位置，以符号名为准。

## 1. 现状

`gap.result_bundle` v2.0.0（`packs/gap/ops/result_bundle.cpp`）输出一份 `Record{type:"GapResultBundle"}`：gap / flush、fits[]、
effective_roi、icp[]、crop_status、fallback(_reason)、roi_source、input / preprocessed / left / right_point_count、point_counts、
timings / runtime_us（恒 0）、consistency_*（占位）、pack_versions、graph_sha256。示例在 `packs/gap/ops/port_examples.cpp`。
业务侧的 results.csv、metadata.json、交互计算 JSON、异常图几何都从它取（`gap-inspector-integration-design.md`，
`phase-b-plan.md` B2 / B4 / B7）；`lyflow eval` 与 m5 的指标路径也按路径读它的字段。

## 2. 三处缺口与做法

### (a) `graph_sha256` 要调用方填

- 现状：只透传参数 `graphSha256`，默认空串；op.doc 写着「执行器还没有注入口子」。A1-9 要求执行器注入到 ctx，推迟到阶段 B
  （`phase-a1-acceptance.md`）。core 里没有 SHA-256；cacheKey 是逐节点的 XXH3-128；`specDigest`（`bridge/src/recipe.rs`）只覆盖
  图参数规格。`ExecContext` 只有 cancelled / progress / log / baseDir / threadBudget，RunOptions 也没有图的身份信息。
- **做法（本轮）**：导入器给图声明一个顶层图参数 `graphSha256`（text，默认空串），绑到 `n_bundle.graphSha256`；宿主在
  `params_json`（CLI `--param graphSha256=…`）传图文件的 sha256。core 零改动；值进 cacheKey，换了图不会命中旧的汇总。
- 否掉的（暂不做）：core 注入 —— RunOptions / C ABI 加 `graphDigest`，或 core 对 graphJson 原字节算 SHA-256，ExecContext 加
  `runInfo()`（新 virtual，要升 ABI），还得把 digest 混进该节点的 cacheKey。约 1.5–2 天，等业务口径定了再说。

### (b) `roi_source` 是推断出来的

- 现状：取值顺序 参数 → `rois.info.source` → 推断（有 alignment 记 template / config、有 cropStatus 记 model、回退选了 b 强制 template）。
  `make_roi_set.source` 只有 template / model，`gap.locate_template` / `gap.locate_model` 写死。
- 原算法（`packs/gap/algo/gap_detection/GapDetection.cpp`）：**选中的候选自带 `rois` 记 `template`，没带（只回放配置里的全局框）记
  `config`**；只有单个模板（没有 `template_candidates`）时也是 `config`；`roi_override` 时取 `roi_override_source`。
  LyFlow 里每个槽都有自己的四个框（导入时没带的已经用全局框填好），所以 `config` 永远出不来。
- 选中哪个候选是运行时才定的（`gap.select_alignment`），导入器写一个固定值只在候选一致时才对。
- **做法（本轮）**：
  - `gap.align_template` 加参数 `roisFrom`（enum `template` / `config`，默认 `template`，advanced），对齐记录带上它；版本 1.0.0 → 1.1.0。
  - 导入器按候选写：`template_candidates` 里带 `rois` 键的写 `template`，没带的、以及单模板写 `config`。
  - `make_roi_set`（source = template 时）与 `gap.locate_template` 的 `info.source` 取对齐记录里的 `roisFrom`（没有这个字段 = 老图，照旧 `template`）；
    `result_bundle` 的推断同样先看它。
  - 回退选了 b（`roi_override`）仍记 `template`，与现在一致。

### (c) `point_counts` 缺字段

- 缺的（`phase-a1-acceptance.md`）：非 line end 配置下的 `flush_ref_roi`；`preprocess_*` 口径在跟随裁剪之前（基线在之后），
  且 `input == preprocess`；`input_*_removed_non_finite`、`filter_before_*`、`segmentation_*`、`consistency_gate_exceeded` 没有端口带；
  `*_global_coarse_*` 只转发选中候选的那条；原算法大量逐圆诊断键；`left / right_point_count` 两个都填合并云的点数。
- 阶段 B（`phase-b-plan.md`）：CSV 要的列已经齐了；diagnostics.jsonl 多出来的键不再产出，相关脚本在 B10 标记改读 bundle 或退休。
- **做法（本轮）**：不补，在 `result_bundle` 的 op.doc 与 gap README 里逐项写明「这些键不产出、原因、以哪个为准」。

## 3. 测试

- (a) gap 导入器的 doctest（`packs/gap/tests/test_import*.cpp`）：图参数 `graphSha256` 在、绑到 `n_bundle.graphSha256`；
  传值后汇总里 `graph_sha256` 等于它（在已有的导入 + 运行用例里加断言）。
- (b) 导入器 doctest：候选带 / 不带 `rois` 时各槽的 `roisFrom`；`result_bundle` 已有用例里加「对齐记录 roisFrom = config → roi_source = config」一行。
- (c) 只改文档。
- 需要 `LYFLOW_PACKS=gap;dts` 构建（本机已装 yaml-cpp）。

## 4. 仍卡在业务定义上的（阶段 B）

- (a) 哈希覆盖什么：图文件本身，还是连同交互计算的参数补丁 / params_json（打过补丁后文件哈希不再代表实际跑的图）；B4 的
  `config_sha256` 改成图文件 sha256 的列名语义。
- (b) 新模型下 `config` 这个来源还有没有意义、CSV 这一列要不要保留原来三种取值。
- (c) diagnostics 里业务还要哪些键（B10）。
- 另：M7 起「逐字段对基线」的验收口径本身待定（`phase-b-plan.md`、`gap-inspector-integration-design.md`）。
