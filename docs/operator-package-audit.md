# 算子分包清单

[operator-package-audit.json](operator-package-audit.json) 为每个原生算子和随附 Python 示例记录一行当前归属与迁移目标，供[迁移计划](operator-package-migration-plan.md)排期使用。分包边界的理由见[包边界](operator-package-boundaries.md)。

## 字段

| 字段          | 含义                                                                              |
| ------------- | --------------------------------------------------------------------------------- |
| `id`          | 算子 ID；Python 示例为 `@node(id=...)` 中的 ID。                                  |
| `currentPack` | 当前注册所在的包，须与 `lyflow-dump-manifest` 导出的 `pack` 一致。                |
| `targetPack`  | 迁移完成后的归属，取值见顶层 `targetPackages`。                                   |
| `action`      | `retain` 原地保留、`move` 整体迁包、`split` 拆分到多个包、`solution` 归入场景包。 |
| `wave`        | 计划完成归属调整的波次 `M0`–`M7`。                                                |

`retiredOperators` 记录已退役的 ID 及替代它们的算子，供 `lyflow migrate` 的诊断与文档引用。行按 `id` 排序。

清单不记录端口、参数、版本、源码位置或验收证据：契约以各包的代码与测试为准，验收证据写在 PR 描述里。

## 检查

```bash
python scripts/check-operator-package-audit.py
```

检查字段与取值、ID 唯一与排序、退役记录，以及 Python 示例与 `python/examples` 中的声明一致。带实际 manifest 时再核对原生 ID 全集与当前包归属：

```bash
python scripts/check-operator-package-audit.py --manifest <lyflow-dump-manifest 的输出>
```

manifest 只来自部分包组合（例如 CI 的轻量构建）时加 `--subset`，只要求导出的每个算子都已登记且归属一致。

## 何时更新

- 新增算子：在 `operators` 中按 `id` 顺序插入一行。
- 改变注册所在的包：修改 `currentPack`；迁移完成时 `currentPack` 与 `targetPack` 相同。
- 退役算子：从 `operators` 删除该行，在 `retiredOperators` 中记录替代算子。
- 端口、参数、版本或实现变化不需要改清单。
