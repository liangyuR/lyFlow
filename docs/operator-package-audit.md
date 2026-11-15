# 现有算子分包审计

状态：规划，未实施。源码基线 `9940c8c`，产品 1.3.12。

本清单逐项覆盖 **251 个原生算子**及 **7 个随附 Python 示例节点**，每项记录当前/目标包、真实依赖、端口和参数单位、当前/目标数据契约、前置条件、迁移顺序及验收要求。

参见[包边界](operator-package-boundaries.md)、[迁移计划](operator-package-migration-plan.md)、[完整 JSON](operator-package-audit.json)。

## 当前包逐项入口

| 当前归属 | 数量 | 逐项清单 |
|---|---:|---|
| core | 4 | [查看](operator-package-audit/core.md) |
| std-value | 16 | [查看](operator-package-audit/std-value.md) |
| std-geometry | 35 | [查看](operator-package-audit/std-geometry.md) |
| std-pointcloud | 52 | [查看](operator-package-audit/std-pointcloud.md) |
| std-image | 76 | [查看](operator-package-audit/std-image.md) |
| std-ml | 1 | [查看](operator-package-audit/std-ml.md) |
| dts | 8 | [查看](operator-package-audit/dts.md) |
| gap | 39 | [查看](operator-package-audit/gap.md) |
| glue | 20 | [查看](operator-package-audit/glue.md) |
| 原生合计 | 251 | 所有当前ID各出现一次 |
| python-examples | 7 | [查看](operator-package-audit/python-examples.md) |

## 目标主归属

| 目标包 | 当前原生算子主归属数量 |
|---|---:|
| core | 3 |
| std-value | 16 |
| std-geometry | 39 |
| std-profile | 10 |
| std-pointcloud | 48 |
| std-image | 81 |
| std-ml | 5 |
| std-metrology | 6 |
| solution-dts | 4 |
| solution-gap | 31 |
| solution-glue | 8 |

上表为每个旧算子的**唯一主责任归属**，不预测重构后的注册数量。`split`会抽出公共内核并保留场景适配，一个旧节点可能对应多个新节点；因此不能把此统计当作未来manifest数量承诺。

## 波次和顺序

| 波次 | 内容 | 纳入迁移/隔离验收的原生项数 |
|---|---|---:|
| M0 | 清单与依赖基线 | 0 |
| M1 | 公共契约与迁移 | 19 |
| M2 | 轻量几何/度量公共核 | 38 |
| M3 | 轮廓与DTS试点 | 11 |
| M4 | gap拆解 | 38 |
| M5 | glue拆解 | 20 |
| M6 | 标准包/后端隔离 | 124 |
| M7 | 切换、交付与退役 | 1 |

M0是全体清点，不以重复登记251项来计数。序号001–251给出尊重已列同波次前置关系的一种执行顺序；252–258为Python示例的M7验收。它不是强制串行，也不是数据流拓扑。波次M6的`retain`项可在先前波次使用其已有接口，M6负责最终隔离和组合验收。例如gap在M4仍使用既有`ml.onnx_run`和`register.icp_2d`，不必等待它们M6的模块整理。

`retain`=主归属保持但需验证模块边界；`move`=主归属调整；`split`=抽公共能力、公开协议或适配；`solution`=保留/重组场景职责。迁移顺序与现有调用顺序分别记录，不能据此改变惰性调度或业务定义。

## 证据与限制

- 243项来自本机1.3.12原生manifest；对应构建源码与9940c8c的core/packs原生实现一致，仅两个Python工具脚本不同。
- DTS8项参考旧1.1.0 manifest并核对当前源码；没有把旧二进制误报为当前main已构建执行证据。
- Python示例取自当前源码装饰器；只有用户配置解释器/目录并导入后才出现，不代表本机已配置这些节点。
- 用户`lib.*`/`py.*`、外部包及测试节点不属于固定原生全集；其迁移策略及附属资产见迁移计划。
- JSON保留完整当前端口与参数快照、sourceFingerprint及逐项约束；本轮检查是静态完整性验证，不是迁移后算法或产品验收。

## 静态复核

在仓库根目录执行：

```powershell
python scripts/check-operator-package-audit.py
```

该命令核对清单集合、描述字段、顺序、来源指纹及引用。若已有导出的当前manifest，可额外传入：

```powershell
python scripts/check-operator-package-audit.py --manifest build/operator-package-audit/native-main.json --manifest build/operator-package-audit/dts-operators.json
```

第二个参数示例是已核验DTS描述数组，不表示运行库自带DTS。若未来导出包含全部包的单份manifest，只传该文件即可。改变算法/声明后应重新审计对应项并刷新基线，而不是直接忽略指纹差异。
