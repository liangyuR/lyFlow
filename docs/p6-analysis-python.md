# 阶段 6：Python 端口与可移植示例

本轮 C02 在现有独立常驻 Python worker 上扩展数据适配，保留解释器选择、任务串行、维护窗口、手动刷新、异常诊断、超时和取消恢复机制。未引入嵌入 Python/GIL，也不执行外部 `VisionPythonCustom` 协议。以下依据本机 xyz-vision-lib 的实现语义独立实现，没有复制其源码。

ctx 继续提供 progress、log、check_cancelled、base_dir 和 thread_budget。单次默认 60 秒，取消先协作、500 ms 后未退出即结束 worker 进程树；下一次任务重新启动，失败任务不重试。Python 节点仍禁用结果缓存，原生上游缓存不受影响。模块初始化可在该 worker 生命周期内复用，手动刷新重新加载代码和依赖。

## 分析来源与取舍

核对 `CustomPythonScript.cpp/.hpp`、`CustomPythonTemplates/CustomPython.py` 和测试模板、`property_settings.yml` 的 `pythonscript` 段，以及 CommonScripts 中位姿偏移、比较、筛选、序列化、构造、刚体校验、录制和业务脚本。

旧实现把脚本路径加入模块搜索路径，以模块名导入 `VisionPythonCustom`，通过端口/参数位置列表调用，依赖 XYZ Python 包和 protobuf，部分脚本继续依赖 PyYAML、SciPy、业务文件及环境。直接复制会把模块重名、隐含单位、第三方依赖、文件副作用和不透明信息字段一并带入。LyFlow 按声明的端口名绑定输入输出，使用所选解释器；依赖缺失按包报告，合法包仍可发现。没有输入时遵守端口 required 声明，不制造默认数据。

旧输入有 Double/Int/Bool/String/DoubleVector、Primitive2DVector/Primitive3DVector、Geometry2D/Geometry3D/Geometry3DVector、Image、Cloud。旧输出还受模板列表和转换代码约束，不能把任意 Python 对象当作原生数据。LyFlow 对应关系如下。

| 外部语义 | LyFlow Python 适配 | 边界 |
| --- | --- | --- |
| Double / Int / Bool / String | `scalar()`→`std.Value.v1` Record | number/boolean/string；整数安全范围 ±2^53，超范围 ID 用字符串 |
| DoubleVector | `value_list()`→`std.List.v1` Record | 同类小列表，number 单位显式；大矩阵用 Tensor |
| Primitive2DVector | `pose2d()` / `poses(...,2)` | `vision.Pose2D/Poses2D.v1`；frame、px/m、ID、尺寸、弧度及合法属性保留 |
| Primitive3DVector | `pose3d()` / `poses(...,3)` | `vision.Pose3D/Poses3D.v1`；米、xyzw 单位四元数，保留身份与抓取标记 |
| Geometry2D | Point2D / Line2D / Circle2D / Box2D | 原生类型直接连线；px/m 不隐式互转 |
| Geometry3D / Geometry3DVector | Plane 直接端口；其它 `geometry3d()`→`geometry.Features3D.v1` Record | 点/线/圆/面/球保持已有契约，不新增第二套三维原生类型；集合允许原生投影的 sourceFrame 元数据 |
| 矩阵变换 | Transform | row-major 4×4 有限 F32；适配层不擅自要求刚体矩阵，具体算法自验 |
| Cloud | PointCloud | XYZ F32 Nx3；可选 intensity F32 N、normals F32 Nx3、RGB U8 Nx3，复制保留所有通道 |
| Image | Image | H×W 或 H×W×1/3/4，U8/U16/F32；RGB/RGBA 顺序，保留 scale，不隐式 BGR 或位深转换 |
| 推理矩阵 | Tensor | F32 C-order，最多 8 维、非负 int32 维度、正 int32 scale；空 shape/零轴张量兼容原生语义 |
| 标量测量结果 | Measurement | value/ok/unit/message/verdict 与完整上下限往返 |

直接端口共 **11 种**：PointCloud、Image、Measurement、Record、Transform、Plane、Point2D、Line2D、Circle2D、Box2D、Tensor。Indices/Bundle 不在本轮 Python 直接端口内；外部 CustomPythonScript 没有对应要求，不冒称支持。原生帧/录制体系中的 Indices 源云 ID 处理由 D01 负责，不能拿 Python Record 代替带来源约束的原生索引。

## 数据协议与验证

SDK/worker/执行器使用 **dataProtocolVersion=2**：发现和执行请求、成功响应都必须匹配；缺少、错误版本、字符串或布尔版本均拒绝。发现错版不发布 Python 节点；执行响应错版终止 worker，下一个任务重新启动，失败任务不自动重试。旧装饰器节点源码可继续使用，SDK、worker 和原生运行库需一起更新，不能混用旧 worker。

点云、图像、Tensor 的数组仍走每请求临时二进制文件和 NumPy mmap；descriptor 只有 file/shape/dtype。数组最多 1 GiB，文件必须位于本请求目录且字节数精确匹配。F32/U8/U16 明确，小端线序。输入映射只读，修改前调用 `copy()`；Record 输入深冻结。输出先全部验证并复制为引擎拥有的数据，再发布端口，非法输出不留下部分结果；正常、异常、取消和超时沿用既有清理。

几何坐标必须有限且可表示为 F32，法向/方向归一化，圆半径正，Box 边界有序，线段端点必须成对且不同。Tensor 保留 F32 值，不把含非有限值的推理数据误当几何。Image/Tensor scale 为正 int32，表示原生预览/尺寸元数据，不能当作坐标单位换算。

Record 沿用原生标准值/位姿模型及严格版本，不创建重复类型。小 JSON 上限 1 MiB、深度 16、单数组/对象 4096 项、单字符串 64 KiB，数字有限；标准 Value/List 不接受 null，通用 Record/位姿 attributes 可接受 null。位姿集合禁止重复 ID、混帧、混单位。已知标准记录未知版本报错，通用自定义 recordType 仍可使用。Pose helper 规范角度与四元数符号；输入适配不把错误四元数自动修正成有效结果。

## 可运行示例与 16 脚本对应关系

`python/examples/p6_adapters` 为可直接导入的包，依赖仅 NumPy：

- `offset_poses3d`：真实执行局部/父帧位置偏移，向量参数单位 m，四元数旋转局部偏移；保留集合所有身份和属性，记录 ctx 日志并逐项取消检查。
- `filter_pickable`：仅接受显式 `pickable=true`，缺字段不推断可抓，不重算可抓分数。
- `poses_to_json` / `poses_from_json`：标准 2D/3D 集合类型化 JSON 往返；保留 frame/unit/ID/合法元数据，拒绝错误版本和非法负载。
- `tensor_affine`：F32 张量分块 gain*x+bias，保留 shape/scale 并协作取消。

示例 README 给出发现及连线步骤；Python 测试通过完整 worker 请求实际发现/运行全部五个节点。原点云偏移/图像统计示例仍随 SDK 交付。示例不创建文件，日志通过 ctx；打印日志与计算输出、文件归档分开处理。

| 原 16 脚本项 | 交付定位 | 不可混淆的语义 |
| --- | --- | --- |
| LogString | D01 原生日志/录制能力 | 带时间文件日志有文件副作用；ctx.log 仅运行日志，不等价于文件归档 |
| LogPrimitive3D | B07/D01 原生位姿日志 | 保留时间、文件模式与取消语义，见阶段 6 交付分析 |
| LogPrimitive2D | B07/D01 原生位姿日志 | 同上，px/m 不丢失 |
| Primitive3DtoString | 本轮 JSON 示例 + 已有 pose.to_text | 采用版本化标准 JSON，不宣称兼容旧自定义分隔符字符串或 protobuf 文本 |
| Primitive2DtoString | 本轮 JSON 示例 + 已有 pose.to_text | 同上，米/像素、弧度及身份显式保存 |
| StringtoPrimitive3D | 本轮 JSON 示例 + 已有 pose.from_text | 只接受本契约 JSON；旧裸数值字符串需显式转换 |
| Compare2Primitive3D | P5 原生位姿比较 | 比较阈值/旋转定义与单位由该算子描述；不把 Record 容器说成比较算法 |
| Compare2Primitive2D | P1 原生 pose.compare_2d | 保留对称角、位置/尺寸和匹配规则；不在 Python 重复一套阈值 |
| CheckPickablePrimtives2D | 原生可抓匹配能力，另有筛选示例 | 原脚本名为 IoU 实际使用交集/min(area)，忽略旋转；本轮筛选仅消费已确认布尔标记，不能替代可抓推断 |
| ShrinkBoxDimension | 保留 P3 业务迁移项 | 纸箱/碰撞/安全裕量依赖业务，未以通用 Tensor/Box 类型标为完成 |
| RecordReplayer | D01 原生帧录制/回放 | 外部录制格式兼容边界见交付分析，不把任意 JSON 解析冒称旧录制兼容 |
| MappingBoxDimension | 保留 P3 SKU 业务迁移项 | 尺寸表、匹配业务和坐标约定尚需业务输入，未迁移 |
| MakeOffset | 本轮偏移示例 + 已有 pose.offset_3d | 原脚本动态值+参数后 mm→m；示例直接 m，局部/父帧显式，不修改上游 |
| ConstructPrimitive3D | P1 原生 pose.construct_3d + SDK helper | 原生支持显式 m/rad、XYZ 欧拉与规范 xyzw；不自动识别旧隐含单位 |
| MoveDebugImage | 保留 P3 业务归档项 | 整理已有 debug 目录有移动文件副作用，未偷偷加入可移植计算示例 |
| RigidDistanceCheck | P5 原生刚体一致性 | 注册模板/文件、点对距离与刚体拟合各模式在 P5 记录；Python 类型适配不等于新增这些算法 |

此外，CommonScripts 的 `xvl_filter_by_pickable_info.py` 并非清单 16 项之一，本轮提供其结构化筛选语义；旧脚本从 info 字符串读 JSON，示例消费经过标准契约校验的 pickable 字段。

## 验证

Python：`python -B -m unittest discover -s python/tests -v`，14 组，全部通过、无跳过。覆盖直接类型的 mmap 往返、只读/副本、各图像位深/通道、scale、零轴/空/8维 Tensor、非法维度/预算/形状/单位、标准值/位姿/三维几何、旧示例、新包真实执行、中英关键词、请求版本负例、取消、异常 traceback 和映射清理。

原生：`core/tests/test_python.cpp` 保留原 3 组执行契约，新增 1 组真实命名管道几何/Tensor/标准位姿往返，检验空 Tensor、单位、scale、上游未修改、非法 dtype、执行/发现版本错配和下一任务恢复。原有图像矩阵增加 scale 断言；原异常、无限等待取消、超时、崩溃、导入失败、版本变化/刷新恢复回归保留。新增组与已有组一起由集成构建运行。

独立 MSVC C++17 `/MD /O2` 对 `python_runtime.cpp`、`test_python.cpp` 对象编译通过（不触碰共享 build/core/PCH）。完整 native 链接运行结果由阶段 6 总体验证记录，不将对象编译写成运行验收。
