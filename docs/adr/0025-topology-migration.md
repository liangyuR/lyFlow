# ADR-0025：迁移可以改连线 —— 端口增删走「删边 / 插节点 / 加边」三种动作

- 状态：已采纳
- 日期：2026-09-25
- 修订：[ADR-0008](0008-migration-as-diagnostic.md)（它的「复议条件」在这里成立了）

## 背景

m8a 把 `gap.result_bundle` 的七个散端口（`roiFlushBase` / `roiFlushRef` / `roiGapLeft` / `roiGapRight`、
`cloudPrimary` / `cloudSecondary` / `cloudMerged`）换成了两个 Bundle 端口 `rois`、`scan`，版本却只从 1.0.0 升到 1.1.0。
KUN10 的 19 张线上图（2026-09-15 手搭，节点都没打 `opVersion`）还连着旧端口，于是在当前 core 里全部报
`unknown_port`，一次都跑不起来。

ADR-0008 的迁移只碰参数：`MigrateFn = json(const json&)`，端口增删「要靠新算子 id + aliases」。
这条路在这里走不通 —— `result_bundle` 这个名字没有错，改名只是为了绕开机制；而且等价接法要**多一个节点**：
三片云是 `PointCloud`，`scan` 是 `Bundle<gap.ScanPair>`，中间得插一个 `gap.make_scan_pair`。
这正是 ADR-0008 写的复议条件：「一次迁移要动图的拓扑」。现在有了真实的例子来定动作集合。

## 决策

**迁移步骤可以带一个拓扑函数；core 把它的结果当场用在内存里，并随迁移诊断给出 `edits`，写回仍归前端与 CLI。**

- `Migration` 多一个可选成员 `topology`：`TopologyEdit (*)(const json& params, const vector<MigrationInput>& inputs)`。
  它看得见该步迁移之前的参数，和本节点的全部入边（端口、上游节点、上游端口、上游算子）。`apply` 与 `topology`
  至少给一个；只动连线的一步不必写一个原样返回参数的 `apply`。
- `TopologyEdit` 只有三种动作：`dropInputs`（删掉连到本节点这些输入端口的边）、`addNodes`（`key`、`op`、`params`、`title`）、
  `addEdges`（两端节点写 `@self`、`@new:<key>` 或已有节点 id）。外加 `notes`。插入节点的真实 id 由 core 分配
  （`<节点 id>_<key>`，撞了加 `_2`、`_3`……）。
- `buildPlan` 一开头对所有节点跑一遍迁移链，拓扑改动落在图的副本上，插进来的节点和其他节点一样走逐节点校验、
  进计划、有 cacheKey。拓扑函数只看原图的入边，节点之间互不影响，顺序无关。
- 迁移诊断多一个可选字段 `edits`：`{ removeEdges: [{id, from, to}], addNodes: [{id, op, opVersion, params, title, near}],
  addEdges: [{id, from, to}] }`，id 都已解析好。写回方照做即可：编辑器的 `applyMigrations`（同一条撤销记录）与
  `lyflow migrate --write`（`GraphDoc::apply_migration`）是同一套语义；插进来的节点摆在 `near` 节点的左下方，标题写进 `ui.title`。
- **没打 `opVersion` 的节点按 v1 的写法去试**：当前主版本大于 1、迁移链完整时从 1 开始跑；只有参数真变了或者有拓扑改动
  才出迁移诊断。这要求每个迁移函数对新写法幂等 —— `filter.random_sample` 与 `gap.locate_template` 的现有迁移本来就是，
  拓扑函数没有旧端口的边就返回空改动。
- 等价不了的就删边，并在 notes 里写明**哪几项从此不再记录**。迁移不猜：凑不齐一个 Bundle 的散线不造假数据补齐。

`gap.result_bundle` 因此升到 **2.0.0**，带一条 `Migration{1, nullptr, &migrateResultBundleFromV1}`：
三片云齐全 → 插 `gap.make_scan_pair` 接到 `scan`；四个框齐全 → 插 `gap.make_roi_set` 接到 `rois`（`source` 按 v1 的推断写：
接了 `cropStatus` 是 `model`，否则 `template`）；不齐全 → 删边，notes 列出丢了哪几项。

## 理由

**写回的边界不变。** C++ 仍然不拥有文档（ADR-0002）：它只说「该改成什么」，id 也替写回方分配好，
免得前端与 CLI 各自起名、写出两份不一样的文件。前端仍然不拥有迁移逻辑：「`cloudMerged` 曾经意味着什么」
是算子作者的知识，和 `compute` 长在一起。

**动作集合只取三种。** 删边、插节点、加边足以表达「端口换成 Bundle」「一个端口拆成两个」这类改动，
又小到三个写回方（core 内存、编辑器、CLI）都能各写十几行实现、用同一份测试夹具对上。
「删节点」「改别的节点的参数」没有收：一个算子的迁移不该去动它上游的节点。

**未打版本的节点也要能迁。** 线上图多是手搭或 m8c 之前导出的（导入器从 `9bc7071` 起才写 `opVersion`），
正是最需要迁移的那一批。把「没有版本」当成「最新版本」等于让迁移机制对它们永远失效；
当成「v1」又只在确有改动时才报，不会让已经是新写法的图凭空多出一条迁移诊断。

## 代价

- 迁移函数必须幂等，这条约束从「最好如此」变成了「必须如此」。`gap.locate_template` 的迁移原来就为此加过判断。
- 子图内部节点（路径 id）的拓扑改动只在内存里生效，写回方只写顶层节点 —— 与 ADR-0008 的参数迁移同一个限制。
- 迁移前后运行事件里会多一个节点（插进来的那个），它的 id 在写回之前就已经出现在 `node_state` 里。
