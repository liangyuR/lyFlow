# solution-gap：双头轮廓间隙与面差场景

版本 0.3.0，默认关闭。原生契约、模块、组合值、编排和旧图重搭见 [Gap 指南](../../docs/gap-profile-migration.md)。

场景入口仍使用 `gap.*` 算子 ID，包名改为 `solution-gap`。使用明确的 Profile 单位、轴布局、坐标系、手性、有效性和原槽索引；物理几何以米表示并带 `profile.Geometry.v3` 空间上下文，读数单位显式选择。旧通用 Gap 算子退出注册，由公共 Profile/几何/度量能力承接。

```powershell
$env:LYFLOW_PACKS = "solution-gap"
$env:LYFLOW_MODULES = "gap-template;gap-model;gap-import"
pnpm check:gap
```

这组模块覆盖完整模板、模型、导入和临时兼容入口；基础场景不要求全部启用。正式组合由 [release.json](../../packaging/release.json) 统一声明，依赖准备与打包见[发布指南](../../docs/deployment/packaging.md)。旧 `LYFLOW_PACKS=gap` 不作为别名接受。

| 目录                          | 职责                                                             |
| ----------------------------- | ---------------------------------------------------------------- |
| `ops/profile_*.cpp`           | 原生场景数据、模板/模型适配与量测编排；调用公共 typed helper。   |
| `ops/import_standard_gap.cpp` | StandardGap 自动/模板/模型三种模式，各提供高层与细粒度图。       |
| `ops/result_bundle.cpp`       | 结果 schema v4、质量、状态与回退证据。                           |
| `algo/`                       | 声明式模块使用的定位和模型适配算法。 |
| `tests/`                      | 原生契约、旧数值/边界、真实运行与升级回归。                      |
| `snippets/`                   | 随能力注册的片段；每个节点固定实际算子版本。                     |
| `tools/`                      | 历史对拍工具，旧图使用前先经过显式图级升级。                     |

旧图以业务目标为参考。使用当前版本重新导入 StandardGap 配置，核对传感器、量测空间及命名输出。退役算子的诊断会定位旧节点。

整体实施状态见[分包迁移计划](../../docs/operator-package-migration-plan.md)。Glue、后端和退役工作另行验收。
