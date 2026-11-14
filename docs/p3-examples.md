# P3 可执行示例

示例位于仓库与SDK的 `examples/` 目录，所有资产使用相对图文件的路径。识别码和工作空间图需要std-image、std-geometry及固定OpenCV/STag字典依赖，两张CAD图还需要std-pointcloud。PNG、STL、相机、工作空间和许可应与图一起保留。

六张图已通过JSON Schema静态检查，并在Windows x64、产品1.3.9、OpenCV5.1.0当前构建的真实CLI完成validate与run，全部退出0、summary.status=ok。这里验证合成示例的组合链路，不代表相机标定精度、现场误报率、抓取安全或生产吞吐验收。

```powershell
lyflow validate examples/p3-marker-workspace.lyflow.json
lyflow run examples/p3-marker-workspace.lyflow.json --no-cache --outputs --summary
lyflow run examples/p3-stag-workspace.lyflow.json --no-cache --outputs --summary
lyflow run examples/p3-workspace-native.lyflow.json --no-cache --outputs --summary
lyflow run examples/p3-workspace-legacy.lyflow.json --no-cache --outputs --summary
lyflow run examples/p3-shape-cad.lyflow.json --no-cache --outputs --summary
lyflow run examples/p3-shape-parallel.lyflow.json --parallel 2 --no-cache --outputs --summary
```

这些命令也适用于SDK根目录；本次本机验证使用 `build/rust/debug/lyflow.exe`，SDK使用 `bin/lyflow.exe`。CLI stdout为JSON Lines，`--outputs`中大型原生数据仍由运行库承载，图文件没有Tensor数值正文。

图级参数可以按次覆写，无需同时修改绑定节点：识别码图的markerSize声明米制边长；工作空间图的targetFrame同时绑定加载目标帧与往返节点；单模型图的modelId、并联图的leftModelId/rightModelId控制模型与输出实例身份。

```powershell
lyflow run examples/p3-marker-workspace.lyflow.json --param markerSize=0.08 --no-cache --outputs --summary
lyflow run examples/p3-workspace-native.lyflow.json --param 'targetFrame="world"' --no-cache --outputs --summary
lyflow run examples/p3-workspace-legacy.lyflow.json --param 'targetFrame="Camera7"' --no-cache --outputs --summary
lyflow run examples/p3-shape-cad.lyflow.json --param 'modelId="cad-L-alternate"' --no-cache --outputs --summary
```

## 识别码到工作空间

[识别码图](../examples/p3-marker-workspace.lyflow.json)将PNG与 `vision.CameraCalib.v1` JSON分别接入 `image.marker_pose`，再把相机帧中的实例与 `workspace.load.toWorkspace` 接入 `pose.transform_3d`。输出保留原生cameraPoses、workspacePoses、逐码质量诊断与叠图。

ArUco夹具为 `DICT_4X4_50` ID7、物理边长0.1m。相机真值位置[0.12,0.06,0.65]m；工作空间帧真值[0.22,-0.24,0.95]m。图保留重投影与平面双解拒绝门限，检测质量不合格时不会制造位姿。

[STag图](../examples/p3-stag-workspace.lyflow.json)使用真实HD15 ID0图样及单独1000×1000相机文件，不把ArUco图当作STag。该相机只为合成示例声明，不是设备标定。原图正视几何仍受平面双解检查，结果应连同detections中的accepted/reason/rms查看。

## 工作空间格式与体积

[原生图](../examples/p3-workspace-native.lyflow.json)读取 `p3-workspace.json`。文件声明schemaVersion=1、unit=m、world根帧、camera/bin_frame子帧和底面锚点体积。输出camera帧中的体积中心[-0.1,0.3,0.3]m；经toWorkspace后中心为[0,0,0.6]m，再经fromWorkspace回到原中心，尺寸始终为[0.8,0.6,1.2]m。

[外部YAML图](../examples/p3-workspace-legacy.lyflow.json)显式选择 `xyz-unversioned-yaml`、`legacyUnit=m`、`legacyRootFrame=World`。YAML只有pose、length/width/height和成套父帧字段，没有被编造的协议版本。图输出World中的中心[0.3,0.1,0.9]m；局部体积中心仍为[0,0,0.6]m。这里的pose是底面，标准Pose输出是体积中心，二者相差半个高度。

两个工作空间图都把region和双向原生Transform接入现有位姿变换节点，实际消费帧树与体积，不停留在元数据容器。

## CAD采样、投影与形状匹配

[单模型图](../examples/p3-shape-cad.lyflow.json)依次执行：

1. `io.load_cloud`按固定seed对米制L形STL的三角面积采样150000点。
2. `cloud.to_depth_image`用显式针孔内参输出128×128的f32深度图；无投影像素为0，平面为1m。
3. `image.range_mask`选取0.5..1.5m范围，形成u8二值投影图。
4. `image.shape_template`从该图提取梯度几何，输出原生features Tensor与小型model Record。
5. `image.shape_match`在独立场景PNG中搜索该模型。

范围掩膜接的是template.image，未同时接template.mask；否则会把轮廓外侧排除，损失要训练的边缘。这是离散点采样投影，不是连续三角网格光栅器。模板中心由实际特征包围区域定义，不能等同于CAD物理重心。

[并联图](../examples/p3-shape-parallel.lyflow.json)使用独立L/H CAD投影、建模及匹配分支，共享场景图像，末端由 `pose.concat` 拼接。两分支没有相互依赖，可由 `--parallel 2` 调度；这是多个真实匹配节点，不声称单节点完成多类融合或跨模型去重。模型id为cad-L/cad-H，输出同为image帧、px单位。

夹具固定搜索角0°和尺度1，目的是检查完整组合链路。匹配门限为coverage≥0.85、score≥0.7、支持RMS≤1.5px，未为了示例通过而关闭质量门控。单模型中心真值为实际template.model.center+[90,60]px；并联L/H分别加[60,60]、[200,120]px。完整预设真值见 [p3-fixtures.json](../examples/p3-fixtures.json)，它不作为图输入。

## 资产与验证边界

来源、许可、尺寸和采样定义见 [p3-assets.md](../examples/p3-assets.md)，第三方完整文本见 [p3-asset-licenses.txt](../examples/p3-asset-licenses.txt)。STag图样来自固定HD15字典，不编译上游ED/LSD。

本次执行使用图中原始参数、`--no-cache --parallel 2 --outputs --summary`，没有运行时覆写，也没有降低质量门限。逐图结果如下。

| 图 | 真实输出与误差 |
| --- | --- |
| marker-workspace | ID7，1个位姿；相机位置误差4.733mm，在夹具预设6mm范围内；重投影RMS 0.0874px，双解标识false。工作空间位置[0.220966907,-0.239612304,0.954616949]m。低重投影RMS不能代替米制真值误差。 |
| stag-workspace | ID0，1个位姿；相机z=0.125018m，重投影RMS 0.0129px，双解标识false。相机主点500与像素外框中心499.5的半像素差已计入夹具真值。 |
| workspace-native | 原中心[-0.1,0.3,0.3]m；局部中心[0,0,0.6]m，float32误差约1.7e-8m；往返中心与尺寸符合预设值。 |
| workspace-legacy | World中心[0.3,0.1,0.9]m；局部中心[0,0,0.6]m，误差约1.7e-8m；往返与尺寸符合预设值。 |
| shape-cad | 1个cad-L实例，中心[151,124]px；与实际模型中心加预设平移的误差小于1e-6px；66/66特征支持。 |
| shape-parallel | cad-L/cad-H各1个，中心[121,124]与[264,184]px，concat后2个；66/66与88/88特征支持，score>0.9999998、RMS<1e-6px。事件中match_l与match_h均先进入running，再完成，确认实际分支并行。 |

这些轮廓夹具与投影栅格精确对齐，因此形状匹配误差很小；它们不检验亚像素相机噪声、遮挡或未知物体下的现场准确率。永久算法测试承担那些已声明的变换与拒绝场景。当前本地证据在 `build/p3-examples-review/results.json` 与各图JSON Lines日志，属于验证记录，不是产品身份指纹。

本批交付还完成了安装目录隔离验证：PATH仅含System32时，安装后的六张图全部通过。临时移出安装目录中的opencv_geometry510.dll，并把有效同名DLL分别放在CWD和PATH，加载仍按预期拒绝；随后恢复原文件。加载器允许runtime目录和System32，不通过工作目录或PATH补齐缺失的这项依赖。C++ Client头也已在不引入vcpkg、Eigen的独立消费者中编译通过；这不表示运行库本身没有第三方依赖。

永久跨接口回归位于 `core/tests/integration/p3_calibration.cpp`（四张识别码/工作空间图）和 `p3_shape.cpp`（两张CAD图）。每例把图与依赖资产保存到独立Unicode临时目录并重载，按默认→运行覆写→默认三轮分别执行原scheduler、真实C ABI和动态C++ Client SDK，共54次图执行。检查预设真值、稳定Record/Transform、覆写隔离与磁盘图恢复；CAD例另通过原生Tensor借阅接口检查Nx4特征、单位法向和跨接口数据一致性，不把特征正文转存为工作流JSON。图像集成只在所需图像/几何包开启时编译，CAD集成另要求点云包。

本次Windows完整core套件已通过564组、137237断言、0失败、0跳过，其中包含上述六例；证据为 `build/core-full-p3-final.log`。这是本次构建的测试结果，不是只统计P3的数量。
