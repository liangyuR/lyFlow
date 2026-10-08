# glue 的 MCP 调参与评估

本流程基于已合并的 glue 包。检测和量测由 C++ core 执行，真值匹配由 Rust CLI 执行，MCP 管理冻结试验、搜索预算与呈现。先配置 [MCP 服务](mcp.md)，并用 `LYFLOW_PACKS=glue` 构建 core 和 CLI。将 `LYFLOW_WORK_DIR` 设为持久目录，才能在 MCP 重启后续跑。

## 1. 确认环境与参数入口

依次调用 `get_environment`、`inspect_graph {graphPath}`。前者比较 HTTP/CLI manifest 与构建指纹；`compatible` 只说明清单相同，`buildVerified` 才证明指纹相同。远端未提供指纹时会给提示，不能据此声称单图与离线性能可比。

`inspect_graph.params` 的值来自 `lyflow params`，同时给出 `role`、`graphParam`、`editPath`、单位与文件依赖。角色由算子 manifest 的 `tuningRole` 定义：

| 角色 | glue 示例 | 搜索行为 |
|---|---|---|
| `input` | 模板、stations、标定文件 | 固定；逐样本输入可覆盖 |
| `geometry` | nozzle、zone、sector、模板 anchor | 默认固定；明确给 `allowedRoles:["detection","geometry"]` 才能搜索 |
| `detection` | 对比度、搜索范围、边缘选择、断口最短长度 | 默认搜索入口 |
| `acceptance` | judge 的宽度/边距限值、允许断口长度 | 搜索始终锁定，比较时要求相同 |
| `unspecified` | 未声明角色的旧算子参数 | 搜索锁定，先补 manifest |

绑定到顶层图参数的节点字段只通过 `graphParams` 修改。`n_breaks.minLength` 绑到 `breakMin` 时，搜索写 `space:{"breakMin":[8,12]}`；写节点字段返回 `param_conflict`，并给出顶层名。

`validate_graph`、`plan_graph`、`run_graph` 使用相同的 `graphParams` / `set` / `recipe`。单图的 `set` 是解析过的对象；CLI 包装工具的 `set` 是 `node.param=<JSON>` 字符串数组。优先级为图默认值 → 配方 → 评估参数组 → 显式 graphParams → 样本 graphParams/set。扫描轴仍写 `param:["node.param=8:12:3"]`；顶层离散候选写 `params:[{"breakMin":8},{"breakMin":12}]`。

冻结节点扫描轴时保留 CLI 的分量广播：例如 `voxel.leafSize=0.01:0.05:5` 每档冻结为三个相同分量，续跑、失败 replay 和导出均使用完整向量。分量数依次取图中当前数组、manifest 默认数组、局部子图参数默认数组。

`unitSource` 表示参数单位取决于某个运行输出，例如 `bead.info.unit`。检查图时不假装已经解析成 mm；应先 full 运行，确认校准、输出单位和图像尺度。

## 2. 给样本和真值

`eval.samples` 可直接给样本数组，也可以给 `samplesPath` JSONL。目录与 glob 选择沿用 CLI；三种文件来源与内联数组互斥。每行 `id` 非空且唯一，`tags.workpiece` 标识工件。同一工件的多帧必须使用同一分组标签。

机器可读定义在 [`eval-sample.schema.json`](../schema/eval-sample.schema.json)，完整随动/飞拍样例在 [`eval-samples.example.json`](../schema/examples/eval-samples.example.json)。

```json
{
  "id":"part001-frame03",
  "set":{"n_load.path":"frames/003.png"},
  "tags":{"workpiece":"part001","batch":"A"},
  "truth":{
    "pose":{"output":"pathInfo","ok":true},
    "verdict":{"output":"verdict","ok":false},
    "measurements":[{"path":"outputs.bead.stations.width.mean","value":20,"unit":"px","unitPath":"outputs.bead.info.unit","tolerance":2}],
    "breaks":{"output":"breaks","coordinate":"image_px","lineOutput":"line","intervals":[{"start":[250,300],"end":[260,280]}],"minIou":0.5,"endpointTolerance":4,"maxProjectionDistance":10}
  }
}
```

图级输出需声明 `pathInfo → n_path:path.info`、`line → n_path:path.line`、`bead → n_width:bead`、`breaks → n_breaks:breaks`、`verdict → n_judge:verdict`。Bundle 字段可直接命名为图输出；指标也能读取 `outputs.bead.stations.width.mean`。

断口默认坐标 `path_s_px`，用 `sStart < sEnd` 表示沿胶路的像素距离。`image_px` 使用原图上的 start/end；每个候选投影到自己的 line.points / line.s，避免胶路变化时照搬旧的 s。投影失败计缺失，不能算通过。`length` 的 mm 与 `sStart/sEnd` 的 px 是不同维度。

区间按正重叠和 `minIou` 做一对一最大数量匹配；一个预测不能抵消多个真值。空 `intervals` 表示确认没有断口，预测出的段计 FP。没有标注 breaks 表示未知，不等于没有断口。量测只有单位一致才计算误差，null、单位不符和定位失败都有独立缺失原因。

绝对量测误差不跨单位汇总。同一帧或同一统计组含有效的 px 与 mm 误差时，`measurementMae` / `measurementMaxError` 及其分布返回 null；行证据保留逐项误差和 `measurementErrorUnits`，汇总给出 `units` 与 `incomparableReason: "mixed_units"`。按单位分组后可分别比较。`measurementWithinTolerance` 是各量测满足自身 tolerance 的比例，可跨单位汇总。

### 随动与飞拍

随动检测使用 glue1 / glue2 图，离线帧设置 `n_load.source="file"`，每样本覆盖 `n_load.path`。nozzle/zone/sector 固定后先检查胶路，再评估宽度、边距与断口。

飞拍检测使用 flyshot 图，样本 `graphParams` 给 template、anchor、stations、calib；`truth.pose.output` 指向定位 Record，量测路径指向 `glue.StationMeasure` 的 width/distance 等字段。飞拍图只量不判，未声明 Verdict 时不填 verdict 真值；跨帧合并与最终产品判定仍由宿主负责。

glue 使用完整 u8 灰度/RGB/RGBA 图像和 `mode:"full"`；不从预览或缩小图量测。CLI 的 `--input` 是 PCD 注入，图像离线输入走 load_image 的 file/path。

## 3. 冻结分组，先训练再验证

```json
{
  "graphPath":"D:/workspace/glue.lyflow.json",
  "samplesPath":"D:/workspace/samples.jsonl",
  "split":{"groupTag":"workpiece","seed":"trial-001","train":0.6,"validation":0.2,"holdout":0.2},
  "space":{"breakMin":[8,12,16]},
  "mode":"one_factor",
  "objectives":[{"metric":"run.durationMs","direction":"minimize","stat":"p95","weight":1}],
  "constraints":[{"metric":"quality.defectMissRate","max":0.02,"stat":"mean"},{"metric":"quality.defectFp","max":0,"stat":"max"}],
  "maxRuns":1000,
  "timeoutMs":600000,
  "jobs":1
}
```

调用 `start_tuning` 返回 jobId，立即用 `get_job` 查询。分组按 seed 和工件标签稳定划分，与样本顺序无关；同一工件不跨集合。比例总和必须为 1，每个正比例集合至少需要一个工件组。搜索只执行 train；validation 和 holdout 的完整样本文件在快照中保留。

`one_factor` 提供基线及逐参数候选和数值敏感度；`grid` 做有限笛卡尔组合。组合最多 10,000，用户的 `lockedParams` 额外锁定。评分是声明权重与方向的加权和；不同单位混合时需要自己给合理尺度。敏感度 slope 只针对该评分尺度，离散或复合参数不输出数值 slope。

候选必须完整执行且 executionOk=1，定位/量测/判定输出缺失不能参与排名。有标注时，默认要求定位失败、FN、FP、误放行、误拒绝为零；标注给出量测 tolerance 或断口 endpointTolerance 时，默认要求相应 `WithinTolerance` 的 min=1，即每帧已匹配的标注全部满足容差。相应显式质量约束可以定义非零限额，或用 `WithinTolerance` 的 min 定义通过率及统计方式。欠约束的方向不能解除默认门槛，例如 defectFn 的 min 不替代其 max，WithinTolerance 的 max 不替代其 min。质量指标缺失不会被填成 0。defectRecall/defectMissRate 的 mean 是逐帧宏平均，quality_summary 另给按 TP/FN 汇总的微平均；productFalseAccept/Reject 是每帧 0/1，汇总 falseAcceptRate/falseRejectRate 分别以 NG/OK 真值样本为分母，必须同时读输出缺失数。训练集没有真值时只证明已声明的指标，不证明检测准确率。

`get_job.validationArgs` 可直接交给 `eval`。另对相同 frozen graph / dataset / split 做一次 `params:[{}]` 基线验证，再 `compare_evaluations {baselineId,candidateId,objectives,constraints}`。默认比较 validation，给出满足约束情况、改善/退化/缺失样本和完整比较 artifact。同输入、构建、分组、缓存和验收规格是比较前提。

使用 `groupBy` 或 CLI `holdout` 时，显式指定汇总中的完整 `group` 名，如 `A` 或 `holdout/A`；使用默认 split 分组时可指定 `validation`。逐样本计数、例子和 artifact 只包含该统计组，前后评估的分组规则也必须相同。比较 `quality.*` 时，未在原 eval 的 metric 中请求的指标仍读取每行自动保存的 `quality.metrics`；真实缺失保持 null。

候选确定后将同一 eval 参数改成 `partition:"holdout"`，作为最后的独立验收。工具不会自动拿留出集排名；工作流中不应根据 holdout 结果再次挑参数。

## 4. 记录、预算与失败回放

每个 evaluationId 保存图及原图、有效参数、配方、候选、样本/真值、划分、缓存策略、manifest、core 指纹、CLI 可执行文件 SHA-256 和外部文件 SHA-256。运行前后核对依赖，变化则评估失败；续跑和导出也重新核对。输入文件不复制，需保留其路径和内容。图/样本/配方快照和 artifact 位于 `LYFLOW_WORK_DIR`。

CLI 完整 JSON 行到达时立即追加 rows.jsonl，尾行没有换行也会处理。stderr 有大小上限。超时/取消保留已完成行，终止 CLI；HTTP 的取消返回 requested/acknowledged/executionFinished，确认接受取消和实际执行结束分别表示。

`get_job` 给 startedRuns、runBudget、elapsedMs、timeBudgetMs。每次实际开始算一次尝试，取消后的重试也消耗预算；`maxRuns` 到期是 budget_exhausted。`cancel_job` 请求取消，随后查询收尾状态。`resume_job {jobId,additionalRuns?,additionalMs?}` 使用原候选和分组，跳过已完成行；对应预算用尽必须显式追加。MCP 正常关闭会取消后台子进程；进程意外中断后，遗留 running 状态会被识别为 interrupted。

离线评估默认 `cachePolicy:"cold"`，传 CLI `--no-cache`。`warm` 明确允许缓存，但跨进程命中还需要配置 `LYFLOW_CACHE_DIR`。报告保留策略，两种策略不能互相比较；p50/p95 反映图运行时间，不包含 MCP 的快照和展示开销。

`get_evaluation {evaluationId,failuresOnly:true,offset:0,limit:10}` 同时列出执行失败和标注质量失败。汇总最多返回 100 条、每条最多 20 个组，并标记截断；get_evaluation 的 paramSet 可过滤更多摘要，完整 summariesArtifact 可按字段路径和数组分页读取。任务查询只返回有界候选与排名。每行 replay 可直接调用 `run_graph`；再用 `view_output_image` 的 overlays 引用同一个 run 的 overlay 输出，ROI 用原图 `[x,y,width,height]`。PNG 仅用于显示，原图坐标 = origin + PNG 像素 × pixelScale，编号的文字在 labels。

Record/Bundle 用 `summarize_output {fields,offset,limit}` 读取；返回数值分布、缺失计数、类别分布和完整 artifact。外层分页不再裁掉内层点坐标。`read_artifact` 通过 artifactId 分页取已封存结果，任务续跑不会改变旧的 artifact。

## 5. 导出与边界

`export_candidate {evaluationId,paramSet:0,format:"graph"|"recipe",out?}` 要求评估完整、身份一致，写新文件和 `.provenance.json`，保留评估引用、输入/构建身份、划分与缓存策略。已有文件不会覆盖。图支持节点和顶层候选；配方只能表达顶层参数，节点覆盖需导出图。返回 targetGraph/baseDir 供配方回放；相对输入仍按 provenance.baseDir 解析。

导出证明候选已执行评估，不自动宣称达到现场质量指标。先审核 validation/holdout 约束和失败证据，再采用候选。本次代码验收使用合成图和标注正反例；真实模板、stations、标定与现场漏检率仍需产线数据验证。点云 `perturb` 的几何位移不适用于图像真值。


本次逐项检查与测试结果见 [实现验收](mcp-tuning-acceptance.md)。
