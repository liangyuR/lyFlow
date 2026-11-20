# P3 B03：可消费的工作空间文件与参考帧树

本项独立实现 `workspace.load`，源码位于 [p3_workspace.cpp](https://github.com/liangyuR/lyFlow/blob/v2.0.3/packs/std-geometry/ops/p3_workspace.cpp)，永久回归位于 [test_p3_workspace.cpp](https://github.com/liangyuR/lyFlow/blob/v2.0.3/packs/std-geometry/tests/test_p3_workspace.cpp)。它从文件建立本次执行的明确参考帧树，输出工作空间体积 Pose 和双向原生 Transform，可直接接既有位姿变换、点云变换和邻域工作空间输入。它由 std-geometry 的 workspace-io 模块注册。默认构建启用该模块，精简构建可显式选择。

## 参考源码与实际资产边界

已逐行阅读本机 `xyz-vision-lib` 的 `Input/LoadWorkspaceInfo.cpp/.hpp`、`property_settings.yml` 对应默认值及参数说明，并核对 `LoadCalibrationInfo` 的 workspace 分支、`LoadCameraCalibInfo` 的相机分支、`FrameTree` 和 `CutCloudByWorkspaceV2` 消费路径。参考文件声明 proprietary，只用于核用途、数据方向和失败边界；本实现没有复制专有函数体。

本机参考目录没有 `.git` 元数据，不能证明与清单中的 `72d…` 或任何上游提交一致。`test/data/integration_test` 没有实际展开，只有 DVC 索引；索引列有 `bag_depalletize/calibration/workspace/ws_0.yml`（99B）及 `tube_350/calibration/workspace/工作空间标定.yml`（505B），但本地不存在其载荷。没有下载云端资产，也不把从字段规则制作的测试数据称为外部实采文件。

| 核对项 | 参考源码的静态语义 | LyFlow 明确处理 |
| --- | --- | --- |
| 独立工作空间文件 | 从根节点读取 `pose`、`length`、`width`、`height`。Eigen YAML 转换器要求 pose 为嵌套4×4行矩阵。 | 仅显式选择 `xyz-unversioned-yaml` 时接受这一结构子集，不自动猜格式。 |
| 版本与单位 | `LoadWorkspaceInfo` 没有读取 workspace 文件 version/unit；`WorkspaceV2` 是 C++ 类型名。 | 不把 V2 称为文件版本2；旧 YAML 必须由调用者显式选择 `legacyUnit=m`，默认 unspecified 会拒绝。 |
| 相机标定 | 另一文件/加载路径读取 `version=1`、`frame_type/id` 等字段。 | 相机格式和未知顶层字段明确拒绝，不据 `pose` 存在就认作 workspace。 |
| 参考帧 | 默认世界父帧；若有 parent_id，再读 parent_frame_type/parent_pose；父帧姿态置于世界下，workspace.pose 置于该父帧下。 | 三个 parent 字段必须同时提供；父帧ID按已知外部规则 type+id 形成，显式根名由 legacyRootFrame 声明。 |
| 外部状态更新 | 全局帧树中已存在父帧时不重新加载其 pose；已存在 workspace 时有未校验的动态类型转换。 | 每次从完整输入重建本地树，验证全部节点，无跨运行残留和全局树副作用。 |
| 体积锚点 | 工作空间局部XY范围为 ±length/2、±width/2，Z范围为0..height；原点为底面中心。 | 原生 workspace frame 保留底面中心；输出标准 Pose3D 位于体积中心，即沿局部+Z移动height/2。 |
| Primitive 覆盖 | 仅使用列表第一项；Top 沿局部-Z移动完整height得到底面；非World父帧仅记录告警。 | 可选 overrideRegion 必须是单个、同目标帧、米制标准中心 Pose3D；不接含糊的首项列表或外部顶面 Pose。 |
| Camera/Qt/离线模块 | 检查相机ID，Virtual路径可替换文件；更新全局树、写日志并透传原Color/Depth/Cloud。 | 不构造不存在的传感器、相机附加标定、请求模式或日志副作用。明确由上游选择文件路径、帧及数据变换链。 |

以上为静态分析，没有执行专有程序，不声称复现外部现场缺陷。

## 原生 JSON v1

`format=lyflow-json-v1`（默认）要求恰有以下五个根字段，版本和米制单位显式声明：

```json
{
  "schemaVersion": 1,
  "unit": "m",
  "rootFrame": "world",
  "frames": [
    {"id":"camera","parent":"world","transform":[1,0,0,1,0,1,0,2,0,0,1,3,0,0,0,1]},
    {"id":"tote-frame","parent":"camera","transform":[1,0,0,0.1,0,1,0,0.2,0,0,1,0.3,0,0,0,1]}
  ],
  "workspaces": [
    {"id":"workspace","frame":"tote-frame","size":[0.6,0.4,0.3],"anchor":"bottom"}
  ]
}
```

`transform` 为行优先16项、**child→parent** 刚体矩阵，平移单位米。rootFrame 隐式恒等，不能在 frames 中再次定义。节点可乱序；全树必须最终连接该根，拒绝重复ID、缺父级、自环/多节点环、未知字段、镜像、尺度、剪切、非齐次末行及非有限值。旋转正交数值容差为1e-8。标识为1..128字节、非空且不含控制字符。

workspaces 的 id 唯一，frame 必须引用树内节点，size 为三个正的米制尺寸。anchor 只接受 bottom 或 center；两者都输出**中心位置**的标准体积 Pose。frame 可以是根；但 overrideRegion 不允许移动根帧。单个坐标、平移、尺寸及合成帧平移的绝对值上限1e12m；这不是可实现的测量精度承诺，原生交付还有独立精度门限。

## 显式无版本外部 YAML 子集

选择 `format=xyz-unversioned-yaml` 且 `legacyUnit=m` 后，读取已核对的字段：

```yaml
pose:
  - [1, 0, 0, 0.1]
  - [0, 1, 0, 0.2]
  - [0, 0, 1, 0.3]
  - [0, 0, 0, 1]
length: 0.6
width: 0.4
height: 0.3
```

可选 `parent_id`、`parent_frame_type`、`parent_pose` 必须全有或全无，parent_pose 同为嵌套4×4、parent→root。已核对的父帧类型为 World/Camera/Workspace/Hand/Robot/Joint/Custom；根同名父帧的 parent_pose 必须恒等。`legacyRootFrame` 默认 World，workspaceId 同时声明外部文件对应的 workspace frame 名，拒绝与父/根名冲突。

该文件没有自带单位或 workspace ID，这两个事实通过显式参数体现；不会根据数字大小推测毫米或米，不把文件名、相机编号或运行时 ws_id 隐式拼成未声明的帧。拒绝 version、相机字段、OpenCV matrix 映射、重复字段、多文档、anchor/alias 和未知结构；实际外部资产若不是这一已知结构，应先提供其真实协议，不能当通用 YAML 兼容成功。

## 输出和消费方向

| 输出 | 语义和直接消费 |
| --- | --- |
| region | 单个 `vision.Pose3D.v1`，frame 为 targetFrame，ID为选中 workspaceId，size 为完整体积尺寸、位置为体积中心、pickable=false。可接 `cloud.neighbor_height.workspace` 等同帧体积输入。 |
| fromWorkspace | 原生 float32 Transform，workspace frame→targetFrame；bottom frame 的Z零点仍在底面。可接 `transform.apply` 或 `pose.transform_3d`。 |
| toWorkspace | 原生 float32 Transform，targetFrame→workspace frame；可把场景转入工作空间局部坐标。 |
| workspace | `geometry.Workspaces3D.v1` 有界快照：显式根/目标/工作空间标识、局部及root变换、所有工作空间尺寸/锚点、文件格式/路径/内容指纹、覆盖来源和原生角点误差。 |

targetFrame 为空时使用文件声明的根；非空时必须属于树内。双向变换的源目标方向不依赖隐式坐标树，标准Pose消费时仍应在下游配置相同 sourceFrame/targetFrame。完整测试包含实际调用既有 `pose.transform_3d` 将加载后的体积转回其 workspace frame。

overrideRegion 使用现有标准**中心** Pose，不是外部原语的Top/Bottom约定。对 bottom workspace，会先从中心减去局部+Z方向的sizeZ/2得到 frame 原点，再根据该节点的父帧变换更新本次树；center workspace 直接使用中心原点。新尺寸来自该 Pose，工作空间ID保持文件中身份，overrideId独立记录。它不修改磁盘文件；树内后代会依照更新后的同一父子关系重算。拒绝把工作空间自身或其后代作为覆盖的目标帧，避免自引用移动；同帧、单位及有限尺寸必须通过校验。

## 原生精度、资源和取消

不只检查double矩阵可解析。交付前检查 float32 系数、右手刚体及完整体积八角点：比较 double→native 前向坐标、目标→workspace坐标、原生往返和标准Pose边界。最大误差必须不超过 maxTranslationError（默认1e-5m）；旋转系数量化不超过 maxRotationError（默认1e-6，系数差而非弧度）。大坐标下精度不足会明确失败，不能以漂亮double配置掩盖原生点云链误差。调用者可显式调整容差，并从 nativeCornerError 检查实际交付误差。

每文件最多1MiB，最多256个含根参考帧及256个工作空间；JSON/YAML深度最多16，解析事件/节点最多32768，快照沿用Record的1MiB/深度/字符串限制。YAML先经有界事件检查，再构造节点，拒绝alias/anchor。JSON重复键在解析时拒绝；不依赖最后一个同名字段覆盖。读取、解析、解树、发布前轮询取消；错误/取消不发布四端口中的部分结果。

算子关闭结果缓存，每次读取文件，不让同路径、同大小甚至同mtime的标定覆盖命中旧结果。contentFingerprint 是读取字节的FNV-1a 64位诊断指纹，不是签名或真实性校验。原始输入Pose与文件保持不变；不自动读入关联相机或其他文件，也不以有效文件代表相机/机械臂标定或生产就绪。

## 验证入口

永久测试覆盖乱序两级参考帧、底面/中心锚点、非根目标帧、双向坐标与体积边界、既有Pose算子消费、显式旧YAML父子关系、错误单位/版本/相机格式、完整树错误、覆盖锚点及帧错误、巨大有限覆盖、原生精度拒绝、文件/帧预算、真实调度器同大小覆盖重读，以及原子取消。测试文件存在不等于通过；最终构建与运行数字由本阶段统一验证报告提供。
