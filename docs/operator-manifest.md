# OperatorManifest — 算子描述

C++ 侧算子注册表导出的机器可读描述。前端据此生成节点搜索面板、节点外观、参数表单和端口类型校验。

**这个格式做对了，后面加几十个算子都很轻松；做错了，每加一个算子都要改前端。**

## 一个算子的完整描述

```jsonc
{
  "id": "filter.voxel_grid",       // 全局唯一，用 . 分命名空间
  "version": "1.2.0",              // semver，破坏性变更升 major
  "label": "Voxel Grid",
  "category": "Filter/Downsample", // 用 / 分层，决定搜索面板的树形结构
  "keywords": ["downsample", "降采样", "体素"],
  "doc": "用体素栅格对点云降采样，每个体素保留质心。",

  "inputs": [
    { "name": "cloud", "type": "PointCloud", "label": "Cloud", "required": true }
  ],
  "outputs": [
    { "name": "cloud", "type": "PointCloud", "label": "Cloud" }
  ],

  "params": [
    {
      "name": "leafSize",
      "type": "vec3f",
      "label": "Leaf Size",
      "default": [0.01, 0.01, 0.01],
      "min": 0.0001,
      "max": 1.0,
      "step": 0.001,
      "unit": "m",
      "doc": "体素边长。越大点越少。"
    },
    {
      "name": "minPointsPerVoxel",
      "type": "int",
      "label": "Min Points / Voxel",
      "default": 0,
      "min": 0,
      "advanced": true
    }
  ],

  "capabilities": {
    "cancellable": true,      // 支持中途取消 → 可用于 live preview
    "previewable": true,      // 支持降级质量快速预览
    "deterministic": true     // 同输入同输出 → 可缓存
  }
}
```

JSON Schema： [`schema/operator-manifest.schema.json`](../schema/operator-manifest.schema.json)

## 参数类型 → 控件映射

前端维护一张表，把 `param.type` 映射到 React 控件。**这张表是前端唯一需要知道的"算子知识"。**

| `type` | 控件 | 附加字段 |
|---|---|---|
| `bool` | Checkbox | — |
| `int` / `float` | 拖动数字框（drag-to-change） | `min` `max` `step` `unit` `softMin` `softMax` |
| `vec2f` / `vec3f` / `vec4f` | 分量数字框 + 锁定联动 | 同上，`componentLabels` |
| `enum` | Select | `options: [{value, label, doc?}]` |
| `flags` | 多选 chips | `options` |
| `string` | 单行输入 | `placeholder` `pattern` |
| `text` | 多行输入 | `rows` |
| `path` | 输入框 + 文件选择 | `filters: [{name, extensions}]` `mode: 'open'\|'save'\|'dir'` |
| `color` | 取色器 | `alpha: bool` |
| `transform` | 平移/旋转/缩放分组 | — |
| `curve` | 曲线编辑器（P2） | — |

新增控件类型的成本是**前端加一个 case**，不是改架构。这是可以接受的。

## 参数联动

参数可以声明可见/可用条件，避免一个算子塞几十个永远用不上的字段：

```jsonc
{
  "name": "radius",
  "type": "float",
  "visibleWhen": { "param": "mode", "eq": "radius" }
}
```

`visibleWhen` 有意做得很弱（只支持对同节点其他参数的等值 `eq` / 不等 `ne` / 包含 `in` 判断），不是通用表达式引擎。
需要复杂逻辑说明算子该拆了。三个判据同时给时按 `eq` → `ne` → `in` 取第一个；schema、core（`Condition`、
`conditionHolds`、manifest 导出）、编辑器（`lib/params.ts` 的 `isConditionMet`）三方同一套（param-recipe P1.6）。

分组用 `group` 字段，`advanced: true` 的参数默认收进折叠区。

## 参数的语义标记（`semantic`，M8b）

参数可以带一个只给编辑器看的语义标记，执行器与校验一概不看。目前只有一种：

```jsonc
{
  "name": "template1DatumRoi", "type": "vec4f", "unit": "mm",
  "semantic": "roi",                       // [xMin, yMin, xMax, yMax]，XY 平面上的一个框
  "roiBackdrop": {                         // 可选：框画在哪片云上
    "dir": "templateDir",                  // 本算子的一个 path 参数
    "files": ["template1Left", "template1Right"],  // 本算子的 string / path 参数
    "label": "模板 1",                     // 可选：这组框在切换条上的名字
    "labelParam": "template1Id"            // 可选：名字后面接上这个 string 参数的当前值 →「模板 1 · f1」
  }
}
```

- `semantic: "roi"` 只能标在 `vec4f` 上（`lyflow manifest --check` 查）；单位按 `unit`，`mm` 按 0.001 换成米。
- 编辑器在 2D 剖面视图里把选中节点**当前可见**的 roi 参数画成可拖、可拉伸的框（拖框身平移、拖四角拉伸，
  吸附 0.1 mm，整段拖动一条撤销），与数字框是同一个参数的两种编辑方式。
- 没有 `roiBackdrop`：框在数据坐标系里，画在节点显示的那片云上（例如 `gap.overall_roi.roi`）。
  有：框在 `<dir>/<file>` 那几个文件的坐标系里（例如 `gap.locate_template` 的四个角色框在模板坐标系里，
  底图是那个槽的左右模板），编辑器经宿主读这几个文件（Tauri 的 `load_cloud_file`），**不需要先跑图**。
  坐标系不同的框不会画在同一片底图上：可见的 roi 参数按 `roiBackdrop` 分组（`dir` + `files` 相同 = 同一组），
  视图**一次只画一组**，组不止一个时顶上有切换条（名字取 `label` + `labelParam` 的值），还有「复制到其它槽」
  （按组内声明顺序一一对应写过去）；Inspector 里各组所在的参数组变成手风琴，与切换条联动（M8c / L20）。
  有带底图的组时，数据坐标系里的框（例如 `locate_template.overallRoi`）不进 2D 拖框。
- 子图提升参数时 `semantic` 跟着走，`roiBackdrop` 不带（它指的是内部算子的参数名）。

## 片段（`snippets`，M8b）

manifest 顶层可能多一段 `snippets`：算子包随附的片段（一组节点 + 边 + 对外端口提示），
文件格式是 `*.lyflow-snippet.json`（[schema/snippet.schema.json](../schema/snippet.schema.json)），
放在包目录的 `snippets/` 下、构建时编进包里，写法见 [op-packs.md](op-packs.md)「随包的片段」。

```jsonc
"snippets": [
  { "id": "gap.measure_skeleton", "label": "测点骨架", "category": "间隙", "pack": "gap@0.2.0",
    "nodes": [ { "id": "line", "op": "gap.role_line", "params": { "role": "datum" },
                 "ui": { "position": { "x": 0, "y": 0 }, "title": "基准线" } }, … ],
    "edges": [ { "from": { "node": "line", "port": "line" }, "to": { "node": "flush", "port": "baseLine" } }, … ],
    "ports": { "inputs":  [ { "node": "line", "port": "scan", "hint": "定位之后的剖面对" }, … ],
               "outputs": [ { "node": "flush", "port": "value" }, … ] } }
]
```

- 插入就是**带自动连线的粘贴**：节点与内部边照搬（id 重新分配），`ports.inputs` 列的那些输入
  （没给这一项时是片段里所有没接的必需输入）按「恰好一个类型兼容的输出」接到插入之前就在图里的节点上。
  插完是普通节点，**没有展开 / 收回**。
- 启动自检（`Registry::validate`）查：算子都注册了、参数名都认得、边两端的端口存在且类型接得上、
  对外输入在片段里没接边。坏片段让 `lyflow manifest --check` 失败，而不是等人插进画布才发现。
- 用户自己的片段不进 manifest：桌面宿主扫 app data 下的 `snippets/` 与环境变量 `LYFLOW_SNIPPET_DIRS`
  （分号分隔），经 `list_snippets` 给编辑器；同 id 时用户的那份覆盖包里的。

## 端口类型系统

V1 的规则刻意简单：

1. **精确匹配** — `type` 字符串相同即可连。
2. **`"Any"` 通配** — 用于 Reroute、Debug View 这类透传节点。
   M3 起 `Any` 端口的**实际类型由连线推导**（E6）：沿边把已知的具体类型传播到定点，
   C++ 的 `buildPlan` 和前端的 `typecheck.ts` 各有一份同样的实现。
   一个节点的**全部** `Any` 端口共用一个类型变量 —— `util.reroute` 正是这个语义，
   需要多个互相独立的 `Any` 端口的算子请拆开。推不出来的端口仍是 `Any`，不报错。
   > 阶段 A 加了个逃生口：C++ 侧的 `Port::anyGroup`（不进 manifest）让同节点上
   > 分组不同的 `Any` 端口各用各的类型变量。目前只有 `flow.select` 的 `cond` 用了它
   > （[ADR-0016](adr/0016-error-as-value-and-lazy-ports.md)）。前端不需要知道这件事：
   > 推导结果一致，因为 `cond` 与 `a`/`b` 之间本来就没有边。
3. **显式转换表** — manifest 包里附一张全局类型表，声明哪些类型可隐式转换：

```jsonc
{
  "types": [
    { "name": "PointCloud", "color": "#4a9eff" },
    { "name": "Indices",    "color": "#c586c0" },
    { "name": "Transform",  "color": "#a0d030" },
    { "name": "Plane",      "color": "#e0a030" },
    { "name": "Any",        "color": "#888888" }
  ]
}
```

> `castableTo` 目前一条都没用上，这是有意的。曾经有过一个
> `PointCloudXYZI castableTo: ["PointCloud"]`，M2 把它删了（m2-plan.md 的 D10）：
> 强度、法线、颜色是 `PointCloud` 的**可选通道**，不是另一个类型。
> 让类型系统承诺一件数据模型做不到的事，迟早要在
> 「XYZI 连到 XYZ 端口之后强度去哪了」这种问题上翻车。
> 字段留着 —— 真出现「Image 与 ImageGray 可以隐式转」这类需求时它就位。

`color` 让前端能给端口和连线着色——这是零成本的巨大可读性提升，一定要在 M0 就做。

**不做泛型。** `PointCloud<T>` 这类参数化类型会把校验器复杂度抬一个量级。
真需要时，做法是让 C++ 侧针对具体实例化导出多个算子条目，而不是让前端做类型推导。

### Bundle（M8a）

一根线带一组有关系的数据（m8-plan L1–L3）。端口类型写作 `Bundle<kind>`，kind 由算子包在
manifest 的 `bundles` 段里**声明**字段表：

```jsonc
{
  "bundles": [
    { "kind": "gap.ScanPair", "label": "剖面对", "pack": "gap",
      "fields": [ { "name": "primary", "type": "PointCloud" },
                  { "name": "secondary", "type": "PointCloud" },
                  { "name": "merged", "type": "PointCloud" } ] }
  ]
}
```

- **类型检查只认 kind 相等**：`Bundle<gap.ScanPair>` 接 `Bundle<gap.RoiSet>` 报 `type_mismatch`，
  Bundle 与普通类型之间也不隐式转换；`Any` 照常兼容（`flow.fallback` 的 `Any` 端口推导成
  `Bundle<kind>`）。类型表里的 `Bundle` 一项只给所有 Bundle 端口一个共同的颜色，
  裸的 `"Bundle"` 不能当端口类型。
- **字段**是类型表里的具体类型，不能是 `Any`、`Error`，也**不嵌套** Bundle；字段名不含 `.`。
- **执行期按声明查**：算子写出一个 Bundle 端口之后，执行器查字段齐不齐、类型对不对、有没有多出来的，
  不符报 `contract_violation`（portName 是那个输出端口），summary 的 `contractViolations`
  记一条 `expected: {bundle, fields}` / `actual`。
- **按字段寻址**：`<port>.<field>`（如 `scan.merged`）在结果仓、C ABI（`lyflow_output_cloud` 等
  取数函数不改签名）、事件的 `stats.outputs`、`lyflow_output_info`、summary 与图输出上都认。
  边**不**按字段接 —— 想把一个字段接给普通端口，用包里的拆分算子（gap 包的 `gap.split_*`）。
- `Registry::validate()` 拒掉：字段类型未知或非法、端口引用了没声明的 kind、裸 `Bundle`。

## 端口上的两条执行语义（阶段 A）

输入端口可以多带两个布尔标志（[ADR-0016](adr/0016-error-as-value-and-lazy-ports.md)）：

```jsonc
"inputs": [
  { "name": "a", "type": "Any", "acceptsError": true },
  { "name": "b", "type": "Any", "lazy": true, "acceptsError": true }
]
```

- `acceptsError`：上游失败时本节点不被连坐，而是在这个端口上收到一个 `Error` 值
  （类型表里因此多了一个 `Error` 类型）。
- `lazy`：这个端口的上游闭包不进初始计划，算子返回 `Status::Demand` 时才被调度。

两条都只对 `inputs` 有意义；写在 `outputs` 上会被 `Registry::validate()` 拦下。
前端只需把它们画成角标（`lazy` 的边画成虚线），语义全在 C++ 侧。

## 端口契约与端口样例（M6）

端口还可以多带两项，两者是两件不同的事：

```jsonc
"inputs": [
  { "name": "primary", "type": "PointCloud",
    "contract": { "elementCount": { "eq": 1280 } } },
  { "name": "logits", "type": "Tensor", "contract": { "shape": [2, -1, 1280] } }
],
"outputs": [
  { "name": "quality", "type": "Record",
    "example": { "kind": "Record", "type": "GapFitQuality",
                 "data": { "inlierCount": 113, "rmsResidualMm": 0.0257 } } }
]
```

- **`contract`**（[ADR-0024](adr/0024-port-contracts-four-kinds.md)）**只有四种键**：
  `elementCount: { eq | min | max }`、`finite: true`、`shape: [..]`（-1 = 任意）、
  `recordType: "<type>"`。执行器在**输入绑定时**检查，违反报 `contract_violation`
  并进 run summary 的 `contractViolations`。四种之外的键让 `lyflow manifest --check` 失败。
  刻意不做表达式：与参数的 `visibleWhen` 同一条原则 —— 需要更复杂判断的时候，
  通常说明这个算子该拆了。
- **`example`** 是一份样例值（任意 JSON），**不参与任何校验**。Record 端口只有一个
  `type` 字串的话，「`data.inlierCount` 到底存不存在」得翻算子实现才知道。
  前端在端口详情里把它做成可展开的一块。

## 加载期校验（`validate`，M7）

manifest 里**没有**这一项：它是 `OperatorDesc` 上可选的 C++ 函数指针，前端与 Agent 看到的是它产出的诊断。

```cpp
using ValidateFn = std::vector<lyflow::Issue> (*)(const lyflow::ParamView& params,
                                                  const std::set<std::string>& connectedInputs);
// Issue = { Severity severity; Status status; }，status.code 通常是 bad_param，
// paramPath / portName 用来定位
```

- **纯函数**：只看解析后的参数（已合并默认值与绑定值）和已连接的输入端口名集合，**不给任何数据**。
- `buildPlan` 在参数解析与连边之后对每个节点调用。**error** 进 `Phase::Validate` 诊断并使该节点无效
  （plan 被阻断，`lyflow validate` 非零退出）；**warning** 出现在 `lyflow validate` 的诊断数组里
  （`severity: "warning"`），运行时走 warn 日志通道。
- 只依赖参数与连接关系的检查写在这里，`compute` 里不再保留副本；依赖数据的约束用运行期信号
  （quality 字段 / warn 日志 / 错误值）；用法说明写进 `doc` 或参数的 `doc`。写法见 [op-packs.md](op-packs.md)「约束写在哪里」。

## 导入器（阶段 A）

manifest 顶层可能多一段 `importers`（[ADR-0017](adr/0017-graph-outputs-injection-importers.md)）：

```jsonc
"importers": [
  { "kind": "StandardGap.yml", "label": "StandardGap 配置", "pack": "gap@1.0.0" }
]
```

`kind` 是传给 `lyflow_import` 的第一个参数。前端据此列「导入…」，不硬编码任何格式名。
一个导入器都没注册时这一段整个不出现。

## 版本与迁移

算子改动分三档：

| 变更 | 版本 | 老图 |
|---|---|---|
| 加可选参数、改文案、改默认值 | patch / minor | 直接打开，必要时提示默认值变更 |
| 改参数含义、删参数、改端口类型 | major | 打开时警告，走迁移表 |
| 重命名算子 | 保留 `aliases: ["old.id"]` | 自动重定向 |

迁移表放在 C++ 侧，前端只负责把结果写回文档
（[ADR-0008](adr/0008-migration-as-diagnostic.md)）：

```cpp
op.version = "2.0.0";
op.migrations = { Migration{1, &migrateFromV1} };   // json(const json&)
```

`Registry::validate()` 要求链条覆盖 `1..currentMajor-1` 且无断档，缺一环启动就报。
校验阶段套用迁移后产出一条 `kind: "migration"` 的诊断
（`{ nodeId, op, opVersion, params, notes[] }`，severity 是 warning），
执行器**在内存里**用迁移后的值继续跑 —— 老图当场就能运行，不必先存一次盘。
前端把这批动作交给 `applyMigrations`：一条撤销记录、置 dirty、toast 提示。

迁移逻辑不能在前端，否则脚本生成的图和 headless 执行走不到迁移；
写回不能在 C++，因为它不拥有文档（ADR-0002）。

迁移只碰参数。端口改名、增删要靠新算子 id + `aliases` —— 连线不归算子管，
它没法在自己的迁移函数里改边。

现成的例子：`filter.random_sample` 1.0.0 → 2.0.0，`count`/`ratio` 改名成
`keepCount`/`keepRatio`。

## 热重载

开发期最值钱的功能之一，M3 已实现（[ADR-0009](adr/0009-hot-reload-by-copy.md)）：
C++ 侧算子库重新编译后推一条 `manifest-updated`，前端整份替换 manifest store，
**当前打开的图与撤销栈一个字都不动**。算子被删掉的节点保留在 doc 里，
画布上显示成「算子缺失」，可删可等。

实现上要求前端把 manifest 存在独立 store、节点渲染时按 `op` 现查，
而不是在创建节点时把 manifest 快照进节点里。这个决定在 M0 就做对了，
所以 M3 这一段几乎没有前端改动。

热重载会**清空结果缓存并取消正在跑的 run**：缓存里的 `shared_ptr` 指向旧 DLL 里的对象，
跨代持有它是未定义行为。这是 E4 定死的取舍。

## 子图与库算子也是算子（M4）

`sub:<subgraphId>` 与 `lib.<id>` 两种节点的 OperatorDesc 是**合成**出来的
（[ADR-0010](adr/0010-subgraph-by-expansion.md)）：端口来自子图的 `inputs`/`outputs`，
参数来自提升出来的 `params`。合成结果与内置算子在形态上没有任何区别 ——

- **库算子进 manifest**，所以节点面板、搜索、参数表单、类型着色全都零改动就支持它，
  分类挂在 `Library/` 下。这是 ADR-0003 第三次兑现。
- **文档内的子图不进 manifest**：它的定义随文档走，而 manifest 是进程级的。
  前端在渲染时把 `doc.subgraphs` 合成进算子表（`lib/subgraph.ts` 的 `augmentOperators`）。
- 合成出来的算子有一个只会在「展开漏了」时被调到的 compute 桩：
  `Registry::validate()` 要求 compute 非空，而一个静默产出空结果的算子是最难查的。
