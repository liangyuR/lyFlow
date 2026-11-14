# P2 B02：三维模板检索、边缘与可见面采样

本阶段逐个阅读本机 `xyz-vision-lib` 的 SurfaceBasedEstimator、SurfaceBasedEstimatorV2、CloudEdgeExtract、CloudViewSampling 的 cpp/hpp 与 property_settings.yml，只参考语义，不复制专有函数体。外部目录未提供可核验的 Git 修订信息，不声明来源 revision。

## 原节点的完整意图和风险

Surface/V2 的核心算法位于未提供的 xyz_matching3d 中；包装层提供 STL 建模、保存/加载、采样、全局候选、稀疏/密集 ICP、对称聚类、多实例、2D 分割约束及渲染。V2 把密度、速度、精度档位映射为实际半径、步长和门限，不能只复制档位名称。包装层只检查首项帧；空 clouds 配图像可能访问 clouds[0]；仅路径缓存会漏文件内容变化；空二维检测靠 dummy ROI 防止下层全图 fallback。LyFlow 使用显式原生 ROI 索引，空索引直接得到空结果。

CloudEdgeExtract 只有 organized 实现；无组织输入静默空输出。边缘包括无效深度边界、遮挡近侧、遮挡远侧及高曲率。LyFlow 的原生 PointCloud 没有图像宽高，不能猜测 organized 布局；organized 的 NaN/Inf XYZ 形成无效像素，可显式用 validIndices 选择有限点；unorganized 仍要求所有 XYZ 有限。

CloudViewSampling 从外部预采样视库按位姿查询且未检查 Query 状态。LyFlow 使用明确分辨率的角度深度缓冲、近裁面及可选法线背面剔除；这是离散点云可见性近似，不是连续三角网格可见性保证。

## CPU 后端选择与独立适配

用户选定 Windows CPU 主线为 KISS-Matcher + small_gicp；早期三角不变量草稿已移到忽略的 build/drafts，不编译、不注册为默认算法。FoundationPose 是独立的可选 GPU 路线，不属于本文件的 CPU 验证结果。

核验并固定的上游：

| 来源 | 固定修订 | 实际使用范围 |
|---|---|---|
| [KISS-Matcher](https://github.com/MIT-SPARK/KISS-Matcher/tree/8b58b118e00c7a5a22448741f8d956d638a44ae2) | 8b58b118e00c7a5a22448741f8d956d638a44ae2 | FasterPFH、互检对应、max-core 兼容图剔除、完整三轴 GNC |
| [ROBIN](https://github.com/MIT-SPARK/ROBIN/tree/5623f02f62c63711052bbb8d6096a410ddca0ee4) | 5623f02f62c63711052bbb8d6096a410ddca0ee4 | BSD-2-Clause 图构造及默认 BZ_SERIAL k-core |
| [small_gicp](https://github.com/koide3/small_gicp/tree/fa0cfc983417c044b6036f71dfa448038bb9c820) | fa0cfc983417c044b6036f71dfa448038bb9c820 | MIT GICP 精化，Windows 稳定串行路径 |
| xenium | 1c449ae953ce2a440b0d16c5ed1181d2754860ab | 固定源码头依赖，避免上游浮动 main |
| oneTBB | v2022.0.0，归档哈希锁定 | FasterPFH 的有界 task_arena；使用上游推荐的 DLL 构建 |

KISS 顶层 MIT 不代表全部递归依赖都是 MIT。上游 PMC 为 GPL-3.0-or-later，本次不编译、不链接、不交付 PMC；MAX_CLIQUE 分支未启用时明确 unsupported，默认 max-core 剔除和 GNC 保持。正式来源、补丁、文件清单和运行库由匹配依赖清单固定。

Windows 实测采用经典 `/openmp`、动态 CRT 和已有 vcomp140，不分发仅在本机 debug_nonredist 找到的 LLVM OpenMP DLL。适配包含标准 alignas/ptrdiff_t、VLA→vector、等价 MSVC Interlocked 原子、OpenMP 有界 signed 索引、每调用线程预算、配置相关 static→局部值、局部静默流、图 RAII 清理、空描述子保护及反向 NN 单写入。FasterPFH 并行写 vector<bool> 改逐字节标记；无效/self 邻点不进入 SPFH，输出描述子必须有限。

OpenMP region 从调用线程的 TLS 预算取 team 大小，不调用全局 omp_set_num_threads，也不替换 cout/cerr 的全进程 rdbuf。TBB 使用 min(调用预算,16) 的局部 arena。small_gicp 上游明确警告 MSVC task-based KdTree/Eigen 路径，本阶段精化固定串行。上游局部随机抽样和并行采样具有非确定性，因此全局节点关闭结果缓存。

## 原生数据与算子契约

### cloud.template_3d

cloud → model（原生 PointCloud）+ template（cloud.Template3D.v1）。按显式 sampleDistance 栅格和 maxModelPoints 最远点抽样，以原始 AABB 中心居中，轴不旋转。元信息v1严格校验12个固定字段，拒绝缺字段、未知字段、类型或范围错误；绑定输出 model.id/点数，校验 sourceFrame/modelFrame、m、源中心、16项无旋转刚体居中矩阵及其原生float32舍入一致性、尺寸/直径关系、采样配置及方法；不在 Record 放坐标数组。至少四个非共面点，协方差最小特征值必须大于最大值的 1e-10，不给零厚度模型编造尺寸。可选通道经原生 select 和刚体法线处理保留。

持久化示例应保存完整原 CAD 采样云，再 io.load_cloud→同参数 template_3d 重建新进程身份。只保存已稀疏化的居中 model 再构建，AABB/anchor 可能变化，不能声称原 CAD anchor 自动保持；导出稀疏模型须同时保留原模板 anchor 元信息并显式重绑定。

### register.global_3d

输入 model/template/scene，可选 sceneIndices 和 sceneGroups；输出 instances（vision.Poses3D.v1）、绑定原 scene 的 indices/groups（cloud.Groups.v1）及 quality（cloud.GlobalMatches.v1）。sceneGroups 必须配套原 scene 身份的 sceneIndices 且帧匹配；组间索引可重叠，组内去重；空索引/空组没有全云 fallback。

Pose 表示居中 model→scene。position 是模型原点在场景的位置，size 保持模板局部轴尺寸；confidence 是 `modelInlierRatio / (1 + deliveredRmse / maxRmse)` 的几何质量分数，不是识别概率；pickable=false。

模型和场景分别居中并均匀缩放，使 KISS 内部 voxel 固定 0.01，规避上游硬编码最小 5mm 限制；返回时恢复原米制平移。KISS 提供任意 SE(3) 初值，GICP 精化后量化原生 float32 Transform，用与 transform.apply 相同的运算交付，再对原始模型/当前完整 ROI 重查 NN。门控包含收敛、RMSE、模型覆盖率、独立场景支持比例和非共面实际支持；不能凭 KISS valid 标记认定识别成功。多实例在真实匹配后移除模型附近的场景几何，再执行下一次有界全局阶段，跨重叠 ROI 仍抑制重复实例。

对称规则用显式模型局部轴及离散折数生成候选，逐个重评质量并记录 symmetryScores/equivalentOrientations/ambiguous；不把 PCA 轴正负号或确定性排序当作唯一朝向证据。不声明自动识别任意连续对称。朝向约束在交付坐标的最终候选上检查。几何质量合格而业务朝向被拒绝的实例不输出Pose，但安全移除其真实支持并继续查询其余实例；它与几何质量失败分别计数，不能因先遇到另一朝向的物体就漏掉后续合格实例。

参数全部显式。默认分辨率1mm，法线/特征倍率3/5、对应预算512、粗采样每侧最多8192点、maxInstances16、maxPasses32、GICP50次；默认对应3mm、RMSE1mm、模型覆盖0.5、独立场景支持0.5。SurfaceV2 易用档位通过同引擎的可运行参数展开片段交付，实际配置记录于 quality.effective，不做隐藏覆盖。精度档名不等于误差保证。

### cloud.edges_3d

输入 cloud、可选 validIndices；输出边缘 cloud、去重并集 indices、boundaryIndices/occludingIndices/occludedIndices/curvatureIndices 和质量。organized 模式必须显式 organizedWidth，点数整除且至少2×2，有效点正 Z；缺失点可用 NaN/Inf XYZ 自动识别，也可用 validIndices 显式标记；只对有效点要求 XYZ/强度/法线有限，未选洞的法线/强度可同样为 NaN，输出仅保留有限点。显式有效索引若选中 NaN 点报错。深度差的近侧/远侧分别标记，法线可提供或由局部差分估计。unorganized 模式以有界 KNN、半径和局部 PCA 切平面邻点最大角缺口检测边界，不假装有像素遮挡邻接；请求该模式的遮挡分类会报错。退化邻域计数，不制造正常法线。

### cloud.view_sample

model + model→camera 的 Pose3D/Poses3D → cloud、indices/groups、cloudIndices/cloudGroups、quality。每个视图按 atan2(x,z)/atan2(y,z) 分箱，保留最近 Z 深度及 depthTolerance 内的点，实际执行前后遮挡；近裁面、角分辨率和背面剔除显式。

indices/groups 绑定原 model（可跨视图重复点）；cloud 是按视图拼接的可见云，可保持模型坐标或 transformByPose 到相机；cloudIndices/cloudGroups 单独绑定输出 cloud，能直接接分组下游，避免把原 model 身份错误接到拼接云。

## 资源、空结果和取消

原生输入≤131072点，模板≤8192点，场景 ROI≤64组；超限诊断，不静默丢场景。粗采样点预算是算法配置并记录实际配置，不是完整连续搜索空间的穷举保证。全局阶段调用超过 maxPasses 报 resource_limit；maxInstances 是用户请求的返回上限，达到时 quality 明示。视图≤128，视图×模型点数≤4Mi；Record≤1MiB/深度16，输出原子提交。

共享 native fit 的 NN 查询预算64Mi不覆盖 FasterPFH 内部。本次另在库调用前根据 feature-radius 的27相邻栅格计保守邻域对上界：模型+场景≤16Mi对；内存估算包含持久 indices/float distances 的容量、兼容图 O(maxCorrespondences²)、描述子和线程 scratch，预计峰值≤512MiB。预检先于 FasterPFH，密集大半径不能靠 bad_alloc 或主机 OOM 退出。归一化跨度也预检，防止上游 int voxel 越界后悄悄跳点。

合法空场景、显式空 ROI、无特征和无合格候选返回成功空集合；非法数据/帧/绑定/参数、取消和预算失败不伪装为空匹配。取消在粗匹配、GICP、原生重评及外层扫描边界检查，库内部阶段不能抢占，本阶段没有承诺硬超时或固定取消延迟。

## 验证证据与适用边界

独立 Windows MSVC19.51/RelWithDebInfo/MD/经典 OpenMP probe 已编译并运行 KISS→small_gicp；正式发布另按固定 MSVC14.44 CI 验证。ICV=1/TLS=4 的 PKC 回归包括99孤点+最后2点相连的101顶点图，与预分配 BZ 参考逐项一致，覆盖>=98% visited后的缩图分支；不把曾由测试误传空 BZ vector 引起的 AV 记为上游缺陷。

毫米级合成件是三个长方体并集的外表面，约75×42×36mm，1.8mm面采样3146点，非对称 L 臂和凸台；真值三轴旋转及约0.5m平移。每组3个种子，有20µm/200µm XYZ 噪声、30%/60%裁掉的模型点、700个背景点、双实例场景和纯背景。所有对照保持对应3mm、RMSE0.5mm、覆盖0.35和独立场景比例0.5；这是另行显式配置的遮挡基准，不等于默认0.5覆盖门限。

| 噪声 | 邻域倍率 | 全重叠 | 遮挡30% | 遮挡60% | 双实例场景首候选 | 纯背景误接受 |
|---|---|---|---|---|---|---|
| 20µm | 3/5 | 3/3 | 3/3 | 3/3 | 3/3 | 0/3 |
| 200µm | 3/5，安全修复后 | 0/3 | 1/3 | 0/3 | 2/3 | 0/3 |
| 20µm | 6/10 | 3/3 | 3/3 | 3/3 | 3/3 | 0/3 |
| 200µm | 6/10 | 3/3 | 2/3 | 0/3 | 3/3 | 0/3 |

原始未修复3/5的200µm基准（0/3、0/3、0/3、2/3）另保留，不能把并发修复和尺度对照混为一个原因。失败是粗候选错误、GICP未收敛或原生 RMSE 约0.503mm超过冻结门限，均拒绝；未为过样本放宽质量门限。当前证据不能承诺200µm噪声+60%遮挡。独立 adapter 多实例 probe 另实跑两物体+500背景：6792→3646→500点，两次各移除3146个正确支持，位置误差0.753/1.137µm、旋转误差5.33e-5/1.70e-5rad，最后不接受背景。它仍不能代替完整多实例算子测试；原生双实例、空 ROI、通道/绑定、边缘与遮挡回归在 test_p2_matching.cpp 另行执行。

方向过滤的独立 C ABI 复验保留默认3/5、512对应配置下首组单物体/双物体偶发粗候选后 GICP 未收敛的基准，不把它归为方向过滤旧问题。永久双实例/方向回归改用显式6/10邻域、1024对应配置，保持 RMSE0.5mm、覆盖0.5、对应距离3mm不变；同一新 DLL 实跑10轮单A、单B、双物体不筛选、只允许A、只允许B，共50次均得到预期1/1/2/1/1实例，方向拒绝仍继续检索另一实例。这是该配置的有限重复验证，算子仍标记非确定性，不引入隐藏重试，也不说明默认参数对所有采样密度稳定。

这些是软件/合成几何验证，不能推出工业实物误检率、漏检率、相机标定精度、机器人抓取安全、吞吐和长期稳定性已验收。三个无目标样本的0误接受也不是生产误报率承诺。
