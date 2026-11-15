# std-value 逐算子迁移清单

状态：实施中；当前 16 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 004 · list.concat

- 当前：`std-value` / `1.0.0` / 列表拼接。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:558](../../packs/std-value/ops/register.cpp#L558)。
- 端口：`a:Record`, `b:Record` → `list:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：同契约列表按A后B拼接，重复与身份保留，不扁平化
- 目标契约：同契约列表按A后B拼接，重复与身份保留，不扁平化；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：空列表/重复项/混单位与数量上限
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 005 · list.constant

- 当前：`std-value` / `1.0.0` / 列表常量。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:526](../../packs/std-value/ops/register.cpp#L526)。
- 端口：无 → `list:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：std.List.v1同类型列表，最多4096项，不含null、非有限或大数组
- 目标契约：std.List.v1同类型列表，最多4096项，不含null、非有限或大数组；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：合法空列表、异类型拒绝、数量上限
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 006 · list.get

- 当前：`std-value` / `1.0.0` / 列表元素提取。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:566](../../packs/std-value/ops/register.cpp#L566)。
- 端口：`list:Record` → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：零起点非负索引提取列表元素，越界诊断
- 目标契约：零起点非负索引提取列表元素，越界诊断；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：负数、空列表与越界失败；元素原单位/身份保留
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 007 · list.pack

- 当前：`std-value` / `1.0.0` / 值合并为列表。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:550](../../packs/std-value/ops/register.cpp#L550)。
- 端口：`a:Record`, `b:Record?` → `list:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：一到两个同类型值按端口序成列表，不去重或扁平化
- 目标契约：一到两个同类型值按端口序成列表，不去重或扁平化；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：嵌套值/重复项保持；类型和单位统一校验
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 008 · list.stable_sort

- 当前：`std-value` / `1.0.0` / 列表稳定排序。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:585](../../packs/std-value/ops/register.cpp#L585)。
- 端口：`list:Record` → `list:Record`, `indices:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：按标量或记录字段稳定排序，输出原索引
- 目标契约：按标量或记录字段稳定排序，输出原索引；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：相等键输入序保持；字段缺失/混类型明确处理
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 009 · list.subset

- 当前：`std-value` / `1.0.0` / 列表子集提取。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:576](../../packs/std-value/ops/register.cpp#L576)。
- 端口：`list:Record` → `list:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：按显式索引顺序提取，可重复，空索引得到空列表
- 目标契约：按显式索引顺序提取，可重复，空索引得到空列表；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：非单调/重复索引保持请求序；任一越界原子失败
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 010 · value.arithmetic

- 当前：`std-value` / `1.0.0` / 数值算术运算。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:449](../../packs/std-value/ops/register.cpp#L449)。
- 端口：`a:Record`, `b:Record` → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：有限数算术；加减同单位，乘除限支持的标量量纲，幂限无单位
- 目标契约：有限数算术；加减同单位，乘除限支持的标量量纲，幂限无单位；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：除零、溢出、非法复合量纲拒绝；保持既有单位规则
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 011 · value.boolean

- 当前：`std-value` / `1.0.0` / 布尔常量。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:402](../../packs/std-value/ops/register.cpp#L402)。
- 端口：无 → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：std.Value.v1独立boolean，不用Measurement.ok承载普通布尔
- 目标契约：std.Value.v1独立boolean，不用Measurement.ok承载普通布尔；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：true/false严格类型；与flow.select的显式path互通
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 012 · value.compare

- 当前：`std-value` / `1.0.0` / 数值与标量比较。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:464](../../packs/std-value/ops/register.cpp#L464)。
- 端口：`a:Record`, `b:Record` → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：数值绝对/相对容差比较；字符串/布尔只支持相等和不等
- 目标契约：数值绝对/相对容差比较；字符串/布尔只支持相等和不等；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：容差边界、类型不符及Unicode；不自动升级为生产判定
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 013 · value.concat

- 当前：`std-value` / `1.0.0` / 字符串拼接。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:501](../../packs/std-value/ops/register.cpp#L501)。
- 端口：`a:Record`, `b:Record` → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：A/connector/B拼字符串，保留空串与Unicode，64KiB上限
- 目标契约：A/connector/B拼字符串，保留空串与Unicode，64KiB上限；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：长度边界与Unicode顺序；不静默截断
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 014 · value.expression

- 当前：`std-value` / `1.0.0` / 受限表达式计算。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/expression.cpp:603](../../packs/std-value/ops/expression.cpp#L603)。
- 端口：`a:Record?`, `b:Record?`, `c:Record?`, `d:Record?`, `e:Record?`, `f:Record?`, `g:Record?`, `h:Record?` → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：有限数值/布尔受限表达式，a..h单位校验；4096字节/1024节点/64层
- 目标契约：有限数值/布尔受限表达式，a..h单位校验；4096字节/1024节点/64层；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：幂右结合、条件短路、单位不符和复杂度上限；上游连接仍正常调度
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 015 · value.field

- 当前：`std-value` / `1.0.0` / 记录字段提取。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:440](../../packs/std-value/ops/register.cpp#L440)。
- 端口：`record:Record` → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：按JSON Pointer从Record提标量，数值单位由参数显式指定
- 目标契约：按JSON Pointer从Record提标量，数值单位由参数显式指定；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：缺字段/非标量/非法Pointer失败；不推断未知物理单位
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 016 · value.from_measurement

- 当前：`std-value` / `1.0.0` / 测量值转数值。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:427](../../packs/std-value/ops/register.cpp#L427)。
- 端口：`measurement:Measurement` → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：仅从有效有限Measurement提取数值并保留单位
- 目标契约：仅从有效有限Measurement提取数值并保留单位；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：无效测量明确失败；m/mm标签和值均不偷偷转换
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 017 · value.logic

- 当前：`std-value` / `1.0.0` / 布尔逻辑运算。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:484](../../packs/std-value/ops/register.cpp#L484)。
- 端口：`a:Record`, `b:Record?` → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：严格布尔逻辑，普通上游均求值；惰性调度属于flow.select
- 目标契约：严格布尔逻辑，普通上游均求值；惰性调度属于flow.select；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：truth-table与二元输入诊断；不得把逻辑函数假装成惰性闭包
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 018 · value.number

- 当前：`std-value` / `1.0.0` / 数值常量。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:392](../../packs/std-value/ops/register.cpp#L392)。
- 端口：无 → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：std.Value.v1有限number，单位显式字符串
- 目标契约：std.Value.v1有限number，单位显式字符串；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：非有限值拒绝；空单位与m/mm/px不自动互换
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID

## 019 · value.string

- 当前：`std-value` / `1.0.0` / 字符串常量。目标：`std-value`；保留并验边界；**M1**。
- 源码：[packs/std-value/ops/register.cpp:417](../../packs/std-value/ops/register.cpp#L417)。
- 端口：无 → `value:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：core数据/Record/几何校验与C++标准库；当前及目标均不引PCL/OpenCV/ORT
- 当前契约：std.Value.v1 UTF-8字符串，上限64KiB
- 目标契约：std.Value.v1 UTF-8字符串，上限64KiB；计算语义不变，依赖仅来自公开core数据契约或公共内部库
- 迁移前置：M0
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：Unicode/空串与超限处理；不承载大数组
- 说明：注册归属调整与数据语义升级分开；当前算子ID用于跟踪，不要求按目标包统一改ID
