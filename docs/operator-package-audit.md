# 现有算子分包审计

状态：实施中。当前产品 1.4.5，277 个原生算子及 8 个随附Python示例。

原规划基线仍为 251 原生 / 7 示例，源码基线 `9940c8c45ef301910add57e48186fce0b543e341`；完整原文件可从 Git `a2f6883b2c16e36c534dd4c9d74412041b12439c:docs/operator-package-audit.json` 追溯，当前机器清单的baselineReference保留原ID全集与证据摘要。

参见[包边界](operator-package-boundaries.md)、[迁移计划](operator-package-migration-plan.md)、[完整JSON](operator-package-audit.json)及[Profile契约](profile-data-contract.md)。

## 当前包逐项入口

| 当前归属        | 数量 | 逐项清单                                          |
| --------------- | ---: | ------------------------------------------------- |
| core            |    4 | [查看](operator-package-audit/core.md)            |
| std-value       |   16 | [查看](operator-package-audit/std-value.md)       |
| std-profile     |   20 | [查看](operator-package-audit/std-profile.md)     |
| std-image       |   76 | [查看](operator-package-audit/std-image.md)       |
| solution-dts    |    7 | [查看](operator-package-audit/solution-dts.md)    |
| std-geometry    |   39 | [查看](operator-package-audit/std-geometry.md)    |
| std-metrology   |    7 | [查看](operator-package-audit/std-metrology.md)   |
| solution-gap    |   40 | [查看](operator-package-audit/solution-gap.md)    |
| glue            |   20 | [查看](operator-package-audit/glue.md)            |
| std-ml          |    1 | [查看](operator-package-audit/std-ml.md)          |
| std-pointcloud  |   47 | [查看](operator-package-audit/std-pointcloud.md)  |
| 原生合计        |  277 | 每个注册ID恰好一次                                |
| python-examples |    8 | [查看](operator-package-audit/python-examples.md) |

## 目标主归属

| 目标包         | 当前原生算子主归属数量 |
| -------------- | ---------------------: |
| core           |                      3 |
| std-value      |                     16 |
| std-profile    |                     21 |
| std-image      |                     82 |
| solution-dts   |                      5 |
| std-geometry   |                     41 |
| std-metrology  |                      8 |
| solution-gap   |                     40 |
| solution-glue  |                      8 |
| std-pointcloud |                     48 |
| std-ml         |                      5 |

每项仍只有一个主责任归属。split可能抽出公共内核并保留场景适配，统计不预测未来注册数量，也不是数据流拓扑。

## 阶段证据

| 阶段 | 状态               | 原生 / 示例     | 证据边界                                                                                                                                                                                                                        |
| ---- | ------------------ | --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M0   | complete           | 251 / 7         | 原逐项清单与分包规划已交付；不代表任何算法迁移已完成。                                                                                                                                                                          |
| M1   | complete           | 256 / 8         | 全包manifest SHA256 `5b3ac7097b5dc75a766501b37cbc28582ca51baacb5174d12b214e962fc4cd51`；阶段完成另由合并、CI及运行记录证明；[PR62](https://github.com/liangyuR/lyFlow/pull/62)已合并 `f0c043c6341755e8f254ac499a1593df087b6d94` |
| M2   | complete           | 256 / 8         | 全包manifest SHA256 `59253822a594df08927c9a5d263f3bee5b2a4bf002191cd12c122cf65add33cb`；阶段完成另由合并、CI及运行记录证明；[PR63](https://github.com/liangyuR/lyFlow/pull/63)已合并 `36c969149e119614b7d221c8006d1a311d6d2995` |
| M3   | complete           | 261 / 8         | 全包manifest SHA256 `a3a6972600786c2adb2e7f8e81f76036223b67377897f4cf1247133275bf755f`；阶段完成另由合并、CI及运行记录证明；[PR64](https://github.com/liangyuR/lyFlow/pull/64)已合并 `b22fde7fad01b7aea67886b6fe3345e92a5fe8bb` |
| M4   | planned            | 待确定 / 待确定 | 38项Gap公共/场景边界分切片实施；先公开保方法的RANSAC/LM数值核与可选后端，再迁节点、双头槽/来源契约及导入器。公共核抽取不等于整个M4完成。                                                                                        |
| M4a  | complete           | 261 / 8         | 全包manifest SHA256 `82215e8bce4c0a080e72509dc84d7f824935451ee48eff8708e2538c9cebe425`；阶段完成另由合并、CI及运行记录证明；[PR65](https://github.com/liangyuR/lyFlow/pull/65)已合并 `02e15806136df452bd575ff0e20610db7636918c` |
| M4b  | complete           | 271 / 8         | 全包manifest SHA256 `d897da58dcdd07b805a05fddf3f0a813513a1cea643e3ac2de3d1546d662fb21`；阶段完成另由合并、CI及运行记录证明；[PR66](https://github.com/liangyuR/lyFlow/pull/66)已合并 `f48aa13199a55b29088d439b8a7dbe090734a6eb` |
| M4c  | validated_manifest | 277 / 8         | 全包manifest SHA256 `5c95d6ae69bb598ad1cedcd67181454528f3fbc41fae62035822ae2b2beab6b5`；描述匹配，不代表阶段全部验收已完成                                                                                                      |

## 波次和顺序

| 波次 | 内容                | 原生项数 |
| ---- | ------------------- | -------: |
| M0   | 清单与依赖基线      |        0 |
| M1   | 公共契约与迁移      |       24 |
| M2   | 轻量几何/度量公共核 |       38 |
| M3   | 轮廓与DTS试点       |       16 |
| M4   | gap拆解             |       54 |
| M5   | glue拆解            |       20 |
| M6   | 标准包/后端隔离     |      124 |
| M7   | 切换、交付与退役    |        1 |

原项目标包、依赖、迁移前置、数据约束与验收要求保留，M2当前依赖与验收边界另列；M1/M2不表示M3场景算法已迁移。序号随新增项重排，只提供满足已列前置的一种实施顺序，不改变惰性调度或业务定义。

## 静态检查与复核刷新

```powershell
python scripts/check-operator-package-audit.py
python scripts/check-operator-package-audit.py --manifest build/audit/manifest.json
```

静态检查比较契约来源集合/指纹、ID、版本、包归属、端口、参数及本地文档链接；不能替代运行、SDK、算法或现场验收。全包manifest必须来自真实启用全部包的构建，轻量构建不构成全集证据。

审查实际源码差异后执行刷新，逐个传入变化路径：

```powershell
python scripts/refresh-operator-package-audit.py --stage M4c --manifest build/audit/manifest.json --product-version 1.4.5 --reviewed-change "当前阶段公共契约、包归属及依赖边界逐项复核" --reviewed-path <变化路径1> --reviewed-path <变化路径2>
```

工具要求reviewed-path精确覆盖所有变化契约文件；拒绝漏审、虚增ID或缺少全包描述。M2仅允许明确定义的3项迁包，保持版本/端口/参数，并保留原包和源码历史。只刷新当前manifest快照、源码锚点、统计及逐项Markdown，不覆盖原有目标/依赖/约束。源码复核前后SHA256与真实manifest SHA256写入阶段ledger。重复运行且源码不变时可不传reviewed-path，保留先前复核证据；工具只将当前阶段设为validated_manifest，完成需另行验收后登记，先前阶段证据保持。

Python示例由源码装饰器AST发现，只有用户导入后注册；用户lib._/py._、外部包和测试节点不属于固定原生全集。附属资产按当前真实全包manifest逐字段登记，历史快照另行保留。

## 已退役计算ID

| 原ID                       | 显式替代                                                                                 | 历史源码                                                                                                                         |
| -------------------------- | ---------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `dts.profile_clean`        | `profile.clean`；通过显式图升级                                                          | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/36c969149e119614b7d221c8006d1a311d6d2995/packs/dts/ops/profile_clean.cpp) |
| `dts.split_faces`          | `profile.split_faces`；通过显式图升级                                                    | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/36c969149e119614b7d221c8006d1a311d6d2995/packs/dts/ops/split_faces.cpp)   |
| `gap.fit_line`             | `profile.fit_line_robust`；通过显式图升级                                                | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/f48aa13199a55b29088d439b8a7dbe090734a6eb/packs/gap/ops/fit.cpp)           |
| `gap.flush`                | `metrology.flush_point_line`；通过显式图升级                                             | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/f48aa13199a55b29088d439b8a7dbe090734a6eb/packs/gap/ops/measure.cpp)       |
| `gap.gap`                  | `metrology.circle_gap`；通过显式图升级                                                   | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/f48aa13199a55b29088d439b8a7dbe090734a6eb/packs/gap/ops/measure.cpp)       |
| `gap.nearest_to_line`      | `profile.nearest_to_line`；通过显式图升级                                                | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/f48aa13199a55b29088d439b8a7dbe090734a6eb/packs/gap/ops/geometry.cpp)      |
| `gap.point_offset`         | `metrology.point_offset`；通过显式图升级                                                 | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/f48aa13199a55b29088d439b8a7dbe090734a6eb/packs/gap/ops/offset.cpp)        |
| `gap.selected_point`       | `geometry.box_anchor`, `profile.bind_geometry`, `profile.nearest_sample`；通过显式图升级 | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/f48aa13199a55b29088d439b8a7dbe090734a6eb/packs/gap/ops/geometry.cpp)      |
| `gap.to_measurement_frame` | `profile.from_cloud`, `profile.remap_axes`；通过显式图升级                               | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/f48aa13199a55b29088d439b8a7dbe090734a6eb/packs/gap/ops/io.cpp)            |
| `gap.drop_non_finite`      | `profile.filter_validity`；通过显式图升级                                                | [原契约及实现](https://github.com/liangyuR/lyFlow/blob/f48aa13199a55b29088d439b8a7dbe090734a6eb/packs/gap/ops/model_roi.cpp)     |

完整原行、端口/参数/约束及源码修订保留在JSON的retiredOperators；退役ID不计入当前注册全集，也不保留算法别名。
