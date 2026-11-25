# P5 深度与几何工作流

八个工作流使用正式 LyFlow 调度器与原生点云、图像、张量、索引和变换。需要包含 std-image、std-pointcloud、std-geometry 阶段5算子的 LyFlow 1.3.11 运行库。没有模型下载或外部服务依赖。夹具是本仓库生成的数学场景，不代表真实相机、测量不确定度或生产验收。

## 运行与夹具

从仓库根目录执行：

```powershell
python build/native-sdk/scripts/generate-p5-fixtures.py --output-dir build/p5-regenerated-fixtures
lyflow validate examples/p5-depth-inspection.lyflow.json
lyflow run examples/p5-depth-inspection.lyflow.json --no-cache --outputs --summary
```

生成脚本只需要 Python 标准库；它生成带完整 intensity/normals/RGB 的 PCD、RGBA 图片、相机标定、实例掩膜标注和按真实三维圆盘边界渲染的圆孔图片。[p5-fixtures.json](../examples/p5-fixtures.json) 记录来源、数学真值、误差范围和十份资产的字节数/SHA256。`build/p5-regenerated-fixtures` 是独立再生目录，不覆盖正式示例。

工作流的路径参数均默认相对于工作流所在目录解析。复制、另存或安装示例时，应保留这些 JSON、PCD、PNG 和标注索引的相对目录。标注内部路径相对于索引文件。文件名 `1700000001000-p5-scene.png` 对应明确时间戳，不自动读取“最近一张图”。

主场景为 64×48，fx=fy=120、cx=31.5、cy=23.5，参考面 z=1m。3066 个父云点包含 3063 个可见点、一个同像素后方点、一个视野外点、一个相机后方点；九个像素缺测。凸起区域有 42 个 Z=1.02m 点，凹陷区域有 36 个 Z=.985m 点。正交场景保留有效零高度、负高度与同样的缺测区域。

## 八个工作流

| 示例 | 默认行为 | 合同覆盖参数 | 预期变化 |
| --- | --- | --- | --- |
| [深度检查](../examples/p5-depth-inspection.lyflow.json) | 相机投影→闭运算候选与原观测中值补洞→双向缺陷及六条真实截面 | `defectMode="up"` | 缺陷由凸起/凹陷两组变成仅 42 点凸起；填补、截面与上游保持原结果 |
| [深度滤波](../examples/p5-depth-filters.lyflow.json) | mean、kernelSize=1，真实 XYZ/通道无变化 | `mode="fill"`、`kernelSize=7` | 九个孔洞补值，3063→3072 点；原观测不被当作新增实测 |
| [正交图与掩膜提云](../examples/p5-orthographic-roi.lyflow.json) | height_map 与 orthographic_map；显示原生有符号高度，P4 标注掩膜关联提云 | `outputMode="normal_angle"` | 显示切换为 RGB 法线角图；真实有符号深度、索引与 94 点 ROI 集合不变 |
| [盒体采样与裁云](../examples/p5-box-crop.lyflow.json) | 两个有 ID 的盒体，每面 100 点、种子7；box 裁剪和紧凑移除 | `shape="cylinder"` | 真实有限圆柱成员不同于外接盒；内部和外部仍完整分割 1200 个采样点 |
| [刚体模板与姿态规则](../examples/p5-pose-rigid.lyflow.json) | 三个非共线定位点建立模板，分别做点对距离检查和刚体配准 | `currentC=[0.02,0.12,0.01]` | 只改变观测 C，点对距离与配准残差均超差，配准仍输出最优刚体变换 |
| [顶面投影与平面回投](../examples/p5-pose-project-plane.lyflow.json) | box 顶面真实掩膜，回投 z=1.01m 平面并测尺寸 | `liftMode="center"` | 使用显式已知尺寸 `[.12,.08,.02]`；定位中心仍在参考平面 |
| [空间圆孔](../examples/p5-circles-3d.lyflow.json) | 已知半径 .05m，识别正视实心圆孔并保留真实圆盘投影 | `imagePath="p5-circle-tilted.png"` | 按倾斜圆孔的独立三维真值验证，两歧义解保持明确 |
| [掩膜关联移除](../examples/p5-mask-remove.lyflow.json) | 两个三维实例显式关联 patch-up/down 掩膜 | `topExpand=2` | 删除78→104点；视野外点及相机后方未知点继续保留 |

覆盖参数只影响当前运行，不写回工作流文件。再次不带覆盖参数运行应恢复默认结果：

```powershell
lyflow run examples/p5-depth-inspection.lyflow.json --param 'defectMode="up"' --outputs --summary
lyflow run examples/p5-depth-filters.lyflow.json --param 'mode="fill"' --param kernelSize=7 --outputs --summary
lyflow run examples/p5-pose-rigid.lyflow.json --param 'currentC=[0.02,0.12,0.01]' --outputs --summary
lyflow run examples/p5-mask-remove.lyflow.json --param topExpand=2 --outputs --summary
```

### 深度、观测与缺陷

`cloud.project_camera` 使用真实 K/D、Z-buffer 和源点索引，显示图仍保留原 RGBA，结果云保留源 RGB/intensity/normals。它交付 compact 可见云、二维组织视图以及独立父云索引和遮挡状态，不给 PointCloud 偷加 organized 宽高。

填洞使用 closeKernel=5、fillKernel=7，只从原 observed 邻域取值，不级联用本次补点扩散。新点在新身份云中追加，status=interpolated，`filledMask/filledIndices` 独立标记。默认缺陷/截面不把这些合成点当实测。九个缺测在总画面中占比小于 .01，因此示例 `minCoverage=.99` 可以判定已有真实缺陷；缺少足够观测时返回 unknown，而不是 good。

缺陷量测采用实际观测 Z 重建像素完整四角，再沿基准面法向正交投影。对水平参考面，每像素面积为 `Z²/(fx×fy)`，凸起/凹陷分别使用 1.02² 和 .985² 的比例，不能用参考面 z=1 的射线单元面积代替。体积是中心高度在单元内常值的离散法向柱积分，不是未知像素内部曲面的完整重建。四个属性独立启用，任一严格超过阈值才判退，完整末行末列参加处理。具体数学真值以夹具清单中的 up/downAreaM2、up/downVolumeM3 为准。

截面是组织三角网格与真实三维切平面的交线，保留分链及源三角顶点索引。图参数 `aType/bType` 支持六种实际特征：left-right-inflection、right-left-inflection、top、bottom、left-most、right-most；`profileDirection` 为区域 x/y，`attributeMode` 为 x-x/y-y。梯度不跨孔洞链，缺支持属性以 attributeValid 明确标记。示例默认测完整横向跨度，避免用极窄默认兴趣区误导业务结果。交点 intensity/RGB 重心插值，法线归一化并报告退化情况。

### 滤波与正交 ROI

滤波图通过 `mode` 支持 mean、median、gaussian、fill、eliminate 五模式；`fillMethod` 为 idw/min/max，`direction` 为 x/y/xy。可显式选择奇数 kernelSize。默认 kernelSize=1 的 no-op 是几何合同：新云身份可以变化，但原 XYZ 和全部通道值应保持。真正改变深度时沿原射线回投，结果云不混入被替代的噪声点；父索引单独追踪。

正交图使用 resolution=.01m，像素0中心 [-.315,-.235]，三维 gridFrame/cloudFrame 为 workspace，二维 imageFrame 为 image。outputMode 支持 height、normal_angle、pseudo_height 三图；真实 depth/status 始终独立，不能把合法零/负高度当无效。P4 标注的真实多层掩膜裁云，三组为 patch-up 42点、patch-down 36点、hole-region 16点，总计94；缺测九像素不被框选操作补成观测。`selectedRoi` 只选择组织深度视图，分组集合仍保留全部 ROI。

### 有限体、姿态与圆孔

盒体 A 全尺寸 [.125,.125,.03125]、B [.0625,.0625,.0625]，中心分别为 [0,0,.5]、[.25,0,.5]。这些二进制可精确表示的边界便于验证 f32 采样和 closed 裁剪。圆柱模式的 X/Y 直径必须一致，因此不能用不等边长方体假装圆柱。图参数 `shape` 控制查询几何，`maxCount` 独立控制计数分类。同一份查询的命中索引接入 `segment.extract_indices`，其 `selected` 和 `rest` 分别交付 `cropped` 和 `outside`。紧凑删点后的剩余云就是 `outside`。保序移除由原生专项测试覆盖。

刚体图用 A=[0,0,0]、B=[.1,0,0]、参考 C=[0,.1,0] 构建模板。图参数只绑定 `currentC.position`。`pose.rigid_check` 比较观测与模板的点对距离。`pose.rigid_register` 计算刚体变换并输出配准残差，下游把实际 `Transform` 应用到观测位姿。配准可观测但超公差时交付候选变换并返回 `accepted=false`。定位点退化时返回 `unobservable`。支链还示范有限对称翻转、显式连续轴对称旋转和二维工件向三维平面的嵌入。

顶面投影图的 CameraCalib.frame=camera 与二维 imageFrame=image 分开。原盒体中心 z=1、厚度 .02，顶面 z=1.01；回投定位中心就在该参考面，不自动减半厚度猜测原实体位于哪侧，也不声称恢复原盒体体积中心。dimensions 模式由真实掩膜的完整半像素足迹测尺寸，存在离散像素量化；center 模式只使用已知尺寸。`surface` 另可选 full，并据真实有限体射线支持处理，不填外接轮廓中的未知区域。

圆孔图以实际黑孔边界表示已知半径；具有厚度的黑环有两个不同边界，不能把中线半径强加给其中一条。每个空间圆保留与实际相机约束一致的解，法向按无向平面比较，attributes.shape=circle 与真实圆盘投影关联。倾斜图至少一个解必须满足清单 centerToleranceM 和 normalCosineMinimum，不能只检查非空。所有几何输出仍 pickable=false。

掩膜移除只通过三维实例 attributes.primitive2DId 显式关联真实检测 ID，禁止按数组位置猜测，也不会偷偷添加三维体积裁剪条件。topExpand=2 按每列原掩膜最上方增加两行，两个区域共增加26个删除点。原图之外与没有可信正深度的点继续保留并分开统计。也可在掩膜移除后的云上用 `cloud.query_primitives` 查询三维有限体，再用 `segment.extract_indices.rest` 紧凑删点，或用 `cloud.remove_indices` 保序移除。

## 正式合同验证

只检查八图结构，不需要原生运行库：

```powershell
python tests/release/p5_contract.py --schema-only
```

完整验证使用已经构建的正式 CLI，不自动构建、不跳过缺算子或坏夹具：

```powershell
python tests/release/p5_contract.py --cli build/rust/release/lyflow.exe
```

可用 `--examples <安装SDK/examples>` 验证安装后的文件，`--output-dir` 指定日志目录。脚本先核十个夹具的尺寸/SHA256，把八图和相关文件保存到新的 Unicode 目录。每张图运行一次默认参数。深度滤波图和位姿配准图额外运行覆盖和恢复默认，共 12 次校验与 12 次执行。每轮检查正式 summary、原生大载荷元信息和业务 ID 等结构契约，全部源文件的摘要在测试后复核。物理真值在原生测试验证，其中正面和斜面圆孔由 `packs/std-image/tests/test_p5_inspection.cpp` 覆盖。

点云 ID 只在进程内有效。脚本在示例副本中加入 `cloud.statistics`，逐次核对 `sourceCloudId`、`parentCloudId`、`resultCloudId` 和 `sliceCloudId` 的绑定，然后按角色比较各次输出。工件 ID、掩膜 ID、帧名与几何摘要均参与比较。CLI 交付云、掩膜与四边形的元信息。原生集成测试通过深度检测图，从 C ABI 和动态 C++ SDK 读取完整载荷，核对索引与来源点云的绑定。Rust CLI 经 Rust 客户端执行，Rust SDK 的原生数组读取需单独验证。

独立安装的 C++17 SDK 门禁使用 `tests/release/p5_sdk_consumer`，必须显式传入 `LYFLOW_CONSUMER_VERSION=X.Y.Z` 并以 EXACT 查找安装包，编译时不读取源码构建路径或 vcpkg。`p5_sdk_contract.py --sdk <安装目录> --consumer <编译后的程序> --bait-dir <构建bin> --output-dir <日志目录>` 实际运行八图、核结构契约和资产不变，并在 Windows 暂移SDK内OpenCV依赖时验证有效CWD/PATH诱饵仍被拒绝。该门禁纳入原生Windows CI；不能把旧补丁客户端兼容运行冒充当前头文件重新编译。

本轮 Windows 本地完整验证实际执行七个算子包、P1四模型、P4分割/OCR真实模型与全部平台回归：691组、370939断言通过，0失败/0跳过。最终DLL安装后，八图CLI默认→覆盖→默认24次通过，当前1.3.11 EXACT/C++17独立消费工程的八图和依赖诱饵负例通过。编辑器94项及React18/19类型检查通过。CI与Codex结论仍以PR最新提交为准，这些软件证据不替代现场精度、吞吐或安全验收。

详细算法、预算和坐标契约见 [深度投影](p5-analysis-projection.md)、[点云检查](p5-analysis-cloud-inspection.md)、[姿态几何](p5-analysis-pose-geometry.md)及 [ADR-0031](adr/0031-depth-grid-association-and-derived-observations.md)。
