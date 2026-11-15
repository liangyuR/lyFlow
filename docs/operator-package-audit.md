# 现有算子分包审计

状态：实施中。当前产品 1.4.0，256 个原生算子及 8 个随附Python示例。

原规划基线仍为 251 原生 / 7 示例，源码基线 `9940c8c45ef301910add57e48186fce0b543e341`；完整原文件可从 Git `a2f6883b2c16e36c534dd4c9d74412041b12439c:docs/operator-package-audit.json` 追溯，当前机器清单的baselineReference保留原ID全集与证据摘要。

参见[包边界](operator-package-boundaries.md)、[迁移计划](operator-package-migration-plan.md)、[完整JSON](operator-package-audit.json)及[Profile契约](profile-data-contract.md)。

## 当前包逐项入口

| 当前归属 | 数量 | 逐项清单 |
| --- | ---: | --- |
| core | 4 | [查看](operator-package-audit/core.md) |
| std-value | 16 | [查看](operator-package-audit/std-value.md) |
| std-profile | 3 | [查看](operator-package-audit/std-profile.md) |
| std-image | 77 | [查看](operator-package-audit/std-image.md) |
| dts | 9 | [查看](operator-package-audit/dts.md) |
| std-geometry | 35 | [查看](operator-package-audit/std-geometry.md) |
| std-pointcloud | 52 | [查看](operator-package-audit/std-pointcloud.md) |
| gap | 39 | [查看](operator-package-audit/gap.md) |
| glue | 20 | [查看](operator-package-audit/glue.md) |
| std-ml | 1 | [查看](operator-package-audit/std-ml.md) |
| 原生合计 | 256 | 每个注册ID恰好一次 |
| python-examples | 8 | [查看](operator-package-audit/python-examples.md) |

## 目标主归属

| 目标包 | 当前原生算子主归属数量 |
| --- | ---: |
| core | 3 |
| std-value | 16 |
| std-profile | 13 |
| std-image | 82 |
| solution-dts | 5 |
| std-geometry | 39 |
| std-metrology | 6 |
| solution-gap | 31 |
| solution-glue | 8 |
| std-pointcloud | 48 |
| std-ml | 5 |

每项仍只有一个主责任归属。split可能抽出公共内核并保留场景适配，统计不预测未来注册数量，也不是数据流拓扑。

## 阶段证据

| 阶段 | 状态 | 原生 / 示例 | 证据边界 |
| --- | --- | --- | --- |
| M0 | complete | 251 / 7 | 原逐项清单与分包规划已交付；不代表任何算法迁移已完成。 |
| M1 | validated_manifest | 256 / 8 | 全包manifest SHA256 `5b3ac7097b5dc75a766501b37cbc28582ca51baacb5174d12b214e962fc4cd51`；描述匹配，不代表阶段全部验收已完成 |

## 波次和顺序

| 波次 | 内容 | 原生项数 |
| --- | --- | ---: |
| M0 | 清单与依赖基线 | 0 |
| M1 | 公共契约与迁移 | 24 |
| M2 | 轻量几何/度量公共核 | 38 |
| M3 | 轮廓与DTS试点 | 11 |
| M4 | gap拆解 | 38 |
| M5 | glue拆解 | 20 |
| M6 | 标准包/后端隔离 | 124 |
| M7 | 切换、交付与退役 | 1 |

原项目标包、依赖、迁移前置、数据约束与验收要求保留；M1新增适配不表示M3场景算法已迁移。序号随新增项重排，只提供满足已列前置的一种实施顺序，不改变惰性调度或业务定义。

## 静态检查与复核刷新

```powershell
python scripts/check-operator-package-audit.py
python scripts/check-operator-package-audit.py --manifest build/m1-full/manifest.json
```

静态检查比较契约来源集合/指纹、ID、版本、包归属、端口、参数及本地文档链接；不能替代运行、SDK、算法或现场验收。全包manifest必须来自真实启用全部包的构建，轻量构建不构成全集证据。

审查实际源码差异后执行刷新，逐个传入变化路径：

```powershell
python scripts/refresh-operator-package-audit.py --manifest build/m1-full/manifest.json --product-version 1.4.0 --reviewed-change "M1公共契约及显式适配逐项复核" --reviewed-path <变化路径1> --reviewed-path <变化路径2>
```

工具要求reviewed-path精确覆盖所有变化契约文件；拒绝漏审、虚增ID或缺少全包描述。只刷新当前manifest快照、源码锚点、统计及逐项Markdown，不覆盖原有目标/依赖/约束。源码复核前后SHA256与真实manifest SHA256写入阶段ledger。重复运行且源码不变时可不传reviewed-path，保留先前复核证据；工具只将M1设为validated_manifest，阶段完成需另行验收后登记。

Python示例由源码装饰器AST发现，只有用户导入后注册；用户lib.*/py.*、外部包和测试节点不属于固定原生全集。原6导入器、7片段、6Bundle等附属资产仍按机器清单登记。
