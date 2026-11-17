# solution-gap：双头轮廓间隙与面差场景

版本 0.2.0，默认关闭。当前原生契约、模块、组合值、编排与旧图升级见[Gap 迁移指南](../../docs/gap-profile-migration.md)。不能以源码搬迁或 manifest 自检代替运行与交付验收。

场景入口仍使用 `gap.*` 算子 ID，包名改为 `solution-gap`。使用明确的 Profile 单位、轴布局、坐标系、手性、有效性和原槽索引；物理几何以米表示并带 `profile.Geometry.v3` 空间上下文，读数单位显式选择。旧通用 Gap 算子退出注册，由公共 Profile/几何/度量能力承接。

```powershell
$env:LYFLOW_PACKS = "solution-gap"
$env:LYFLOW_MODULES = "gap-template;gap-model;gap-import;gap-compat"
pnpm check:gap
```

这组模块覆盖完整模板、模型、导入和临时兼容入口；基础场景不要求全部启用。正式组合由 [release.json](../../packaging/release.json) 统一声明，依赖准备与打包见[发布指南](../../docs/deployment/packaging.md)。旧 `LYFLOW_PACKS=gap` 不作为别名接受。

| 目录                          | 职责                                                             |
| ----------------------------- | ---------------------------------------------------------------- |
| `ops/profile_*.cpp`           | 原生场景数据、模板/模型适配与量测编排；调用公共 typed helper。   |
| `ops/import_standard_gap.cpp` | StandardGap 自动/模板/模型三种模式，各提供高层与细粒度图。       |
| `ops/result_bundle.cpp`       | 结果 schema v4、质量、状态与回退证据。                           |
| `algo/`                       | 声明式模块引用的唯一领域算法；真正旧引擎仅由 `gap-compat` 使用。 |
| `tests/`                      | 原生契约、旧数值/边界、真实运行与升级回归。                      |
| `snippets/`                   | 随能力注册的片段；每个节点固定实际算子版本。                     |
| `tools/`                      | 历史对拍工具，旧图使用前先经过显式图级升级。                     |

上一主版本的图由 `lyflow migrate` 删除已退役来源端口的连线后升级；更早的裸点云主版本不做猜单位的原地转换，须通过 core `gapProfile.v1` 请求核对旧图、声明传感器与量测空间、生成可审查的整图变更，再由宿主保存。不可迁移结构必须说明原因；没有安装对应能力时不能生成一张表面完整但不可执行的图。

整体实施状态见[分包迁移计划](../../docs/operator-package-migration-plan.md)。Glue、后端和退役工作另行验收。
