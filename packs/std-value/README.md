# 通用值与集合

`std-value` 不依赖 PCL、OpenCV 或额外运行时；默认随标准包启用，也可单独构建：

```powershell
cmake -S core -B build/value -DLYFLOW_STD_PACKS=OFF -DLYFLOW_PACKS=std-value
cmake --build build/value
```

## 数据契约

端口复用现有 `Record`，不新增核心数据种类或 C ABI。生产者和每个消费者都校验类型标签与载荷：

```json
{"type":"std.Value.v1","data":{"kind":"number","value":12.5,"unit":"mm"}}
```

`kind` 为 `number`、`boolean`、`string` 或 `record`。只有数值可带非空单位；不接受 null、NaN/Inf、二进制 JSON 或未知字段。记录值形如 `{"kind":"record","value":{"type":"domain.Part.v1","data":{"id":"a"}},"unit":""}`，保留领域类型和身份。

数值统一使用 float64 语义。JSON 整数仅接受闭区间 `[-2^53,2^53]`，包括记录字段及排序键；更大的整数标识必须使用字符串，避免转换为 double 后错误地比较相等。`value.number` 的 Float 参数及显式浮点值遵循 float64 舍入，不承诺任意精度整数。数字排序键统一转换为 double 比较，整数与浮点同值时保留原有顺序，不混用 JSON 数字子类型的比较规则。

```json
{"type":"std.List.v1","data":{"kind":"number","items":[1,2,1],"unit":"mm"}}
```

列表同类、同单位；记录列表还要求每项 `type` 一致。每列表最多 4096 项，整个载荷不超过 1 MiB，JSON 嵌套深度最多 16，每个字符串或字段名最多 64 KiB。有效空列表不等同错误。图像、点云、大数组及进程指针不得放进这些 JSON 载荷。

## 算子

| ID | 用途 |
|---|---|
| `value.number` / `value.boolean` / `value.string` | 可连接的数值、布尔、UTF-8 文本常量 |
| `value.from_measurement` | 显式转换有效有限测量值，保留单位 |
| `value.field` | 从领域记录提取标量，路径使用 JSON Pointer，如 `/regions/0/area` |
| `value.arithmetic` | 加、减、乘、除及无单位幂运算 |
| `value.expression` | 八个可选数值/布尔输入的受限公式、比较、逻辑与条件式 |
| `value.compare` | 六种比较，数值支持绝对/相对容差；文本与布尔支持相等/不等 |
| `value.logic` | AND、OR、XOR、NOT、NAND、NOR、XNOR |
| `value.concat` | 按 A、分隔符、B 拼接文本，保留空串 |
| `list.constant` | 从有界 JSON 数组构造列表 |
| `list.pack` | 一或两个标量/记录值按端口顺序组成列表 |
| `list.concat` | 两列表按顺序拼接，不去重、不额外扁平化 |
| `list.get` | 单元素提取，输出 `std.Value.v1` |
| `list.subset` | 按显式索引数组提取，保留索引顺序及重复项 |
| `list.stable_sort` | 稳定排序；同时输出结果列表及原始索引列表 |

索引从 0 开始，负数与越界始终报错。空子集返回有效空列表。排序同键保留输入顺序；记录排序通过 JSON Pointer 指定记录 `data` 内的标量字段，原始身份不变。输入始终只读。

加减和比较要求单位精确一致，不在 `m`、`mm`、`px` 之间隐式换算。乘法至少一端无单位；除法允许无单位除数或相同单位相消，分母为真正的零才拒绝，负分母及合法的小分母可用。幂运算仅支持无单位输入，`0^0`、零的负次幂、非法实数域及非有限结果报错。复合量纲和广播不在本版范围。

数值近似相等定义为 `abs(a-b) <= absoluteTolerance + relativeTolerance * max(abs(a),abs(b))`；不等是其严格补集，容差内的值在大小比较中视为相等。绝对容差沿用输入单位，相对容差无单位。实现对大数采用等价缩放计算，避免差值溢出。

`value.field` 对数字的单位必须由 `unit` 参数显式指定，不猜测父级字段。普通领域记录可包含未选择的 null 字段，例如空区域统计先读取 `/valid=false`；选中的 null、非标量或缺失路径产生诊断。对于 `list.get` 返回的记录包装，路径直接定位内部领域 `data`。

布尔结果可直接连接 `flow.select.cond`，并将 `flow.select.path` 设为 `value`。二元逻辑节点正常调度两端；需要避免计算未选分支时使用 `flow.select` 的现有惰性端口，不声称逻辑节点具有调度短路。

宿主通过顶层图参数绑定 `value.string` / `value.number`，使用公开运行接口的 `params_json` 按次覆盖信息，不需要常驻请求对象或跨运行残留。当前公开 C ABI 输入注入仅支持云与图像，不支持直接注入任意 Record；C++ 内部执行器的 Data 注入不等于公开 ABI 承诺。外部 xyz Request 的工件对象、动态云列表和原协议适配属于后续范围。本实现参考外部能力与边界，独立实现通用逻辑，不复制其 proprietary 源码，也不作未测量的性能提升承诺。

验证入口为 `packs/std-value/tests/test_values.cpp`，覆盖版本/大小/深度限制、单位、算术定义域、比较阈值、稳定集合语义及真实执行器的惰性条件选择。

## 受限表达式

`value.expression` 提供固定可选输入 `a`..`h`，参数 `expression` 默认为 `(a+b)/2`，输出 `value` 继续使用 `std.Value.v1`。仅接受 number/boolean，实际取值的变量必须连接，未连接变量不隐式取零。更改公式保持端口身份，不动态重建连线。

支持十进制/小数/科学记数、布尔常量、`+ - * / ^`、括号、一元 `+ - !`、六种数值比较、`&& ||` 和右结合 `?:`。幂右结合且高于一元符号：`2^3^2=512`、`-2^2=-4`、`2^-2=0.25`。布尔值只支持相等/不等比较和逻辑，不隐式转换数值真假；数值相等精确比较，容差可显式表达为 `abs(a-b) <= c`。

函数白名单为 `abs`、`sqrt`、`floor`、`ceil`、`round`（单参），`min`、`max`、`pow`（双参），以及 `clamp(value, lower, upper)`（三参，要求有序上下界）。加减、数值比较、min/max/clamp同单位；乘除和幂沿用上文算术规则；sqrt仅无单位，abs/floor/ceil/round保留单位。round的半数远离零舍入（±2.5→±3）。无字符串、属性/下标、赋值、循环或宿主代码执行。

完整语法始终验证，包括未取分支中的未知函数和错误参数个数；运行时 `&&`、`||` 和 `?:` 短路，因此 `true ? 2 : a` 不要求连接a、`false && (1/0 > 0)`不触发除零。实际选中分支决定结果类型和单位。连接的图上游仍正常求值，上游失败不会被表达式短路隐藏；需要图级惰性选择时继续使用 `flow.select`。

源码最多4096字节、1024个AST节点，表达式解析嵌套最多64层，AST根到叶最多64节点，解析和计算检查取消。语法/资源超限为指向expression的bad_param并报告从1开始的字节位置；实际输入、类型、单位和数学域错误为bad_input。零除、0^0、零负幂、非法实数域和非有限结果拒绝，输入保持只读。

可运行示例为 [均值与上限选择](../../examples/p2-expression.lyflow.json)；图校验与运行共享语法校验，不要求未求值的可选变量已连接。逐项参考源码分析、支持矩阵与回归入口见 [P2表达式分析](../../docs/p2-expression-analysis.md)。此项仅完成P2首个受限公式切片，不包含外部任意变量名、动态端口或原配置协议兼容。
