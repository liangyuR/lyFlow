# DTS Profile 图迁移

DTS v1 的轮廓是毫米 XZ 点云；旧点/线即使带 `Meter` 标签，数值仍是毫米。当前公共处理使用显式 `Profile`，场景点/线输出为真实米。不能只把场景节点版本改为 2，也不能从标签推断单位。

先预览迁移，确认来源声明后再保存：

```powershell
lyflow migrate old.lyflow.json --profile-source n_in --profile-frame scanner --profile-source-id sensor-1
lyflow migrate old.lyflow.json --profile-source n_in --profile-frame scanner --profile-source-id sensor-1 --write
```

三个参数必须同时提供且各一次。来源必须精确为 `dts.profile_in@1.0.0:profile`；CLI 声明已知 v1 的 `unit=mm`、`plane=XZ`、`handedness=right`、`sampleOrder=acquisition`。有反射手性时，把下面的请求存为 JSON 文件，将 `handedness` 设为 `reflected`，再运行 `lyflow migrate <graph> --request <file.json> --write`。

```json
{
  "schemaVersion": 1,
  "rules": [
    {
      "kind": "dtsProfile.v1",
      "source": { "node": "n_in", "op": "dts.profile_in", "opVersion": "1.0.0", "port": "profile" },
      "profile": {
        "frame": "scanner",
        "sourceId": "sensor-1",
        "unit": "mm",
        "plane": "XZ",
        "handedness": "right",
        "sampleOrder": "acquisition"
      },
      "adapterId": "declared_profile"
    }
  ]
}
```

迁移只在 CLI 离线执行；图顶层仍带旧 `contractUpgrade` 请求的文件直接运行 `lyflow migrate <graph> --write`。多来源图在 `rules` 中分别声明，每项必须匹配完整同源八节点消费链；CLI 的单组三参数只选择一条来源链。未知外部来源、未来版本、缺链、混用来源、旁路节点或未声明的外部消费边都会拒绝迁移并给出诊断，原文件不改。

子图使用显式定义作用域：CLI 的 `--profile-source inside/n_in` 对应 rule 的 `"scope":{"subgraph":"inside"}`，而 `source.node` 仍为子图内的 `n_in`。作用域选择 GraphDoc 的全局 `subgraphs` 定义，使用此定义的所有实例一起升级；子图参数、接口、图级命名输出及相对资源路径保留。外部库图应对库的原始 `.lyflow.json` 资产运行同一命令，不会通过主图的库引用修改外部文件。M1 原请求没有子图作用域字段；未退休的嵌套 M1 请求不能通过猜测补出来源。

迁移保留原节点 ID、UI、分组、原参数和绑定，插入 `profile.from_cloud`，将清洗/分面改为公共算子，将五个场景节点升为 v2，并连接 `flush.footInfo → bundle.footInfo`。旧宿主仍向原来源节点的 PointCloud 输出注入。已有明确来源的 M1 几何桥保留节点身份，升级为 `dts.geometry_to_metric@2` 的 `sourceContract=dts.geometry.v2` 米制直通，避免再次乘 `0.001`。单独迁移旧桥时，v1 来源声明仍保留毫米换算行为。

CLI 使用同目录临时文件后改名保存，并移除顶层 `contractUpgrade`。编辑器和宿主加载时不执行迁移：文档仍带 `contractUpgrade` 时只报 `migration_required` 错误诊断，提示先运行 `lyflow migrate <graph> --write`；`meta.contractUpgradeHistory` 作为普通元数据原样保留，不锁定后续拓扑修改。

`migrate --write` 只在原生校验没有错误时写入。旧 M1 请求可能已经生成合法的边界适配报告，但其 v1 DTS 消费链仍会返回 `migration_required`；此时 CLI 拒绝写入，不能把 M1 结构报告当作整图可执行证明。提供完整 M3 来源声明后再迁移。原生结构报告与 `GraphDoc::apply_migration` 仍可用于明确只处理结构的宿主；该写回不代表图已通过执行校验。
