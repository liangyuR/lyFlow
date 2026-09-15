# ADR-0020：评估与扰动是 CLI 子命令，指标是值路径

日期：2026-09-16（M5）　状态：已采纳（`eval` 已实现；`perturb` 与 `edit.translate_region` 在后续步骤）

## 背景

2026-09-15 一个 Agent 在 xyz-gap-inspector 上从零设计并调稳了 KUN10 的 19 个测点
（51 帧同 VIN 重复采集），约 4 小时。事后复盘的硬数字（见 [m5-plan.md](../m5-plan.md) §来源）：

- 九个子命令里只用了 `run` 与 `manifest` 两个。
- 自己写了 12 个 Python 脚本，其中 **JSON Lines 解析器、批跑器、统计各一份** —— 全是重复劳动。
- 「合成位移验证」是判断「测的是不是那条缝」的唯一手段，三版脚本里两版给出错误结论且不报错。
- 51 帧全部参与调参，**没有留出**；零点建立在「这批件处于标称」的未验证假设上。

结论不是「现有子命令不好用」，而是**现有子命令的形态不是 Agent 的工作单元**。
Agent 的工作单元是「一组样本 × 一组参数 → 一个标量指标 → 一组统计」，
而 `sweep` 的 `--metric` 只认 `elementCount | byteSize | durationMs` 三个字段 ——
这是它被重写成脚本的唯一原因。

## 决定

### G1　评估、扰动都是 CLI 子命令，与 `run` 共用 `execute()` 与结果仓

不是独立进程、不是 core 里的新入口、不是编辑器里的面板。
与桌面完全同一条路径（[ADR-0012](0012-headless-cli.md)）；进程内的结果仓让上游在整个网格里只算一次。
MCP（[ADR-0021]）只是它们的另一张皮：CLI 先被人用一轮，再决定 MCP 的工具面。

### G2　`lyflow eval` 取代并包含 `sweep`

```
lyflow eval <graph> [--samples <samples.jsonl> | --samples-glob <pat> --bind <node>.<param>]
                    [--params <paramsets.json>] [--param <node>.<param>=<start>:<end>:<steps>]...
                    --metric <path> [--metric <path>]...
                    [--holdout <tag>=<value>] [--group-by <tag>]
                    [--csv <out.csv>] [--base-dir <dir>] [--parallel <n>] [--no-cache]
                    [--set <node>.<param>=<json>]...
```

`sweep` **保留**为轴展开的语法糖：它现在是一层壳，解析完 `--param` / `--metric` 就去调 eval 的引擎，
`sweep_row` 与 csv 的形状一个字节没变。旧的 `--metric nodeId:port.field` 写法两个子命令都继续接受。

### G3　指标是**值路径**

| 路径 | 取的是什么 |
|---|---|
| `outputs.<名字>` | 图级命名输出（[ADR-0017](0017-graph-outputs-injection-importers.md)）的 value |
| `outputs.<名字>.a.b` | 同上，继续深入对象字段 |
| `nodes.<节点>.<端口>` | 该节点 `stats.outputs[].value` 里那个端口的值 |
| `nodes.<节点>.<端口>.a.b` | 同上，继续深入 |
| `nodes.<节点>.<端口>.elementCount` | 该端口的元素数 |
| `nodes.<节点>.durationMs` / `.elementCount` / `.byteSize` | 节点级 `stats` |
| `run.durationMs` | `run_finished` 的墙钟 |

三条取值规则，都是为了让人写出来的路径和他脑子里想的一样：

- **Measurement 自动拆包。** 路径落在一个带数值 `value` 字段的对象上时取那个 `value`。
  `outputs.gap` 因此直接是个数，不用写成 `outputs.gap.value`（两种都认）。
- **Record 的 `data` 透明。** 写 `outputs.bundle.point_counts.left`，不写 `…bundle.data.point_counts.left`。
- **bool 按 0/1。** `outputs.gap.ok` 是 1 或 0，可以直接对它求 mean 得到通过率。

取到的值不是数也不是 bool（比如 `outputs.<点云>`）时，该样本该指标记为缺失，
在 summary 里进 `failCodes.metric_missing`。

**路径拼错是 `EXIT_USAGE`**，并且 stderr 列出这张图跑一次之后**所有可用的标量路径**。
先跑第一个样本一次来枚举，那一次的结果直接当第一行用，不浪费。
这一条不是锦上添花：Agent 第一次总会拼错，没有这份清单它只能回头读源码。

### G4　样本集是一份 JSON Lines

```jsonc
{ "id": "15-09-2026-07-52-22",
  "set": { "n_load.primaryFile": "…/Master.pcd", "n_load.secondaryFile": "…/Slave.pcd" },
  "tags": { "half": "a" } }
```

`set` 的语义与 `--set` 完全一样（值是 JSON 字面量，解析不出来就当字符串），
与注入（ADR-0017）正交。`tags` 可省。

`--samples-glob <pattern> --bind <node>.<param>` 是一步生成：每个匹配到的文件是一个样本，
绝对路径写进 `--bind` 指的那个参数，`id` 取文件名去扩展名（重名时前面补上父目录名）。
**只支持一个 `--bind`** —— 双相机那类要两个路径的情况请写 `samples.jsonl`，
让「哪两个文件算一帧」显式落在文件里，而不是靠两个 glob 的排序恰好对齐。

行格式**预留 `scene` 字段**：这一轮遇到它直接 `EXIT_USAGE`「本版本不支持 scene 注入」。
将来接业务服务的 sceneId 注入时，已经写好的样本文件不用改（m5-plan §8.1）。

覆盖顺序是**全局 `--set` → 参数组 → 样本的 `set`**，后面的盖前面的。

### G5　统计内建，留出是一等公民

每个（参数组 × 样本）一行 `eval_row`，每个（参数组 × 指标）一行 `eval_summary`：

```jsonc
{"kind":"eval_row","paramSet":0,"params":{…},"sample":"…","tags":{…},"holdout":false,
 "status":"ok","metrics":{"outputs.gap":1.0801},"errors":[],"durationMs":3.2}
{"kind":"eval_summary","paramSet":0,"params":{…},"metric":"outputs.gap",
 "groups":{"train":{"n":26,"ok":26,"failCodes":{},"mean":…,"std":…,"min":…,"max":…,"p2p":…}}}
```

- `std` 是**样本标准差（n-1）**，`n < 2` 时是 `null`。`p2p` = max − min。
- `--holdout <k>=<v>`：带该 tag 的样本进 `holdout` 组，其余进 `train`，两组分别报数。
  **留出是方法论底线，切法由人定** —— 工具只负责把两组数分开报，不替人选切分维度。
- `--group-by <k>`：按 `tags[k]` 分组，缺这个 tag 的样本进 `(none)`。
- 两个一起给时组名是 `train/<值>`、`holdout/<值>`；都不给时只有一组 `all`。
- `failCodes` 是失败原因直方图：节点错误码，校验失败是诊断的 code，
  跑通了但指标取不到是 `metric_missing`。

退出码沿用 ADR-0012：全 ok 是 0，有样本校验失败是 1，有样本执行失败是 2（两者都有取 2），
用法错是 4，Ctrl+C 是 3（当场停，不再跑剩下的样本）。

### G6　扰动 = 图手术 + eval

```
lyflow perturb <graph> --after <node>:<port> --region <json> --axis <x|y|z>=<start>:<end>:<steps>
                       --samples … --metric <path> [--expect <slope>] [--tolerance <v>]
```

新标准算子 `edit.translate_region`（`packs/std-pointcloud`）：输入 cloud，
参数 `region: { kind: halfspace, point, normal } | { kind: box, min, max }` 与 `translation: vec3f`，
输出 cloud；选区内的点加平移，其余原样。确定性、可缓存，在编辑器里也能单独用来「模拟缝张开」。

`perturb` 把它插在 `--after` 指定的端口之后，对 `translation` 的一个分量做轴扫描，其余走 eval，
报每个样本的斜率 `d(指标)/d(位移)`（最小二乘）与残差。
两个真实失效模式都要能在报告里直接看出来：
「读数不响应」是斜率 ≈ 0，「取绝对值折叠」是正负两侧斜率符号相反且残差大。

**不写 900 个合成 PCD。** 复盘里那 900 个文件是三版脚本各写一遍的产物，
其中两版切错了还不报错 —— 把这件事做对一次，做在平台里。

### G7　选区只做几何选区

半空间 `{point, normal}` 与盒，就这两种。
「最深点 + 偏置」这类领域规则**不进平台**：由调用方或 gap 包算出选区后传进来
（将来要自动定选区，就做一个输出 region 的 gap 算子再喂给 `edit.translate_region`）。

复盘里两版切错都出在领域判断。平台能做的是把「切在哪」变成显式的、可审计的输入，
而不是替人猜。文档里要写明：切分线穿过近竖直壁、或压在被选中的锚点上，会得出错误结论，
**选区必须在剖面上核对**。

## 后果

**`sweep` 的 `--metric nodeId:port.field` 三个字段各自翻译成一条路径**，
其中 `byteSize` 翻译成**节点级** `nodes.<节点>.byteSize`：事件流里 `stats.outputs[]` 从来没有过
per-port 的 `byteSize`，老写法在那一档上一直返回 `null`。翻译成节点级是把它修好，不是改语义。

**样本之间顺序跑，`--parallel` 是传给 core 的节点并行度**，与 `run` 同义。
bridge 不自己开线程调 core —— 单次 run 只有几十毫秒，瓶颈是判断不是算力（复盘：墙钟 0.05 s／次），
为一个不在关键路径上的加速去赌 core 的 run 可不可重入不划算。

**缓存默认开。** 参数变了 cacheKey 就变，不会拿到错误答案（[ADR-0007](0007-cache-authority.md)）；
一张 19 节点的 gap 图扫 16 组参数 × 51 帧（816 次运行）在本机不到一分钟。`--no-cache` 仍然在。

**每个样本先单独 validate 一次。** 校验失败与执行失败是两个退出码，混在一次 run 里分不开 ——
与 `run` 的理由完全一样。代价是每行多一次 core 往返，那是微秒级的。

**实现落在 `bridge/src/eval.rs`**，`cli.rs` 只留 `cmd_sweep` 这层壳与子命令分发。
纯逻辑（路径解析与求值、样本装载、glob、参数组展开、分组统计、CSV）不碰 core，
`cargo test` 能直接对着手算的数字断言。`cargo build --bin lyflow --no-default-features` 照旧不碰 Tauri。

**不做自动优化器**（m5-plan G13）。复盘的数字很清楚：上万次 run 累计也就几分钟，
瓶颈从来不是搜索速度，是「这个读数到底是不是那条缝」的判断。
平台负责把判断所需的证据一次拿全，判断留给人和 Agent。
