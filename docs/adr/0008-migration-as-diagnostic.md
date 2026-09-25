# ADR-0008：算子迁移由 C++ 出诊断，前端以语义化动作写回

- 状态：已采纳；「迁移只碰参数」一条由 [ADR-0025](0025-topology-migration.md) 修订（迁移可以删边、插节点、加边）
- 日期：2026-09-06

## 背景

算子会演进。`filter.random_sample` 的 `count` / `ratio` 读不出来是「保留」还是「丢弃」，
改名成 `keepCount` / `keepRatio` 是个正确的改动 —— 但它是破坏性的：主版本要升到 2，
而磁盘上已经存着一堆写着 `"count": 10000` 的图。M2 对这种图的处理是报 `version_mismatch`
让它红着，用户只能手改 JSON。

谁来把老参数改成新参数？三个候选都能自圆其说：

1. **C++ 直接改**：它有迁移函数，改完在内存里接着跑。
2. **Rust 改**：它正好在读文件那条路上。
3. **前端改**：它拥有 GraphDoc 和撤销栈。

## 决策

**E3：C++ 在 validate 阶段产出「迁移诊断」，前端以 `applyMigrations` 动作写回 GraphDoc。**

- `OperatorDesc` 加 `migrations: vector<Migration{ int fromMajor; MigrateFn }>`，
  `MigrateFn = json(const json&)`。`Registry::validate()` 要求链条覆盖 `1..currentMajor-1`。
- validate 里：`aliases` 命中 → 重定向；`saved.major < current.major` → 依次套用迁移；
  产出一条 `kind: "migration"` 的诊断，`severity: warning`，带
  `{ op, opVersion, params, notes[] }`。
- 执行器**在内存里**用迁移后的参数继续跑 —— 老图当场就能运行，不必先存一次盘。
- Rust `load_graph` 返回 `{ doc, migrations }`（内部调一次 `lyflow_validate` 再筛
  `kind === "migration"`）。**桥接层不改图。**
- 前端把这批动作交给 `applyMigrations`：一条撤销记录、置 dirty、toast「已迁移 N 个节点」。

诊断数组里每一项现在都带 `kind`（`"diagnostic"` | `"migration"`），
前端不用靠「有没有某个字段」去猜自己拿到的是什么。

## 理由

**C++ 不拥有文档。** 它拿到的是一段 `graph_json`，改完还得原样送回去 —— 那就要么让
`lyflow_validate` 返回一份新文档（一个「校验」函数返回文档，接口就烂了），
要么加一个 `lyflow_migrate`（然后前端要处理「改了但没存」的状态，回到同一个问题）。
ADR-0002 说 GraphDoc 是前端的唯一真实数据源，让 C++ 往里写就是在这条线上开洞。

**前端不拥有迁移逻辑。** 迁移函数得知道 `count` 曾经意味着什么 —— 那是算子作者的知识，
和 `compute` 长在一起才不会漂移。更硬的理由是 **headless 走不到前端**：
M4 的 CLI 直接拿 `graph_json` 去跑，如果迁移在 TypeScript 里，CLI 就打不开老图。

**诊断是唯一干净的通道。** 它已经存在、已经是双向契约的一部分、已经有 severity 和 nodeId。
迁移只是多一种 `kind`，不需要新的 IPC、新的事件、新的状态机。

**为什么写回是「一条撤销记录 + dirty」而不是静默改。** 用户打开一张老图，参数被换了名字，
这是**他的文档被改了**。撤销栈里必须有这一步，标题栏必须有那个 `*`。
静默改写等于告诉用户「你存的东西我可以随便动」，那是最快失去信任的做法。

**为什么 `params` 是完整对象而不是补丁。** 改名参数没法用补丁表达 —— 补丁要同时表达
「删 count」和「加 keepCount」，而 JSON 里没有「删除」这个值。整份替换只是多传几十个字节。

## 落地：一个真实的 fixture

`filter.random_sample` 真的升到了 **2.0.0**，真的注册了 `Migration{1, migrateFromV1}`，
`count`/`ratio` 真的改名成了 `keepCount`/`keepRatio`。
这不是为验收造的假算子 —— 一条只在测试里走过的迁移路径，第一次真用的时候一定是坏的。

## 代价

- `manifest.h` 现在要 `#include <nlohmann/json.hpp>`，它被每个算子 TU 间接包含，
  全量构建慢几秒。备选是让 `MigrateFn` 收发 JSON 文本，但那要求每个算子作者
  自己 parse/dump，把成本转嫁给了更常发生的那一侧。
- 迁移只碰参数。端口改名、输入输出增删不在射程内 —— 那些要靠新算子 id + `aliases`，
  因为连线不归算子管，它没法在自己的迁移函数里改边。
- 迁移链断档只在启动自检里报，不在编辑期报。这是对的：`lyflow-dump-manifest --check`
  和桥接层启动都会把它当 fatal，算子作者跑一次就发现了。

## 复议条件

如果将来出现「一次迁移要动图的拓扑」（比如一个算子拆成两个），这条 ADR 不够用：
诊断里得能表达节点的增删和边的重连。届时的形态大概是把 `MigrationPlan` 从
「新参数」扩成「一组语义化动作」，而前端的 `applyMigrations` 变成一个小解释器。
现在不做，因为还没有一个真实的例子来定那套动作集合。

2026-09-25：条件成立（`gap.result_bundle` 的散端口换成 Bundle 端口，等价接法要多插一个节点），
动作集合定为删边 / 插节点 / 加边三种，见 [ADR-0025](0025-topology-migration.md)。
