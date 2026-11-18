# P2 B02：混合约束拟合与六轴局部搜索

[文档中心](README.md) · [六阶段交付](p2-sequential-delivery.md) · [P1配准分析](p1-analysis-cloud.md)

本页记录阶段2的 `BestFit` / `SearchPoseLocally` 逐行分析和独立实现契约。实现入口为 [p2_advanced_fit.cpp](../packs/std-pointcloud/ops/p2_advanced_fit.cpp)，共享执行与质量核对为 [p2_fit.cpp](../packs/std-pointcloud/algo/p2_fit.cpp)，永久回归为 [test_p2_advanced_fit.cpp](../packs/std-pointcloud/tests/test_p2_advanced_fit.cpp)。阶段2包含的其他高度、全局匹配、边缘与可见面采样在相应分析中跟踪；本页不代表整阶段已合并。

## 参考实现核对

已逐行阅读本机 `xyz-vision-lib` 的 `3D_Processing/BestFit.cpp/.hpp` 与 `SearchPoseLocally.cpp/.hpp`。文件声明 proprietary，仅核对用途、数据流和失败边界，不复制函数体。目录没有Git元数据，不能证明与外部清单标注修订完全一致；以下是静态观察，没有执行外部程序，也不声称复现其崩溃或工业误差。

| 条目 | 参考实现的静态行为 | LyFlow的独立处理 |
|---|---|---|
| BestFit输入 | Geometry3DVector按交替目标/源项读取前三坐标；运行路径未先验证元素数为偶数，没有使用几何种类的线/面方向。 | 明确point、line、plane、axis约束和source→target方向，不以几何容器名冒充混合约束。 |
| 3-2-1规则 | input_sequence必须形成三个连续轴块；非零权重后分别至少3/2/1项，DoubleVector作为轴向偏移。 | 显式datum=0/1/2及正权，不依赖列表顺序；检查组内平行、组间正交和最终六自由度可观性。 |
| 两种加权路径 | Process建立加权正规方程并直接inverse；Result枚举三点/两点组合、按候选近邻距离排除，再加权平均方向/平移。 | 一个有界SE(3)加权最小二乘目标；中心和空间尺度归一化、特征值条件检查、可选Huber、线搜索及真实残差门控，不沿用组合爆炸或显式矩阵逆。 |
| 姿态假设 | 两候选方向按接近指定坐标轴选择，后续法向有小角约定；部分路径仅记录告警。 | 定义为需要合理初值的局部拟合，拒绝退化/不收敛/质量失败；不承诺全局唯一解。 |
| 注册开关/输出 | register_mode分支跳过实际优化；SVD路径仅接受六点并以三组边向量求旋转。 | 已有P1 `register.rigid_fit`保持一般对应点Kabsch；新增约束拟合单独命名，不用开关产生看似优化的初值。 |
| 局部搜索范围 | 单次仅选择一个X/Y/Z轴及Translation或Rotation；支持均匀采样与围绕前次胜者的一维递进采样。 | 按用户接受的补齐计划扩展为XYZ平移和XYZ旋转全部Cartesian组合及分级网格，保留明确预算，不把逐轴扰动称作六轴组合。 |
| 搜索质量 | 每个初始工件共享模型/场景，使用固定距离内最近点比例；同分时均匀/分级路径的选择规则不同。 | 最终仅从通过全部质量门限的候选中按源模型覆盖比例优先、实际float32输出RMSE次优，完全同分保留稳定先到候选；粗层引导可使用暂未通过最终残差门限的可观测候选。 |
| 输入/终止 | 清除非有限点后建索引；均匀采样除以num_step-1，分级循环没有独立最大层数，running_info未用于取消。 | 非有限/通道错误显式诊断，不悄悄改变工件点序；奇数采样数3..9，最多8层/4096总候选，逐阶段轮询取消。 |
| 结果与可视化 | 输出继承原工件后修改pose/score，对每个胜者变换模型并合并。 | 复用受校验Pose3D集合，绑定ID/元数据、原生点云/索引及cloud.Groups，保强度/RGB、旋转法线，不将大数组放入Record。 |

## 三维加权几何约束拟合

桌面与CLI可先使用 `register.constraints_3d` 构造节点：sourceFrame/targetFrame、mode与下游拟合器保持一致，items为最多64KiB的JSON Text参数，可绑定顶层图参数进行覆写。该入口只构造cloud.Constraints3D.v1的小几何约束列表，复用拟合器相同的字段、类型、权重、方向及3-2-1结构校验；不提供通用Any/Record加载或大点云JSON通道。缺省items=[]需要用户配置，空/非法JSON、未知字段和非法约束均报参数诊断；最终几何可观性仍由拟合器验证。约束构造→拟合的真实调度图与顶层Text覆写已纳入同一永久回归组。

`register.constrained_fit_3d@2.0.0` 接收 `cloud.Constraints3D.v1` 约束和可选的原生 `initial` 变换。初值为 source→target，缺省为恒等。输出 `transform` 和 `quality`。需要对齐同源点云时，将变换连入 `transform.apply`。使用 `lyflow migrate --request … --write` 和 `{"kind":"constrainedFit.v1","source":{"node":"fit"}}` 规则迁移旧图，其中 `fit` 为旧节点 ID。

```json
{
  "sourceFrame": "source",
  "targetFrame": "target",
  "unit": "m",
  "items": [
    {"id":"point-1","type":"point","source":[0,0,0],"target":[0.1,0,0],"weight":1},
    {"id":"line-1","type":"line","source":[1,0,0],"target":[1.1,0,0],"direction":[0,0,1],"weight":1},
    {"id":"plane-1","type":"plane","source":[0,1,0],"target":[0.1,1,0],"direction":[1,0,0],"offset":0,"weight":1}
  ]
}
```

这是字段示意，不是足够观测六自由度的示例；实际约束必须通过可观性检查。三个顶层标签必须匹配算子sourceFrame/targetFrame及米制，items为1..4096项且ID唯一。坐标/offset绝对值不超过1e12m；weight严格为正，最大/最小权重比不超过1e12，内部统一除以最大权重。direction在输入时归一化，必须有限非零。未知字段、无关字段和错误类型报诊断，避免错拼参数被静默忽略。

令x=R·source+t，q=target，n/u为目标帧单位方向：

| type | 残差 | 可消费字段 |
|---|---|---|
| point | x−q，三维距离 | id/type/source/target/weight |
| line | (I−uuᵀ)(x−q)，到无限目标直线的正交距离 | 上述字段及direction；不接受offset/datum |
| plane | n·(x−q)−offset，有符号距离；优化使用其沿n的向量形式 | direction为目标平面法向；可有offset/datum |
| axis | 与plane相同的有符号轴向差，显式用于3-2-1偏移要求 | direction为目标量测轴；可有offset/datum |

这不是源线/源面法向对齐：每个source是待变换的观测点，line/plane定义目标约束。所有残差与offset以米为单位。line方向反向不会改变几何；plane/axis的方向与offset同时翻转才保持同一有符号约束。

`mode=mixed`接受上述组合；`weighted_321`只接受plane/axis及datum=0/1/2，三组至少3/2/1项，同组方向平行、跨组正交；实际点布置仍必须观测全部六自由度。该模式实现同一加权几何目标，不保留外部Qt的参数树或Result组合平均协议。

| 参数 | 默认 | 范围/语义 |
|---|---:|---|
| sourceFrame / targetFrame | source / target | 显式非空参考帧 |
| mode | mixed | mixed / weighted_321 |
| huberDelta | 0m | 0关闭；正数为Huber转折点，最多1e6m |
| maxRmse | 0.001m | (0,1e6]，最终按原始权重计算，不用Huber掩盖残差 |
| maxError | 0.01m | (0,1e6]，所有约束最大几何误差 |
| maxIterations | 100 | 1..1000 |
| translationTolerance | 1e-8m | 正有限平移收敛阈值 |
| rotationTolerance | 1e-8rad | 正有限角度收敛阈值 |

每轮以加权中心和源支持空间尺度构造SE(3)正规方程；归一化六维矩阵最小/最大特征值比必须大于1e-10，不通过时给出degenerate_geometry，而非输出伪姿态。旋转更新每步不超过0.5rad，并进行最多16次目标函数线搜索；约束残差工作量不超过16Mi次计费预算。

最终Transform量化为float32，并检查右手刚体。质量按该变换的真实float32乘加点坐标计算，再针对交付支持点复核空间尺度与六自由度可观性；不能只报告double迭代空间的漂亮误差。约束source在该交付核对中按原生PointCloud的float32坐标表示，目标几何和offset保留声明的double值，精度不足时会失败。`cloud.ConstrainedFit.v1`包括accepted/converged、迭代数、交付conditionRatio、weightedRmse/maxError及每约束ID/type/归一化weight/前后距离；plane/axis另有signedBefore/signedAfter。weightedRmse为sqrt(sum(weight·distance²)/sum(weight))，不是按残差分量数稀释的误差。aligned若存在则使用同一float32乘加路径，保留强度/RGB、旋转并归一化法线；输入保持只读。

## 三维工件六轴局部搜索

新增 `register.pose_search_3d`。必需source模型云和target场景云；初值可以是单Transform `initial` 或目标帧 `vision.Pose3D.v1` / `vision.Poses3D.v1`的 `poses`，两者互斥，均未提供时使用恒等单初值。工件集合不能为空，ID唯一；所有工件共享模型，位姿表示模型原点到目标帧的变换。单初值使用id参数生成身份及模型AABB尺寸；不把模型坐标中心偷偷平移到AABB中心。

| 参数 | 默认 | 语义 |
|---|---:|---|
| translationRadius | [0.02,0.02,0.02]m | XYZ各轴半径，0固定；各轴0..1e6m |
| rotationRadius | [0,0,0.1]rad | XYZ各轴半径，0固定；各轴0..π |
| steps | 3 | 每个有效轴含两端与中心的3..9奇数采样 |
| searchMode / levels | grid / 3 | grid只运行完整一层；hierarchical运行1..8层 |
| composition | target | target或source，见下文 |
| maxEvaluations | 4096 | 所有工件×全部轴组合×层数的总上限，超限不截断组合 |
| refinement | none | 显式选择none / point_to_point / point_to_plane |
| maxCorrespondenceDistance | 0.1m | 真实交付点到场景NN的距离上限 |
| maxRmse / minInlierRatio | 0.01m / 0.7 | 最终真实几何RMSE与模型覆盖比例 |
| maxIterations | 100 | ICP上限1..1000；none不使用迭代 |
| translationTolerance / rotationTolerance | 1e-7m / 1e-7rad | ICP收敛门限 |

三个旋转参数组成Rz·Ry·Rx。target组合为R=Roffset·Rinitial、t=tinitial+toffset，即绕工件模型原点旋转而不是绕世界原点公转；source组合为T=Tinitial·Toffset，平移轴随模型初始朝向旋转。六个有效轴各steps项的完整乘积共同评估，绝非每轴单独扰动。

hierarchical围绕最佳偏移继续采样，半径每层除以steps−1，并限制偏移不越初始各轴搜索范围。粗层候选可未通过最终maxRmse，但必须存在有限的实际匹配供下一层细分；全部层完成后只从所有已评估且通过质量门限的候选中交付胜者；不允许更高覆盖但RMSE超限的候选挤掉合格解。可选ICP属于无搜索盒约束的局部精化，能够离开网格边界，必须显式启用；不将精化结果冒称仍在搜索盒内。

输出 `poses` 是目标帧受校验集合，保留原ID/size/name/class/attributes，更新位置/方向与质量confidence，pickable=false。confidence=modelCoverage/(1+rmse/maxRmse)，不是识别概率或抓取许可。Pose四元数遵循已有归一化约定；质量中的16项transform记录交付float32系数，是aligned重算的精确依据。

`aligned`按输入工件顺序串接各胜者模型云，保可选通道；`indices`绑定aligned，`groups`为`cloud.Groups.v1`且id逐一对应工件。`cloud.PoseSearch.v1`质量记录每对象的ID、匹配数、模型覆盖、实际RMSE、迭代数、获胜偏移及每层每个候选的offset/accepted/converged/observable/matchedCount/inlierRatio/rmse；无匹配候选rmse=null，不能被解释为零。完全同分稳定保留先到项。任一对象没有有效胜者，整节点报诊断且不产生部分成功/初值替代的假Pose。

## 共享精配准与边界

[共享接口](../packs/std-pointcloud/algo/p2_fit.h)提供TargetIndex、CloudFitOptions、FitBudget、evaluateCloudFit/refineCloudFit。全局模板匹配可复用相同交付质量路径，而不复制一套定义不一致的ICP结果校验。P1既有算子行为不变。

每云最多1Mi点；目标索引检查finite、通道一致、法线非零及float平方距离安全跨度。全批最多64Mi次模型点NN查询；搜索aligned总输出最多4Mi点。结果按float32变换和float32点坐标真实重建，再查询原目标点、以double重算欧氏距离。匹配比例分母是源模型点数，不是整个场景；targetIndices按源点顺序保留原目标索引/−1，targetSupport为去重排序的场景支持，供全局多实例抑制复用。所有最终有效NN配对必须至少覆盖3个不同目标索引，交付源点与目标点各自非共线且配对交叉协方差至少秩2；重合、共线及float32平移后坍缩的支持仅标记该候选不可观测/不接受，不中断其他候选搜索。ICP点到面必须有目标法线，最终交付NN/法线重新建立的中心化/列归一化六维Jacobian也必须可观测；所有精化最终仍以欧氏NN RMSE与模型比例门控，不拿点面目标函数替代几何质量。

循环在阶段/候选/点批次轮询取消；单次PCL索引构建不宣称可抢占。没有静默移除NaN、截断候选或自动放宽阈值。Record沿用1MiB、深度16和4096项限制，超出返回resource_limit，不将large arrays编码进JSON；点云/索引通过原生载荷流动。

这些算子是算法与宿主共享执行能力，不等同外部Qt配置/工件发布协议，也不构成工业重复性或吞吐验收。当前永久回归覆盖已知真值的混合点线面/加权3-2-1偏移、可观性/权重/非法输入、全部六轴729组合、粗到细分级、source/target组合、工件ID/组索引/通道，以及float32真实质量/共享support/取消/预算。运行数字和最终结果以对应PR最新源码修订的完整验证为准；不能将已写测试称作已经执行通过。
