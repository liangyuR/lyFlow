# P2 A05：高光直线检测

[文档中心](README.md) · [六阶段交付](p2-sequential-delivery.md) · [补齐计划](operator-completion-plan.md#a05) · [能力对照](operator-coverage.md) · [图像包](../packs/std-image/README.md)

本项是用户授权的六阶段交付中的阶段1，对应外部 `DetectHighLights` 的高光直线、分组、旋转区域和叠图用途。高光核心用途已实现并通过本地专项和完整适用回归；阶段1仍进行中，需对应PR的CI与Codex审查通过并合并后才能开始下一阶段。本页记录源码分析及实际契约，不将后续阶段提前标为完成。算子位于std-image，复用已有OpenCV和原生Image/Record/Pose契约，不新增PCL依赖、Data种类或C ABI。

## 源码核对与独立实现选择

逐行阅读本机 `xyz-vision-lib/src/xyz_vision_lib/models/2D_Processing/DetectHighLights.cpp` 及 `include/xyz_vision_lib/models/2D_Processing/DetectHighLights.hpp`。参考文件带 proprietary 声明，仅用于核对用途与边界；LyFlow 独立实现，不复制函数体。本机参考目录没有Git元数据，不能声称等同原始清单标注修订或旧Qt配置协议。

| 核对项 | 参考实现的静态行为 | LyFlow实施要求 |
|---|---|---|
| 图像与颜色 | 仅接受 `CV_8UC3`，按BGR灰度转换；输出保持原快照元数据。 | 通过现有适配层遵循RGB/RGBA约定，明确u8/u16/f32范围，避免直接搬用BGR转换。 |
| ROI | 读取工件列表中的第一个矩形，使用其轴对齐包围范围扫描，并用旋转多边形测试排除外侧；ROI列表其余元素不处理。 | 显式区分轴对齐Box2D和单个旋转Pose2D区域，校验像素单位/参考帧/互斥输入，不默默丢弃额外ROI。 |
| 扫描上界 | ROI下界使用半开 `max_y`，列窗口上界又以包含等号的方式遍历到 `max_y`；当ROI触及图底且窗口扩张时存在读取第rows行的路径。 | 所有像素采样使用明确闭/开边界及ROI有效域；图底、图边及旋转ROI外部需要永久反例。 |
| 局部峰值 | 当前点只在邻点严格更亮时被排除，因此亮平台中的同值像素都可能成为极值。 | 使用明确亮度和双侧对比证据，处理平台中心，不把均匀亮背景直接当高光线。 |
| 固定参数 | 阈值50、最小域面积200、膨胀核、Hough票数/最短线/间隔等写在流程内；用户min_length主要用于末端筛选。 | 检测门限、连续性、最短长度、间隔和拟合质量均由明确契约控制；不以宽松结果掩盖低支持或超限。 |
| 垂线与角度 | 角度由dy/dx再atan计算，合并时还在整数中用dx作为除数；垂直线存在零分母路径，端点方向和整数除法影响结果。 | 使用二维向量和无向角距离，支持垂线，明确角度周期和单位；几何拟合不通过整数斜率或固定中点外推。 |
| 分组与拟合 | 先按长度排序，再按角差与ROI中线处y差合并；扩展一侧端点后未重新拟合支持点，最后score取长度。 | 基于实际支持点的距离、角度、投影间隔及覆盖率分组并重新拟合；近平行线、交叉、断裂及曲线必须有反例，质量分数不能冒充识别概率。 |
| 可视化与取消 | 在克隆图上画线与矩形；遍历全图像素、连通域和Hough等操作，running_info未参与取消。 | 保持输入只读，叠图和mask走原生Image；逐阶段检查取消并限制像素/候选/拟合预算，不把大数组写入Record。 |

这些是对当前源码的静态观察，没有运行或编译外部 proprietary 程序，不声称已复现其崩溃或现场误检率。外部BGR约定本身不被当成缺陷，重点是跨库接入必须显式转换。

## 交付和验收口径

旋转区域使用 `vision.Poses2D.v1`；现有原生 `Box2D` 是轴对齐矩形，不能被描述成已经支持旋转。有限线段端点、分组身份和拟合质量需与相应Pose对应；坐标与长度恢复到原图像素，掩膜/叠图保留图像比例。无目标是有效空集合；非法输入、无法处理的配置和资源超限应报告诊断。

核心软件验收包含水平/垂直/斜线、部分裁剪和旋转ROI、亮平台、低对比和噪声、邻近平行线、断裂长度两侧、曲线/短段拒绝、稳定顺序、颜色/位深/alpha、只读输入、预算和取消，以及真实调度器的Full/Preview原图坐标一致性。量测阈值需对应实际输出的质量；原始生产重复性与光照/材质适用范围仍需实测样本，不以合成线条代替现场验收。

## 执行契约

以下契约与当前实现源码及永久回归逐项对应；最终合并状态由对应PR给出。

| 端口 | 数据及语义 |
|---|---|
| 输入 `image` | u8/u16/f32的灰度、RGB或RGBA原生图像；f32为有限0..1，沿用RGB色序。 |
| 可选 `roi` | 像素轴对齐Box2D，与 `region` 互斥。 |
| 可选 `region` | 单个 `vision.Pose2D.v1`，像素单位、帧匹配的旋转矩形；与 `roi` 互斥。 |
| 输出 `instances` | `vision.Poses2D.v1`旋转区域，`pickable=false`，ID与segments一致；confidence是几何/对比质量，不是识别概率。 |
| 输出 `segments` | `image.Highlights.v1`，`data={frame,unit:"px",items:[...]}`；每项含 `id`、两点 `endpoints`、`length`、`rms`、`maxError`、`coverage`、`support`、`meanContrast`、`score`、弧度 `angle`。坐标、长度、RMS和最大正交误差使用原图像素。 |
| 输出 `mask` | 原生u8候选亮脊图，非最终旋转区域填充图。 |
| 输出 `image` | 源图的叠图副本，保持位深、通道和alpha，源图只读。 |

`frame`是调用方显式声明的图像参考帧标签，不推断设备来源或全局帧树。Box2D由引擎按工作图比例换算；Record不自动换算，因此旋转region的中心/尺寸由算子按Image.scale显式换到工作像素。输出Pose和线段Record再恢复原图像素；输出Image.scale保持输入比例。不能把手动恢复后的Record再次套原生像素几何的自动恢复。需要最终旋转区域填充图时，可将原图与instances连接既有 `image.instances_mask`；调试亮脊用mask，查看线段/矩形叠画用image输出。

| 参数 | 默认值 | 原图或无量纲范围 |
|---|---:|---|
| `frame` | image | 图像参考帧，必须与输入region匹配 |
| `minBrightness` | 0.3 | 归一化[0,1] |
| `minContrast` | 0.05 | 归一化(0,1] |
| `ridgeRadius` | 4px | [1,64]原图像素 |
| `smoothSigma` | 1px | [0,16]原图像素，0关闭平滑 |
| `minLength` | 30px | [2,1000000]原图像素 |
| `maxGap` | 10px | [0,1000000]原图像素 |
| `maxResidual` | 1px | [0.1,64]原图像素 |
| `boxHeight` | 8px | [0.25,1000000]原图像素 |
| `referenceAngle` | 0° | [-360,360]；与angleTolerance按无向mod180比较 |
| `angleTolerance` | 90° | [0,90] |
| `minSupport` | 12 | 整数[2,4096] |
| `minCoverage` | 0.5 | [0,1] |
| `mergeDistance` | 2px | [0,64]原图像素 |
| `mergeAngle` | 2° | [0,90] |

像素参数按原图声明，预览的工作像素值由图像比例转换，校验仍采用对应原图范围。`ridgeRadius`在工作像素中四舍五入并至少为1；为抑制归一化浮点平面的数值噪声，亮脊对比度还必须严格大于1e-6。`minSupport`统计实际工作图上的亮脊点，不按原图比例伪造点数。

`coverage`是沿拟合线的支持区间并集长度除以线段长度。每个工作像素点投影区间半宽为 `(|dir.x|+|dir.y|)/2`，并与段首尾相交；分组时以相邻投影间距减去同一像素投影宽度定义缺失间隙，超出 `maxGap` 才拆开。Hough仅产生整数种子。先在整数端点量化误差包络内检查33个方向，寻找最大支持的法向窄带，以该窄带的TLS方向初始化，再按精化方向重新收集支持点并进行法向分带，避免邻近平行亮脊被拟合成中间无支持的假线；随后按沿线投影间隙分组，经TLS重拟合、残差修剪和最终重拟合，检查最少支持、长度、RMS、每点最大正交误差、覆盖率和无向角度。严格angleTolerance只对最终TLS方向生效，不用整数Hough种子的角度提前拒绝合法窄角目标。RMS与maxError均不得超过maxResidual。对于最多64个亮脊点，增加有界成对种子以覆盖Hough可能遗漏的三像素短线，不放宽最终质量门限。

质量分数与Pose的confidence均为 `coverage * clamp(meanContrast,0,1) / (1 + rms/maxResidual)`，不是识别概率，也不表示可抓取。方向规范化为x非负、近垂直时y为正，angle使用atan2的弧度；方向差按modπ处理。候选按长度降序，其后按RMS、端点及支持身份排序；重复去除还要求方向/距离合格且共享至少80%较短支持集，不跨间隙补造支持。ID按输出顺序生成，重复运行可比较，不承诺跨帧实例跟踪或Full/Preview排名完全相同。

实际工作图最多8Mi像素，输入与工作缓冲估算不超过512MiB；预检还按 `360 * (2*(width+height)+1)` 个极坐标cell、每cell20B，保守合计IPP最大候选容器与标准Hough累加器，因此极长窄图不能仅凭像素总量绕过内存预算。亮脊点不超过262144，Hough/短线种子及拟合候选最多4096，累计拟合/距离检查不超过64Mi次，候选支持索引最多32MiB，最终线段最多256。Record最多1MiB并沿用深度/字符串限制。超限诊断而不暗中截断目标；OpenCV内部单次调用不宣称可抢占。

阶段合并还需最新提交的CI与Codex审查通过，见[交付门禁](p2-sequential-delivery.md#每阶段的合并门禁)。

## 示例与永久验证入口

[示例图](../examples/p2-highlights.lyflow.json)读取随附的[合成高光PGM](../examples/p2-highlights.pgm)，连接旋转ROI，导出旋转区域、线段质量、叠图和候选亮脊图。顶层图参数可覆盖输入路径、最短长度、参考方向和方向容差：

```powershell
lyflow validate examples/p2-highlights.lyflow.json
lyflow run examples/p2-highlights.lyflow.json --outputs --summary
```

实际实现见[image_highlights.cpp](../packs/std-image/ops/image_highlights.cpp)，永久功能回归见[test_highlights.cpp](../packs/std-image/tests/test_highlights.cpp)，跨执行器/C ABI/C++ SDK的例图与方向覆写核对见[集成回归](../core/tests/integration/p2_highlights.cpp)。当前源码、文档和例图共同构成可审查对象，最终运行结果以对应PR最新源码修订的完整验证为准，不混用不同可选包或过滤运行的测试数字。

## 本地验证记录

当前源码修订的原生完整适用回归为288组、18411断言，0失败、0doctest跳过；真实执行器/C ABI/C++ SDK专项为12组、1347断言。编辑器94项全部通过，typecheck、schema与格式检查通过。CLI实际执行示例的默认、方向覆写、复位分别得到2、1、2条线段，核对原图中心/长度；CMake SDK安装布局包含工作流和PGM。实现验证扫描60种角度的单线、窄方向容差和2px近平行双线；独立审查另完成204组动态及Full/Preview、ROI、资源边界核对。上述是本地软件证据；CI/Codex与合并门禁仍须跟踪当前PR。未配置ONNX的旧ML可选分支不计为本阶段新增能力验证，合成图也不替代材质/光照的现场样本。
