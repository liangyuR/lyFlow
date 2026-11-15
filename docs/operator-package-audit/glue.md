# glue 逐算子迁移清单

状态：实施中；当前 20 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 112 · glue.locate

- 当前：`glue` / `1.1.0` / 二维模板定位。目标：`std-image`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/locate.cpp:145](../../packs/glue/ops/locate.cpp#L145)。
- 端口：`image:Image` → `pose:Record`, `score:Measurement`, `overlay:Record`。
- 参数单位标注：anchor=px, searchRadius=px, angleRange=°, angleStep=°。
- 依赖：当前直接：OpenCV归一化互相关/旋转插值/高斯平滑；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 image:Image → 输出 pose:Record, score:Measurement, overlay:Record。Image→glue.Pose2D/score/overlay；模板带pad、anchor/teachCenter；受限转角NCC、抛物线亚像素峰与护栏。
- 目标契约：图像模板刚体定位与明确的source→target 2D Pose契约；旧glue.Pose2D和护栏参数通过场景适配，保留现算法。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：半像素平移/小转角、angleRange<angleStep、模板pad/锚点、低分/护栏和pose_fail传播。
- 说明：先比较现image NCC实现差异，不因已有同名匹配节点直接删除此算法。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 113 · glue.polyline

- 当前：`glue` / `1.0.0` / 折线胶路。目标：`std-geometry`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/polyline.cpp:97](../../packs/glue/ops/polyline.cpp#L97)。
- 端口：无 → `line:Record`, `overlay:Record`。
- 参数单位标注：zone=px。
- 依赖：当前直接：当前使用OpenCV Point2d类型；弧长/重采样算法仅标准库；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：core几何数据/标准库；移除OpenCV公共类型依赖。
- 当前契约：输入 无 → 输出 line:Record, overlay:Record。参数折线→Polyline/overlay；px、zone按弧长，重复相邻点跳过，2px重采样，长度至少8px；不读取图像。
- 目标契约：有单位/坐标帧的通用折线与弧长重采样；去除cv::Point2d公共类型依赖，旧8px/2px默认和zone由场景适配。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：重复点、末端zone=0、短折线拒绝、切向符号、站号和弧长一致。
- 说明：像素是合法几何单位；不能为了轻量将大折线数组塞进不限长Record。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 114 · glue.color_model

- 当前：`glue` / `1.0.0` / 胶色建模。目标：`std-image`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/color_model.cpp:175](../../packs/glue/ops/color_model.cpp#L175)。
- 端口：`image:Image`, `line:Record`, `prior:Record?` → `model:Record`, `overlay:Record`。
- 参数单位标注：glueHalf=px, bgBand=px, stationStep=px, shiftMax=px, blurSigma=px, satDilate=px。
- 依赖：当前直接：OpenCV图像/对数色度、高斯拟合、确定性EM；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 image:Image, line:Record, prior:Record? → 输出 model:Record, overlay:Record。RGB Image+Polyline+[ColorModel prior]→ColorModel/overlay；沿线胶/背景取样，排高光暗区；统计可相加、与累计顺序无关。
- 目标契约：通用示教前景/背景颜色模型；显式RGB、线坐标px、模型schema/像素参数和prior兼容；胶带采样预设留场景。
- 迁移前置：M1；glue.polyline
- 关联算子：glue.polyline
- 验收：RGB顺序、累加交换顺序、确定性EM、无有效样本、饱和/暗区与重居中轮次基线。
- 说明：不能换成HSV阈值后宣称只是分包。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 115 · glue.color_map

- 当前：`glue` / `1.0.0` / 胶色得分图。目标：`std-image`；迁移归属；**M5**。
- 源码：[packs/glue/ops/color_map.cpp:94](../../packs/glue/ops/color_map.cpp#L94)。
- 端口：`image:Image`, `model:Record?` → `maps:Bundle<glue.ColorMaps>`。
- 参数单位标注：roi=px。
- 依赖：当前直接：OpenCV色度/图像视图与高斯判别；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 image:Image, model:Record? → 输出 maps:Bundle<glue.ColorMaps>。RGB Image+ColorModel→ColorMaps；score f32[0,1]、state图；高光/无信息为NaN，明确暗背景为0；ROI块与整图一致。
- 目标契约：通用带有效性/原因状态的颜色得分图，图像数组走原生Image，模型/ROI原点及三态必须版本化。
- 迁移前置：glue.color_model；M1
- 关联算子：glue.color_model
- 验收：高光/暗/背景三态、NaN不可改成0、超模型兼容范围拒绝和ROI块等值。
- 说明：NaN影响unknown而0是观测到背景，不能合并。

## 116 · glue.part_edges

- 当前：`glue` / `1.0.0` / 找零件边。目标：`std-image`；迁移归属；**M5**。
- 源码：[packs/glue/ops/part_edges.cpp:121](../../packs/glue/ops/part_edges.cpp#L121)。
- 端口：`image:Image`, `line:Record` → `edges:Record`, `overlay:Record`。
- 参数单位标注：stationStep=px, sRange=px, start=px, searchLength=px, farRun=px。
- 依赖：当前直接：OpenCV灰度剖面/一维高斯与亚像素插值；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 image:Image, line:Record → 输出 edges:Record, overlay:Record。Image+Polyline→PartEdges/overlay；px；输出两侧所有合格跳变，基于极性、局部最大下降率和远侧持续暗区，不选侧。
- 目标契约：通用沿路径边缘候选提取；局部/整图坐标显式，零候选与无效采样分开；场景负责选零件边和剔野。
- 迁移前置：glue.polyline；M1
- 关联算子：glue.polyline
- 验收：所有候选保留、极性、图像边界、三点亚像素细化和首边/最强边区别。
- 说明：可与现有image caliper共享采样/插值内核，但判据需逐项比对。

## 117 · glue.response

- 当前：`glue` / `1.0.0` / 响应图。目标：`std-image`；迁移归属；**M5**。
- 源码：[packs/glue/ops/response.cpp:50](../../packs/glue/ops/response.cpp#L50)。
- 端口：`image:Image` → `maps:Bundle<glue.ResponseMaps>`。
- 参数单位标注：diameter=px, roi=px, responseSigma=px, graySigma=px, levelSigma=px。
- 依赖：当前直接：OpenCV黑/白顶帽、GaussianBlur、f32图像视图；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 image:Image → 输出 maps:Bundle<glue.ResponseMaps>。Image→Bundle<glue.ResponseMaps>含response/gray/level三张单通道f32及frame；ROI块数值与整图逐位相同，极性归一化。
- 目标契约：通用带极性的响应图能力；新image命名Bundle保留大数组为原生Image，明确ROI原点/完整图坐标；旧Bundle须图迁移适配。
- 迁移前置：M1；M2
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：明暗极性、ROI边界、结构元尺度、f32布局及块/整图逐位一致；不相交ROI仍报错。
- 说明：不能仅提取response而遗漏gray/level，后续横向剖面依赖三者。

## 118 · glue.fan_scan

- 当前：`glue` / `1.0.0` / 扇形扫描。目标：`std-image`；迁移归属；**M5**。
- 源码：[packs/glue/ops/fan_scan.cpp:156](../../packs/glue/ops/fan_scan.cpp#L156)。
- 端口：`maps:Bundle<glue.ResponseMaps>` → `scan:Record`, `overlay:Record`。
- 参数单位标注：anchor=px, sector=deg, radial=px, angleStep=deg, radialStep=px, peakSeparation=deg。
- 依赖：当前直接：OpenCV响应图视图/双线性采样；峰选择为标准库；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 maps:Bundle<glue.ResponseMaps> → 输出 scan:Record, overlay:Record。ResponseMaps→FanScan/overlay；px径向、deg角度；得分=有效值和/总采样数，无效比例达门限为null，含无效峰策略可配。
- 目标契约：通用响应场扇形扫描；统一采样视图/有效掩码和角度约定；gray/color可共享内核但不能改变null/归一化分母。
- 迁移前置：glue.response；M1
- 关联算子：glue.response
- 验收：ROI块外、NaN、invalidMax等号、360度环绕、峰分离/平局与includeInvalid。
- 说明：只给候选方向，不在通用层选择生产方向。

## 119 · glue.color_fan

- 当前：`glue` / `1.0.0` / 彩色扇形扫描。目标：`std-image`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/color_fan.cpp:20](../../packs/glue/ops/color_fan.cpp#L20)。
- 端口：`maps:Bundle<glue.ColorMaps>` → `scan:Record`, `overlay:Record`。
- 参数单位标注：anchor=px, sector=deg, radial=px, angleStep=deg, radialStep=px, peakSeparation=deg。
- 依赖：当前直接：OpenCV ColorMaps.score 视图；复用 fanScanCompute；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 maps:Bundle<glue.ColorMaps> → 输出 scan:Record, overlay:Record。ColorMaps→与fan_scan相同FanScan；高光/暗区NaN和块外均无效，scores规则完全相同。
- 目标契约：与灰度扇扫共用通用采样视图能力；旧输入Bundle通过显式适配，保留同一候选Record和无信息标记。
- 迁移前置：glue.fan_scan；glue.color_map
- 关联算子：glue.fan_scan；glue.color_map
- 验收：相同score场的灰/彩结果一致；全NaN、ROI块外和无峰行为一致。
- 说明：合并实现不要求立刻删除旧算子id，id切换走统一图迁移计划。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 120 · glue.lateral_runs

- 当前：`glue` / `1.0.0` / 横向卡尺。目标：`std-image`；迁移归属；**M5**。
- 源码：[packs/glue/ops/lateral_runs.cpp:105](../../packs/glue/ops/lateral_runs.cpp#L105)。
- 端口：`maps:Bundle<glue.ResponseMaps>`, `line:Record` → `runs:Record`, `overlay:Record`。
- 参数单位标注：stationStep=px, sRange=px, searchHalf=px, mergeGap=px, mergeWidthMax=px, edgeProbe=px, backgroundProbe=px。
- 依赖：当前直接：OpenCV响应场法向采样；分段/半高/合并标准库；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 maps:Bundle<glue.ResponseMaps>, line:Record → 输出 runs:Record, overlay:Record。ResponseMaps+Polyline→LateralRuns/overlay；每0.5px采样，connected/half_max规则；给出所有暗段，不筛宽、不判有无胶。
- 目标契约：通用沿路径法向区间提取；新Record明确站号/弧长/法向方向、完整边与阈值模式，保留全部候选供场景选择。
- 迁移前置：glue.response；glue.polyline；M1
- 关联算子：glue.response；glue.polyline
- 验收：双峰半高、窄缝合并、宽度上限、边缘不完整、锐度组成和站序；不新加presence门。
- 说明：测量记录与业务Bead中选中的段需区分。

## 121 · glue.color_runs

- 当前：`glue` / `1.2.0` / 彩色横向卡尺。目标：`std-image`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/color_runs.cpp:317](../../packs/glue/ops/color_runs.cpp#L317)。
- 端口：`maps:Bundle<glue.ColorMaps>`, `line:Record` → `runs:Record`, `overlay:Record`。
- 参数单位标注：stationStep=px, sRange=px, searchHalf=px, mergeGap=px, mergeUnknownGap=px, mergeWidthMax=px。
- 依赖：当前直接：OpenCV颜色得分场/状态图法向采样；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 maps:Bundle<glue.ColorMaps>, line:Record → 输出 runs:Record, overlay:Record。ColorMaps+Polyline→LateralRuns；每0.5px，含infoFraction、unknownSpans、边缘cause、unknownWidth；合并不跨明确背景。
- 目标契约：通用有信息掩码的候选区间测量；保留edge/caliper/highlight/dark/outside原因及未知间隙规则；场景再选胶段。
- 迁移前置：glue.color_map；glue.lateral_runs；M1
- 关联算子：glue.color_map；glue.lateral_runs
- 验收：真实边/窗口端/高光/暗区/块外分类，可信邻站合并、明确背景不跨越及unknown宽度。
- 说明：不能与lateral_runs的有限阈值分段无条件互换。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 122 · glue.station_calipers

- 当前：`glue` / `1.0.0` / 胶路逐点卡尺测量。目标：`solution-glue`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/station_calipers.cpp:285](../../packs/glue/ops/station_calipers.cpp#L285)。
- 端口：`image:Image`, `pose:Record?`, `calib:Record?` → `measure:Record`, `coverage:Measurement`, `distanceMean:Measurement`, `overlay:Record`。
- 参数单位标注：searchHalf=px, caliperWidth=px, innerSearch=px, beadSearch=px, widthMin=px。
- 依赖：当前直接：OpenCV卡尺、2D位姿与平面标定；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image/std-metrology/std-geometry按节点组合；业务策略不反向成为标准包依赖。
- 当前契约：输入 image:Image, pose:Record?, calib:Record? → 输出 measure:Record, coverage:Measurement, distanceMean:Measurement, overlay:Record。Image+[glue.Pose2D]+[calib]→StationMeasure/coverage/distance；名义站点上的内边/胶边专用选择；pose_fail/incomplete_bead是无效量测。
- 目标契约：通用卡尺/姿态映射下沉std-image、距离归std-metrology；内边极性、最近暗谷和胶边规则留场景，判定仍归宿主。
- 迁移前置：M1；M2；glue.locate；glue.lateral_runs
- 关联算子：glue.locate；glue.lateral_runs
- 验收：无效pose、胶边越窗、最近暗谷与其他暗结构、标定mm、跨帧缺测不算断胶/合格。
- 说明：它不是完全通用的image.caliper_pair，需先抽内核再保留薄适配。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 123 · glue.taught_path

- 当前：`glue` / `1.0.0` / 示教胶路。目标：`solution-glue`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/taught_path.cpp:150](../../packs/glue/ops/taught_path.cpp#L150)。
- 端口：无 → `path:Bundle<glue.Path>`, `overlay:Record`。
- 参数单位标注：zone=px, tolerance=px, widthMax=px。
- 依赖：当前直接：OpenCV几何类型/折线重采样；示教走廊规则；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image/std-metrology/std-geometry按节点组合；业务策略不反向成为标准包依赖。
- 当前契约：输入 无 → 输出 path:Bundle<glue.Path>, overlay:Record。示教折线/可选当前图像→与bead_path同形Path，info.lineSource=taught；路径存在不等于沿线有胶。
- 目标契约：折线重采样下沉std-geometry；示教容差/检测区与可信度约定留场景；沿线有胶由后续测量判断。
- 迁移前置：M1；glue.polyline
- 关联算子：glue.polyline
- 验收：无图/无胶、折线端点、检测区裁剪和lineSource保留，不能把示教线存在判为OK。
- 说明：与glue.polyline共享重采样，不共享所有场景参数。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 124 · glue.track_path

- 当前：`glue` / `1.0.0` / 粗跟踪与拟合。目标：`std-image`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/track_path.cpp:173](../../packs/glue/ops/track_path.cpp#L173)。
- 端口：`maps:Bundle<glue.ResponseMaps>`, `fan:Record?` → `line:Record`, `track:Record`, `overlay:Record`。
- 参数单位标注：anchor=px, heading=deg, zone=px, bearingRange=deg, bearingMargin=deg, searchHalf=px, mergeGap=px, mergeWidthMax=px, widthMin=px, widthMax=px, coarseStep=px, maxGap=px, jumpMin=px, edgeProbe=px, backgroundProbe=px, dirWindow=px, fitWindow=px, fitSigma=px。
- 依赖：当前直接：OpenCV响应场采样/稳健局部拟合/高斯平滑；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 maps:Bundle<glue.ResponseMaps>, fan:Record? → 输出 line:Record, track:Record, overlay:Record。ResponseMaps+[FanScan]→Polyline/Track/overlay；px和deg；连续性门选最近段，跟丢可退射线，点数不足输出直线而非OK。
- 目标契约：通用受约束路径跟踪和证据质量归std-image；胶路参数预设/接受门留solution-glue；fallback直线显式标识，不冒充观测。
- 迁移前置：glue.fan_scan；glue.lateral_runs；M1
- 关联算子：glue.fan_scan；glue.lateral_runs
- 验收：断口后重接、无峰、minPoints直线回退、锐度门及检测区端点/重采样步长。
- 说明：不要与color_track强行合并默认策略，它们的无信息/回退规则不同。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 125 · glue.bead_path

- 当前：`glue` / `1.0.0` / 胶路提取。目标：`solution-glue`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/bead_path.cpp:146](../../packs/glue/ops/bead_path.cpp#L146)。
- 端口：`image:Image` → `path:Bundle<glue.Path>`, `overlay:Record`。
- 参数单位标注：nozzle=px, zone=px, sector=deg, heading=deg, headingTol=deg, widthMax=px, maxGap=px。
- 依赖：当前直接：OpenCV形态/响应图与路径搜索；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image/std-metrology/std-geometry按节点组合；业务策略不反向成为标准包依赖。
- 当前契约：输入 image:Image → 输出 path:Bundle<glue.Path>, overlay:Record。Image→Bundle<glue.Path>/info/overlay；图像像素路径，自动寻找直胶/螺旋胶及胶路可信度。
- 目标契约：响应/扇扫/跟踪下沉std-image；胶路形态、路径质量门与业务ok含义留场景，保留没有胶路的显式状态。
- 迁移前置：M1；glue.response；glue.fan_scan；glue.track_path
- 关联算子：glue.response；glue.fan_scan；glue.track_path
- 验收：直胶/螺旋、反光/背景暗带、无胶和路径长度不足，不生成虚构合格路径。
- 说明：通用跟踪结果valid/unknown不直接等于有胶。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 126 · glue.bead_width

- 当前：`glue` / `1.0.0` / 胶宽测量。目标：`solution-glue`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/bead_width.cpp:206](../../packs/glue/ops/bead_width.cpp#L206)。
- 端口：`image:Image`, `path:Bundle<glue.Path>`, `calib:Record?` → `bead:Bundle<glue.Bead>`, `widthMean:Measurement`, `widthMin:Measurement`, `widthMax:Measurement`, `coverage:Measurement`, `overlay:Record`。
- 参数单位标注：widthRange=px, stationStep=px, searchHalf=px, mergeGap=px, window=px。
- 依赖：当前直接：OpenCV采样/卡尺与场景暗段选择；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image/std-metrology/std-geometry按节点组合；业务策略不反向成为标准包依赖。
- 当前契约：输入 image:Image, path:Bundle<glue.Path>, calib:Record? → 输出 bead:Bundle<glue.Bead>, widthMean:Measurement, widthMin:Measurement, widthMax:Measurement, coverage:Measurement, overlay:Record。Image+Path+[平面标定]→Bead/宽度Measurement；无标定px、有标定mm；直胶/螺旋的有胶判据不同。
- 目标契约：通用横向采样/候选段下沉std-image，胶段选择、相对参考宽/峰值和present语义留场景；标定后的度量交std-metrology。
- 迁移前置：glue.bead_path；glue.taught_path；glue.lateral_runs；M2
- 关联算子：glue.bead_path；glue.taught_path；glue.lateral_runs
- 验收：直/螺旋、示教空胶、半高/外包络、标定前后宽度与有胶判据对照。
- 说明：不能用通用两边缘间距直接替代presence判据。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 127 · glue.bead_breaks

- 当前：`glue` / `1.0.0` / 断胶检测。目标：`solution-glue`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/bead_breaks.cpp:116](../../packs/glue/ops/bead_breaks.cpp#L116)。
- 端口：`bead:Bundle<glue.Bead>` → `breaks:Record`, `count:Measurement`, `longest:Measurement`, `overlay:Record`。
- 参数单位标注：minLength=px。
- 依赖：当前直接：OpenCV Point2d/平面标定辅助；区间连接本身标准库；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image/std-metrology/std-geometry按节点组合；业务策略不反向成为标准包依赖。
- 当前契约：输入 bead:Bundle<glue.Bead> → 输出 breaks:Record, count:Measurement, longest:Measurement, overlay:Record。Bead→断胶Record/count/longest/overlay；半站距补边，px或标定mm；无胶路整区断胶，区端是否计数可配。
- 目标契约：通用连续区间/弧长度量可下沉std-metrology；无胶/unknown、区端、minLength与计数策略留场景。
- 迁移前置：glue.bead_width；M2
- 关联算子：glue.bead_width
- 验收：半站距、minLength等号、区端忽略、弯曲标定弧长及无胶路整区行为。
- 说明：unknown与无胶不能合并为同一状态。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 128 · glue.color_track

- 当前：`glue` / `1.2.0` / 彩色粗跟踪。目标：`std-image`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/color_track.cpp:180](../../packs/glue/ops/color_track.cpp#L180)。
- 端口：`maps:Bundle<glue.ColorMaps>`, `fan:Record?` → `line:Record`, `track:Record`, `overlay:Record`。
- 参数单位标注：anchor=px, heading=deg, zone=px, bearingRange=deg, bearingMargin=deg, searchHalf=px, mergeGap=px, mergeWidthMax=px, mergeUnknownGap=px, widthMin=px, widthMax=px, coarseStep=px, maxGap=px, jumpMin=px, dirWindow=px, fitWindow=px, fitSigma=px。
- 依赖：当前直接：OpenCV颜色得分场采样/稳健拟合；彩色连续性/unknown策略；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image + OpenCV；纯颜色/采样能力不引ONNX/PCL，模型与后端按需启用。
- 当前契约：输入 maps:Bundle<glue.ColorMaps>, fan:Record? → 输出 line:Record, track:Record, overlay:Record。ColorMaps+[FanScan]→Polyline/Track；无锐度门、不回射线；无信息不计maxGap；extent允许超widthMax完整宽度，歧义/不完整计unknown。
- 目标契约：可选策略的通用得分场跟踪；胶路参数留场景；unknown/ambiguous/incomplete及extent模式是公共质量契约，不能简化成失败/无胶。
- 迁移前置：glue.color_map；glue.track_path；glue.color_runs
- 关联算子：glue.color_map；glue.track_path；glue.color_runs
- 验收：高光遮挡、纯背景、宽度extent、半宽吸附、断口重接和unknown长度原子保留。
- 说明：与灰度track_path共享内核前先将策略显式化，不改变既有默认。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 129 · glue.edge_distance

- 当前：`glue` / `1.0.0` / 胶路边距测量。目标：`solution-glue`；拆公共能力/适配；**M5**。
- 源码：[packs/glue/ops/edge_distance.cpp:135](../../packs/glue/ops/edge_distance.cpp#L135)。
- 端口：`image:Image`, `bead:Bundle<glue.Bead>`, `calib:Record?` → `edge:Record`, `distanceMean:Measurement`, `distanceMin:Measurement`, `distanceMax:Measurement`, `overlay:Record`。
- 参数单位标注：searchLength=px, farRun=px。
- 依赖：当前直接：OpenCV灰度剖面、平面标定和稳健统计；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image/std-metrology/std-geometry按节点组合；业务策略不反向成为标准包依赖。
- 当前契约：输入 image:Image, bead:Bundle<glue.Bead>, calib:Record? → 输出 edge:Record, distanceMean:Measurement, distanceMin:Measurement, distanceMax:Measurement, overlay:Record。Image+Bead+[calib]→逐站Edge和距离Measurement；多数侧判零件边，沿法向找首个合格强边；无标定px/有标定mm。
- 目标契约：边缘候选/剖面采样归std-image，已标定点距归std-metrology；胶边/零件边选侧与过滤策略留场景。
- 迁移前置：glue.part_edges；glue.bead_width；M2
- 关联算子：glue.part_edges；glue.bead_width
- 验收：反光划痕、首个合格边/最强边差异、多数侧、MAD剔野与标定单位。
- 说明：不能悄悄换成最大梯度位置。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 130 · glue.judge

- 当前：`glue` / `1.0.0` / 涂胶质量判定。目标：`solution-glue`；保留场景职责；**M5**。
- 源码：[packs/glue/ops/judge.cpp:311](../../packs/glue/ops/judge.cpp#L311)。
- 端口：`bead:Bundle<glue.Bead>`, `breaks:Record`, `edge:Record?` → `verdict:Record`, `ok:Measurement`, `overlay:Record`。
- 参数单位标注：widthLimits=px, distanceLimits=px, maxBreak=px, minDefectLength=px。
- 依赖：当前直接：core Record/Measurement；当前通过glue几何头携带OpenCV类型；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image/std-metrology/std-geometry按节点组合；业务策略不反向成为标准包依赖。
- 当前契约：输入 bead:Bundle<glue.Bead>, breaks:Record, edge:Record? → 输出 verdict:Record, ok:Measurement, overlay:Record。Bead/Breaks/Edge→业务verdict/ok/overlay；连续缺陷长度、无胶优先理由、宽/边距0关闭阈值；限值同bead单位。
- 目标契约：判定明确留场景或宿主；可复用std-value区间逻辑，但不得放进std-metrology或把缺测当OK。
- 迁移前置：glue.bead_breaks；glue.edge_distance；glue.bead_width
- 关联算子：glue.bead_breaks；glue.edge_distance；glue.bead_width
- 验收：无胶唯一原因、宽窄胶/边距连续长度、关闭阈值、px/mm混用拒绝及断胶优先级。
- 说明：保留业务verdict不等于所有后续宿主必须使用同套策略。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。

## 131 · glue.synth_break

- 当前：`glue` / `1.0.0` / 断胶缺陷合成。目标：`solution-glue`；保留场景职责；**M5**。
- 源码：[packs/glue/ops/synth_break.cpp:93](../../packs/glue/ops/synth_break.cpp#L93)。
- 端口：`image:Image`, `bead:Bundle<glue.Bead>` → `image:Image`, `info:Record`, `overlay:Record`。
- 参数单位标注：sStart=px, length=px, margin=px, shiftExtra=px, feather=px。
- 依赖：当前直接：OpenCV图像拷贝/羽化，胶路几何采样；当前随包携带：std-image 的 OpenCV/YAML/字典/识码支持目标；不表示此节点逐项执行所有后端。；目标：std-image/std-metrology/std-geometry按节点组合；业务策略不反向成为标准包依赖。
- 当前契约：输入 image:Image, bead:Bundle<glue.Bead> → 输出 image:Image, info:Record, overlay:Record。Image+Bead→人造断胶图/info/overlay；取干净背景平移覆盖，两侧均脏时原图透传info.ok=false；坐标px。
- 目标契约：测试/示例能力单列场景testing可选模块，不默认进入生产能力；真值必须来自成功注入的区间。
- 迁移前置：glue.bead_width
- 关联算子：glue.bead_width
- 验收：两侧脏背景不注入、羽化边界、图像原子复制、info假值不计入有效缺陷样本。
- 说明：不是通用修图/inpaint，也不能将合成验证当现场召回率。 原生场景编排保留时，C++复用通过明确的内部算法库接口；不得用查找注册算子并调用其compute作为最终跨包复用接口。
