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
