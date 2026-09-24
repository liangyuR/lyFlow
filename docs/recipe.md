# 参数配方

同一张 flow，不同的产品、车型、工位用不同的一组参数值：这一组值就是一个**配方**，存在独立的文件里。
设计与分期见 [param-recipe-plan.md](param-recipe-plan.md)（K1–K8、P3），调研见 [param-recipe-research.md](param-recipe-research.md)。
这一份是 P3 定下来的格式与规则；CLI（`--recipe`）、MCP、宿主怎么按名字切配方是 P4 的事，届时补在后面。

- **配方 = 顶层图参数的稀疏覆盖**，只有一层，配方之间不继承（K1）。没写的图参数用它的 `default`，叫「基础」。
- **有效值 = default ← 当前配方**，总是从基础重新叠，不叠在上一个配方上（K4）。
- core 不知道配方的存在：编辑器（以后是 CLI、宿主）把有效值合成 `{名字: 值}`，经 `RunOptions.params` / C ABI 的
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

加载时（以及之后 doc 或配方每变一次）对每个配方算一份报告。实现是编辑器的纯函数 `recipeReport`
（`packages/editor/src/lib/recipes.ts`）；P4 的 CLI 在 Rust 里再实现一遍，**两边对着同一组夹具**：
[`schema/fixtures/recipes/`](../schema/fixtures/recipes/)（`graph.lyflow.json` + `graph.recipes/` 里每类一个配方文件 +
`expected.json` 里期望的条目）。规则改了，两边与夹具一起改。

报告的顺序：④ 在前（整个配方的事），然后按参数名的码点序；**每个参数至多一条**，取第一个不满足的：① → ② → ③。
一条 = `{ kind, param, fix }`，`fix` 是 `{action: "delete"}`、`{action: "set", value}` 或 `{action: "rebase"}`。

| # | 类别 | 判据 | 建议 |
|---|---|---|---|
| ① | 多出（`extra`） | `values` 里的名字在图参数里不存在（改名或删掉了） | 删除这个值 |
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

## 8. 待 P4 补充

CLI `--recipe <文件>`（与 `--param` 叠加，`--param` 优先；失配 ①–③ 退出码 4、stderr 打印报告）、MCP 的 `recipe` 参数与
`list_recipes`、宿主按名字切配方的推荐流程（读配方文件 → 失配检查 → 合成 `params_json`）写在这里与 [embedding.md](embedding.md)。
