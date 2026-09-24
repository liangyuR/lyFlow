# 参数配方

同一张 flow，不同的产品、车型、工位用不同的一组参数值：这一组值就是一个**配方**，存在独立的文件里。
设计与分期见 [param-recipe-plan.md](param-recipe-plan.md)（K1–K8、P3），调研见 [param-recipe-research.md](param-recipe-research.md)。
格式与规则是 P3 定下来的（§1–§7）；CLI 的 `--recipe`、MCP 的 `recipe` 与 `list_recipes`、宿主怎么按名字切配方是 P4（§8–§10）。

- **配方 = 顶层图参数的稀疏覆盖**，只有一层，配方之间不继承（K1）。没写的图参数用它的 `default`，叫「基础」。
- **有效值 = default ← 当前配方**，总是从基础重新叠，不叠在上一个配方上（K4）。
- core 不知道配方的存在：编辑器、CLI、宿主各自把有效值合成 `{名字: 值}`，经 `RunOptions.params` / C ABI 的
  `params_json` 交给它（K3）。结果仓按内容寻址，切回原来的配方就是命中缓存。
- 任何节点参数都可以一键「纳入配方」= 提升为图参数（K2，见 [graph-doc.md](graph-doc.md)「顶层图参数」）。

---

## 1. 文件格式

`<名字>.lyflow-recipe.json`，schema 是 [`schema/recipe.schema.json`](../schema/recipe.schema.json)：

```json
{
  "schemaVersion": 1,
  "name": "车型A·左前门",
  "graph": {
    "id": "01JRECIPEFIXTURE0000000000",
    "specDigest": "sha256:4dfbd22b3f0487a10474d3c21500824b0706314a057738d803d8c09a852059ad"
  },
  "values": {
    "leafSize": [0.03, 0.03, 0.03],
    "cutMax": 2.5
  },
  "note": "可选",
  "updatedAt": "2026-09-24T08:00:00.000Z"
}
```

| 字段 | 说明 |
|---|---|
| `schemaVersion` | 恒为 1。比编辑器认识的新就拒绝读（给原因）。 |
| `name` | 配方名，也是文件名去掉 `.lyflow-recipe.json`。**两者不一致时以文件名为准**（手工复制改名的文件），下次写这个文件时改正。 |
| `graph.id` | 写这个配方时对着的那张图的 GraphDoc `id`。 |
| `graph.specDigest` | 那时图参数规格的摘要（§3）。`id` 与摘要都只用于失配 ④，不阻止运行。 |
| `values` | `{ 图参数名: 值 }`，**只存与基础不同的值**：写一个等于 default 的值 = 删掉这条覆盖。按图参数在 doc 里的顺序写，图里已经没有的名字排在后面、原样保留（由失配报告处理）。 |
| `note` | 可选的备注。 |
| `updatedAt` | 最后一次改这个配方的时刻（ISO 8601）。 |

编辑器读文件是宽松的：缺 `schemaVersion` / `name` / `values` / `graph` 的照读（缺 `graph` 报失配 ④），不认识的顶层字段丢掉、
下次写时去掉；不是 JSON、顶层不是对象、`schemaVersion` 太新的读不出来，列在「配方管理」页底部。

## 2. 目录约定

- 默认目录是**图文件同目录的 `<图文件名去扩展名>.recipes/`**。`.lyflow.json` 整个去掉（`车门缝隙.lyflow.json` → `车门缝隙.recipes/`），
  其它扩展名去最后一个（`demo.json` → `demo.recipes/`）。
- 目录里可选的 **`index.json`**（schema [`schema/recipe-index.schema.json`](../schema/recipe-index.schema.json)）：
  `{ "default": "名字", "order": ["名字", …] }`。`default` = 打开图时选中的配方（没有就停在「基础」）；`order` = 下拉框、矩阵、
  管理页的顺序，没列到的按名字（码点序）接在后面。编辑器存盘时总是写它。
- **`autosave~.json`**：30 s 自动备份写的整个内存配方集合（§6），不是配方文件，列目录时不当配方读。
- 没存过盘的新图没有目录，**不能建配方**（提示先保存）。另存为到新路径时，内存里的配方全部写进新目录，旧目录不动。
- **配方名**就是文件名，按 Windows 文件名的规矩：不能空、首尾不能有空格、不能有 `\ / : * ? " < > |` 与控制字符、不能以 `.` 结尾、
  不能是 `CON` `PRN` `AUX` `NUL` `COM1–9` `LPT1–9`、最长 80；重名不分大小写；不能叫「基础」。

## 3. specDigest（图参数规格的摘要）

失配 ④ 用它判断「这个配方写的时候，图参数的规格和现在一样吗」。只收**决定什么值合法**的那几项：

1. 每个顶层图参数一个五元组 `[名字, type, min, max, options]`：
   - 有 `type` 的：`type` 原样；`min` / `max` 是有限的数就写，否则 `null`；`options` 只取每项的 `value`，按各自 JSON 文本的
     码点序排序，没有或为空写 `null`。
   - 没有 `type` 的老格式：`[名字, null, null, null, null]`（它的 min/max 在 core 里本来就不算规格，P1.2）。
2. 五元组按名字的 **Unicode 码点序**排（不是 UTF-16 码元序；Rust 的 `String` 比较按 UTF-8 字节，与码点序一致）。
3. 整个数组用 `JSON.stringify` 序列化：无空白；数用 ECMAScript 的 Number::toString（`0.0001`、`1e+21`；Rust 侧用 ryu-js 同一格式）；
   字符串按 JSON 转义，非 ASCII 原样。
4. 摘要 = `"sha256:"` + 对第 3 步 UTF-8 字节的 SHA-256（64 位小写十六进制）。

label、单位、group、softMin/softMax、step、default、binds、doc 都**不进**摘要：改它们不改变「什么值合法」。
共享夹具的图（`schema/fixtures/recipes/graph.lyflow.json`）的规范形与摘要写在 `expected.json` 的 `specCanonical` / `specDigest` 里。

## 4. 四类失配

加载时（以及之后 doc 或配方每变一次）对每个配方算一份报告。实现有两份：编辑器的纯函数 `recipeReport`
（`packages/editor/src/lib/recipes.ts`）与 CLI 的 `recipe_report`（`bridge/src/recipe.rs`，MCP 经 CLI 用它），
**两边对着同一组夹具**：[`schema/fixtures/recipes/`](../schema/fixtures/recipes/)（`graph.lyflow.json` + `graph.recipes/`
里每类一个配方文件 + `expected.json` 里期望的条目，连 `message` / `fixLabel` 的文案一起，`@lyflow/editor` 的单测与
`cargo test` 逐字比）。规则改了，两边与夹具一起改。

报告的顺序：④ 在前（整个配方的事），然后按参数名的码点序；**每个参数至多一条**，取第一个不满足的：① → ② → ③。
一条 = `{ kind, param, fix }`，`fix` 是 `{action: "delete"}`、`{action: "set", value}` 或 `{action: "rebase"}`。

| # | 类别 | 判据 | 建议 |
|---|---|---|---|
| ① | 多出（`extra`） | `values` 里的名字在图参数里不存在（改名或删掉了）。只认 doc `params` 自己的键：`constructor`、`toString` 这类名字也是多出 | 删除这个值 |
| ② | 类型不符（`type`） | 值不符合图参数的 `type`（下表） | 能转换就转换（**转换后再夹进限位**），否则删除 |
| ③ | 越界 / 非法（`range`） | 不满足 min/max，或 enum 不在 options 里 | 数夹到限位（向量、颜色、transform 逐分量，曲线逐个 y）；enum 改为默认（删除这条覆盖） |
| ④ | 规格变了（`spec`） | 文件里的 `graph.id` 或 `specDigest` 与当前图不同，或文件里没记 | 提示，不阻止；建议「按当前图更新记录」（`rebase`） |

**有 ①–③ 的配方不能运行**（界面上的每一种运行都在开跑前拦下并写明原因），其余配方与「基础」不受影响。④ 只提示。
没有 `type` 的老格式图参数只查 ①（core 也不按它校验）。配方里**缺**某个值不算失配：稀疏，用基础。

类型判据与 core 的 `coerceParam` + `checkRange` 同一套（P1.2），转换建议是编辑器加的：

| type | 合法 | ② 的转换 |
|---|---|---|
| `bool` | `true` / `false` | `0` / `1`；字符串 `"true"` / `"false"`（不分大小写） |
| `int`、`flags` | 整数 | 有限的数或十进制数字符串 → 四舍五入（**.5 远离 0**，与 Rust `f64::round` 一致）；布尔不转 |
| `float` | 有限的数 | 十进制数字符串 → 数 |
| `vec2f` `vec3f` `vec4f` | 恰好 N 个有限的数 | 一个数 → 重复 N 次；更长的数组 → 取前 N 个 |
| `color` | 3 或 4 个有限的数 | 一个数 → 重复 3 次；更长 → 取前 4 个；`#rrggbb` / `#rrggbbaa` → 各字节 / 255 |
| `transform` | 16 个有限的数 | 更长 → 取前 16 个 |
| `enum` | 字符串（在 options 里，否则 ③） | 数或布尔 → `String(v)`，在 options 里才算 |
| `string` `text` `path` | 字符串 | 数或布尔 → `String(v)` |
| `curve` | `{points, interp}`（格式见 [operator-manifest.md](operator-manifest.md)「transform 与 curve 的值」） | 格式不对不转（删除）；y 越界是 ③ |

「全部按建议修复」把整份报告一次修完（一次撤销），单条的按钮只修那一条；修过的配方记成当前图（§5 的 touch）。

## 5. 编辑语义（编辑器）

**当前配方**在工具栏右侧的下拉框里选：「基础」+ 各配方 +「新建配方…」。切换不弹窗、**不算一步撤销**、不改 GraphDoc；
没存的改动留在内存里（K6 ③）。选着配方时，交给 core 的取值、实时校验（K5）、stale 与「将重算 N 个节点」、实时预览都按
「default ← 这个配方」算；开着「自动运行」时，人切一次配方就补一次运行（打开图时自动选中默认配方不算）。

| 情形 | 行为 |
|---|---|
| ① 选着配方，改一个已纳入的参数 | 写进**当前配方**（不改 default）。想改基础就切到「基础」，或者点这一行的「写回基础」 |
| ② 选着配方，改一个没纳入的参数 | 照常改图（影响所有配方），这一行提示「此改动影响所有配方」，并给「改为只在本配方生效」：把这次改动撤回、这个参数纳入配方（default = 改之前的值，别的配方行为不变）、新值写进当前配方 —— 一个动作、一次撤销 |
| 被当前配方覆盖的行 | 左侧橙色竖条 + 浅橙底 +「配方 · 基础 X」标签；行尾「恢复基础」（删掉这条覆盖）与「写回基础」（值写进 default、删掉覆盖；沿用基础的其它配方随之变） |
| 图参数改名 | 内存里每个配方的那个键跟着改名（同一步撤销）。删掉图参数不动配方：它们的值成为失配 ① |

**撤销（K7）**：撤销记录同时快照 GraphDoc 与内存里的配方集合，改配方值、新建 / 复制 / 重命名 / 删除 / 设默认 / 导入 /
修失配都进同一个撤销栈；拖滑块改配方里的值合成一条。当前配方按配方的内存身份跟随：改名、撤销改名时它还是当前配方；
被撤没了就回到「基础」。

**保存（K6 ③）**：Ctrl+S 一次保存图与所有有改动的配方文件（改名的改名、删掉的删文件、`index.json` 一起）。工具栏的脏标记
是合并的（图或任何一个配方没存都亮），下拉框旁的橙色小圆点只看配方。存盘前检查要动的文件是不是在上次读 / 写之后
**被外部修改**过（内容与编辑器记着的不同、被删了、或者目录里冒出了同名文件）：

- **覆盖**：写编辑器里的版本；
- **重新载入**：这几个文件以磁盘为准替换内存里对应的配方（一步撤销，Ctrl+Z 回到编辑器里的版本），其余照常保存；
- **取消**：图和配方都不存。

不做合并（计划「不做」一节）。**管理动作都只改内存**，Ctrl+S 才落盘；**导出**例外，它立即把配方按当前格式写到选的位置。
**导入**把外部配方文件读进内存（存盘时写进配方目录），重名时让用户改名；文件里记的图 id 与摘要原样保留，不一样就报 ④。

一个配方被**改过**（任何一种编辑、修失配、改名），它的 `graph` 就换成当前图的 id 与摘要、`updatedAt` 换成那一刻：
它从此是对着这张图维护的，④ 随之消失。只是打开、切换、设为默认不算改过。

## 6. 自动备份

30 s 一拍（与 `<图>~` 同一个定时器）：配方有没存的改动时，把整个内存配方集合写进 `<配方目录>/autosave~.json`（每个配方
记着它对应的磁盘文件，恢复后改名 / 删除照样认得），并照样写一份 `<图>~`。下次打开图时「有比正文更新的自动备份，要恢复吗」
只问一次，选恢复就连配方一起恢复成未保存状态；选否两份备份都删。存盘成功后删掉 `autosave~.json`。

## 7. 传输层与路径安全

`Transport` 的五个方法（`listRecipeDir` / `readRecipeFile` / `writeRecipeFile` / `deleteRecipeFile` / `renameRecipeFile`），
Tauri（`bridge/src/commands.rs`「配方文件」）与 HTTP（[http-transport.md](http-transport.md)「配方文件」，参考实现
`packages/editor/test-server`）都实现，Static transport 只读。文本进、文本出，后端只挡路径：

- 配方目录（最后一段以 `.recipes` 结尾）里的 `*.lyflow-recipe.json`、`index.json`、`autosave~.json`：读、写、删；
  `*.lyflow-recipe.json` 之间在同一个目录里改名（目标已存在拒绝，只差大小写的除外）；
- 任意位置的 `*.lyflow-recipe.json`：读（导入）、写（导出）；
- 路径里不许有 `..`；Tauri 要绝对路径；HTTP 只认工作区里的路径（逃出去 403）；
- 写的内容必须是一个 JSON 对象的文本，先写临时文件再改名覆盖。

## 8. CLI：`--recipe`

`lyflow run` / `validate` / `plan` / `params` / `eval` / `patch` 认 `--recipe <配方文件>`（同样经图装载的 `dump` / `sweep` /
`perturb` 也认；`migrate` 拒绝它，免得 `--write` 把配方烙进图）。一次只能给一个配方（配方之间不叠加，K4）。

```bash
lyflow run   车门缝隙.lyflow.json --recipe 车门缝隙.recipes/车型A·左前门.lyflow-recipe.json --summary
lyflow run   车门缝隙.lyflow.json --recipe 车门缝隙.recipes/车型A·左前门.lyflow-recipe.json --param cutMax=2   # --param 优先
lyflow eval  车门缝隙.lyflow.json --recipe …/车型A·左前门.lyflow-recipe.json --samples s.jsonl --metric outputs.gap
lyflow recipes 车门缝隙.lyflow.json --json        # 列配方目录：名字、值个数、失配、默认配方
```

- **取值的叠法**：基础（图里的 `default`）→ `--recipe` → `--param`，后写的赢。实现上配方的值与 `--param` 一样写成图参数的
  `default` 再交给 core（与 `params_json` 同一个结果，cargo test 对着 cacheKey 与点数比过）；`--set` 照旧只能改没被绑定的节点参数。
- **`eval`**：配方作用于所有样本。`--params <paramsets.json>` 的参数组里**不含 `.` 的键是顶层图参数**（与 `--param` 同一条区分
  规则），写在配方之上；命令行的 `--param` 最后写，永远说了算。完整顺序：基础 → 配方 → 参数组 → `--param`。参数组里写了图没声明
  的名字是 `unknown_param`，退出码 4。
- **`patch --recipe`**：把配方的值**写回基础**（落盘改图参数的 `default`，等于在编辑器里对每一行点「写回基础」）。动作顺序
  remove → add → rewire → set → recipe → param，回执 `applied.recipe` 列出改了的名字；全部同值是 no-op。
- **失配 ①–③**：不跑、不写，**退出码 4**，stderr 逐条列出（与编辑器同一套用语，`[类别] 参数：原因 → 建议`）：

  ```
  recipe_mismatch: 配方「range」有 6 处失配，不能运行（…/graph.recipes/range.lyflow-recipe.json）：
    [越界] cutField：'w' 不在选项里（x / y / z / intensity） → 改为默认（删除这条覆盖）
    [越界] cutMax：不能大于 5 → 夹到限位：5
    [越界] leafSize：第 2 个分量不能大于 10 → 夹到限位：[0.02,10,0.0001]
    …
    有失配时这个配方不能运行；其余配方不受影响。到编辑器的「配方管理」按建议修复，或者改配方文件
  ```

  判定对着配方文件本身，不看 `--param`：同名的 `--param` 盖掉一个越界值也仍然退出码 4 —— 该修的是配方文件。
- **失配 ④**：stderr 一行「提示：配方「X」[规格变了] …」，退出码不变，值照常用上。CLI 不改配方文件（不重盖章，§5 的 touch
  只在编辑器里发生）。
- 配方文件读不出来（不存在、不是 JSON、`schemaVersion` 太新）：`bad_recipe:`，退出码 4。
- **`lyflow recipes <graph> [--recipe <文件>]... [--json]`**：只读。按目录约定（§2）找 `<图名>.recipes/`，每个配方一行
  `{kind: "recipe", name, file, default, values, blocking, items: [{kind, param, message, fix, fixLabel}], note?, notes?}`，
  最后一行 `{kind: "recipe_dir", dir, exists, default, order, count, problems, graphId, specDigest}`；给了 `--recipe` 就只看那几个
  文件（可以在目录外），每行另带 `params`（合成好的取值，给宿主与 MCP 用）。失配是报告的内容，退出码 0。

## 9. MCP

- `run_graph` 与 `get_params` 多一个 `recipe`（配方文件路径，`list_recipes` 给的 `file`）；`eval` 同样有 `recipe`（透传 `--recipe`）。
  `run_graph` 带 `recipe` 时先经 `lyflow recipes --recipe` 查失配、取合成好的 `params`，再经 HTTP 信封的 `params` 交给后端
  （判定只有 Rust 一份，MCP 不写第三份）；①–③ 时不碰后端、报错里带条目，④ 放在返回的 `recipe.warnings`。需要 `LYFLOW_CLI`。
- `list_recipes { graphPath }`：图旁配方目录里的每个配方（名字、`file`、值个数、`runnable`、四类各几条、条目与建议、默认星标）。
  只读；**不提供写配方的工具**（P4.2）。形状见 [mcp.md](mcp.md)。

## 10. 宿主按名字切换配方

C ABI 不认识配方：宿主读配方文件、查失配、合成 `params_json`，完整流程与失败处理见
[embedding.md](embedding.md)「按名字切换配方」。要点：

1. 配方文件在 `<图文件名去扩展名>.recipes/<名字>.lyflow-recipe.json`；名字是文件名（§2）。
2. 失配检查：把配方的 `values` **原样**当 `params_json` 交给 `lyflow_validate_params`。①多出的名字 core 报 `unknown_param`，
   ②③ 报 `bad_param`（`nodeId` 为空、`paramPath` 是图参数名），判据与编辑器、CLI 同一套（core 本来就是权威，P1.2；共享夹具的
   `type` / `range` 两份配方逐条对过）。有 error 就拒绝切换。④ 由宿主自己比文件里的 `graph.id` 与 `specDigest`（算法 §3），或者
   交给 `lyflow recipes`，它给出与编辑器同一份报告与修复建议。
3. 校验通过的那份 `params_json` 就是运行用的：没写的名字 core 用 `default`，不必把基础抄一遍。
4. 切换在两次检测之间做，失败就留在上一个配方，并把原因报给 PLC / 上位机。
