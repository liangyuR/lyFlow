# 阶段 6：三维模型的真实栅格输出

## 原实现核对

核对本机 xyz-vision-lib 的 `VisualizePrimitives3D.cpp/.hpp` 与 `property_settings.yml`：其输入为图像与三维工件，使用帧树转换到相机；STL 分支通过 CPU renderer 生成 mask、depth、normal，再画入展示图。配置候选还包含 edge_mask，但所读实现的分支只有 mask/depth/normal，不能据下拉框名称宣称旧分支已实现边缘模式。点云模板与 STL 表面渲染、真实数值深度与伪彩叠图须分别说明。

LyFlow 的 `image.render_model_3d` 原已处理居中模型点云、Pose3D 集合、显式场景到相机刚体变换、内参/畸变、scale 及最近 Z 可见性，但仅输出彩色叠图。此次在该渲染器上独立实现真实栅格输出，复用原投影和遮挡计算，没有复制外部代码或引入其 renderer。

## 输出契约

| 端口 | 原生格式 | 数值与有效性 |
| --- | --- | --- |
| image | 由 renderMode 选择 | 默认 overlay 保持原输入位深、通道、背景与 alpha；其它模式与同名真实输出一致 |
| mask | U8 单通道 | 最近可见模型采样点占用像素为 255，其余为 0 |
| edge_mask | U8 单通道 | 命中像素的四邻域有背景、图像外侧或另一可见实例即为 255；内部像素为 0 |
| depth | F32 单通道 | 经过模型 Pose 和 sceneToCamera 后的相机 Z，单位 m；无命中为 0，以 mask 判有效。不是欧氏视距、归一化深度或彩色亮度 |
| normal | 可选 F32 三通道 | 模型点法线依次经模型 Pose 与 sceneToCamera 的旋转，归一化为相机坐标 XYZ；有符号分量范围 [-1,1]，背景为 (0,0,0)，以 mask 判有效 |
| quality | image.ModelProjection.v1 Record | 保存相机 frame、完整 imageSize、scale、实例/命中像素数、renderMode、normalAvailable、各输出 encoding |

renderMode 有 overlay（默认）、mask、edge_mask、depth、normal。独立输出始终保留，normal 仅在模型每点都有有限非零法线时产生；模型没法线或包含零法线时，普通模式成功但 `normalAvailable=false`、normal 端口缺省，选择 normal 模式返回定位到 model 端口的明确错误。不从颜色或模板形状猜测法线，也不把零向量说成可用法线。

`quality.outputs.depth.encoding=camera-z-meters`、`quality.outputs.normal.encoding=camera-unit-vector-xyz`；主 image 选择深度/法线时也携带对应 encoding。Viewer 应按编码把法线 [-1,1] 映射到展示 RGB，深度可用数值范围或色带显示；底层原生值不因显示转换改变。各图像输出与输入共享 scale，像素网格继续对应完整标定图的工作尺度。

最近 Z 同时决定实例所有者、深度、法线和 mask，避免远处实例法线覆盖近处像素。相同 Z 保留先出现的采样点/实例，行为确定。mask 是可见点的像素覆盖，不填充点云没有覆盖的三角面；edge_mask 是该覆盖及可见实例交界，不是未提供 Mesh 的折线/锐边恢复，也不是对彩色叠图做 Canny。

点云、实例、像素预算及逐块/逐行取消沿用现有限制，输出检查完才统一发布。所有计算在独立结果上进行，不修改输入图像、模型坐标或法线。normal 可选端口缺省时，当前缓存的全声明端口复用检查可能导致重算；结果正确性不受影响，本次未修改共享缓存框架。

## 验证

`packs/std-image/tests/test_p2_projection.cpp` 从原 4 组扩展到 6 组。独立 OpenCV **5.1.0**、MSVC C++17 Release probe 完整编译并运行该文件：**6 组、306 条断言全部通过，0 跳过**。没有构建或修改共享 build/core。

覆盖原 ROI/投影、内参畸变、跨帧变换、U8/U16/F32 与 alpha 回归；新验证二值 mask、稠密内部不误报边界、相邻实例交界、最近层遮挡、离轴点仍为真实 Z=2m、法线归一化与负分量、Pose 与相机两次旋转、缺法线/零法线诊断、scale=2、一致的模式输出、取消无部分结果和未知模式拒绝。浏览器的真实输出选择与法线色彩映射由 C03 单独验证。
