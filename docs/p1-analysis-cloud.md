# P1 点云与单剖面源码分析

[补齐计划](operator-completion-plan.md) · [能力对照](operator-coverage.md)

本批参考用户本机 `xyz-vision-lib/src/xyz_vision_lib/models` 的以下实现及头文件，独立实现 LyFlow 算法与契约。该目录没有 Git 元数据，不能据此声明清单指定修订或原工件/Request 协议兼容。外部源码包含 proprietary 标识，没有复制其函数体。

| 外部条目 / 源码 | 核实的行为与风险 | LyFlow 的实现选择 |
|---|---|---|
| CloudPCA / `3D_Processing/CloudPCA.cpp` | 先下采样、按质心距离截取，再估计 PCA；截取使用严格小于阈值可能少于期望覆盖；轴有正负和重复特征值歧义。 | `cloud.pca` 使用全体有效输入的 double 中心化协方差；轴符号规范化、右手系，输出特征值与 `orientationStable`，不把退化主轴当作稳定工件朝向。离群滤波作为显式上游节点。 |
| BoxEstimator / `3D_Processing/BoxEstimator.cpp` | 实际为主平面表面最小面积矩形，带边缘栅格去噪、法向角约束和可选表面云，不是一般三维 OBB。 | `cloud.statistics` 输出 AABB；`cloud.bounds_obb` 提供 PCA 方向包围盒，尺寸/中心/box→frame 变换可核对。保留平面箱面专用规则为未覆盖，不将 PCA 包围盒称为最小体积盒。 |
| GetCloudHeight / `3D_Processing/GetCloudHeight.cpp` | 既有全云最大 Z，也有 Request 点附近圆/方 ROI 与 workspace 相关安全高度；结果使用字符串输出。 | `cloud.statistics.height` 为指定输入帧的最大 Z、米制 Measurement；空云 `ok=false`。相对高度、安全高度与 workspace ROI 属后续统计规则。 |
| GetCloudClusters / `3D_Processing/GetCloudClusters.cpp` | 欧式和法线条件聚类；按簇大小排序并静默截去超过数量上限的簇，输出复制后的 CloudVector。 | `segment.euclidean_clusters` 使用真正 XYZ 欧式连通分量，点数范围显式，超过 `maxGroups` 报错；输出共享来源云、原生成员索引及分组身份。法线条件模式仍独立列为未覆盖。 |
| FilterByClustering / `3D_Processing/FilterByClustering.cpp` | 包含 organized 云像素邻接与 XY/Z 阈值的 DFS，以及按大小滤簇；不是只有普通三维欧式聚类。 | 同一欧式节点做显式大小范围筛选；organized 邻接语义没有伪装为已支持。 |
| FindCloudPlane / `3D_Processing/FindCloudPlane.cpp` | 支持逐云单平面/迭代多平面，输出平面云、剩余云、系数和中心工件姿态；organized 模式需要恢复原索引。 | `segment.multi_planes` 顺序 RANSAC，始终恢复到原始云索引，输出规范化平面系数、质心和组 ID；剩余云通过原生 `select` 保留通道。最多平面数是显式提取上限，余点保持可见。 |
| BestFit / `3D_Processing/BestFit.cpp` | 将几何向量按相邻两项配对，提供 SVD 和加权 3-2-1 专用解；默认直接访问 `i+1`，需要调用方保证偶数及有效约束。 | `register.rigid_fit` 使用等长 PointCloud 的显式点序对应；double 中心化 SVD，修正反射，拒绝共线/重合和质量超限。加权点线面 3-2-1 规则不在基础切片。 |
| CloudICP / `3D_Processing/CloudICP.cpp` | 输入工件初姿与 CloudVector，通过外部 PointMatcher 执行 2D/3D ICP，再按匹配比例筛工件；帧不匹配仅日志警告。 | `register.icp_3d` 显式 source→target 初值、源/目标帧、迭代与质量门限；点到点 Kabsch 与目标法线点到面两种模式。点面 Hessian 必须六自由度可观测；不收敛、匹配不足和 RMSE 超限拒绝，不把矩阵存在视为成功。 |
| SearchPoseLocally / `3D_Processing/SearchPoseLocally.cpp` | 按指定轴平移/旋转离散搜索及逐级搜索，用最近点命中比例评分，保留最优候选。 | `register.local_search_3d` 在显式初值附近测试至多 9 个候选：原始、三轴正负平移、Z 正负旋转；每个候选运行同一 ICP 和质量门控，匹配比例达标后按 RMSE 选择。不是全局配准或全部轴组合网格搜索。 |
| CloudLoader / `Input/CloudLoader.cpp` | PCD/PLY/STL；STL 调外部网格 Monte Carlo；支持目录按工件名称查找，错误 JSON 可继续复用上一次路径，并缓存加载云。 | `io.load_cloud` 单文件路径明确，PCD/点顶点 PLY 和 ASCII/小端二进制 STL；预检文件/点数/字段边界，拒绝不支持通道与 alpha。STL 按面积、固定种子生成点和几何法线。显式 m/mm 后应用文件→输出帧刚体初姿；ASCII 字段按声明的整数有符号性/字节宽度/小数语义及浮点范围预检，拒绝 PCD 头部字段数量和 POINTS/尺寸不一致；不复用旧路径、不持有 Mesh 拓扑。 |
| CloudVectorToCloud / `3D_Processing/CloudVectorToCloud.cpp` | 从第一云继承元数据后拼接，空集合报错，末尾剔除 NaN。 | `cloud.group_merge` 按原生索引顺序平坦化，保留重复点和全部通道；合法空集合输出空云。云集合视图的 frame 和来源严格校验。 |
| ProfileProjector / `Input/ProfileProjector.cpp` | 使用主/副云 pose 元数据和可选主云质心作为旋转中心，更新输出 pose；自动中心写回参数。 | `profile.project_pair` 必须输入 master→common、slave→common 外参，可再输入 common→output；无内置云 pose，不猜元数据，不反写配方。由调用者明确构造绕中心变换，旋转法线并保留其他通道。 |
| MonoGapFlushDetection / `3D_Processing/MonoGapFlushDetection.cpp` | 角度分簇、去噪、左右类型和参考类型组合，包含线/点/圆等间隙面差定义及选择点/投影规则。 | `profile.gap_flush` 做基础单扫描双表面切片：两个显式不相交横坐标区间拟合，横向采样边缘距离为 gap，同一 referenceX 处右减左为 flush；正交 RMS、竖直线、点数及外推门限均检查。圆角/槽和专用斜投影缝型继续使用专用算子。 |
| SignalEvaluator / `2D_Processing/SignalEvaluator.cpp` | XZ 信号转换后调 SinglePoint/Line/Circle、两点/线/圆及混合分析器；ROI 和边缘/曲率等参数按界面毫米转米。 | `profile.analyze` 提供明确 XY/XZ/YZ 米制单剖面正交直线和统计，不把一条线的分析宣称为全部信号特征。原生 Line2D 收窄后重新计算正交 RMS，并以实际输出直线计算面差；不能交付的 float32 精度按门限拒绝。二维点/线/圆组合继续复用现有通用几何算子。 |

## 云集合的数据与索引所有权

`cloud.Groups.v1` 只保存 `sourceCloudId`（字符串）、`pointCount`、`frame`、`unit=m` 与最多 4096 项的 `{id,offset,count}`。索引值存放在原生 Indices，点与通道存放在原生 PointCloud。每个消费者核对两个来源 ID、点数、连续区间、唯一组 ID、每个索引范围及有界元数据；不把点/索引数组序列化进 JSON。

`cloud.group_pack` 包装单云；`cloud.group_get` 输出新云与仍指向来源云的原始索引；`cloud.group_subset` 保留选择顺序和组 ID，拒绝重复身份；`cloud.group_concat` 拼接两个显式同帧云或已有集合，可选 `groupsA/indicesA` 与 `groupsB/indicesB` 必须成对，组 ID 分别加 `a/`、`b/` 命名空间，索引重绑至拼接云。非空云通道集合不一致时报错；`cloud.group_merge` 平坦化；`cloud.group_statistics` 输出有界逐组统计。聚类 ID 只表示本次结果的规范顺序，不承诺跨帧跟踪；质心使用规范 XYZ 点序求和，同质心同点数时继续比较完整 XYZ 字典序，避免输入簇/点顺序决定身份。

ICP 的 PCL 近邻查询使用 float 平方距离。目标跨度和参与搜索的查询点相对目标首点范围限制为 `sqrt(FLT_MAX)/4`，避免平方距离溢出；超范围目标明确拒绝，远处查询点不形成对应。最终配准门控、候选选择和质量报告以交付的 float32 Transform 按原生 `transform.apply` 坐标运算得到的 aligned 云重新求对应及 double 残差；不使用尚未收窄的 double 解伪报精度。大坐标抵消造成的实际输出误差超限会拒绝，aligned 与复用 Transform 的 XYZ 一致。该限制不影响米级工业场景，也不承诺任意 float32 极端坐标的 ICP 捕获能力。

## 验证入口及边界

`packs/std-pointcloud/tests/test_p1_cloud.cpp` 使用自制确定性点云、已知 SE(3) 真值、正交三面、两个 Z 分离簇、两平面、带通道云、单扫描与生成文件。验证正反变换、真实点面可观测性、退化拒绝、分组身份、严格文件限制及不修改输入。

这些软件契约与合成真值不代表现场测量精度、传感器质量、捕获范围、生产节拍或长期稳定性验收。默认点云单位为米；帧字符串是调用者明确声明，原生 PointCloud 没有自动携带的坐标帧证明。
