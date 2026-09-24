# 参数配方 P1（图参数成形）验收记录

对应 [param-recipe-plan.md](param-recipe-plan.md) 的 P1（P1.1–P1.6，验收 1–8）。2026-09-24，Windows 11，WebView2，
`LYFLOW_PACKS=gap;dts`。

- `pnpm check`：退出码 0，「全链路绿」。doctest **295/295**（新增 6 个用例：`test_graph_params.cpp` 5 个图参数规格、
  `test_cache.cpp` 1 个 `ne`），`cargo test`（bridge）**143/143**（新增 3 条），graph-doc 样例 2 份 + 负例 1 份，
  `@lyflow/editor` 单测 **29/29**（新文件 `graph-params-actions.test.mjs` 13 条，`graph-params.test.mjs` 多 1 条），
  两个前端构建，MCP 45/45，`lyflow-client` 单独构建。
- `pnpm e2e`：退出码 0，**787/787 项通过**，其中新分组 `scripts/e2e/params_p1.mjs` **82 项**（验收 1–7，四组）。完整输出落盘后
  grep「未验」「跳过」「FAIL」「✗」均为 0 行；gap 的张量组、M8b / M8c 的分组都真的跑了。M8b 验收 7 这次没有卡住。
- `pnpm e2e:http`：`LYFLOW_E2E_HEADLESS=1` 下退出码 0，**51/51**，其中新增「图参数取值经信封 params → CLI --param」一组 5 项
  （run 带 params、取点云时重跑 CLI 也带着同一组值、validate 带 params 报图参数的 `bad_param`）。

| # | 验收项 | 结果 |
|---|---|---|
| 1 | schema：完整规格通过、老格式通过、负例被拒；Rust 与编辑器往返不丢新字段 | ✅ 通过 |
| 2 | core：default 越过图参数硬限位报 `bad_param`（paramPath = 名字、nodeId 空）；params_json 越界同样；无 type 老参数照常 | ✅ 通过 |
| 3 | 普通参数「纳入配方」：规格从 manifest 复制、default = 当前值、显式值删除、cacheKey 不变、一次 Ctrl+Z 还原 | ✅ 通过 |
| 4 | 子图内部参数：两级提升、另一个实例不变、一次撤销还原两级；库算子内部无此动作 | ✅ 通过 |
| 5 | 被绑定行上编辑改 default，无 `param_conflict` | ✅ 通过 |
| 6 | `RunOptions.params` 传别的值：cacheKey 与结果跟着变；传回 default 命中缓存 | ✅ 通过 |
| 7 | P1.6：复制路径名带 nodeId、`ne` 三方一致、撤销回到保存点 dirty 复原 | ✅ 通过 |
| 8 | 不回归：`pnpm check`、带包的 `pnpm e2e`、headless `pnpm e2e:http` | ✅ 通过 |

## 1. schema 与往返

- `graph-doc.schema.json` 的 `graphParam` 改成 `allOf: [$ref operator-manifest.schema.json#/$defs/paramSpec]`
  + `required: [default, binds]` + `unevaluatedProperties: false`。manifest 那边把原来的 `param` 拆成
  `paramSpec`（全部规格字段，都可选）与 `param`（`name` + `allOf paramSpec` + 必填 `name/type/default`），
  **一份定义两处用**。C++ 现场导出的 manifest（71 个算子）照样通过。
- `scripts/validate_schema.py` 把同目录的 `*.schema.json` 按 `$id` 登记进 `referencing` 的 registry（本地解析，不联网；
  原来的 `jsonschema.validate` 会去 github 取引用、404），另加 `--expect-fail` 给负例用。
- 夹具 `schema/examples/graph-params.example.lyflow.json`：三个完整规格（vec3f 带限位 / 单位 / 分量名 / group，
  enum 带 options 与 `advanced`，int 带 placeholder）+ 一个老格式；`graph-params.invalid.lyflow.json` 是拼错的字段名。

```
ok: 3 node(s), 2 edge(s) 符合 D:\project\LyFlow\schema\graph-doc.schema.json
ok: D:\project\LyFlow\schema\examples\graph-params.invalid.lyflow.json 按预期不符合 D:\project\LyFlow\schema\graph-doc.schema.json
```

- Rust：`commands::tests::graph_params_full_spec_survives_a_save_load_roundtrip` —— 读夹具 → `save_graph` →
  `load_graph`，`params` 深相等且键顺序（外层与 leafSize 内部）一致，规格字段逐个在，老格式那条仍没有 type，
  再过一遍 core 校验为 `[]`。
- 编辑器：`graph-params.test.mjs` 新增一条，完整规格的夹具打开、做别的编辑、撤销重做后 `params` 的 JSON 文本不变。
- e2e（`params_p1.mjs` 验收 1）：经 Tauri 的 `loadGraph → saveGraph → loadGraph` 以及磁盘上的文件，`params`
  逐字相同；整张图跑通，gen 用的是图参数 `pointCount` 的 default 20000；Inspector 的图参数简表列出四个。

## 2. core 按图参数自己的规格校验（doctest + e2e）

展开期（`Expander::checkGraphParamValues`）对声明了 `type` 的图参数，用 `paramFromDecl`（原子图的 `paramFromJson`，
改成公开并对坏字段类型不抛）建一份 `Param`，走与节点参数**同一个** `coerceParam` + `checkRange`（从 plan.cpp 的
匿名命名空间里挪出来）。`default` 与宿主给的值都查（`applyGraphParamValues` 不再覆盖 decl 的 default，改记在
`GraphParam::given`）。不合法只报诊断并把 `RawGraph::paramValuesOk` 置 false：绑定照常写、`buildPlan` 照常把节点
那一层规整完、诊断收齐，最后不给可运行的计划。

```
$ lyflow validate gp.lyflow.json --param pointCount=30000000
[{"kind":"diagnostic","nodeId":"","severity":"error","phase":"validate","code":"bad_param",
  "message":"图参数 'pointCount'（Synthetic · Point Count）的 default：不能大于 2e+07","paramPath":"pointCount"},
 {"kind":"diagnostic","nodeId":"n_gen","severity":"error","phase":"validate","code":"bad_param",
  "message":"Point Count：不能大于 2e+07","paramPath":"pointCount"}]
2 条错误
```

（CLI 的 `--param` 本来就是写进 default，所以这里说「default」；经 `params_json` 传进来的说「传入的值」。）

```
$ lyflow-core-tests.exe -tc="*图参数规格*,*visibleWhen 的 ne*"
TEST CASE:  图参数规格：default 越过硬限位报 bad_param，paramPath 是名字、nodeId 为空
TEST CASE:  图参数规格：params_json 传入越界值同样报错，合法值照常
TEST CASE:  图参数规格：类型与 options 也按图参数自己的规格查
TEST CASE:  图参数规格：没有 type 的老图参数跳过第一步，照常运行
TEST CASE:  图参数规格：图参数的错与节点的错一次报全
TEST CASE:  visibleWhen 的 ne：不等于时才可见，与 schema、编辑器同一套判据（param-recipe P1.6）
[doctest] test cases:  6 |  6 passed | 0 failed | 289 skipped
```

- plan 被阻断（返回诊断数组）、run 整次 error 且没有任何节点进 running；
- 老格式里写了 `max` 但没有 `type`：不算规格，500 照常跑出 500 个点；被绑定节点自己的规整照旧（-1 仍是 `src.pointCount` 的错）；
- `enum` 没给 options 时只查是字符串（否则每个值都会被判非法）。
- bridge：`execution::tests::graph_param_values_reach_the_core_through_params_json`（经 C ABI 的 params_json：
  1234 真的进了 compute；9 越过 min 10 → `run_finished.error.code == bad_param`，零 running）；
  `commands::tests::validate_and_plan_carry_graph_param_values`（validate / plan 带取值走新入口
  `lyflow_validate_params` / `lyflow_plan_params`；传回 default 键不变，换值后被绑定节点与下游的键都变）。
- e2e 验收 2：default 设成 30000000 → `bad_param`、nodeId 空、Inspector 图参数简表那一行 `data-param-error=1`，
  F5 被拦下；`params: {pointCount: 0}` 同样拦下；`cutField: "w"` 按 options 报；老参数 `cutMax: 99` 没有图参数层的错、照常运行。

## 3. 普通参数「纳入配方」（e2e + 单测）

gen → voxel（leafSize = 0.02）→ passthrough，先跑一遍；在 Inspector 的 leafSize 行上**真的右键**，点「纳入配方（提升为图参数）」：

- doc 多了 `params.leafSize`：`type: vec3f`，`min/max/step/unit/doc` 与 manifest 声明逐个相等，
  `label = "体素网格 · Leaf Size"`，`default = [0.02, 0.02, 0.02]`，`binds = ["<voxel>.leafSize"]`；节点上的显式值删掉；
  撤销记录「纳入配方 leafSize」。
- 这一行 `data-graph-param="leafSize"`，标「由图参数 leafSize 提供」，显示 0.02。
- 立刻编计划：三个节点的 cacheKey 与纳入前的 `run_started` 逐个相同；再跑一遍三个都是 `skipped`（命中缓存），
  输出点数不变 —— 结果仓按内容寻址，键相同就是逐位相同的同一份结果。
- 真按 Ctrl+Z：doc 的 JSON 与纳入前逐字相同，这一行不再由图参数提供。

## 4. 子图内部参数（e2e + 单测）

gen 接两个实例 A、B（同一份子图定义，里面一个 voxel）。进 A，内部 voxel 的 `minPointsPerVoxel` 行右键「纳入配方」：

- 第一级：子图定义多了提升参数 `minPointsPerVoxel`（绑内参，默认值 0 = 内参当前值）；
  第二级：`params.minPointsPerVoxel` 绑 `<A>.minPointsPerVoxel`，default 0，label「<A 标题> / 体素网格 · Min Points / Voxel」；
  B 上什么都没写。子图里这一行标着由图参数提供、而且可编辑（改的是图参数）。
- 纳入后 A、B 内部节点的 cacheKey 都不变；把图参数改成 3 再跑：A 内部的键变了，**B 的键不变、`skipped`、点数不变**。
- 两次 Ctrl+Z（改值一次、纳入一次）后 doc 与纳入前逐字相同 —— 两级一起撤掉。
- 库算子：把这个子图存成 `lib.e2e_p1_*`、放进画布，Ctrl+Enter 进不去（`path` 仍是 `[]`），所以内部参数行不存在、
  也就没有这个菜单项；硬塞一个指进库算子的 `ui.path` 调 `promoteToGraphParam`，store 返回 null（`pathIsValid` 不过）。
- 单测另覆盖：已经提升过的内参只补最外一级（默认值取实例上的显式值，实例上的显式值删掉）；内部行上 `setParam`
  改的是图参数；取消子图提升时顶层那条 bind 一并摘掉。

## 5. 被绑定行上编辑（e2e）

纳入 gen.pointCount 后在这一行的数字框里输 30000 再失焦：`params.pointCount.default == 30000`，节点上没有显式值，
撤销记录「修改图参数 pointCount」，core 校验返回空数组（没有 `param_conflict`），运行 gen 出 30000 点。
路由只在一处：graph store 的 `setParam` 发现目标被图参数提供（`resolveGraphBinding`，含子图提升链）就转给
`editGraphParamValue`，所以 Inspector、2D 拖框、粘贴、重置都不会再写出显式值。

## 6. RunOptions.params（e2e）

当前配方是「基础」，交给 core 的是 `{pointCount: 30000}`（`snapshot().recipe`）。`__lyflow.run({ params: { pointCount: 12000 } })`：
gen 出 12000 点，gen、voxel、cut 的 cacheKey 全变，doc 的 default 仍是 30000（运行传参不进 GraphDoc）；
再按 F5（传回 default）：三个键回到原来那一组，三个节点都 `skipped`。

## 7. P1.6 三项

- **复制路径名**：右键「复制路径名」写进剪贴板的是 `<cut>.max`（子图里是展开后的路径 `A/voxel.param`，与
  `lyflow params`、事件的 nodeId 同一套）。e2e 把 `navigator.clipboard.writeText` 换成记录器断言。
- **`ne`**：core 的 `Condition` 补 `ne`，`conditionHolds` 按 eq → ne → in 取第一个（与编辑器 `isConditionMet`、schema
  一致），manifest 导出写得出 `ne`（以前 `writeCondition` 静默丢掉）。doctest 用一个 `visibleWhen {source ≠ identity}`
  的 path 参数验藏/露与必填；e2e 用子图参数声明（编辑器按 manifest param 形态渲染）验 Inspector 藏/露。
- **dirty 复原**：graph store 记 `savedDoc`（对象身份），dirty = `doc !== savedDoc`；`markSaved(path, doc)` 记真正写下去的
  那一份。e2e：存盘后不脏 → 改一下脏且标题栏有 ● → Ctrl+Z 回到保存点 dirty=false、● 消失 → Ctrl+Y 又脏。

## 8. 不回归

见页首三行。过程中有一次把 e2e 跑坏了：跑的途中我给改过的文件统一换行符（CRLF → LF），`tauri dev` 的 vite 热更新把页面
重载了，motion 分组的探针随之丢失、报「分组 suiteDelete 中断」。这不是代码问题，停掉后原样重跑一遍 787/787 全绿；
e2e 期间不再动 `packages/`、`app/`、`bridge/` 下的文件。最后一次 core 改动（诊断文案去掉一个空格）之后又跑了一遍
`pnpm check` 与 `e2e:http`，都绿。

## 取舍与计划没覆盖的决定

1. **图参数规格与被绑定目标规格不一致时以谁为准：两者都要满足。** 图参数的规格决定控件与第一道校验（P1.2），
   被绑定算子的声明是它自己的硬契约，core 照旧查；所以图参数放宽限位也放不过算子不认的值，收紧限位则只影响图参数这一层。
   编辑器在 `bindToGraphParam` 时拒绝类型不同的目标（两道校验必有一道永远不过）；限位不同不拒（常见的是图参数更严）。
2. **子图提升链在嵌套多层时的命名：每一层各起各的名字，都用原参数名，重了在那一层加 `_2`、`_3`**（与现有「提升为子图参数」
   同一条规则）；已经提升过的层直接沿用那个名字。图参数名同样取原参数名、全局重名加后缀。label 带路径上的实例标题
   （「实例 / … / 内部节点 · 参数 label」），因为同一子图的两个实例各纳入一次时名字只差后缀、label 要分得开。
3. **`removeGraphParam` / `unbindFromGraphParam` 写回的值：当前配方下的有效值**（default ← 当前覆盖；P1 就是 default），
   与「解除绑定，行为不变」同一个口径 —— 用户看着的那一组跑出来不变。写回仍按稀疏存储（等于算子默认就不写）。
   P3 里其它配方对这个名字存的值会成为失配 ①（多出），由失配报告处理。
4. **Inspector 顶上加了一个「图参数」简表**（值可改、可解除单条绑定、可删除；图参数自己的诊断贴在行下；多于 4 个默认收起）。
   计划的「图参数」分组是 P2 的面板，这里只是让 P1 的动作在界面上看得见、测得到，P2 可以直接替换。
5. **子图里整条链通到图参数的内参行可编辑**（改的是图参数）。F4 原规则是「提升过的内参只读」，因为外层表单才是写入处；
   链通到图参数时写入处就是图参数，编辑这一行与顶层被绑定行同一个语义，没有第二个写入处。链没通到图参数的照旧只读。
6. **C ABI 追加 `lyflow_validate_params` / `lyflow_plan_params`**（v11 里追加，ABI 号不变；老入口转调新入口）。
   计划写的是「传到 C ABI 的 params_json」，但 validate / plan 原本没有这个参数；K5 要求校验带取值，P3.8 的「将重算 N 个」
   要计划带取值，所以两个都加。`client.hpp` 与 `lyflow-client` 各多一个可选参数。编计划（`requestPlan`）也带上合成的取值。
7. **顺手堵上的几个 `unknown_bind` 陷阱**：删节点摘掉指着它的 bind；合成子图时被绑定的参数自动提升成外参、bind 改指到
   新实例；解散子图时 bind 改指回内联出来的内参（不再往内参上写会撞 `param_conflict` 的显式值）；取消子图提升时摘掉
   指着那个外参的 bind。
8. **没做的**：复制粘贴 / Ctrl+D 出来的副本不带绑定（绑定是图参数的属性，不是节点的），副本上的这个参数回到算子默认值；
   宿主经 HTTP 的 MCP 客户端（`packages/mcp`）没加 params（P4）。
9. `validate_schema.py` 原来用 `jsonschema.validate`，跨文件 `$ref` 会触发联网取 `$id`（github 404）；改成本地 registry。
