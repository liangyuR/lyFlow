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

  // 本算子成立的前提与明确不适用的情形。给使用者与 Agent 看，Inspector 在 doc 下方
  // 以「适用前提」列出。没什么可写的就不出现这一项。写法见 docs/op-packs.md。
  "preconditions": [
    "每个体素只留质心，原始点被丢弃；后续要逐点强度或法线的算子得接在它前面。",
    "叶子尺寸接近点间距时等于没降采样，远大于特征尺度时会把特征一起抹平。"
  ],

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

`visibleWhen` 有意做得很弱（只支持对同节点其他参数的等值/包含判断），不是通用表达式引擎。
需要复杂逻辑说明算子该拆了。

分组用 `group` 字段，`advanced: true` 的参数默认收进折叠区。

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
