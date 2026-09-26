# 参数配方 P4（CLI、MCP、宿主接入）验收记录

对应 [param-recipe-plan.md](param-recipe-plan.md) 的 P4（P4.1–P4.3，验收 26–29）。2026-09-25，Windows 11，WebView2。
规则与用法写在 [recipe.md](recipe.md) §8–§10（CLI / MCP / 宿主）与 [embedding.md](embedding.md)「按名字切换配方」。

- `pnpm check`：退出码 0，「全链路绿」。doctest **190/190**（34184 断言），`cargo test`（bridge）**161/161**（新增 16 条：
  `recipe.rs` 10 条 —— 摘要、失配报告对着共享夹具逐条比、JS 数字格式、目录约定、宽松读取；`cli.rs` 6 条 —— `--recipe` 与 `--param`
  同结果且 `--param` 优先、失配退出码 4 与 stderr、④ 只提示、eval 四层叠加、`patch --recipe`、`lyflow recipes`），
  `@lyflow/editor` 单测 **64/64**（`recipes.test.mjs` 多 1 条，夹具那条改成连文案一起比），MCP **47/47**（多 2 条：`recipe` 的
  argv 映射、配方的集成冒烟；工具面断言改为 14 个）。
- `pnpm e2e`（`LYFLOW_PACKS=gap;dts`）：退出码 0，**1086/1086**，其中新分组 `scripts/e2e/params_p4.mjs` **50 项**（验收 26 与工具栏）。
  完整输出落盘后 grep「未验」「跳过」「FAIL」「✗」「中断」均为 0 行；gap 的张量组、M8b / M8c、P1–P3 分组都真的跑了。
- `pnpm e2e:http`（`LYFLOW_E2E_HEADLESS=1`）：退出码 0，**58/58**，同样 grep 五个词均为 0 行。
- 以上三项都是最后一次代码改动之后跑的（顺序：`pnpm check` → `pnpm e2e` → `pnpm e2e:http`）。
- （2026-09-26 起 e2e 断言做过合并精简，这里的日志与条数是当时的快照；见 test/prune 精简提交）

| # | 验收项 | 结果 |
|---|---|---|
| 26 | `lyflow run --recipe A` 的结果等于编辑器选配方 A 运行的结果（同一张图、同一组样本，逐位比较 flush / gap）；`--param` 覆盖配方中的同名值 | ✅ 通过 |
| 27 | 失配的配方文件：CLI 退出码 4，stderr 列出条目；`validate --recipe` 同样 | ✅ 通过 |
| 28 | MCP：`run_graph` 带 `recipe` 能跑且结果与 CLI 一致；`list_recipes` 返回正确；`packages/mcp` 的测试覆盖 | ✅ 通过 |
| 29 | 不回归（`pnpm check`、带包的 `pnpm e2e`、headless `pnpm e2e:http`）；`pnpm check` 覆盖新增的 cargo 与 MCP 测试 | ✅ 通过 |
| 附 | 顺手修：1280–1440 宽下工具栏的图名框完整放下 8 个汉字 | ✅ 通过 |

## Rust 与编辑器两份实现一致（P4.1 的前提）

四类失配与 specDigest 在 `bridge/src/recipe.rs` 里又实现了一遍（编辑器那份是 `packages/editor/src/lib/recipes.ts`），两边对着
同一份夹具 `schema/fixtures/recipes/expected.json` 断言：

- **摘要逐字节相同**：`spec_digest_matches_the_shared_fixture_byte_for_byte` 断言规范形 `specCanonical` 与
  `sha256:4dfbd22b…059ad` 与夹具逐字相同（编辑器单测断言的是同一个字符串）。数与 JSON 文本按 ECMAScript 写：`js_number` 对着
  20 个样例（`0.0001`、`1e-7`、`1e+21`、`128/255 = 0.5019607843137255`、`5e-324`…）逐个断言；对象的键序照 JS（下标键在前）。
  码点序、BMP 外字符的排序、options 只取 value 并排序、老格式只留名字，各有一条断言。
- **报告逐条相同**：夹具 expected.json 现在**连文案一起钉住**（每条 `{kind, param, fix, message, fixLabel}`，由编辑器的实现
  重算写入）。`reports_match_the_shared_fixture_item_for_item` 对 6 个配方文件逐条比 kind / param / message / fixLabel，fix 按
  JS 的 JSON 文本比（`5` 与 `5.0` 不算一样），外加 blocking 条数；编辑器的 `recipes.test.mjs` 也改成连 message / fixLabel 一起
  deepEqual。按建议修完后 ①–③ 清零、报告外的值一个不动，两边各有一条。
- CLI 的 stderr 用的就是这些文案：`a_mismatched_recipe_stops_every_command_with_exit_4_and_the_report` 对夹具里每个会阻止运行的
  配方跑 `validate --recipe`，断言 expected.json 里的每一条都以 `[类别] 参数：message → fixLabel` 出现在 stderr 上。

发现并按 docs/recipe.md 修掉的编辑器问题（两处，夹具随之更新）：

1. **`recipeReport` 从原型链上摸到「图参数」**：`doc.params?.[name]` 对 `constructor`、`toString`、`__proto__` 这类名字取到的是
   `Object.prototype` 上的东西，于是不报「多出」—— 与 recipe.md ①「名字在图参数里不存在 → 多出」不一致。改成只认 `doc.params`
   自己的键（`withValue` 取基础时同样）。夹具的 `extra` 配方加了一个 `constructor`，两边都报两条多出；另有一条单测
   （编辑器、Rust 各一条）覆盖 `constructor` / `toString` / `__proto__`。
2. **文案多一个空格**：「应当是 3 个数 的数组」「应当是 3 或 4 个数（RGB / RGBA） 的数组」→ 去掉「数」与「的」之间的空格。
   这一条不是规则问题，但 CLI 的 stderr 与编辑器共用文案，趁夹具钉住文案时一起改了。

## 26. CLI --recipe 与编辑器选配方逐位相同

分组 `scripts/e2e/params_p4.mjs` 的 `suiteCliMatchesEditor`，真实数据：图是 KUN10 线上的点 2
（`luoshi/database/KUN10/device_0/2/2.lyflow.json`，软装 V 缝开口 + 面差，`gap.notch_width` 出 gap 与 flush），样本是
`luoshi/cloud/KUN10` 里按名字排前 3 帧的 `device_0/2_0`。

- 图拷进中文工作区（`车门缝隙 P4/点2 开口与面差.lyflow.json`），只动两处不碰量测参数的地方：去掉三条连到 `gap.result_bundle`
  已不存在端口（`cloudPrimary` / `cloudSecondary` / `cloudMerged`，m8a 起删掉了，现在的 core 校验不过）的边；`n_load.layout =
  profile`（`cloud/KUN10` 里归档的是剖面拍平的布局；09-15 调参时用的另转成传感器布局的那份已经不在了）。
- 编辑器里把 `n_notch` 的 `gapOffset`、`levelDepth`、`flushOffset` 纳入配方，建配方「车型A·P4」（-0.3 / 1 / 0.05；基础是
  -0.42 / 1.2 / 0），工具栏下拉框选中它，真按 Ctrl+S：配方文件里正好这三个值，交给 core 的取值是 `default ← 配方`。
- 每一帧：改 `n_load.dir`、Ctrl+S、**真按 F5**，取 `runOutputs(runId)` 的 gap / flush；再对同一个图文件、同一个配方文件跑
  `lyflow run --recipe … --outputs --no-cache`，两边的 `outputs.gap` / `outputs.flush` 整段 JSON 文本比较：

  | 帧 | gap（配方 A，两边相同） | flush（配方 A，两边相同） | gap（基础） | flush（基础） |
  |---|---|---|---|---|
  | 14-09-2026-03-44-38 | 0.600656187658954 | -0.3440075877228724 | 0.17514934028347556 | -0.39045428135984867 |
  | 15-09-2026-07-52-22 | 0.537113215024019 | -0.3982468184694523 | 0.1220759759448678 | -0.4085120928229669 |
  | 15-09-2026-07-53-17 | 0.6246834451892291 | -0.3091928104629753 | 0.182924012999698 | -0.388735962025612 |

  比较的是 `outputs.gap` / `outputs.flush` 的整段 JSON（`{node, port, type:"Measurement", elementCount, byteSize, value:{kind,
  value, ok, unit:"mm"}}`），不是只比数值。

  三帧 gap、flush 都逐位相同，并且都与基础下的结果不同（配方真的起了作用）。2026-09-26 起 e2e 只在第 1 帧跑一次基础对比，
  后两帧只比编辑器与 CLI。
- `--param` 覆盖配方：`run --recipe A --param gapOffset=-0.42`（盖回基础）的 gap 等于 `run --param levelDepth=1 --param
  flushOffset=0.05`（只用配方另外两个值）的 gap，且与配方 A 的不同；stderr 报「配方「车型A·P4」：3 个值，3 个与基础不同」。
- CLI 这一侧另有 cargo test `recipe_runs_like_the_same_values_given_as_param_and_param_wins`：`--recipe` 与把同样的值写成
  `--param` 的 cacheKey（`run_started` 的每个节点）与点数相同；`--param count=777` 盖掉配方里的 count；`plan`、`validate`、
  `params --only graph`（`source: graph`、`graphParam`）同样按配方取值。

## 27. 失配：退出码 4，stderr 列出条目

cargo test `a_mismatched_recipe_stops_every_command_with_exit_4_and_the_report`：一个配方同时有越界（count 0）、类型不符
（leaf "abc"）、多出（nope），`run` / `validate` / `plan` / `params` / `eval` 都是退出码 4、stdout 一行都没有（一个节点都不跑），
stderr：

```
recipe_mismatch: 配方「坏」有 3 处失配，不能运行（…\r.recipes\坏.lyflow-recipe.json）：
  [越界] count：不能小于 1 → 夹到限位：1
  [类型不符] leaf：应当是 3 个数的数组，实际是 "abc" → 删除这个值（用基础）
  [多出] nope：图里没有图参数 nope（改名或删掉了？） → 删除这个值
  有失配时这个配方不能运行；其余配方不受影响。到编辑器的「配方管理」按建议修复，或者改配方文件
```

夹具的 `extra` / `type` / `range` 三个配方跑 `validate --recipe`，退出码 4，expected.json 的每一条都在 stderr 上；`ok` / `spec` /
`nograph` 不是 4。④ 只提示（`a_recipe_written_for_another_graph_only_warns`）：图 id 与摘要都不对的配方照常跑、退出码 0，stderr
一行「提示：配方「别的图」[规格变了] 图 id 不同（配方记的是 01JSOMEOTHERGRAPH000000000）…」，值照常用上。读不出来的配方文件
（不存在）是 `bad_recipe:`，退出码 4；给两个 `--recipe` 也是 4。`patch --recipe` 遇到失配不写图（文件逐字节不变）。

## 28. MCP

- 新增只读工具 `list_recipes { graphPath }`；`run_graph`、`get_params` 加 `recipe`，`eval` 也加了 `recipe`（见取舍 4）。
  工具面 13 → 14（`server.test.ts` 断言工具名单与每个工具的输入字段）。没配 `LYFLOW_CLI` 时 `list_recipes` 与带 `recipe` 的
  `run_graph` 在碰后端之前就说清楚。`argv.test.ts` 覆盖 `--recipe` 的映射与 `recipes --json`。
- 集成冒烟（`smoke.test.ts`，test-server + 真 CLI）新增一条：图有两个图参数，配方目录里「车型A」（count 3000、leaf 0.05）与
  「坏」（count 0、多出 nope），`index.json` 默认车型A。
  - `list_recipes`：2 个、默认车型A；车型A `runnable: true`、没有条目；坏 `runnable: false`、`mismatches {extra 1, type 0,
    range 1, spec 0}`、条目 `[range count 夹到限位：1]`、`[extra nope 删除这个值]`。
  - `run_graph { graphPath, recipe: 车型A }`：`status: ok`、返回里 `recipe {name, file, values: 2}`；gen 出 3000 点，voxel 的点数
    **与 `lyflow run --recipe … --outputs` 的 `thinned.elementCount` 相同**，且与基础下的不同。
  - `run_graph { recipe: 坏 }`：报错「配方「坏」有 2 处失配，不能运行」，`recipe.blocking = 2`，不碰后端。
  - 内联 `graph` 带 `recipe` 也能跑（MCP 落临时文件给 CLI）。
  - `get_params { recipe: 车型A, only: graph }` = `[gen.pointCount 3000 count]`、`[voxel.leafSize [0.05,0.05,0.05] leaf]`；
    带「坏」时报错、stderr 里有 `[越界] count`。

## 29. 不回归

见页首。

## 附：工具栏

P3 截图（1440 宽）里图名框被挤到 72 px，「车门缝隙检测」只剩「车门缝隙检」。改法（`styles.editor.css` 末尾的 1440 断点 +
`Toolbar.tsx`）：1440 宽及以下间距 14 → 8、按钮内边距 10 → 6、撤销 / 重做只留箭头（字移进 `aria-label` / `title`）、文件名收起
（图名框的 `title` 是路径）；图名框 `min-width` 是 8 个汉字加内边距（`calc(8em + 22px)`），有富余长到 180 px；再挤先收运行区
右侧的状态字（运行错误文字带省略号）。既有的 `data-testid`、类名、按钮的 textContent（m8b 按「整理」找按钮）都没变（撤销 / 重做的字包进了 `.toolbar__label`），另给图名框加了
`data-testid="doc-name"`。分组 `suiteToolbarWidth` 在验收 26 之后的真实状态下（有文件名、选着配方且没存、跑过一次、改过参数
→「已过时」「将重算 N 个节点」），用 CDP 的 `Emulation.setDeviceMetricsOverride` 把视口设成 1280 / 1366 / 1440，每个宽度断言：
图名框的内容区 ≥ 按它自己的字体量出来的「车门缝隙检测左前」宽度且不滚动、工具栏不溢出、运行区状态字不被裁、配方下拉框整个在
窗口里、撤销按钮只剩箭头。（开发时另在 Chrome 里量过：1280 宽且有一条长错误时图名框正好停在下限 118 px，错误文字省略号。）

## 取舍与计划没覆盖的决定

1. **CLI 怎么把配方交给 core**：配方的值与 `--param` 一样写成图参数的 `default` 再交给 core（`load_graph` 里定死顺序：基础 →
   `--recipe` → `--param` → `--set`），而不是走 `params_json`。两条路给 core 的是同一个东西（P1 起 cargo test 就对 `--param`
   与 `params_json` 比过 cacheKey），这样 `--param` 优先是自然成立的，`plan` / `params` / `eval` / `dump` / `sweep` / `perturb`
   也不用各自再接一遍。K3「core 不知道配方」照旧成立。
2. **失配判定只看配方文件，不看 `--param`**：`--param` 盖掉一个越界的值，仍然退出码 4。理由：报告说的是「这个配方文件能不能用」，
   与编辑器（有 ①–③ 的配方不能运行）一致；让命令行把坏配方临时补好反而会让现场的文件一直带病。
3. **`patch --recipe` = 把配方写回基础**（落盘改 `default`，动作顺序 remove → add → rewire → set → recipe → param）。计划只说
   patch 加 `--recipe`；patch 的 `--param` 本来就是「改 default 并落盘」，照同一个语义，`--recipe` 就是编辑器里对每一行点
   「写回基础」。另一个候选是「只拿配方做最后的校验、不写」，没选：那样 `--param` 写盘而 `--recipe` 不写，同一个命令里两个取值
   选项语义相反。`migrate` 拒绝 `--recipe`（它 `--write` 写回整份读进来的 doc，会把配方烙进图）。
4. **eval 的参数组能写图参数了**：原来 `--params paramsets.json` 的键必须是 `<节点>.<参数>`，写被图参数绑定的节点参数会在 core 里
   撞 `param_conflict`（P1 记录没提，是实际行为）——也就是说参数组根本碰不到图参数，「基础 → 配方 → paramset → --param」里
   paramset 这一层是空的。按「实际情况处理」：参数组里**不含 `.` 的键 = 顶层图参数**（与 `--param` 同一条区分规则），落成
   default，叠在配方之上；命令行的 `--param` 在它之后再写一遍，永远说了算。写了图没声明的名字是 `unknown_param`、退出码 4。
   原来合法的参数组文件行为不变。cargo test `eval_layers_base_recipe_paramsets_then_param` 覆盖四层（两个样本都生效）。
   MCP 的 `eval` 因此也加了 `recipe`（agent-tuning.md 的规矩：CLI 有的选项，MCP 要么有同名字段要么写明不提供）；`perturb` 写明不提供。
5. **新增 `lyflow recipes <图> [--recipe <文件>]... [--json]` 子命令**：计划没列，是 MCP `list_recipes` 与 `run_graph recipe` 的
   底座 —— 让失配判定只有 Rust 一份（MCP 不写第三份 TS 实现）。只读、不需要 core、退出码 0（失配是报告内容）；给 `--recipe` 时每行
   带合成好的 `params`，MCP 拿它经 HTTP 信封的 `params` 交给后端（`run_graph` 本来就走 HTTP，结果要留在后端结果仓里给
   `get_node_outputs` / `summarize_output` 取，所以不能改成起 CLI 的 `run`）。代价是带 `recipe` 的 `run_graph` 需要 `LYFLOW_CLI`。
6. **CLI 不给配方重盖章**：P3 取舍 5 的 touch（改过的配方换成当前图的 id 与摘要）只在编辑器里发生；CLI、MCP 都不写配方文件，④ 只提示。
7. **宿主的失配检查用 core 的 `lyflow_validate_params`**：把配方的 `values` 原样当 `params_json` 校验，多出 → `unknown_param`、
   类型 / 限位 / options → `bad_param`。用 CLI 对共享夹具核过：`type` 配方的 10 个图参数、`range` 配方的 6 个都报 `bad_param`，
   `extra` 报 `unknown_param`，`spec` 干净。所以 embedding.md 推荐宿主不必自己实现四类判定，要与编辑器同样的建议文案时交给
   `lyflow recipes`；④ 宿主自己比 `graph.id`（或按 recipe.md §3 算摘要）。
8. **真实图的两处改动**（26 节）：线上图因 m8a 删掉的三个端口在当前 core 里校验不过，归档点云是 profile 布局。两处都不碰量测参数，
   只在 e2e 的工作区副本里改，线上文件不动。
9. **e2e 里的 CLI**：`params_p4.mjs` 用 `bridge/target/debug/lyflow.exe`；它没带 gap 包（比如先跑了不带 `LYFLOW_PACKS` 的
   `pnpm check`）时，分组记一条失败，原因里给出重编命令（`LYFLOW_PACKS="gap;dts"` 下
   `cargo build --manifest-path bridge/Cargo.toml --bin lyflow --no-default-features`）。原先是在 e2e 里现编一次（超时 30 分钟），
   2026-09-26 改掉：不在验收脚本里编，也不静默跳过（跳过的分组照样显示全绿）。数据目录找不到或 app 没有 gap 包时分组标「未验」并记一条失败。

## P4 之外发现的问题（没修）

- KUN10 的 19 张线上图（`luoshi/database/KUN10/device_0/*/`）在当前 core 里都校验不过：`gap.result_bundle` 的 `cloudPrimary` /
  `cloudSecondary` / `cloudMerged` 输入端口 m8a 起没有了，图里还连着。要么给这个算子加一条迁移（把三条边丢掉），要么重导这批图。
  **2026-09-25 已修**：`gap.result_bundle` 升 2.0.0、带拓扑迁移（ADR-0025），19 张图迁移写回并改 `layout=profile`，
  见 [kun10-graphs-migration-acceptance.md](kun10-graphs-migration-acceptance.md)；26 节的 e2e 不再替线上图改东西。
