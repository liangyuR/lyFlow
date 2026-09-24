# 参数配置页与参数配方 —— 调研与方案讨论稿

状态：**讨论稿**（2026-09-24）。先定方向，再写实施计划（`param-recipe-plan.md`）。

目标：一个完整的参数配置页，集中展示并编辑一张 flow 里所有节点的所有参数类型；同一张 flow 可以切换「参数配方」（不同产品/车型/工位用不同的一组参数值）。

---

## 1. LyFlow 现状

### 已有的基础

- **参数类型 14 种**（`schema/operator-manifest.schema.json:167`、`core/include/lyflow/manifest.h:147`）：bool、int、float、vec2f、vec3f、vec4f、enum、flags、string、text、path、color、transform、curve。
  约束与语义：`min/max`（硬限位）、`softMin/softMax`（滑条范围）、`step`、`unit`、`options`、`group`、`advanced`、`visibleWhen/enabledWhen`、`semantic:"roi"` + `roiBackdrop`。
- **实际用量**：71 个算子，约 383 个参数。float 157、enum 60、int 49、bool 40、string 27、vec4f 22、path 16、vec3f 12。
  flags、color、transform、curve、text、vec2f 在现有包里**一个都没用到**；transform 和 curve 的控件目前显示「尚未实现」。
  gap 包占 286 个参数；`gap.locate_template` 一个算子就有 48 个参数，含 4 个模板槽，每槽 4 个 ROI 框。
- **图参数（顶层 `params`）**：`{type?, default, binds:["节点.参数"], doc?}`，一处定义，按名字从宿主、CLI（`--param`）、C ABI（`params_json`）传值，cacheKey 自动正确。
  子图提升参数用的是「完整参数声明 + binds」的格式（`graph-doc.schema.json:418`）。
- **可以直接复用**：控件表、分组、条件显示、稀疏存储、重置、实时校验（诊断挂在 `paramPath` 上）、实时预览、拖框写回、撤销。
- **结果仓按内容寻址**：同一组参数值的结果会被缓存，所以配方 A → B → A 切回来是瞬时的。这是 LyFlow 相对多数工业视觉软件的一个优势。

### 缺口

1. **编辑器不解释图参数**：没有界面、没有 store 动作；Inspector 显示的值没有合并绑定值；改一个被绑定的参数会写成显式值，然后报 `param_conflict`。
2. **图参数声明太薄**：只有 type、default、doc，没有 label、min/max、options、group、unit，画不出完整控件。
3. **没有「具名参数集」的数据结构和存放位置**：GraphDoc 顶层是 `additionalProperties:false`。eval 的 paramsets 是匿名外部文件，而且不能覆盖图参数。
4. **编辑器运行不能传图参数取值**：`RunOptions` 没有 `params`，只能改 default（会进撤销栈、置 dirty）。
5. **MCP 的 run_graph、get_params、patch 没有图参数入口**。
6. **零碎问题**：advanced 没有真正折叠；图参数的诊断没有落点；撤销回到保存点后 dirty 不复原；`ne` 条件在三方之间不一致；「复制路径名」没带 nodeId。

---

## 2. 业界做法（摘要）

调研覆盖了工业视觉、工控配方标准、节点式/DCC 工具和配置管理四类（出处见 §6）。

| 模式 | 代表 | 优点 | 缺点 |
|---|---|---|---|
| **全量快照** | Keyence 程序编号、In-Sight Recipe Manager、Aurora Vision HMI 状态文件 | 简单；每份独立，切换语义清楚 | 改了基础传不到各份；份与份之间慢慢漂移；工程结构一变就失效 |
| **暴露参数 + 配方表**（结构与值分离） | Cognex Designer、MERLIC、WinCC 参数集、RecipePlus、KNIME Configuration 节点 | 结构统一定义，校验和限位集中；能做矩阵对比；对产线友好 | 要事先决定暴露哪些参数；结构变更会造成失配 |
| **分层覆盖** | Houdini Takes、Nuke LiveGroups、Helm/Kustomize、ISA-88（概念上） | 改基础能传播到所有配方；差异显式，便于评审 | 继承链一长就难诊断；必须有清楚的覆盖标记 |

**可以直接借鉴的做法**

- **结构与值分离**：参数规格（类型、限位、单位、枚举、必填、显示名）只定义一次，配方只存值。来自 WinCC 的参数集类型、RecipePlus 的配料。
- **滑条范围和硬限位分开**：LyFlow 已有 soft/hard 的区分。来自 TouchDesigner。
- **只存显式纳入的参数，不自动纳入**：编辑时一改就写进当前配方，是覆盖失控的首要原因。来自 Houdini Takes。
- **覆盖要一眼可见**：被覆盖的参数有色标，每条可以「恢复为基础值」或「写回基础」。来自 Nuke LiveGroups。
- **「纳入配方」和「给操作员看」是两个独立开关**。来自 MERLIC、Grasshopper Remote Panel。
- **加载时做四类失配检查**：ID 失配、类型不符、缺失、多余，给出明确报告，而不是只记日志。来自 Cognex Designer 的排障页、Substance 预设。
- **校验合并后的有效值**：校验的是「基础 + 覆盖」的结果，越界或缺失就阻止运行。来自 Helm values schema。
- **矩阵视图**：行是参数，列是配方，可以「只显示差异」，可以勾选单元格复制到其它配方。来自 RecipePlus 的 Compare 列、LaunchDarkly。
- **切换总是从基础重新计算**：不叠加在当前值上，否则结果取决于切换顺序。这是 MERLIC 的反例：它只应用新配方里定义了的参数，没定义的留着上一个配方的值。
- **当前配方和临时改动要常驻显示**：未保存的临时改动显示为脏状态，重载前提示。这是 MERLIC 的教训：操作员在前端改的值，下次加载配方时会丢失。
- **版本用只读快照加工作副本，审批签核可选**。来自 FactoryTalk Batch。
- **外部切换接口**：按名称或编号切换，返回成功或失败，可以设开机默认配方。来自 Keyence PW 命令 + ACK/NACK、MERLIC 的默认配方。

---

## 3. 建议方案（待讨论）

### 3.1 数据模型：推荐「图参数就是配方的结构，配方是图参数的稀疏覆盖」

- **配方的结构 = 图参数**。把图参数声明扩成完整参数规格，和子图提升参数同一种格式：label、类型、限位、单位、options、group、doc，加 binds。
- **基础值 = 图参数的 default**。
- **配方 = `{ name, values: { 图参数名: 值 } }`**，只存与基础不同的值，**只有一层**（配方不再继承配方）。
- **任何节点参数都能一键「纳入配方」**：这一步就是把它提升为图参数，并把当前值作为 default。
  这样不引入第二套覆盖机制，限位校验、cacheKey、CLI、C ABI、宿主传值全部沿用现有的一套。
- **运行与切换**：运行时把「default + 当前配方覆盖」合成 `params`，经 `RunOptions.params` 传给 core（C ABI 已经支持 `params_json`）。切换配方不改 doc、不进撤销栈。
  core 只是按名字接收值，不知道配方的存在；结果缓存按内容寻址，切回原来的配方就是命中缓存。

**备选：Takes 式的任意 `节点.参数` 覆盖**
- 优点：最灵活，不需要先提升。
- 缺点：和 binds 冲突（同一个参数既被绑定又被覆盖）；要在编辑器、core、CLI 各实现一遍覆盖层；校验要额外做一遍「有效值」。
- 结论：不推荐。

### 3.2 配方存放：两种方案，需要选

| 方案 | 做法 | 适合 |
|---|---|---|
| **A. 存在图文件里** | GraphDoc 新增顶层 `recipes`（要改 schema） | 配方和图一起版本管理；个人或小团队 |
| **B. 独立配方文件** | 每个配方一个 `*.lyflow-recipe.json`，引用图的 id 和图参数规格的摘要 | 产线上工程和配方分开管理；操作员改配方不碰工程；按产品增删文件（MERLIC 的 `.mrcp` 就是这种） |

我的倾向是 **B**：这更符合工业现场的实际，而且四类失配检查天然有了用武之地。第一期也可以先做 A，把 B 留作导出格式。

### 3.3 页面形态

新增一个与画布平级的**「参数」视图**，工具栏切换，快捷键待定。三块内容：

1. **按节点**：全图所有节点的所有参数，按「子图 → 节点 → group」组织，每种参数类型都有控件。
   支持搜索（参数名、label、节点名、值）和过滤（只看改过的 / 只看已纳入配方的 / 只看有诊断的 / 只看某种类型）。
   每行有：「纳入配方」开关、覆盖色标、恢复基础 / 写回基础、复制 `节点.参数` 路径。
   点节点名跳回画布并选中该节点。ROI 类参数旁有缩略图，点开是 2D 拖框视图。
2. **配方矩阵**：行是图参数（按 group 分组），列是基础值和各配方。
   单元格可以直接编辑；可以「只显示差异」；可以勾选单元格复制到其它配方；越界的值标红。
3. **配方管理**：新建、复制、重命名、删除、设为默认；导入导出；失配报告。
   失配报告列出四类：绑定的节点或参数已不存在、类型变了、配方里缺值、配方里多出已删除的参数。

**全局常驻**：工具栏上的当前配方下拉框，加上「有未保存改动」的脏标记。画布上被配方覆盖的参数，在 Inspector 里同样显示覆盖色标。

### 3.4 参数类型

- **补齐 transform、curve 的控件**，让 14 种类型都能编辑。
- 用上 flags、color、text、vec2f 这几种现成控件，做一个**覆盖全部类型的示例图**，作为验收素材。
- **复杂值**（ROI 框、模板槽的四个框）进入配方时是结构化的值，不是文件引用；配方矩阵里显示摘要，点开编辑。

---

## 4. 需要讨论的问题

1. **数据模型**：接受「配方只覆盖图参数，任何参数一键纳入 = 提升为图参数」吗？还是要 Takes 式的任意参数覆盖？
2. **存放位置**：存在图文件里（A），还是独立配方文件（B），还是第一期 A、导出 B？
3. **使用者**：只给工程师用，还是要有操作员视图（只显示部分参数、锁定、权限）？是否需要版本和审批？
4. **页面形态**：与画布平级的独立全屏视图，还是画布旁一个可展开的大面板？
5. **范围**：transform、curve 控件是否在这一期做？宿主和产线按名字切换配方的接口（C ABI、HTTP、MCP、CLI 的 `--recipe`）是否在第一期？
6. **子图**：子图内部的提升参数是否也出现在参数页、能否纳入配方？（建议：出现，可以就地编辑；纳入配方时再提升到顶层。）

## 5. 初步分期（等上面定了再细化）

- **P1 图参数成形**：扩展图参数声明；编辑器能解释、编辑、绑定图参数，消除 `param_conflict` 陷阱；`RunOptions.params` 贯通到 core。
- **P2 参数页（按节点）**：全图参数视图、搜索过滤、全部类型控件（含 transform、curve）、「纳入配方」。
- **P3 配方**：数据结构与存放、切换、矩阵视图、差异与复制、合并后有效值的校验、失配报告。
- **P4 宿主与工具**：按名字切换配方的接口（C ABI、HTTP、CLI、MCP），开机默认配方。如果选了操作员视图，也在这一期。

## 6. 出处

Cognex Designer 配方与排障：docs.cognex.com/designer_450/.../recipes.htm，…/designer_442/.../recipes-troubleshooting.htm
Cognex In-Sight Recipe Manager：docs.cognex.com/is2d_2310/.../spreadsheet-recipe-configmanager.htm
Keyence CV-X 程序切换：manualsdir.com/manuals/658078/keyence-cv-x-series.html?page=19
MVTec MERLIC 配方：mvtec.com/doc/merlic/5.5/manual/en-us/Content/Process_integration/Recipes/merlic_recipes.html
Aurora Vision HMI 状态：docs.adaptive-vision.com/current/studio/hmi/HMIStateControl.html
Siemens WinCC Unified 参数集：docs.tia.siemens.cloud/r/en-us/v20/configuring-parameter-sets-rt-unified/
Rockwell RecipePlus：rockwellautomation.com/en-mde/docs/factorytalk-view/16-00-00/me-help-ditamap/about-recipeplus/about-recipes.html
Rockwell FactoryTalk Batch 参数上提与电子签名：rockwellautomation.com/en-us/docs/factorytalk-batch/17-00/…
Houdini Takes 与 Presets：sidefx.com/docs/houdini/basics/takes.html，…/network/recipes.html
Nuke LiveGroups 覆盖：learn.foundry.com/nuke/content/comp_environment/organizing_scripts/livegroup_overrides.html
TouchDesigner 自定义参数：derivative.ca/UserGuide/Custom_Parameters
Substance Designer 参数预设：experienceleague.adobe.com/en/docs/substance-3d-designer/using/substance-graphs/manage-parameters/parameter-presets
KNIME 组件：docs.knime.com/ap/latest/analytics_platform_components_guide/
Helm values 与 schema：helm.sh/docs/helm/helm_install/
LaunchDarkly 对比与复制：launchdarkly.com/docs/home/flags/compare-copy
海康 VisionMaster、SMT/AOI：只有二手资料，未查证官方模型。

---

## 7. 讨论结论（2026-09-24）

| # | 问题 | 结论 |
|---|---|---|
| D1 | 数据模型 | **接受** §3.1：配方的结构就是图参数；配方 = 图参数的稀疏覆盖，只有一层；任何参数都可以一键「纳入配方」，背后就是提升为图参数 |
| D2 | 存放 | **独立配方文件**：每个配方一个 `*.lyflow-recipe.json`，默认放在图文件同目录的 `<图文件名>.recipes/` 下，也可以打开或导入任意位置的配方文件。文件里记录图的 id 与图参数规格的摘要，加载时做四类失配检查 |
| D3 | 使用者 | **只给工程师用**：不分级，不做权限、操作员视图、审批 |
| D4 | 页面形态 | **与画布平级的大面板**：从画布右侧展开，与画布并排、同时可见，可以拖宽或最大化。打开时替代 Inspector（Inspector 的功能是它的子集），画布上选中节点时，面板定位到该节点 |
| D5 | 范围（我来定） | **做**：transform、curve 控件（14 种类型全部能编辑）；配方文件格式与 schema；CLI `--recipe <文件>`（与 `--param` 叠加，`--param` 优先）；MCP `run_graph` 的 `recipe` 参数。宿主按名字切换：读配方文件，拼成 `params_json` 传给 core，core 不需要知道配方的存在（写进 embedding.md）。**不做**：版本与审批、配方继承、操作员视图、eval 按配方批量评估（后续可以加） |
| D6 | 子图 | **统一做进来**。子图定义是多个实例共享的，所以参数页里每一行标出作用域：「此节点」或「子图定义 · N 个实例共享」。子图内部参数纳入配方时，自动做逐层提升链：内部参数 → 子图参数 → 这个实例上绑成图参数。整条链是一个动作、一次撤销；同一子图的其它实例沿用当前值作默认值，行为不变。库算子（`lib.`）内部只读，只显示，不能编辑或纳入配方 |

下一步：参数面板的设计稿（按节点、配方矩阵、配方管理与失配报告），讨论定稿后写 `param-recipe-plan.md`。
