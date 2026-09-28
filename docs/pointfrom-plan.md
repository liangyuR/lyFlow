# `perturb --region` 的 `pointFrom`：刀口逐帧跟着锚点走 —— 调研与实施计划

> 状态：**已实施**（2026-09-29）。C 档「`perturb --region` 的 `pointFrom`」。
> 行号是写这份计划时的位置，以符号名为准。

## 1. 现状

- 入口 `bridge/src/cli.rs` 分发到 `perturb::cmd_perturb`（`bridge/src/perturb.rs`）。扰动只有一种：几何选区平移。
  `Region` 两种：`Halfspace{point, normal}`、`Box{min, max}`；`parse_region` 只认 `kind` 与这几个键，
  **写了 `pointFrom` 不报错也不起作用**。`Region::params` 展开成 `edit.translate_region` 的平参数
  `regionKind / point / normal / boxMin / boxMax / translation = [0,0,0]`。
- 图手术 `insert_after`：插一个 `__perturb`（`edit.translate_region`，撞名依次 `_2`、`_3`），把从 `--after node:port`
  出发的边与图级 outputs 改从 `__perturb:cloud` 出发，再补 `node:port → __perturb:cloud`。子图内部端口（带 `/`）拒绝。
- 轴扫描 `--axis x=a:b:n` 生成 n 个 ParamSet，每个只写 `__perturb.translation` 的一个分量；交给 eval 的 `Engine`
  跑「位移 × 样本」。覆盖顺序：参数组 writes 先写，样本的 `set` 后写（`Engine::variant`）—— 样本 set 能单独改 `__perturb.point`。
- 输出 `perturb_row` / `perturb_sample`（最小二乘 slope + 两侧 slopeNeg / slopePos）/ `perturb_summary`；给了 `--expect`
  且有样本不通过退出码 2。
- `edit.translate_region`（`packs/std-pointcloud/ops/edit_translate_region.cpp`，1.0.0）：进 `cloud` 出 `cloud`；
  半空间取 `dot(p − point, normal) > 0` 那一侧；参数都是米。
- 值路径（ADR-0020 G3）的 `descend`（`bridge/src/eval.rs`）只下钻对象，不认数组下标 —— Point2D 的 `p` 取不到分量。
- 测试：`perturb.rs` 单元测试；`cli.rs` 的 `perturb_inserts_the_node_and_reports_a_slope`、
  `perturb_flags_a_reading_that_does_not_move_and_exits_failed`（同一张「生成 → crop_box」小图）；MCP `perturb` 工具
  （`packages/mcp/src/tools.ts`）的 `region` 是 `z.record(z.unknown())`，原样 `JSON.stringify` 透传，加字段不用改 schema。没有 e2e。

## 2. 需求来源

`docs/roadmap.md`：`--region` 允许 `pointFrom: <node>:<port>`，让刀口逐帧跟着锚点走（平台级引用，不违背 G7）。
`docs/m5-acceptance.md` 盲测缺口第 5 条：Audio_1 的缝只有 0.04–0.15 mm 宽，却在帧间游走 0.9 mm，固定刀口切不对多数帧；
实际能当锚点的是 Record 字段 `nodes.n_notch.quality.cameras.primary.midXMm`（**毫米**）。
`docs/agent-tuning.md` 警告：切分面压在锚点上时，锚点会跟着一起平移 —— 锚点必须取未扰动的值。

## 3. 否掉的做法

- **给 `edit.translate_region` 加可选锚点输入端口**：锚点通常在 `--after` 的下游，连上就成环，`validate_structure` 拒绝。
- **输出 region 的 gap 算子**（G7 原提法）：同样成环，且只对 gap 有用。

## 4. 做法：CLI 两遍（已确认）

```
--region '{"kind":"halfspace","point":[0,0,0],"normal":[1,0,0],
           "pointFrom":{"x":{"path":"nodes.n_notch.quality.cameras.primary.midXMm","scale":0.001}}}'
```

- `pointFrom` 按分量写：`x` / `y` / `z` 各自可选，每个是 `{path, scale?, offset?}`；刀口该分量 = `值 × scale + offset`
  （`scale` 默认 1、`offset` 默认 0），没写的分量用 `point` 里的。**单位与坐标系显式换算，不猜**。
- `path` 是 G3 值路径（与 `--metric` 同一套）；值路径补上**数字段认数组下标**（`nodes.n_groove.point.p.0`），Point2D 就能当锚点。
- **只支持 halfspace**：box 跟着锚点怎么动没定义，给了 `pointFrom` 就用法错误。
- **第一遍**：同一个 `Engine`，只一个参数组（位移 0，`__perturb` 恒等），指标 = 锚点路径 —— 每个样本在未扰动的图上跑一次读锚点。
  锚点取自未扰动的运行，不会被刀口带着动；上游结果留在进程内缓存里，第二遍复用。
- **第二遍**：把解析出的刀口位置写进该样本的 `set`（`__perturb.point`），照常扫位移。
- **取不到锚点的样本判失败**：不跑第二遍，出一行 `perturb_anchor{status:"anchor_missing"}`，`perturb_sample` 的 n = 0、
  给了 `--expect` 时 pass = false —— 不悄悄退回固定刀口。
- **可审计**（G7）：每个样本一行 `perturb_anchor{sample, point, status, paths}`，写明这一帧实际用的刀口。

## 5. 改动

| 文件 | 改动 |
|---|---|
| `bridge/src/perturb.rs` | `Region::Halfspace` 带 `point_from: Vec<Anchor>`；`parse_region` 解析与校验（box + pointFrom、未知分量、缺 path、scale 非数都是用法错误）；`cmd_perturb` 的第一遍与 `perturb_anchor` 行 |
| `bridge/src/eval.rs` | `descend` 遇到数组按数字段取下标 |
| `bridge/src/cli.rs` | USAGE 里 `--region` 的说明 |
| `packages/mcp/src/tools.ts` | `perturb` 工具描述补 `pointFrom` |
| 文档 | `agent-tuning.md`（perturb 一节、第 331 行附近的警告）、ADR-0020 G6 / G7 修订、roadmap、m5-acceptance 缺口 5 |

## 6. 测试（先读 docs/testing.md，复用为主）

- `perturb.rs` 单元测试：`parse_region` 的正反例放进已有的解析用例（表驱动加行）。
- `eval.rs`：数组下标放进 `available_paths` / resolve 已有用例旁（加一个断言）。
- `cli.rs`：复用 `crop_chain` 那张图 —— 锚点取 `nodes.g.elementCount × 1e-5`，刀口从 x = 0 挪到 x ≈ 0.17，
  ±0.04 的位移再也够不到 0.05 处的裁剪边界：有锚点时 `nodes.c.elementCount` 不响应、没锚点时斜率 > 0；
  `perturb_anchor` 里的 point 等于换算值；锚点路径不存在 → `anchor_missing`、带 `--expect` 退出码 2。
