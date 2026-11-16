# Gap 原生轮廓场景与旧图升级

[分包迁移计划](operator-package-migration-plan.md) · [Profile 契约](profile-data-contract.md) · [公共算子](profile-public-operators.md)

本页说明 M4c 的运行契约，实现已提交 [PR67](https://github.com/liangyuR/lyFlow/pull/67)。正式交付状态以该 PR 的 CI、审查和合并记录为准；模板、模型的合成夹具不能替代现场工件验收。

## 包与能力

场景源码位于 `solutions/gap`，包名为 `solution-gap`。它直接依赖 `std-profile`、`std-geometry`、`std-metrology` 和声明式内部模块，不通过注册表执行另一个包的算子。旧 `gap` 包名不再作为隐式别名接受。

| 能力           | 内容与依赖                                                                                                                                |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| 基础场景       | 原生双头轮廓、相机选择、角色区域、定位结果及量测特征；`gap-geometry` 复用 PCL 几何，`gap-model-adapters` 处理模型特征与标签，不要求 ORT。 |
| `gap-template` | 模板读取、ICP、候选选择和业务 ROI；对实际模板 Profile 应用变换后才发布量测帧几何。                                                        |
| `gap-model`    | 常驻 ONNX 推理能力，经 `ml-runtime` 使用 ORT；没有此能力时不注册推理执行入口。                                                            |
| `gap-import`   | StandardGap YAML 导入，需要 yaml-cpp；选定模式的全部算子必须可用，否则导入失败并说明缺少的能力，不返回悬空工作流。                        |
| `gap-compat`   | 临时保留 `gap.measure_reference` 的真实旧引擎；依赖模板、模型和导入模块，按 M7 退役。它不是新原生场景的计算后端。                         |

SDK 随构建能力交付片段：基础场景为五份，模板与模型能力同时存在时增加原子定位回退片段；正式完整组合为六份。安装文件与 manifest 的能力集合保持一致。

例如只选择原生场景与模板能力：

```powershell
cmake -S core -B build/gap-template -G Ninja -DLYFLOW_STD_PACKS=OFF "-DLYFLOW_PACKS=solution-gap;std-profile;std-geometry;std-metrology" "-DLYFLOW_MODULES=gap-template" -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

PCL、Eigen 等依赖需先按[环境准备](getting-started.md)配置。这个命令说明能力选择，不代表该配置已经通过本机安装包验收。`packaging/release.json` 分别列出正式 `packs` 和 `modules`；自定义组合不能自称正式运行库。

## 数据与单位

原始传感器轮廓为原生 Profile：XYZ、可选通道、有效性掩码和原槽索引继续保存在数组载荷中。双头模型严格要求每头 1280 槽，不以压缩后的有限点补齐槽数。模型六通道中的颜色特征采用原 RGB.R/255，不能替换为原生 intensity。

文件单位、坐标系、轴布局、手性及采样来源必须明确声明。传感器 XZ 到量测 XY 的换轴会改变手性；这是显式轴映射，不能伪装为右手刚体标定。`alreadyAligned` 仅记录调用方对现有坐标关系的声明，不能证明硬件已经标定。

| 原生组合值                | 数据契约                                                                                                               |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `gap.ScanPair.v2`         | 原始两路传感器 Profile、量测帧两路 Profile、合并 Profile、合并来源和小型处理元数据。raw/processed 阶段明确区分。       |
| `gap.RoiSet.v2`           | datum、target、seamLeft、seamRight 四个米制 Box2D，各自具有严格来源上下文；info 保存实际模板/模型/配置来源及处理状态。 |
| `gap.LineFeature.v2`      | 轮廓、直线、端点、几何来源、唯一内点、多重权重内点、质量和采样来源一起传递。                                           |
| `gap.ReferenceFeature.v2` | 参考点、选点方法及来源；非拟合方法的显示线携带不可量测的 GeometryUnavailable，不冒充真实拟合直线。                     |
| `gap.CirclePair.v2`       | 实际选中的双侧圆、对应观测轮廓、内点、来源及候选质量。                                                                 |
| `gap.Location.v2`         | 定位后的轮廓与四角色区域作为一个原子值选择，避免不同回退路径混配。                                                     |

组合字段的 schema 版本与算子版本分别管理。新特征 make/split 节点从算子版本 1.0.0 开始；沿用旧 ID 而改变数据契约的节点使用新的主版本并阻止不安全的自动升级。

合并轮廓保持 secondary 在前、primary 在后，来源索引和所有父来源共同追踪。半径滤波之后的随动裁剪继续裁已有 merged，不能重新合并以恢复已删除样本。共享可选通道按实际有效贡献取交集，不给缺失通道填零。

## 编排与结果

StandardGap 提供自动、模板、模型三种模式，各有高层与细粒度入口。细粒度图使用公共 Profile 选择/拟合与公共度量节点；高层场景入口调用相同的 typed helper，不在内部查找、运行注册节点。距离、半径等旧毫米参数在明确适配处换算，公共物理几何采用米，最终读数可显式输出毫米。

模型与模板定位通过 Location 原子回退，直线与参考点通过各自的原子特征回退。圆拟合保持原候选、弱拟合地板与相机选择策略；不能额外引入一次模板圆重试。惰性备用路径只有在真实需求发生时才运行。旧细粒度图若实际选出的 scan/ROI 路径不一致，升级后明确报 `location_path_mismatch`，不偷偷重选路径。

`gap.result_bundle` 算子 3.0.0 输出 `GapResultBundle` schemaVersion 3。缺失统计、时序和一致性信息使用 null/不可用状态，不能合成为 0 或“已通过”。唯一内点和最终多重内点分别报告；旧 CSV `inlier_count` 保持最终多重权重口径。局部特征回退与定位回退分开记录，不能让一次直线重试改变 `roi_source`。

ROI 底图可引用原生 Profile 或 Bundle 的单层采样字段，例如 `templates.left`、`templates.right`；显示按声明的分析平面、单位及有效性掩码取数。计算失败的部分输出不进入正式结果仓，不能把诊断候选描述为可发布量测。

## 旧图的显式升级

旧图不能只改 `opVersion`，也不能给裸点云重新贴上一个可信来源标签。没有明确声明时，旧主版本返回 `graph_upgrade_required`，要求使用 core 的 `gapProfile.v1` 图级升级。请求指定精确旧来源节点、版本及端口、整段节点集合，以及传感器与量测声明，例如：

```json
{
  "schemaVersion": 1,
  "rules": [
    {
      "kind": "gapProfile.v1",
      "source": { "node": "n_scan", "op": "gap.read_scan", "opVersion": "1.1.0", "port": "scan" },
      "profiles": {
        "primary": {
          "sourceId": "camera-A",
          "unit": "m",
          "fileFrame": "file",
          "sensorFrame": "sensor",
          "fileHandedness": "right"
        },
        "secondary": {
          "sourceId": "camera-B",
          "unit": "m",
          "fileFrame": "file",
          "sensorFrame": "sensor",
          "fileHandedness": "right"
        }
      },
      "measurement": { "frame": "measurement", "plane": "XY" },
      "alreadyAligned": true,
      "component": {
        "style": "blocks",
        "nodes": [
          "n_scan",
          "n_locate",
          "n_line",
          "n_ref",
          "n_flush",
          "n_circles",
          "n_gap",
          "n_bundle"
        ]
      },
      "adapterIdPrefix": "native_gap"
    }
  ]
}
```

`component.nodes` 必须列出实际完整闭包，示例节点列表不能直接套到任意工作流。细粒度请求用 `style=fine`，精确锚定旧 `gap.load_profile_pair` 1.0.0 的 primary 端口。模板帧/手性与已知默认契约不一致时，还须给出 `templates={unit,fileFrame,templateFrame,handedness}`，不能通过 ICP 推断反射。该历史升级器只接受已知旧数值契约的 `unit=m`，传感器或模板文件为毫米时不能靠声明自动缩放，须先明确调整图或重新导入。

请求结构同时受正式 GraphDoc JSON Schema 和编辑器 TypeScript 类型约束；C++、Rust、HTTP 与 CLI 将同一请求交给 core，不自行猜测来源。

升级前核验原图及已知旧版本；未知、未来版本、无来源的外部注入、跨组件未知消费者及未支持结构均拒绝推断。升级作为整图 CAS 结构替换交给宿主确认并保存，保留 UI、其他元数据、合法顶层参数及命名输出；涉及单位换算的动态绑定、无法证明同源的 source/datumSide 覆盖会明确拒绝。共享子图以 `scope.subgraph` 指定，提升参数使用原生 SubgraphDef 数组与对象绑定格式，不支持把内部节点路径冒充提升参数；已完成请求进入历史并支持幂等重载。合法参数绑定只移除升级器新增的显式默认值，原图已有的显式值与绑定冲突仍拒绝；图顶层与共享子图提升参数分别核验。动态单位/布局/definition、可能更换实际相机来源的标签 row 绑定不能安全转换时明确拒绝，模型原槽路径也不接受已压缩采样的声明。

此机制与[历史 DTS 升级](dts-profile-migration.md)共享 core 升级能力，不由各宿主再实现一套转换。

旧通用 ID `gap.fit_line`、`gap.selected_point`、`gap.nearest_to_line`、`gap.drop_non_finite`、`gap.to_measurement_frame`、`gap.flush`、`gap.gap`、`gap.point_offset` 随消费者迁移退出正式注册。旧源码及真正兼容引擎的最终删除留到 M7，不能将“未参与当前构建”记作全部历史行为已验收。

## 本轮验证记录

2026-11-17，本地产品 1.4.5 验证结果如下；远端门禁见 [PR67 Checks](https://github.com/liangyuR/lyFlow/pull/67/checks)，审查见 [Codex 汇总](https://github.com/liangyuR/lyFlow/pull/67#issuecomment-6924675913)，合并记录以该 PR 为准。

| 验证项           | 结果                                                                                                                                                                                                           |
| ---------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 全包原生运行     | 838 项、413027 条断言通过，0 跳过；ONNX 使用仓库生成的动态批次、静态批次及非法模型夹具。                                                                                                                       |
| 公共静态内核     | 48 项、1099 条断言通过，0 跳过。                                                                                                                                                                               |
| Gap 基础正式配置 | 190 项、37894 条断言通过，0 跳过；未启用模板、模型运行、YAML 导入与旧引擎兼容。                                                                                                                                |
| 编辑器           | 97 项全部通过，0 跳过；React 18/19 与应用严格类型检查通过。                                                                                                                                                    |
| 图升级           | 六种历史模式完整执行，gap/flush/segment/baseLine 与旧 1.4.4 DLL 精确一致；另覆盖实际混路由拒绝、真实共享子图双实例、提升参数及命名输出。                                                                       |
| 片段             | 六份实际执行，包含模型失败后的模板回退、成功时备用不运行、主相机失败后的文件备用；按字段名核验原子结果。                                                                                                       |
| SDK              | 完整配置的 71 项必需文件齐全并交付六片段；原生 Profile 的完整录制/回放/日志/TCP 交付通过，6132 槽 Profile 显式适配 PointCloud 成功且错误类型绑定被拒；基础配置只交付五片段，DLL 直接导入不含 OpenCV/ORT/YAML。 |
| 跨宿主           | 同一合成双头 Gap 图在安装 SDK 的 CLI 与独立 C++ 宿主均输出 `-0.5000382661819458 mm`；通过 C ABI 读取 Bundle 内 200 个原始槽及原始来源索引。                                                                    |
| 静态边界         | 实际 manifest 为 277 算子、13 Bundle、7 片段、6 导入器；逐项审计包含 8 个 Python 示例，模块图 11 项、发布身份 6 项、审计工具 22 项及 GAP 请求 Schema 3 项测试通过。                                            |

回归保留了旧 corner/groove/notch、圆拟合约束、候选与失败策略的原夹具和数值容差。审查修复的实际问题包括缺失的圆拟合前置校验、整体裁剪误计随动状态、细粒度模型图未压缩实际量测轮廓、未连接的拆包叶节点提前触发备用，以及过期相机 guard 证据往返未校验。Codex 审查另修复了旧模板定位显式 `initialPoseMode` 的误拒绝，三种合法模式原样保留；CI 修复了历史审计基线还原、两份 SDK 示例的 EXACT 产品版本同步及 P6 原生 Bundle 录制回放夹具。后续审查修复了新增默认值与合法参数绑定冲突，以及共享子图的外层同名绑定干扰；用独立参数矩阵和真实双实例执行验证。公共内核和稳健拟合均经安装 SDK 的无运行库 DLL 独立静态消费验证。格式检查和 manifest 校验不能代替这些运行回归。

上述数据是软件与合成夹具验收，不代表现场精度、真实相机、PLC、客户模型、遮挡工况或产线吞吐验收。M5–M7 本轮未实施。
