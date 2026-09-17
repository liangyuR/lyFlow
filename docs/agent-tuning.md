# 用 CLI 调一组测点的工作法

面向**只拿得到 `lyflow` 命令行（或 MCP）、拿不到仓库源码**的人和 Agent。
一次真实任务的复盘（51 帧重复采集、19 个测点、无测量基线）里，做这件事的人写了 12 个
Python 脚本，其中解析器、批跑器、统计各一份，合成位移验证三版里两版给出错误结论且不报错。
这份文档把那些脚本换成六条命令，并且把两个最容易踩空的判断写在前面。

## 1. 先读 `preconditions`，再看图

```bash
lyflow manifest | jq '.operators[] | select(.id=="gap.notch_width")
                      | {doc, preconditions, params: [.params[].name]}'
```

`doc` 回答「这个算子干什么」，`params[].doc` 回答「这个旋钮是什么」，
**`preconditions` 回答「什么时候它整个不该用」** —— 后者才是改图时真正要的东西，
它不在自由文本里，是一个结构化数组，可以直接过滤。

先读算子、再读图。图里的参数值只告诉你「上一个人选了什么」，
`preconditions` 才告诉你「他为什么不能选别的」。
遇到读数不对，第一件事是逐条核对这张图有没有踩到某条前提，而不是先去扫参数。

## 2. `validate` 先于 `run`

```bash
lyflow validate graph.lyflow.json && lyflow run graph.lyflow.json --outputs
```

退出码：`0` 成功、`1` 校验失败、`2` 执行失败、`3` 被 Ctrl+C、`4` 用法错。
校验失败和执行失败是**两个不同的码**，别用一个 `try/except` 吞掉：
前者是图写错了（改图），后者是数据喂不进去（换样本或改参数）。

`run` 之后节点状态里 `state=skipped` **不代表没有输出** ——
看 `stats.outputsAvailable`：缓存命中、静音透传、外部注入都是 `skipped` 但输出照样在，
只有 `reason=not_demanded`（这一支根本没被需要）才是真的什么都没有。

## 先看 summary，不要自己数节点

```bash
lyflow run graph.lyflow.json --summary | tail -1
```

最后一行就是这一轮的结论（[ADR-0022](adr/0022-run-summary-as-core-output.md)）。
**别再从 `node_state` 事件里重建成败判定** —— 那要同时处理缓存命中、惰性分支没被 demand、
失败被 `acceptsError` 端口接住三种情况，业务侧做错过两次。

读的顺序是三步。

**① `status`。** 三态，一行定性：

| 值 | 意思 | 该做什么 |
|---|---|---|
| `ok` | 零个节点出错 | 往下走 |
| `degraded` | 有节点坏了，但每一维声明输出要么拿到了值、要么本来就不要 | **结果可用**，但去看是谁坏了 |
| `failed` | 有一维声明输出本该有却崩了（或图没声明输出且有节点出错） | 结果不可用 |

`run_finished.status` 是另一回事，别拿它当成败判定。带 fallback 的图里
「模型路径炸了 → 模板路径接住 → 量出来了」是 `run_finished: ok` + `summary: degraded`：
退出码 0，但确实有东西坏了，值得去看日志。

**② `outputs` 的每一维。** 也是三态，`inactive` 与 `failed` 千万别混：

```jsonc
"outputs": {
  "gap":    { "state": "value",    "value": { "kind": "Measurement", "value": 3.52 } },
  "flush":  { "state": "inactive", "reason": "not_demanded" },  // 这个点位本来就不量 flush
  "bundle": { "state": "failed", "from": "n_fit_datum", "code": "insufficient_points" }
}
```

`inactive` 是「这一维本来就没有」，`failed` 是「本该有、崩了」。判 NG 只看 `failed`。
`from` 已经沿边回溯到最近的那个出错节点了 —— 不用自己顺着边找根因。

**③ `decisions`。** 全图每一个 fallback / select 选了哪一路，一次给全：

```bash
lyflow run graph.lyflow.json --summary | tail -1 | python -c "
import json, sys
for node, d in json.load(sys.stdin)['decisions'].items():
    print(node, d['choice'], d.get('reason', ''))"
```

条数应当等于图里 `flow.fallback` + `flow.select` 节点的个数。少了就是有节点这一轮没跑到，
去 `nodes` 里查它的 `state`。**别再拿某个下游算子的 `roi_source` 之类的字段去猜
「模板路径用没用过」** —— 那个字段回答的是别的问题，猜错过两次。

批量跑时同一份东西在每行 `eval_row` 里（`--no-summary` 可以关掉以省体积）：

```bash
lyflow eval graph.lyflow.json --samples s.jsonl --metric outputs.gap \
  | grep eval_row | python -c "
import json, sys
for line in sys.stdin:
    r = json.loads(line)
    s = r.get('summary', {})
    if s.get('status') != 'ok':
        print(r['sample'], s.get('status'),
              {k: v['state'] for k, v in s.get('outputs', {}).items()})"
```

## 改图结构用 `patch`，不手改 JSON

`--set` 只改参数。**删节点、加节点、改接线走 `lyflow patch`**，不要复制一份 JSON 手改 ——
手改的两个代价是漏删边（悬空边的报错出现在别处）和「不知道自己到底改了什么」。

```bash
# 1. 先看差异。输出与 lyflow diff 逐字相同，不写任何文件
lyflow patch graph.lyflow.json --remove-node 'b_*' \
       --rewire n_fb_line:out=n_fit_base:line --dry-run

# 2. 认了再写。省略 -o 就原地覆写；--json 给一行回执
lyflow patch graph.lyflow.json --rewire n_fb_line:out=n_fit_base:line -o short.lyflow.json --json
```

- **动作顺序定死 remove → add → rewire → set**，与你打字的先后无关。所以「先改接线、再删被短接掉
  的那条分支」是**两条命令**：第一条只 `--rewire`，第二条才 `--remove-node`。
  反过来写（一条命令里又删又接）会报「图里没有节点 X」，因为删在前。
- `--remove-node` 连带删它的所有边；glob 只对 id（`b_*` 这种）。
  **图级 `outputs` 还指着的节点不给删** —— 报错、整体不写，而不是静默把那个读数删掉。
- `--rewire <节点>:<端口>=<节点>:<端口>` 把**所有**从左端口出发的边改为从右端口出发，
  这是「短接掉一段」的写法。
- `--add-node '{"id":…,"op":…}'` 只加节点，加不了边；新节点要么是不需要输入的源算子，
  要么配 `--rewire` 把已有的边挪到它身上。
- **可以重跑。** 删不存在的 id、左端口已经没有出边的 rewire、同值的 set 都是 no-op 并在 stderr 说一句，
  所以同一条命令跑两遍第二遍什么都不做，`lyflow diff` 为空。这正是「改一处 → 全量重跑 →
  证明只有该动的那几格动了」这条工作法的前提。
- 每一步之后过形状校验，最后过 `validate`；**任一步不过就整体不写**，退出 1 并把全部诊断打在 stdout。
  所以「dry-run 说能改」等于「写下去一定合法」。

一个真实例子（带 12 个 `flow.fallback` 的模型路径图，要的是「关掉备用分支，只走主路径」）：

```bash
# 第一步：12 个 fallback 各自短接到它的 a 源
lyflow patch g.lyflow.json --rewire n_fb_line:out=n_fit_base:line \
                           --rewire n_fb_merged:out=n_merge:cloud ... -o g.short.json
# 第二步：这时 n_fb_* 与整条 b 分支都没人要了，一起删
lyflow patch g.short.json --remove-node 'n_fb_*' --remove-node 'b_*'
```

只做第一步里的 `--remove-node 'b_*'` 会被拦下来：12 个 `flow.fallback` 的 `b` 口是必填输入，
删掉 b 分支之后它们全悬空，`patch` 报 12 条 `missing_input` 并且一个字节都不写。

## 3. `eval`：一组样本 × 一组参数 → 一个标量 → 一组统计

样本集是一份 JSON Lines，一行一帧：

```jsonc
{"id":"15-09-2026-07-52-22",
 "set":{"n_load.source":"files",
        "n_load.primaryFile":"…/Master.pcd","n_load.secondaryFile":"…/Slave.pcd"},
 "tags":{"half":"a"}}
```

`set` 的键是 `<节点>.<参数>`，语义与 `--set` 完全一样。

**这份文件不用手写。** 采集通常落成「一帧一个目录」的形状，`--samples-dir` 直接认它：

```bash
lyflow eval graph.lyflow.json \
  --samples-dir <采集根> --sample-subdir 4 \
  --bind-pair n_load.primaryFile,n_load.secondaryFile \
  --pattern "*Master*.pcd,*Slave*.pcd" --split-half half \
  --metric outputs.gap --holdout half=b
```

`<采集根>` 下每个直接子目录是一帧（给了 `--sample-subdir` 就在 `<帧>/<name>/` 下找文件），
两个 glob 各要在那一帧里**恰好匹配到一个**文件，分别写进 `--bind-pair` 的两个参数；
匹配到 0 个或多个就退出 4 并报出是哪一帧 —— 「哪两个文件算一帧」仍然是显式的，
只是不用再自己写脚本拼。样本 id 取帧目录名。

`--split-half half` 把「按时间前后各半打 tag」也内建了：排序后前一半 `"half":"a"`、
后一半 `"half":"b"`（奇数时前半多一个），直接配 `--holdout half=b`。
排序默认 `--sort-by name`，它先从帧目录名里读 `dd-MM-yyyy-HH-mm-ss`
（`12345678998765432_14-09-2026-03-44-38` 这种）按真实时间排，读不出来才退回字典序，
并在 stderr 说一句；目录名带别的时间格式时改用 `--sort-by mtime`。
**这一组选项取代了以前那个「给每个测点生成一份 samples.jsonl」的脚本** ——
盲测里唯一还需要写脚本的地方就是它。想核对或复用生成的结果就加 `--samples-jsonl-out <path>`。
`perturb` 用的是同一组选项。

手写样本集仍然支持（`--samples`）；单路径的情况也可以用
`--samples-glob <pat> --bind <节点>.<参数>` 一步生成。三种样本源只能给一个。

```bash
lyflow eval graph.lyflow.json --samples frames.jsonl \
            --param n_notch.lineDistThresh=0.1:0.4:4 \
            --metric outputs.gap --metric nodes.n_notch.quality.cameras.primary.cornerDepthMm \
            --holdout half=b --csv eval.csv
```

`--metric` 是**值路径**，不是三个固定字段：`outputs.<名字>`、
`nodes.<节点>.<端口>[.字段...]`、`nodes.<节点>.durationMs|elementCount|byteSize`、`run.durationMs`。
Measurement 自动拆包（`outputs.gap` 直接是个数），bool 按 0/1（`outputs.gap.ok` 求 mean 就是通过率）。
**路径拼错会退出 4 并在 stderr 列出这张图上所有可用的标量路径** —— 第一次拼错是常态，照着列表改。

### 留出：按时间前后各半

`--holdout <tag>=<value>` 把带该标签的样本分到 `holdout` 组，其余进 `train`，两组分别报数。
**切法由你定，工具不替你选。** 同一批重复采集里，时间通常是唯一有结构的维度
（前后半段之间夹着环境变化与漂移），随机切会把它抹掉。所以在样本文件里按采集时间排序，
前一半打 `"half":"a"`、后一半 `"half":"b"`，然后只看 `train` 选参数，选完再看 `holdout` 掉了多少。

### 读 summary 的顺序：先 failCodes，再 std

```jsonc
{"kind":"eval_summary","paramSet":2,"params":{"n_notch.lineDistThresh":0.3},
 "metric":"outputs.gap",
 "groups":{"train":{"n":26,"ok":19,"failCodes":{"notch_too_shallow":7},
                    "mean":0.31,"std":0.042,"min":0.24,"max":0.41,"p2p":0.17}}}
```

**先看 `ok/n` 与 `failCodes`，再看 `std`。**
`std` 只统计成功的那些样本：一组参数把 7 帧算失败了，剩下 19 帧当然更整齐 ——
成功率低的组 std 好看是**陷阱**，不是优势。比较两组参数前先确认它们的 `ok` 数相当；
不相当就先解释那几个 `failCodes`，它们通常在告诉你这组参数踩到了某条 `preconditions`。

## 4. `perturb`：std 小不等于测对了缝

这是最重要的一节。**重复性与正确性是两件事。**
51 帧同一个零件重复采集，`std` 小只证明「这个读数很稳」，
不证明「这个读数是那条缝的宽度」—— 一个把两台相机都被遮挡的阴影当成缝来量的图，
读数同样很稳。真实任务里的 Audio_1 就是这样：std 漂亮，但读数对缝张开根本不响应。

唯一的判据是**合成位移**：把缝的一侧推开已知的量，看读数加没加上同样的量。

```bash
lyflow perturb graph.lyflow.json --after n_frame_s:cloud \
   --region '{"kind":"halfspace","point":[0.01345,0,0],"normal":[1,0,0]}' \
   --axis x=0:0.0006:5 --samples frames.jsonl \
   --metric outputs.gap --expect 1000 --tolerance 100
```

它在 `--after` 指的端口后插一个 `edit.translate_region`，把原先从那个端口出发的边全部改到新节点，
然后对 `translation` 的一个分量做等距扫描，报每个样本的 `slope = d(指标)/d(位移)`。

**单位。** `--region` 的 `point` / `min` / `max` 与 `--axis` 的位移**都是米** ——
它们和输入点云同帧同单位，而点云在传感器帧和测量帧里都是米
（`gap.to_measurement_frame` 只交换 y 与 z，不换单位）。
只有 `outputs.*` 这类 **Measurement 是毫米**。
所以「张开 1 mm 读数加 1 mm」是 `slope ≈ 1000`，`--expect` 写 1000 不是 1；
位移范围也按米写：`x=0:0.0006:5` 是 0 到 0.6 mm 五档。
同一句话写在 `edit.translate_region` 的 `doc` 与 `lyflow perturb` 的 `--help` 里，
不用回头翻文档。

### `--after` 插在哪一路相机后面

**插在被 `camera` 参数选中的那一路之后。** gap 的图通常有两条相机支路
（`n_frame_p` / `n_frame_s` 这类），而下游算子用一个 `camera` 参数决定读哪一路：
`camera=primary` 就要 `--after n_frame_p:cloud`，`camera=secondary` 才是 `--after n_frame_s:cloud`。
插错一路的症状是**整批不响应**（`nonResponsive` 接近样本数），
很容易被误读成「这张图测的不是那条缝」—— 先核对 `camera`，再怀疑选区。
本文档后面的例子写 `n_frame_s` 只是因为那张图恰好用 secondary，不是默认值。

`camera=both`（两路各算一次再合并）时没有「那一路」：`--after` 只接受一个端口，
所以**两路各跑一次 `perturb`，两次都要通过**。只推一路而读数只加了一半，
说明合并用的是两路的平均；只推一路读数就整量跟上，说明另一路实际没参与这个读数 ——
两种都要在报告里写清楚。

### 选区怎么定

只有两种形状：半空间 `{point, normal}`（取 `dot(p-point, normal) > 0` 的一侧）与盒。
定选区就三步：

1. 跑一次 `run --outputs`，从质量记录里读出这条缝的几何 ——
   基准侧翼面的拟合窗口、锚点 x、两侧穿出点 A / B 的 x。
2. **切分面放在 A 与 B 之间**，并且落在两个拟合窗口之外。
3. **推非基准的那一侧。** 基准侧定义深度刀口，推它会让刀口一起动，读数就不再严格等于位移。
   基准在左就推右侧（`normal` 取 `+x`，位移取正）；基准在右就推左侧（`normal` 取 `-x`，
   位移取正是"收紧"，`--expect` 要写负数）。

然后**必须回到剖面上核对**。两个真实的切错反例：

- **切分面穿过一面近竖直的壁。** 壁上的点 x 几乎相同，刀口落在中间就把同一面壁劈成两半，
  一半跟着动一半不动，轮廓上多出一个 0.3 mm 的台阶，走轮廓的算法会在那里拐弯。
  症状是 `rmse` 大、`slopeNeg` 与 `slopePos` 差很远。
- **切分面压在下游要用的锚点上。** 锚点跟着一起平移，测的两个点同向同量地动，
  差值纹丝不动 —— 读数看上去"完美跟随"或者"完全不响应"，两种都可能，而且都不报错。

还有一条实测得到的硬限制：**固定选区只在「缝的帧间游走量小于缝宽」时成立**。
点 1 的缝 0.7 mm 宽、帧间游走 0.7 mm，最好的一刀切对 50/51 帧；
Audio_1 的缝 0.04~0.15 mm 宽、帧间游走 0.9 mm，任何固定的刀都切不对多数帧。
分辨办法：看 `nonResponsive` 的样本是不是集中在缝位置偏离中位数最远的那些帧 ——
是的话问题在选区，不在读数。
逐样本的 `perturb_sample` 每样本每指标一行，CLI 直接在 stdout 里，
MCP 落在返回值的 `samplesPath`（**全量**，不受 `failuresLimit` 影响），
拿它和 `eval` 取出来的逐帧缝位置（`nodes.<节点>.quality.…midXMm` 这类）对齐看就够了。

### signFold：取绝对值把两个方向折成一个

```jsonc
{"kind":"perturb_sample","sample":"…","metric":"outputs.gap","n":5,
 "slope":-251.0,"rmse":0.21,"slopeNeg":-1000.0,"slopePos":1000.0,"pass":false}
{"kind":"perturb_summary","metric":"outputs.gap","samples":51,"pass":0,
 "slopeMean":-251.0,"slopeStd":252.7,"nonResponsive":3,"signFold":51}
```

`slopeNeg` / `slopePos` 是只用位移 < 0 和 > 0 的点各拟一次的斜率。
**整体 `slope` 可以接近 0，而两侧是 −1 和 +1** —— 这就是 `signFold`：
量的是「距离」而不是「带符号的偏移」，缝张开和收紧被折成同一个方向。
点 7 是真实案例：`gap.flush` 的 `signed=false` 时 51/51 个样本都报 `signFold`，
`signed=true` 时 49/51 通过、`slope` 均值 983。
只扫单侧永远发现不了这一类 —— 要抓它，位移必须跨过 0。

`pass` 的判据是 `|slope − expect| ≤ tolerance` **且** 两侧斜率同号；
`nonResponsive` 是 `|slope| < tolerance/2`；给了 `--expect` 而有样本不通过时退出码是 2。

## 5. 零点与 offset 是 in-sample 的

`gapOffset`、`flushOffset` 这类零点，通常是「把这批样本的均值对到标称 0」算出来的。
这意味着：

- **重复性可信。** `eval` 报的 `std` 与 `p2p` 是干净的数，它们不依赖零点。
- **绝对准确度不可信。** 零点建立在「这批件处于标称」这个未经验证的假设上，
  样本里没有任何信息能证伪它。换一批件、换一次装夹，偏置就可能整体挪走。
- 要修这一条只能靠实物：几个已知尺寸的标准件，或者一把卡尺。这是数据问题，不是工具问题，
  也不是多跑几组参数能解决的。

所以报告里把两件事分开写：「这条缝的重复性是 std 0.05 mm」是结论，
「这条缝的绝对值是 0.15 mm」是**待验证的假设**。

## 6. 最短命令序列

```bash
# 1. 这个算子什么时候不成立
lyflow manifest | jq '.operators[] | select(.id=="gap.notch_width") | .preconditions'

# 2. 图能不能跑，一帧长什么样
lyflow validate g.lyflow.json && lyflow run g.lyflow.json --outputs --set n_load.source=files \
       --set n_load.primaryFile=a.pcd --set n_load.secondaryFile=b.pcd

# 3. 测的是不是那条缝（先做这一步，别先调参）
#    --after 跟着图里的 camera 参数走：camera=primary 就是 n_frame_p:cloud
lyflow perturb g.lyflow.json --after n_frame_s:cloud \
       --region '{"kind":"halfspace","point":[0.01345,0,0],"normal":[1,0,0]}' \
       --axis x=-0.0003:0.0003:5 \
       --samples-dir <采集根> --sample-subdir 4 \
       --bind-pair n_load.primaryFile,n_load.secondaryFile \
       --pattern "*Master*.pcd,*Slave*.pcd" \
       --metric outputs.gap --expect 1000 --tolerance 100

# 4. 确认测对了，再扫参数；只看 train 组（样本集与打 tag 都是命令的一部分，不写脚本）
lyflow eval g.lyflow.json \
       --samples-dir <采集根> --sample-subdir 4 \
       --bind-pair n_load.primaryFile,n_load.secondaryFile \
       --pattern "*Master*.pcd,*Slave*.pcd" --split-half half \
       --param n_notch.lineDistThresh=0.1:0.4:4 \
       --metric outputs.gap --holdout half=b --csv eval.csv

# 5. 选定参数写回图，重跑一遍看 holdout 组掉了多少
lyflow eval g.lyflow.json \
       --samples-dir <采集根> --sample-subdir 4 \
       --bind-pair n_load.primaryFile,n_load.secondaryFile \
       --pattern "*Master*.pcd,*Slave*.pcd" --split-half half \
       --set n_notch.lineDistThresh=0.3 \
       --metric outputs.gap --holdout half=b
```

第 3 步在第 4 步之前，不是排版顺序：**在一个测错了缝的图上调参数，调出来的是更稳的错数。**

## 7. 用 MCP 时对应的工具名

同一件事换个名字而已，语义与上面各节一模一样，这里不重复解释。
怎么起、每个工具的输出形状见 [mcp.md](mcp.md)。

| 上面用的命令 | MCP 工具 | 差别 |
|---|---|---|
| `lyflow manifest` + `jq '.operators[] \| select(…)'` | `list_operators` → `get_operator` | 列表一行一个算子，详情含 `preconditions` |
| `lyflow manifest` 里的 `types` | `list_port_types` | — |
| `lyflow validate <g>` | `validate_graph` | 图可以给路径，也可以内联 |
| `lyflow plan <g>` | `plan_graph` | — |
| `lyflow run <g> --summary --set …` | `run_graph` | `set` 是对象不是字符串；返回**就是** summary（`status` / `outputs` / `decisions`），没有点云 |
| `run` 之后看 `stats.outputs` | `get_node_outputs` | — |
| `lyflow dump <g> n:port out.pcd` 再自己统计 | `summarize_output` | 不落 PCD，直接给包围盒、每通道 min/max/mean 与前几个点 |
| `lyflow eval …` | `eval` | 默认 `compact`：一组一行，只回 `paramSet/params/metric/group/n/ok/failCodes?/mean/std`；逐行 `eval_row` 落盘给 `rowsPath` |
| `lyflow perturb …` | `perturb` | 只回 `perturb_summary` 与不通过的样本；**全部** `perturb_sample` 落盘给 `samplesPath` |
| `lyflow diff a b --json` | `diff_graphs` | 原样 |
| `lyflow patch <g> --remove-node … --rewire … --dry-run` | `patch_graph` | 四个动作是四个数组（`removeNode` / `addNode` / `rewire` / `set`）；**`dryRun` 默认 true**，要真写得显式给 `dryRun: false`，`out` 必须配着它给 |

### `eval` / `perturb` 的选项逐条对照

CLI 上有的，MCP 上要么有同名字段，要么在这里写明不提供 —— 不留第三种。

| CLI 选项 | `eval` 工具 | `perturb` 工具 | 差别 |
|---|---|---|---|
| `<graph>` | `graphPath` | `graphPath` | 只认路径，不收内联图（CLI 也只认路径） |
| `--samples` | `samplesPath` | 同 | — |
| `--samples-glob` / `--bind` | `samplesGlob` / `bind` | 同 | — |
| `--samples-dir` | `samplesDir` | 同 | — |
| `--bind-pair` | `bindPair` | 同 | 仍是 `"<节点>.<A>,<节点>.<B>"` 一个字符串 |
| `--pattern` | `pattern` | 同 | 两个 glob 用逗号隔开，仍是一个字符串 |
| `--sample-subdir` | `sampleSubdir` | 同 | — |
| `--sort-by` | `sortBy` | 同 | 枚举 `name` / `mtime` |
| `--split-half` | `splitHalf` | 同 | — |
| `--samples-jsonl-out` | **MCP 不提供** | **MCP 不提供** | 要核对生成的样本集就跑一次 CLI；MCP 这边 `rowsPath` 里每行都带 `sample` 与 `tags` |
| `--params <file>` | `params` | — | 直接给对象数组，MCP 自己落成临时文件 |
| `--param` | `param` | — | 字符串数组 |
| `--metric` | `metric` | `metric` | 字符串数组 |
| `--holdout` | `holdout` | — | `perturb` 不分组 |
| `--group-by` | `groupBy` | — | 同上 |
| `--csv` | `csv` | `csv` | 路径原样透传，返回里回 `csvPath` |
| `--base-dir` | `baseDir` | `baseDir` | — |
| `--set` | `set` | `set` | 字符串数组，写法与 CLI 完全一样 |
| `--no-cache` | `noCache` | `noCache` | 布尔 |
| `--parallel` | **MCP 不提供** | **MCP 不提供** | 它是传给 core 的节点并行度，不在判断的关键路径上 |
| `--after` | — | `after` | — |
| `--region` | — | `region` | 给对象不给 JSON 字符串；`point` / `min` / `max` 是米 |
| `--axis` | — | `axis` | 位移是米 |
| `--expect` / `--tolerance` | — | `expect` / `tolerance` | — |
| 只在 MCP 这边 | `compact`（默认 `true`）、`failuresLimit`（默认 20） | `failuresLimit`（默认 20） | 都是为了裁上下文；`failuresLimit: 0` 表示一条都不回，只给路径 |

三条只在 MCP 这边成立的规矩：

- **退出码变成返回值里的 `exitCode`。** 用法错（`4`）时额外带一个 `stderr` 全文 ——
  「这张图上可用的标量路径」在那里面。
- **没有写图的工具。** 改完的图自己用文件系统存，再把路径交给 `validate_graph` / `run_graph`。
- **大结果一律落盘。** `eval` 的 `rowsPath`、`perturb` 的 `samplesPath` 与 `rowsPath`
  是本地文件路径，用 `jq` 去读；返回值里只有统计与被截断过的失败清单。
