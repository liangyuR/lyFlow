# M8 计划 —— 让人也能建图：自带常识的积木

目标：**人在空白画布上，用约九个叫得出名字的节点拼出一个测点，不接任何「同一个框接两次」的线，不填任何「左右」。**
图始终是平的，每个节点都看得见、改得动；复杂度被消化进数据类型和算子本身，而不是藏进一个大节点。

前提：M7 已落地（复刻约束去掉、`fit_line.toward`、Line2D 统一朝向、顶层图参数、加载期校验钩子）。

**已否决的方案**：`gap.station` 展开式大节点（上一版设计稿）。它把复杂度藏进黑盒，简单但封闭；用户要的是灵活、简洁、自由的 Flow。

分两个实施包：**M8a** core 的 Bundle 类型 + gap 积木算子 + 导入器；**M8b** 编辑器（自动连线、片段库、2D 拖框、实时校验）。M8a 先做，M8b 对着 M8a 冻结的接口做。

---

## 1. 现状：人为什么拼不动

模板路径的一个测点约 25 个节点。难的不是数量，是三件事：

| 负担 | 例子 |
|---|---|
| 一步拆成几个节点 | 「拟合基准线」= `crop_box2d` + `fit_line`，同一个框接两次；三片云逐个接给 `fit_gap_circles` |
| 连线里藏着方向 | 哪个框是缝、线朝哪边，全靠人接对端口 |
| 边上是裸数据 | 一个 Box2D 不知道自己是「基准面」还是「缝左」，下游只能靠参数猜 |

## 2. 定死的决定

### M8a：core 与 gap 包

| # | 决定 | 理由 |
|---|---|---|
| L1 | core 新增通用数据类型 **Bundle**：`{ kind, fields: 有序的 名字 → Data }`，字段可以是任何已有类型（不嵌套 Bundle）。端口类型写作 `Bundle<kind>`，类型检查要求 kind 相等；`Any` 照常兼容 | 一根线带一组有关系的数据；kind 让类型检查和自动连线都能精确匹配 |
| L2 | Bundle 的 kind 由算子包在 manifest 里**声明**：`bundles: [{ kind, label, fields: [{ name, type, doc }] }]`。运行时 `outputs.set` 一个 Bundle 时按声明校验字段齐全、类型对得上，不符报 `contract_violation` | 字段表在 manifest 里，编辑器、MCP、校验都读同一份 |
| L3 | 结果仓、C ABI、事件、summary 按字段可寻址：端口名写成 `<port>.<field>`（如 `scan.merged`），`lyflow_output_cloud` 等现有取数函数不改签名，直接认这种写法；`lyflow_output_list` 对 Bundle 端口列出每个字段。图输出（`outputs`）也可以指向字段 | 宿主、Edge Peek、eval 的 metric 路径都不必理解新结构 |
| L4 | gap 包声明两个 kind：**`gap.ScanPair`** = `primary`、`secondary`、`merged`（PointCloud）；**`gap.RoiSet`** = `datum`、`target`、`seamLeft`、`seamRight`（Box2D）+ `info`（Record：`datumSide`、`source: template\|model`、对齐结果）。RoiSet 里的四个框**按角色命名**，不再有 base/ref | 「基准」一词在四个参数里指四样东西（评审问题 5）；角色随数据走，下游不用猜 |
| L5 | 新增六个**积木算子**，每个对应一个人会说出口的步骤（§3 列参数）：`gap.read_scan`（读剖面）、`gap.locate_template`（模板定位）、`gap.locate_model`（模型定位）、`gap.role_line`（按角色拟合直线）、`gap.ref_point`（取参考点）、`gap.seam_circles`（拟合缝两侧圆）。`gap.flush`、`gap.gap`、`gap.judge` 不变；`gap.result_bundle` 改接 `RoiSet` 与 `ScanPair`，原来的七个散端口去掉 | 一个测点从约 25 个节点降到约 9 个 |
| L6 | 积木算子**复用现有实现，不写第二份算法**：把 `fit_line`、`fit_gap_circles`、`business_rois`、`align_template`、`select_alignment` 等的 compute 主体抽成包内函数，细粒度算子与积木算子都调它。**细粒度算子全部保留**，想精细控制时照样可用，两种可以在同一张图里混用 | 同源才能保证两种建图方式结果一致，也保住现有的调参手段 |
| L7 | **方向全部由 RoiSet 推出**：`role_line`、`ref_point` 的「靠缝那一端」取 `seamLeft` 与 `seamRight` 两框之间的中点；`locate_template` 的 `datumSide` 由模板坐标系里 `datum` 框中心与两个缝框中心的相对位置推出，写进 `info`。积木算子上**没有任何 side / toward / baseSide 参数** | 人能填反的参数一个都不留 |
| L8 | `locate_template` 合并「整体框 → 裁剪 → 加载模板 → 对齐 → 选模板 → 业务框」；多模板候选用**固定四个槽位**（`template1`…`template4`，每槽 `enabled` + 左右模板文件 + 可选的四框覆盖），上限与 `select_alignment` 一致。四个角色框是它的参数（模板坐标系，mm） | 2026-09-23 确认：「读剖面」「定位」各合成一个节点的粒度合适 |
| L9 | `locate_model` 输出 `rois: RoiSet` 与 `scan: ScanPair`（模型路径会剔 NaN、做跟随零件的裁剪，下游应当用处理过的云）。**模型→模板回退、备用相机**不做成算子参数，而是用现有 `flow.fallback` 在平图上并联两个节点表达，片段库提供现成组合 | 回退是结构，放在图上看得见；不在算子里埋分支 |
| L10 | 方向基准（长面）做成独立积木 `gap.datum_direction`（`scan`、`rois` → `Line2D`），接到 `role_line.refLine`。中心带参考线仍是一条普通的边 | 可选步骤保持可选，不在别的算子里加开关 |
| L11 | 积木算子都实现 M7 的 `validate`：`locate_template` 静态检查四框非退化、缝框左右有序不重叠、`datum` 与 `target` 落在缝的两侧；`role_line` 等检查必要输入已接 | ROI 在模板坐标系里是常数，「方向填反」在加载期就拦得住 |
| L12 | 导入器默认产出**积木图**；`--fine` 产出今天的细粒度图。两种都不再默认生成黑盒对照 `gap.measure_reference` | 人打开导入的图，看到的就是他自己也拼得出来的东西 |

### M8b：编辑器

| # | 决定 | 理由 |
|---|---|---|
| L13 | **按类型自动连线**：拖入节点时，每个未连接的必需输入若在图里**恰好有一个**类型兼容的输出（含 Bundle kind），就自动连上，写成普通的边；有多个候选时不连，高亮候选端口。从输出拖线时只高亮兼容的输入。自动连线是编辑动作，不是运行时的隐式上下文，执行语义不变 | 省掉最机械的那部分连线，同时不引入全局 context |
| L14 | **片段库**：`*.lyflow-snippet.json` = 一组节点 + 边 + 对外端口提示；插入就是带自动连线的粘贴，插完是普通节点，**没有展开/收回**。gap 包随附：「段差 · 线端点」「段差 · 选点」「间隙 · 圆」「模型定位 + 模板回退」「备用相机回退」「测点骨架」 | 常用组合一键拼好，又不变成黑盒 |
| L15 | **2D 拖框**：选中 `locate_template` 时，2D 剖面视图显示槽 1 的模板云与四个角色框（四种颜色、标角色名），拖动与拉伸直接写回参数；同时在样本云上叠画变换后的框（只读）。普通 `Vec4f` 的 ROI 参数（带 `roi` 语义标记的）同样可拖 | 手填毫米坐标是人建图最慢、最容易错的一步 |
| L16 | **实时校验**：编辑时调用 validate，诊断直接标在节点与参数上（M7 钩子的 error / warning） | 错误在拼的时候就看见，而不是跑完才发现 |
| L17 | Edge Peek 支持 Bundle：浮窗先列字段，点进字段按字段类型复用现有视图（点云 / 框 / 下标） | 数据成组流动之后仍然看得见每一样 |
| L18 | `gap.read_scan` 加两个可选输入 `primary`、`secondary`：接上（或被宿主注入）时直接用它们，不读目录。宿主注入落在 `read_scan` 本身，不必在前面再接一个 `load_profile_pair`（M8a 验收取舍 8 暴露） | 宿主接入也只看见一个「读剖面」节点 |

### 不做

`gap.station` 或任何展开式大节点；运行时全局 context；notch / groove 的积木版（细粒度算子照常可用）；Bundle 嵌套 Bundle；片段的「收回」。

## 3. 积木算子一览

| 算子 | 输入 → 输出 | 人要填的参数（其余在「高级」组折叠） |
|---|---|---|
| `gap.read_scan` 读剖面 | （目录参数，或宿主注入两片云）→ `scan: ScanPair` | 目录与文件前缀；离群滤波开关与半径 |
| `gap.locate_template` 模板定位 | `scan` → `rois: RoiSet`、`alignment: Record` | 模板目录；四个角色框；ICP 最低分；高级：整体框模式、四个模板槽、ICP 细节 |
| `gap.locate_model` 模型定位 | `scan` → `rois: RoiSet`、`scan: ScanPair` | `modelPath`（通常绑到顶层参数）；高级：细化、跟随裁剪 |
| `gap.role_line` 按角色拟合直线 | `scan`、`rois`、`refLine?` → `line`、`innerEnd`、`quality` | `role: datum \| target`；拟合距离；截取点数；高级：取哪片云、方向约束、最少内点 |
| `gap.ref_point` 取参考点 | `scan`、`rois`、`baseLine?` → `point`、`line?`、`quality` | `method: line_end \| selected_point \| nearest_point`；`role`（默认 target） |
| `gap.seam_circles` 拟合缝两侧圆 | `scan`、`rois`、`refLine?`、`refLineRight?` → `left`、`right`、`quality` | 拟合距离；左右半径上下限；`nominal`；高级：固定半径、逐侧相机、中心带、弱地板 |
| `gap.datum_direction` 方向基准 | `scan`、`rois` → `line` | 窗口起点、长度、高度；拟合距离 |

一个模板路径的测点：`read_scan → locate_template → role_line(datum) → ref_point → flush → judge` 与 `seam_circles → gap → judge`，再加 `result_bundle`，共 10 个节点（两个判定各一个）。

## 4. 验收

**M8a**（子代理写进 `docs/m8a-acceptance.md`）

1. 对仓库内的 StandardGap 夹具与 M7 用过的 39 个样本，分别导入成**积木图**与 `--fine` **细粒度图**，所有样本上 flush、gap 的输出**逐帧相同**（两者同源的证据，见 L6）。
2. 导入的模板路径积木图节点数 ≤ 12；积木算子的参数里没有 `side`、`toward`、`baseSide`、`datumSide`。
3. Bundle：字段缺失或类型不符报 `contract_violation`；`lyflow_output_cloud(run, node, "scan.merged")` 取得到点云；图输出可以指向字段；`Bundle<gap.ScanPair>` 接 `Bundle<gap.RoiSet>` 报 `type_mismatch`。
4. `locate_template` 把 `datum` 框拖到与 `target` 同一侧，`lyflow validate` 在执行前报错。
5. 细粒度算子的既有测试全部照过；同一张图里混用积木与细粒度算子能跑。
6. `LYFLOW_PACKS="gap;dts"` 下 `pnpm check`、`pnpm e2e` 全绿，日志落盘 grep 无「跳过 / 未验」。

**M8b**

7. e2e：从空白画布，只靠拖入节点（自动连线）、插入「测点骨架」片段、在 2D 视图拖四个框、改基础组参数，建出模板路径测点并跑出 flush 与 gap 数值，**全程不手连一条边**。
8. 自动连线：唯一候选自动连上；两个候选时不连、候选端口高亮。
9. Edge Peek 在 `ScanPair` 边上列出三个字段，点进 `merged` 显示点云。
10. 编辑时把 `datum` 框拖到错误一侧，节点立即标红并显示诊断。

---

## 5. M8c —— 多模板的框分开处理（2026-09-24 试用反馈）

反馈：ROI 框能用，但模板多时所有框混在一起，难以处理。原因：`locate_template` 照搬了 YAML 的「公共四框 + 每槽可选覆盖」结构，槽 1 开了覆盖时 2D 视图同时画出公共四框与槽 1 的四个覆盖框，一共八个，彼此重叠。

| # | 决定 | 理由 |
|---|---|---|
| L19 | **每个模板槽各有自己的四个角色框**，删掉公共四框（`datumRoi` 等）与 `templateNOverride` 开关。槽 1 恒启用；槽 2–4 由 `templateNEnabled` 控制，启用时它的四框必填。导入器：候选模板自带 rois 的用自己的，没有的用配置里的全局 rois 填进该槽 | 一个模板对应一组框，没有「公共值被谁覆盖」这层间接 |
| L20 | 2D 视图加**模板切换条**（`模板 1 · f1`、`模板 2 · f2` …，只列启用的槽）：一次只显示所选槽的模板云与它的四个框，其余槽的框不画。Inspector 里所选槽的参数组同步展开。切换条上提供「把当前四框复制到其它槽」 | 一次只处理一个模板的四个框 |
| L21 | 框的角色标签**互不遮挡**：相邻框的标签在垂直方向错开，或放在框内侧 | 截图里 Seam Right 的标签盖住了 target 的标签 |
| L22 | `validate` 对每个启用的槽分别检查（非退化、缝框有序、datum 与 target 分居缝两侧），诊断里写明是哪个槽 | 错误能定位到具体模板 |

验收：

11. 导入 39 个样本的积木图，flush 与 gap 与 M8a/M8b 的读数逐位相同（L19 只是参数结构变化，不改算法）；导入的图里没有 `datumRoi` 等公共框参数，也没有 `Override` 参数。
12. e2e：打开一张三模板的图，2D 视图切到每个槽时恰好显示 4 个框；拖动槽 2 的框只改槽 2 的参数；「复制到其它槽」后，其余启用槽的四框与当前槽相同。
13. 两个框相邻时，e2e 断言它们的标签包围盒不相交。
14. 把槽 3 的 datum 拖到 target 同侧，诊断标明「模板 3」。
