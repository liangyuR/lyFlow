# python-examples 逐算子迁移清单

状态：实施中；当前 8 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 257 · py.example.image_metrics

- 当前：`python-examples` / `1.0.0` / Python 图像亮度。目标：`python-sdk-examples`；保留并验边界；**M7**。
- 源码：[python/examples/image_metrics/nodes.py:6](../../python/examples/image_metrics/nodes.py#L6)。
- 端口：`image:Image` → `image:Image`, `mean:Measurement`, `info:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Python >=3.11、NumPy、lyflow_python；用户选择的解释器环境，不属于原生包依赖
- 当前契约：Image像素转float32后乘gain，输出Image、level单位均值Measurement及shape/gain Record；不自动归一化到0..1
- 目标契约：Image像素转float32后乘gain，输出Image、level单位均值Measurement及shape/gain Record；不自动归一化到0..1；同步公开Record/Profile契约适配，示例不替代标准原生能力
- 迁移前置：M1；M2；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同一示例在worker/CLI/桌面/嵌入宿主一致；输入只读、取消与非法输出回归
- 说明：仓库随附示例，导入后才注册；不计入251个原生算子，也不代表用户环境已配置

## 258 · py.example.offset

- 当前：`python-examples` / `1.0.0` / Python 点云平移。目标：`python-sdk-examples`；保留并验边界；**M7**。
- 源码：[python/examples/offset.py:6](../../python/examples/offset.py#L6)。
- 端口：`cloud:PointCloud` → `cloud:PointCloud`。
- 参数单位标注：offset=m。
- 依赖：Python >=3.11、NumPy、lyflow_python；用户选择的解释器环境，不属于原生包依赖
- 当前契约：只读PointCloud复制后修改XYZ第3轴；offset为m，保留可选通道
- 目标契约：只读PointCloud复制后修改XYZ第3轴；offset为m，保留可选通道；同步公开Record/Profile契约适配，示例不替代标准原生能力
- 迁移前置：M1；M2；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同一示例在worker/CLI/桌面/嵌入宿主一致；输入只读、取消与非法输出回归
- 说明：仓库随附示例，导入后才注册；不计入251个原生算子，也不代表用户环境已配置

## 259 · py.p6_adapters.filter_pickable

- 当前：`python-examples` / `1.0.0` / 可抓工件筛选。目标：`python-sdk-examples`；保留并验边界；**M7**。
- 源码：[python/examples/p6_adapters/__init__.py:37](../../python/examples/p6_adapters/__init__.py#L37)。
- 端口：`poses:Record` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Python >=3.11、NumPy、lyflow_python；用户选择的解释器环境，不属于原生包依赖
- 当前契约：仅筛选显式pickable=true项；保持集合frame/unit，不能推断或授予抓取许可
- 目标契约：仅筛选显式pickable=true项；保持集合frame/unit，不能推断或授予抓取许可；同步公开Record/Profile契约适配，示例不替代标准原生能力
- 迁移前置：M1；M2；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同一示例在worker/CLI/桌面/嵌入宿主一致；输入只读、取消与非法输出回归
- 说明：仓库随附示例，导入后才注册；不计入251个原生算子，也不代表用户环境已配置

## 260 · py.p6_adapters.offset_poses3d

- 当前：`python-examples` / `1.0.0` / 三维工件位姿偏移。目标：`python-sdk-examples`；保留并验边界；**M7**。
- 源码：[python/examples/p6_adapters/__init__.py:16](../../python/examples/p6_adapters/__init__.py#L16)。
- 端口：`poses:Record` → `poses:Record`。
- 参数单位标注：offset=m。
- 依赖：Python >=3.11、NumPy、lyflow_python；用户选择的解释器环境，不属于原生包依赖
- 当前契约：vision.Poses3D.v1集合，m位移，local模式先按xyzw旋转；保留身份/frame/size
- 目标契约：vision.Poses3D.v1集合，m位移，local模式先按xyzw旋转；保留身份/frame/size；同步公开Record/Profile契约适配，示例不替代标准原生能力
- 迁移前置：M1；M2；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同一示例在worker/CLI/桌面/嵌入宿主一致；输入只读、取消与非法输出回归
- 说明：仓库随附示例，导入后才注册；不计入251个原生算子，也不代表用户环境已配置

## 261 · py.p6_adapters.poses_from_json

- 当前：`python-examples` / `1.0.0` / 工件位姿 JSON 解码。目标：`python-sdk-examples`；保留并验边界；**M7**。
- 源码：[python/examples/p6_adapters/__init__.py:62](../../python/examples/p6_adapters/__init__.py#L62)。
- 端口：`text:Record` → `poses:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Python >=3.11、NumPy、lyflow_python；用户选择的解释器环境，不属于原生包依赖
- 当前契约：显式type/data封套解码；只接受标准2D/3D工件集合
- 目标契约：显式type/data封套解码；只接受标准2D/3D工件集合；同步公开Record/Profile契约适配，示例不替代标准原生能力
- 迁移前置：M1；M2；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同一示例在worker/CLI/桌面/嵌入宿主一致；输入只读、取消与非法输出回归
- 说明：仓库随附示例，导入后才注册；不计入251个原生算子，也不代表用户环境已配置

## 262 · py.p6_adapters.poses_to_json

- 当前：`python-examples` / `1.0.0` / 工件位姿 JSON 编码。目标：`python-sdk-examples`；保留并验边界；**M7**。
- 源码：[python/examples/p6_adapters/__init__.py:51](../../python/examples/p6_adapters/__init__.py#L51)。
- 端口：`poses:Record` → `text:Record`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Python >=3.11、NumPy、lyflow_python；用户选择的解释器环境，不属于原生包依赖
- 当前契约：保存完整工件type/data的std.Value字符串，拒绝非有限JSON
- 目标契约：保存完整工件type/data的std.Value字符串，拒绝非有限JSON；同步公开Record/Profile契约适配，示例不替代标准原生能力
- 迁移前置：M1；M2；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同一示例在worker/CLI/桌面/嵌入宿主一致；输入只读、取消与非法输出回归
- 说明：仓库随附示例，导入后才注册；不计入251个原生算子，也不代表用户环境已配置

## 263 · py.p6_adapters.tensor_affine

- 当前：`python-examples` / `1.0.0` / 张量线性变换。目标：`python-sdk-examples`；保留并验边界；**M7**。
- 源码：[python/examples/p6_adapters/__init__.py:78](../../python/examples/p6_adapters/__init__.py#L78)。
- 端口：`tensor:Tensor` → `tensor:Tensor`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：Python >=3.11、NumPy、lyflow_python；用户选择的解释器环境，不属于原生包依赖
- 当前契约：Tensor复制后按gain/bias分块计算，形状不变，块间检查取消
- 目标契约：Tensor复制后按gain/bias分块计算，形状不变，块间检查取消；同步公开Record/Profile契约适配，示例不替代标准原生能力
- 迁移前置：M1；M2；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：同一示例在worker/CLI/桌面/嵌入宿主一致；输入只读、取消与非法输出回归
- 说明：仓库随附示例，导入后才注册；不计入251个原生算子，也不代表用户环境已配置

## 264 · py.profile_copy.offset_z

- 当前：`python-examples` / `1.0.0` / Python 轮廓有效采样偏移。目标：`python-sdk-examples`；保留并验边界；**M7**。
- 源码：[python/examples/profile_copy.py:6](../../python/examples/profile_copy.py#L6)。
- 端口：`profile:Profile` → `profile:Profile`。
- 参数单位标注：offset_m=m。
- 依赖：Python>=3.11、NumPy、lyflow_python；用户解释器环境，无原生重后端
- 当前契约：输入Profile.copy后仅平移validity=1槽Z；offset_m按米声明并对mm载荷显式乘1000；保留NaN/有限无效槽、索引、描述及可选通道。
- 目标契约：展示公共Profile契约与只读复制，不实现M3场景检测算法；不把大数组塞Record。
- 迁移前置：M1；M6
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：1280槽且2个无效槽保留、原始输入不变；真实worker往返及单位换算、readonly、取消回归。
- 说明：导入后才注册，不计入固定原生全集；独立main自检不是现场算法验收。
