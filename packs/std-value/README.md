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
