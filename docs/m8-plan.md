# M8 设计 —— 高层测点算子，让人也能建图

状态：**设计稿，待确认**（§4 的开放问题定下来之后再拆实施计划）。前提：M7 已落地（复刻约束已去掉、
`fit_line.side` 已换成 `toward`、顶层图参数、加载期校验钩子）。

目标：**一个懂业务、不懂算法的人，十分钟内从空白建出一个测点的图，只填二十来个业务参数，不接任何一条内部边，
不填任何一个「左右」。** 需要深调时，再把它一键展开成今天这种细粒度图。

---

## 1. 现状：为什么人建不了图

一个测点的图由导入器生成，模板路径约 30 个节点，模型路径加回退约 45 个。人手建图要面对三类负担，
每一类在 KUN10 都真实出过事：

| 负担 | 例子 | M7 之后还剩多少 |
|---|---|---|
| 拓扑 | 裁剪→拟合→选点→段差，每条线都要接对端口；`definition A` 还要多接一条 baseLine | 全部还在 |
| 方向 | `side`、`baseSide`、`datum_window.side`、中心带参考线接哪条 | `fit_line.side` 没了；其余还在 |
| 同步 | `n_circles.offset` 与 `n_gap.offset`、nominal 两处、modelPath 多处 | 靠顶层参数能解决，但要人自己声明 binds |

这三类负担**都不是业务决定**，都能从少数几个业务事实推出来。导入器（`import_standard_gap.cpp`）今天
就在做这种推导，只是推导的输入是 YAML，而不是图里的一个节点。

## 2. 方案：展开式算子 `gap.station`

### 2.1 核心决定

| # | 决定 | 理由 |
|---|---|---|
| K1 | core 新增**展开式算子**：`OperatorDesc` 加可选 `ExpandFn expand`，签名 = (解析后的参数, 已连接的输入端口) → 一份 `SubgraphDef`（JSON，复用 `parseSubgraphDef`）。它在 `expandGraph` 阶段与子图、库算子走同一条路：展开成 `station/<内部 id>` 路径节点，执行器、缓存键、事件流、结果仓对它一无所知（ADR-0010 的四条后果原样成立） | 展开后是平图，并行、缓存、Edge Peek、`lyflow eval` 全部白拿；诊断落在 `station/n_fit_base` 这种路径上，编辑器已有按前缀聚合 |
| K2 | `gap.station` 是一个展开式算子：输入 `primary`、`secondary` 两片云；输出 `flush`、`gap`（Measurement）、`bundle`（GapResult Record）以及四个角色 ROI（Box2D，给 3D 视图叠画）。**展开逻辑与导入器共用一份代码**：把导入器拆成「YAML → `StationSpec`」和「`StationSpec` → 子图」两半，`gap.station` 走「参数 → `StationSpec` → 子图」 | 推导规则只有一份，导入器和测点算子不会各自漂移 |
| K3 | **ROI 按角色命名**：`datumRoi`（基准件的面）、`targetRoi`（被测件的面）、`seamLeftRoi`、`seamRightRoi`。参数里没有 base/ref、没有任何 `side` | 「基准」一词在四个参数里指四样东西，是评审里的问题 5 |
| K4 | **方向全部推导，不给参数**：① 所有 `fit_line` 的 `toward` 接一个内部节点 `gap.box_between(seamLeft, seamRight)`（两框之间的那段，M8 新增的小算子），与哪一侧无关；② 模板模式下 `business_rois.datumSide` 在展开时由模板坐标系里的 ROI 推出：`datumRoi` 中心 x 小于两个缝框中心的平均 x 即 `left`；③ 中心带参考线用角色枚举 `centerBandRef: datum \| target` | 人能填反的参数，一个都不留 |
| K5 | **同步全部内化**：`gap.offset`、`gap.nominal` 各只出现一次，展开时写进 `fit_gap_circles` 与 `gap.gap`/`gap.judge` 的所有副本；`modelPath` 只在 station 上出现一次，宿主通过 M7 的顶层参数绑定它 | 一处定义，两处生效，不一致就不可能发生 |
| K6 | station 自带 `validate`（M7 钩子），加载期静态检查：四个框非退化；两个缝框左右有序且不重叠；`datumRoi` 与 `targetRoi` 落在缝的两侧；nominal 落在上下限之间；半径上下限有序、固定半径在区间内；模型模式缺 `modelPath` | ROI 在模板坐标系里是常数，所以「side 填反」这一整类错误在加载期就能拦住，这正是 M7 之前做不到的 |
| K7 | **展开为细图**：`lyflow patch --explode <节点>` 把展开结果原样写回图里（节点 id 去掉路径前缀、边接好、station 节点删掉），编辑器右键「展开为细图」调同一条命令。**单向**，不支持收回 | 深调时需要直接改内部参数、插节点；做成双向要维护「细图能否还原成参数」的判定，不值 |
| K8 | 导入器默认产出**一张只含 `load_profile_pair → gap.station` 的图**；`--expanded` 产出今天的细图。黑盒对照节点 `gap.measure_reference` 不再默认生成 | 人打开导入的图，看到的就应该是他能改的那一层 |
| K9 | 编辑器：Inspector 按 §2.2 的分组显示，`高级` 组默认折叠；3D 视图的 2D 剖面模式里**直接拖拽四个角色框**（四种颜色、带角色名），拖完写回 station 的 ROI 参数；「新建测点图」模板一键生成 `load_profile_pair → gap.station → 图输出` | 手填毫米坐标是人建图最慢、最容易错的一步 |
| K10 | **v1 覆盖面 = 导入器今天支持的全部**（2026-09-23 确认：与现有算法一致即够用）：圆缝 + 线段差、三种参考点、模板或模型定位与模型→模板回退、备用相机分支、方向基准、中心带、弱地板。多模板候选用**固定四个槽位**表达（`template1`…`template4`，每槽 `enabled` + 左右模板文件 + 可选的四个角色 ROI 覆盖），上限与 `gap.select_alignment` 的四个候选一致，不新增列表参数类型；槽位放进 `高级` 组。**不做**：notch / groove 两种缝型 | 导入器能生成的图，station 都必须能表达，否则人建的图和导入的图就是两套能力 |

### 2.2 人要填的参数

基础组约 20 个，都是业务配置里本来就有的量：

| 组 | 参数 |
|---|---|
| 定位 | `locate: template \| model`；`templateDir`、`icpMinScore`（模板模式）；`modelPath`（模型模式） |
| 角色 ROI（mm，模板坐标系） | `datumRoi`、`targetRoi`、`seamLeftRoi`、`seamRightRoi`（Vec4f，可在 3D 视图拖拽） |
| 段差 | `flushRef: line_end \| selected_point \| nearest_point`；`lineFitDistance`；`segmentPoints`；`flushOffset`；`flushNominal` / `flushUpper` / `flushLower` |
| 间隙 | `gapDefinition: A \| B`；`circleFitDistance`；左右半径上下限；`gapOffset`；`gapNominal` / `gapUpper` / `gapLower` |
| 高级（折叠） | 固定半径、逐侧相机、中心带（`centerBandRef` + 左右 above/tol/mode）、弱地板（minInliers / minArcDeg）、方向基准（长面）、离群滤波 |

### 2.3 展开时推导出的内部值（人不再填）

| 内部参数或连线 | 推导自 |
|---|---|
| 各 `fit_line.toward` | `box_between(seamLeft, seamRight)` |
| `business_rois.datumSide` | 模板坐标系里 `datumRoi` 与缝框的相对位置 |
| `fit_gap_circles.offset` / `nominal` | `gapOffset` / `gapNominal` |
| `fit_line.endpoints` | `locate`（模型模式 `inlier_ends`，模板模式 `roi_intersection`） |
| 中心带的 `refLine` / `refLineRight` 连线 | `centerBandRef` |
| `gap.gap` 是否接 `baseLine` | `gapDefinition == A` |
| `gap.flush.signed` | 恒为 true |
| 各裁剪节点的 `bounds` | 恒为 `open` |

## 3. 实施拆分（确认后再写成实施计划）

1. **core**：`ExpandFn` 与展开阶段接入；`lyflow plan` / `params` 对展开式算子的显示（station 的参数 source 照常，内部参数 source = `derived`）；`patch --explode`。
2. **gap 包**：`gap.box_between`；导入器拆成 `StationSpec` 两半；`gap.station` 的展开与 `validate`；导入器 `--expanded`。
3. **编辑器**：Inspector 分组与折叠；2D 剖面视图里拖拽角色框；「新建测点图」模板；右键「展开为细图」。
4. **验收的核心断言**：对同一份 StandardGap 夹具，`导入 → station 图` 与 `导入 --expanded → 细图` 在全部样本上**逐帧输出相同**（这是 station 与导入器同源的证据，不是在对齐旧算法）；故意把 `datumRoi` 拖到缝的另一侧，`lyflow validate` 在加载期报错；从空白按「新建测点图」建图、只改基础组参数，e2e 跑通出数。

## 4. 开放问题（需要你来定）

1. ~~v1 覆盖面~~ —— 已定（2026-09-23）：与现有算法一致即可，见 K10。
2. ~~2D 拖框放在 M8 还是往后放~~ —— 已定（2026-09-23）：放在 M8，见 K9。
3. **展开为细图是否一定单向**（待定，先把含义讲清楚再改 K7）：如果现场常见的流程是「先用 station 建、深调后又想回到简单视图」，就要考虑可收回，成本会明显上升。
