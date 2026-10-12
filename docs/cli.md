# 命令行使用指南

[文档首页](README.md) · [快速开始](getting-started.md)

`lyflow` 是同一个 crate 的第二个 bin，**不依赖 Tauri**，与桌面共用同一份加载、校验、
迁移和执行代码（[ADR-0012](adr/0012-headless-cli.md)）。构建：

```powershell
pnpm cli:build
```

产物为 `bridge/target/release/lyflow.exe`，与 `lyflow_core.dll` 同目录；构建使用 `--no-default-features`。

stdout 是 **JSON Lines**（`run` 输出的就是 ExecutionEvent 原样，一行一条），
stderr 给人看。退出码：`0` 成功、`1` 校验失败、`2` 执行失败、`3` 被 Ctrl+C 取消、`4` 参数错。

```text
lyflow run      graph.lyflow.json [--to nodeId]... [--set nodeId.param=<json>]...
                                  [--recipe r.lyflow-recipe.json] [--param name=<json>]...
                                  [--base-dir d] [--parallel n] [--no-cache] [--cache-dir d]
                                  [--preview] [--preview-points n] [--outputs] [--summary]
                                  [--input nodeId.port=file.pcd]...
lyflow cache    info|clear --cache-dir d [--stale]
lyflow import   file --kind kind [--fine] [-o out.lyflow.json] [--base-dir d]
lyflow validate graph.lyflow.json [--base-dir d] [--set ...] [--recipe ...] [--param ...]
lyflow plan     graph.lyflow.json [--to nodeId]... [--base-dir d] [--set ...] [--recipe ...] [--param ...]
lyflow params   graph.lyflow.json [--node nodeId]... [--only explicit|default|bound|graph]
                                  [--set nodeId.param=<json>]... [--recipe ...] [--param name=<json>]...
                                  [--base-dir d] [--json]
lyflow migrate  graph.lyflow.json [--write]
lyflow manifest [--check]
lyflow info
lyflow samples  [<样本集>]
lyflow dump     graph.lyflow.json nodeId:port out.pcd [--format binary|ascii|binary_compressed]
                                  # 图像输出写 out.lyim（LYIM 载荷原样落盘）
lyflow sweep    graph.lyflow.json --param nodeId.param=start:end:steps [--param ...]
                                  --metric nodeId:port.elementCount|byteSize|durationMs
                                  [--csv out.csv] [--base-dir d] [--jobs n]
lyflow eval     graph.lyflow.json [<样本集>] [--params sets.json] [--param n.p=start:end:steps]...
                                  (--metric <值路径> [--metric ...] | --list-metrics)
                                  [--holdout tag=value] [--group-by tag] [--csv out.csv]
                                  [--base-dir d] [--parallel n] [--jobs n] [--no-cache]
                                  [--set ...] [--recipe ...] [--param name=<json>]... [--summary]
                                  [--max-runs n] [--resume-rows rows.jsonl] [--progress-json]
lyflow perturb  graph.lyflow.json --after nodeId:port --region <选区 JSON>
                                  --axis x|y|z=start:end:steps [<样本集>]
                                  --metric <值路径> [--metric ...]
                                  [--expect slope] [--tolerance v] [--csv out.csv]
                                  [--base-dir d] [--parallel n] [--jobs n] [--no-cache] [--set ...]
lyflow diff     a.lyflow.json b.lyflow.json [--json]
lyflow recipes  graph.lyflow.json [--recipe r.lyflow-recipe.json]... [--json]
lyflow patch    graph.lyflow.json [--remove-node id|glob]... [--add-node <json>]...
                                  [--rewire 节点:端口=节点:端口]... [--connect 节点:输出=节点:输入]...
                                  [--set ...] [--recipe ...] [--param ...]
                                  [--dry-run] [-o out] [--json] [--base-dir d]

<样本集> 三选一，eval 与 perturb 共用：
  --samples samples.jsonl
  --samples-glob pat --bind n.p
  --samples-dir root --bind-pair n.pA,n.pB --pattern globA,globB    # 单文件时 --bind n.p --pattern glob
                [--sample-subdir name] [--sort-by name|mtime] [--split-half tagKey]
  另可加 --samples-jsonl-out path，把生成的样本集写出来核对、复用
```

每个选项的完整说明（`--input` 的两种注入、`patch` 的动作顺序、`perturb` 的单位、配方的四类失配……）见 `lyflow --help`。
`--cache-dir`（或环境变量 `LYFLOW_CACHE_DIR`）在 `run` / `eval` / `sweep` / `perturb` 上都认：结果缓存落盘，
默认关，按产品版本、源码修订和缓存格式版本组成的构建身份分目录。只有具有有效源码修订的受控正式发布构建才能启用；普通开发构建的 `buildIdentity` 为 `null`，启用落盘缓存会被拒绝，旧哈希目录也不再复用。具体条件见 [磁盘缓存设计](disk-cache-plan.md)。

`eval` 与 `perturb` 的 `--metric` 是**值路径**：`outputs.gap`、`nodes.n_fit.quality.rmsResidualMm`、
`nodes.v.elementCount`、`run.durationMs`。路径拼错时 stderr 会列出这张图上所有可用的标量路径，
`eval --list-metrics` 也能直接列出来（[ADR-0020](adr/0020-eval-and-perturb-as-cli.md)）。
样本集的目录模式（`--samples-dir`）把「双相机配对 + 按时间前后各半打 tag」也内建了，
不用再为每个测点写生成脚本。用法见 [Agent 调参指南](agent-tuning.md)。

## 常用操作

下面假设 `lyflow.exe` 已加入 PATH，`demo.lyflow.json` 是用编辑器保存的图；节点 id 和图参数名需替换成实际值。本地构建时也可以用 `& .\bridge\target\release\lyflow.exe` 调用。

```powershell
lyflow info
lyflow manifest --check
lyflow validate demo.lyflow.json
lyflow plan demo.lyflow.json
lyflow run demo.lyflow.json --summary
lyflow params demo.lyflow.json --only explicit --json
lyflow run demo.lyflow.json --set 'n_voxel.leafSize=[0.02,0.02,0.02]'
lyflow run demo.lyflow.json --param gapOffset=0.12
lyflow diff before.lyflow.json after.lyflow.json --json
lyflow patch demo.lyflow.json --remove-node unused --dry-run --json
```

`info` 给出当前 core 的版本、构建身份（`buildIdentity`）、实际 DLL 路径、算子总数和库目录；切换构建或比较 HTTP/CLI 环境时先检查它。算子包与具体算子通过 `manifest` 查看。`diff` 忽略节点坐标等 UI 布局差异。`patch` 会修改图文件，先用 `--dry-run` 检查；动作顺序是 remove → add → rewire → connect → set → recipe → param，任一步校验失败都不写回。

## 样本评估与灵敏度

先列出实际指标，再扫描参数。下面以双文件测点图为例：

```powershell
lyflow samples --samples-dir kun10/sensor --sample-subdir 4 --bind-pair n_load.primaryFile,n_load.secondaryFile --pattern '*Master*.pcd,*Slave*.pcd' --split-half half > samples.jsonl
lyflow eval demo.lyflow.json --samples samples.jsonl --list-metrics
lyflow eval demo.lyflow.json --samples samples.jsonl --param n_fit.distThresh=0.2:0.8:4 --metric outputs.gap --holdout half=b --jobs 4 --csv eval.csv
```

`samples` 仅生成规范化 JSONL，不执行图。`eval --list-metrics` 会用第一组参数和第一个样本运行一次，以确认可读取的标量路径。`--jobs` 控制同时运行几张图；`--parallel` 控制每张图内部的节点并行度。`--max-runs` 限制本次新增尝试，`--resume-rows` 复用已完成的评估行；`--progress-json` 增加结构化任务进度。

灵敏度检查在指定点云端口后插入位移，再观察量测输出：

```powershell
lyflow perturb demo.lyflow.json --after n_frame_s:cloud --region '{"kind":"halfspace","point":[0.0134,0,0],"normal":[1,0,0]}' --axis x=0:0.0006:5 --samples samples.jsonl --metric outputs.gap --expect 1000
```

`region` 和 `axis` 的位移单位是米；当量测值单位为毫米时，位移 1 mm、读数增加 1 mm 对应 `--expect 1000`。选区需要与插入端口处的点云处于同一坐标系。逐帧锚点、留出集、分组统计和参数来源详见 [Agent 调参指南](agent-tuning.md)；配方格式与失配规则见 [参数配方](recipe.md)。

## 输出与退出码

`run` 默认逐行输出执行事件；`--summary` 额外输出 `run_summary`，`status` 为 `ok`、`degraded` 或 `failed`。`eval` 每行默认不带 summary，按需用 `--summary` 打开。做业务判定时读取 summary，字段说明见 [ADR-0022](adr/0022-run-summary-as-core-output.md)。

| 退出码 | 含义 |
|---|---|
| `0` | 成功 |
| `1` | 图或参数校验失败 |
| `2` | 执行失败 |
| `3` | 被 Ctrl+C 取消 |
| `4` | 命令参数错误 |

CLI 当前的完整选项以 `lyflow --help` 为准。MCP 的对应工具和字段见 [MCP 服务](mcp.md)，冻结试验与预算搜索见 [glue 调参流程](mcp-glue-tuning.md)。
