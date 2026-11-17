# Gap 原生轮廓场景与旧图升级

[分包迁移计划](operator-package-migration-plan.md) · [Profile 契约](profile-data-contract.md) · [公共算子](profile-public-operators.md)

本页说明 Gap 原生场景的运行契约与旧图升级方式。模板、模型的合成夹具不能替代现场工件验收。

## 包与能力

场景源码位于 `solutions/gap`，包名为 `solution-gap`。它直接依赖 `std-profile`、`std-geometry`、`std-metrology` 和声明式内部模块，不通过注册表执行另一个包的算子。旧 `gap` 包名不再作为隐式别名接受。

| 能力           | 内容与依赖                                                                                                                                |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| 基础场景       | 原生双头轮廓、相机选择、角色区域、定位结果及量测特征；`gap-geometry` 复用 PCL 几何，`gap-model-adapters` 处理模型特征与标签，不要求 ORT。 |
| `gap-template` | 模板读取、ICP、候选选择和业务 ROI；对实际模板 Profile 应用变换后才发布量测帧几何。                                                        |
| `gap-model`    | 常驻 ONNX 推理能力，经 `ml-runtime` 使用 ORT；没有此能力时不注册推理执行入口。                                                            |
| `gap-import`   | StandardGap YAML 导入，需要 yaml-cpp；选定模式的全部算子必须可用，否则导入失败并说明缺少的能力，不返回悬空工作流。                        |
| `gap-compat`   | 临时保留 `gap.measure_reference` 的真实旧引擎；依赖模板、模型和导入模块，按迁移计划退役。它不是新原生场景的计算后端。                     |

SDK 随构建能力交付片段：基础场景为五份，模板与模型能力同时存在时增加原子定位回退片段；正式完整组合为六份。安装文件与 manifest 的能力集合保持一致。

例如只选择原生场景与模板能力：

```powershell
cmake -S core -B build/gap-template -G Ninja -DLYFLOW_STD_PACKS=OFF "-DLYFLOW_PACKS=solution-gap;std-profile;std-geometry;std-metrology" "-DLYFLOW_MODULES=gap-template" -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

PCL、Eigen 等依赖需先按[环境准备](getting-started.md)配置。这个命令说明能力选择，不代表该配置已经通过本机安装包验收。`packaging/release.json` 分别列出正式 `packs` 和 `modules`；自定义组合不能自称正式运行库。

## 数据与单位

原始传感器轮廓为原生 Profile：XYZ、可选通道、有效性掩码和原槽索引继续保存在数组载荷中。双头模型严格要求每头 1280 槽，不以压缩后的有限点补齐槽数。模型六通道中的颜色特征采用原 RGB.R/255，不能替换为原生 intensity。

文件单位、坐标系、轴布局及手性必须明确声明。传感器 XZ 到量测 XY 的换轴会改变手性；这是显式轴映射，不能伪装为右手刚体标定。`alreadyAligned` 仅记录调用方对现有坐标关系的声明，不能证明硬件已经标定。

| 原生组合值                | 数据契约                                                                                                               |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `gap.ScanPair.v3`         | 原始两路传感器 Profile、量测帧两路 Profile、合并 Profile 和小型处理元数据。raw/processed 阶段明确区分。                  |
| `gap.RoiSet.v3`           | datum、target、seamLeft、seamRight 四个米制 Box2D，各自带 `profile.Geometry.v3` 空间上下文；info 保存定位方式及处理状态。 |
| `gap.LineFeature.v3`      | 轮廓、直线、端点、几何上下文、唯一内点、多重权重内点和质量一起传递。                                                     |
| `gap.ReferenceFeature.v3` | 参考点与选点方法；非拟合方法的显示线携带不可量测的 GeometryUnavailable，不冒充真实拟合直线。                             |
| `gap.CirclePair.v3`       | 实际选中的双侧圆、对应观测轮廓、内点及候选质量。                                                                         |
| `gap.Location.v3`         | 定位后的轮廓与四角色区域作为一个原子值选择，避免不同回退路径混配。                                                       |

组合字段的 schema 版本与算子版本分别管理。端口或数据契约改变的节点升主版本：上一主版本的图由 `lyflow migrate` 删除已退役端口的连线（如各类 `provenance`、`mergedProvenance`、`leftProvenance`）后原样升级；更早的裸点云主版本仍要求图级升级。

几何上下文 `profile.Geometry.v3` 只记录坐标帧、分析平面和手性，用于判断几何与轮廓是否处于同一空间；不携带内容指纹或来源集合。组合值不再保存 profile/binding/geometry 哈希，读取方不重算、不比对已产出的结果。

合并轮廓保持 secondary 在前、primary 在后，`sourceId` 为 `derived:composite/` 后按顺序以 `+` 连接的父来源，原槽索引保存在原生数组中。半径滤波之后的随动裁剪继续裁已有 merged，不能重新合并以恢复已删除样本。共享可选通道按实际有效贡献取交集，不给缺失通道填零。

## 编排与结果

StandardGap 提供自动、模板、模型三种模式，各有高层与细粒度入口。细粒度图使用公共 Profile 选择/拟合与公共度量节点；高层场景入口调用相同的 typed helper，不在内部查找、运行注册节点。距离、半径等旧毫米参数在明确适配处换算，公共物理几何采用米，最终读数可显式输出毫米。

模型与模板定位通过 Location 原子回退，直线与参考点通过各自的原子特征回退。圆拟合保持原候选、弱拟合地板与相机选择策略；不能额外引入一次模板圆重试。惰性备用路径只有在真实需求发生时才运行。旧细粒度图若实际选出的 scan/ROI 路径不一致，升级后明确报 `location_path_mismatch`，不偷偷重选路径。

`gap.result_bundle` 算子 4.0.0 输出 `GapResultBundle` schemaVersion 4，不再附带 `source_evidence`。缺失统计、时序和一致性信息使用 null/不可用状态，不能合成为 0 或“已通过”。唯一内点和最终多重内点分别报告；旧 CSV `inlier_count` 保持最终多重权重口径。局部特征回退与定位回退分开记录，不能让一次直线重试改变 `roi_source`。

ROI 底图可引用原生 Profile 或 Bundle 的单层采样字段，例如 `templates.left`、`templates.right`；显示按声明的分析平面、单位及有效性掩码取数。计算失败的部分输出不进入正式结果仓，不能把诊断候选描述为可发布量测。

## 旧图的显式升级

旧裸点云图不能只改 `opVersion`。没有明确声明时，旧主版本返回 `graph_upgrade_required`，要求使用 core 的 `gapProfile.v1` 图级升级。请求指定精确旧起点节点、版本及端口、整段节点集合，以及传感器与量测声明，例如：

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

把请求存为 JSON 文件，运行 `lyflow migrate <graph> --request <file.json> --write` 离线迁移；`planeCalib.v1`、`dtsGeometry.v1` 请求同样经 `--request` 提交。图顶层仍带旧 `contractUpgrade` 的文件直接运行 `lyflow migrate <graph> --write`。编辑器与宿主加载不执行升级，遇到 `contractUpgrade` 只报 `migration_required`。

升级前核验原图及已知旧版本；未知、未来版本、无来源的外部注入、跨组件未知消费者及未支持结构均拒绝推断。升级由 CLI 核验后整图写回，保留 UI、其他元数据、合法顶层参数及命名输出；涉及单位换算的动态绑定、无法证明同源的 source/datumSide 覆盖会明确拒绝。共享子图以 `scope.subgraph` 指定，提升参数使用原生 SubgraphDef 数组与对象绑定格式，不支持把内部节点路径冒充提升参数；迁移是一次性的，已迁移的图再跑同一请求会因起点不再匹配而失败。合法参数绑定只移除升级器新增的显式默认值，原图已有的显式值与绑定冲突仍拒绝；图顶层与共享子图提升参数分别核验。动态单位/布局/definition、可能更换实际相机的标签 row 绑定不能安全转换时明确拒绝，模型原槽路径也不接受已压缩采样的声明。

升级器按当前运行库注册的算子版本写出节点，直接生成本页描述的契约：不连任何来源端口，`profile.merge` 只带 `channelPolicy`，选点锚点经 `profile.bind_geometry` 声明为轮廓所在空间。

规则由 solution-gap 注册，与[历史 DTS 升级](dts-profile-migration.md)共用 core 的离线迁移入口，各宿主不再实现转换。

旧通用 ID `gap.fit_line`、`gap.selected_point`、`gap.nearest_to_line`、`gap.drop_non_finite`、`gap.to_measurement_frame`、`gap.flush`、`gap.gap`、`gap.point_offset` 随消费者迁移退出正式注册。旧源码及真正兼容引擎的最终删除按迁移计划进行，不能将“未参与当前构建”记作全部历史行为已验收。

回归保留了旧 corner/groove/notch、圆拟合约束、候选与失败策略的原夹具和数值容差。这些是软件与合成夹具验收，不代表现场精度、真实相机、PLC、客户模型、遮挡工况或产线吞吐验收。
