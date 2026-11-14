# ADR-0029：P0 值、集合、几何与图像结果契约

日期：2026-11-14。状态：采用。关联：[能力对照](../operator-coverage.md)、[补齐计划](../operator-completion-plan.md)。

## 背景

通用比较、集合、区域统计和卡尺需要可连线的小型结果。现有 Measurement、Transform 和 Bundle 各有独立语义，不能分别用成功标志冒充数字、用矩阵冒充工件、用固定字段结构冒充动态列表。

本次逐项阅读用户提供的本机 xyz-vision-lib 实现，核实原算法的实际语义后再修订 LyFlow 实现。原源码目录没有 Git 修订信息，不能据此声明与原清单指定提交或外部工作流兼容。分析与取舍见 [P0 源码分析](../p0-operator-analysis.md)。

## 决策

### 小型值与集合使用版本化 Record

新增无第三方依赖的 `std-value` 包。`std.Value.v1` 保存 `{kind, value, unit}`，kind 为 number、boolean、string 或 record；record 值保存 `{type,data}`。`std.List.v1` 保存 `{kind,items,unit}`，元素同类，记录元素还须具有相同 type。数值必须有限，布尔不通过数值真值转换。空列表合法，null、损坏类型和越界提取产生诊断。

单个载荷最多 1 MiB、4096 项、嵌套深度 16，字符串最多 64 KiB。文本参数在解析前检查大小和深度；进入消费者的 Record 也必须经过校验。PointCloud、Image、Tensor、Mesh 及大数组不进入这些 JSON 容器。数值采用 float64 语义，JSON 整数限定在可连续精确表达的 `[-2^53,2^53]`；更大的业务标识使用字符串，不能静默损失整数精度。

Measurement 通过显式节点转成数值，要求有效、有限且保留原单位。记录字段通过 JSON Pointer 提取；数值字段的单位由用户明确指定，不能从字段名猜测。相同单位才允许加减/比较；乘除的单位规则由算子公开声明，不进行隐式单位换算。`flow.select(path=value)` 直接消费布尔值记录；普通逻辑节点正常求值，惰性依赖仍由 flow.select 负责。宿主信息通过顶层图参数按次绑定/覆盖常量；现有公开 C ABI 的点云/图像注入接口不被描述为任意 Record/Plane 注入。

列表的合并、拼接、取项、子集和排序分别定义。不隐式去重、扁平化或使用负索引；稳定排序同时返回原始索引。记录中的业务身份原样保留，列表位置不是全局实例身份。

### 几何保持原生类型

新增无 PCL 依赖的 `std-geometry` 包。复用原生 Point2D、Line2D、Circle2D 和 Plane；显式区分米与像素。几何坐标的单位和 Measurement 的显示单位分别处理。直线与线段、无向最小距离与有符号间隙、两点中点与点线垂足均为显式契约。

交点使用 `geometry.Intersections2D.v1`：`{unit,relation,points}`，关系区分有限解、无解和重合；重合不会生成虚构单点。通过显式提取节点将有限解转回原生 Point2D。平面满足 `n·p+d=0`，归一化法向时同时缩放 d，拒绝零法向。

P0 原生二维类型只有单位字段，没有 frame ID。输入处于同一参考帧是调用前提；本次不声称能自动发现跨帧错误。相机、工件和动态云集合的 frame/身份及新核心类型仍须在后续 ADR 中定义。已知变换显式执行，不通过命名或界面显示推断坐标关系。

### 图像使用现有缓冲与小型诊断结果

继续采用 Image 的灰度/RGB/RGBA、u8/u16/f32 和只读输入约定。OpenCV 仅位于 std-image 中。新增掩膜、统计、连通域、差分及量测复用原生 Image、Line2D、Measurement；逐域/逐扫描诊断使用有界、版本化 Record，不能将整幅图像或无界轮廓序列塞入 JSON。

无目标区域可以返回有效空集合。空统计通过 valid=false 与空统计值表示；提取无效数值必须失败，不能用 0 伪造测量成功。直线检测失败不构造伪造 Line2D；宽度无有效样本输出无效 Measurement。参数非法与正常无目标分别诊断。

图像领域 Record 声明坐标是原图像素空间；预览缩放中的原生像素参数、Line2D 和 Measurement 继续由现有执行器规则处理，不重复缩放。领域 Record 的几何由生产者显式换回原图坐标，并记录 scale。

### ABI 与交付

不新增 Data::Kind，不改 C 结构、枚举布局或 C ABI 符号。Record 走现有结果、Transport、C++/Rust 与 Python Record 通道，原生几何走现有载荷。新增 value.h 为纯 inline 帮助代码，包含在现有 SDK 头文件安装规则中。

产品版本升至 1.3.4，保持 1.3 系列 C ABI 兼容。正式包清单加入 std-value、std-geometry 和必需 P0 算子；正式源码身份检查与包清单同步。P0 算子在 Windows 原生门禁和独立图像组合门禁中验证，不依赖手动发布任务才运行测试。

## 后续边界

相机内参/畸变、Pose2D/Pose3D、跨帧推断、动态云集合和通用表达式不在本次实现范围。不得将 Record 的存在或此次基础链交付记为这些算法已实现。外部库未公开的依赖内部算法无法直接核验时，记录边界，并以可复核真值验收 LyFlow 自己的实现。
