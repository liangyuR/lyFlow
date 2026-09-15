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

## 3. `eval`：一组样本 × 一组参数 → 一个标量 → 一组统计

样本集是一份 JSON Lines，一行一帧：

```jsonc
{"id":"15-09-2026-07-52-22",
 "set":{"n_load.source":"files",
        "n_load.primaryFile":"…/Master.pcd","n_load.secondaryFile":"…/Slave.pcd"},
 "tags":{"half":"a"}}
```

`set` 的键是 `<节点>.<参数>`，语义与 `--set` 完全一样。
双相机这类「哪两个文件算一帧」的事必须写在文件里，不要靠两个 glob 的排序恰好对齐；
单路径的情况可以用 `--samples-glob <pat> --bind <节点>.<参数>` 一步生成。

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

**单位。** 点云在传感器帧和测量帧里都是**米**（`gap.to_measurement_frame` 只交换 y 与 z，不换单位），
而 `outputs.gap` 这类 Measurement 是**毫米**。所以「张开 1 mm 读数加 1 mm」是 `slope ≈ 1000`，
`--expect` 写 1000 不是 1。位移范围也按米写：`x=0:0.0006:5` 是 0 到 0.6 mm 五档。

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
lyflow perturb g.lyflow.json --after n_frame_s:cloud \
       --region '{"kind":"halfspace","point":[0.01345,0,0],"normal":[1,0,0]}' \
       --axis x=-0.0003:0.0003:5 --samples frames.jsonl \
       --metric outputs.gap --expect 1000 --tolerance 100

# 4. 确认测对了，再扫参数；只看 train 组
lyflow eval g.lyflow.json --samples frames.jsonl --param n_notch.lineDistThresh=0.1:0.4:4 \
       --metric outputs.gap --holdout half=b --csv eval.csv

# 5. 选定参数写回图，重跑一遍看 holdout 组掉了多少
lyflow eval g.lyflow.json --samples frames.jsonl --set n_notch.lineDistThresh=0.3 \
       --metric outputs.gap --holdout half=b
```

第 3 步在第 4 步之前，不是排版顺序：**在一个测错了缝的图上调参数，调出来的是更稳的错数。**
