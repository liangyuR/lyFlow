# Schema

三层之间的接口契约。改这里等于改跨语言 API，需要同步 C++ / Rust / 前端三侧。

| 文件 | 方向 | 说明 |
|---|---|---|
| [`operator-manifest.schema.json`](operator-manifest.schema.json) | C++ → 前端 | 算子描述全量包。启动时下发，热重载时重发。 |
| [`graph-doc.schema.json`](graph-doc.schema.json) | 前端 → C++ | 图文档。也是磁盘文件格式（`.lyflow.json`）。 |
| [`execution-event.schema.json`](execution-event.schema.json) | C++ → 前端 | 执行状态流。 |
| [`snippet.schema.json`](snippet.schema.json) | 包 / 用户 → 编辑器 | 片段文件 `*.lyflow-snippet.json`（M8b）。包随附的编进包、经 manifest 的 `snippets` 段下发；`pnpm check` 对着它校验 `packs/*/snippets/` 下的每一份。 |
| [`recipe.schema.json`](recipe.schema.json) | 编辑器 / CLI ↔ 磁盘 | 参数配方 `<名字>.lyflow-recipe.json`（param-recipe P3）：顶层图参数的稀疏覆盖。格式、目录约定、specDigest 与四类失配见 [docs/recipe.md](../docs/recipe.md)。 |
| [`recipe-index.schema.json`](recipe-index.schema.json) | 同上 | 配方目录里可选的 `index.json`：`{ default?, order? }`。 |

示例见 [`examples/`](examples/)，三个示例都已通过对应 schema 校验，
`pnpm check` 每次都会重跑一遍。

`execution-event.example.json` 是**一串事件**而不是单条。事件流没有「真实产物
文件」可以拿来校验，这份手写样例的作用是让 schema 的每次改动至少被一份具体载荷
验证过 —— 光改 schema 不改样例，很容易写出一份谁都满足不了（或者谁都满足）的约束。

本地校验：

```bash
python -m pip install jsonschema

python scripts/validate_schema.py schema/examples/manifest.example.json \
                                  schema/operator-manifest.schema.json
python scripts/validate_schema.py schema/examples/graph.example.lyflow.json \
                                  schema/graph-doc.schema.json
# --each：数据文件顶层是数组，逐条校验
python scripts/validate_schema.py schema/examples/execution-event.example.json \
                                  schema/execution-event.schema.json --each
```

manifest 那一条在 `pnpm check` 里校验的是 **C++ 现场导出的真实产物**，
不是这份样例 —— 样例只负责让格式说明和实现不脱节。

**跨文件引用。** GraphDoc 顶层图参数的规格（param-recipe P1.1）`$ref` 到
`operator-manifest.schema.json#/$defs/paramSpec`（manifest 的 `param` = `name` + 同一份 `paramSpec`），不抄第二份。
`scripts/validate_schema.py` 把同目录下的 `*.schema.json` 按各自的 `$id` 登记进同一个 registry，引用在本地解析、不联网；
别的校验器要做同样的事（按 `$id` 预载 manifest 的 schema）。

`graph-params.example.lyflow.json` 是带完整规格的图参数（外加一条老格式），`graph-params.invalid.lyflow.json`
是负例（拼错的字段名），`pnpm check` 用 `--expect-fail` 断言它**不**通过：

```bash
python scripts/validate_schema.py schema/examples/graph-params.invalid.lyflow.json \
                                  schema/graph-doc.schema.json --expect-fail
```

**配方的共享夹具**（`fixtures/recipes/`，param-recipe P3）：`graph.lyflow.json` 是一张带各种类型图参数的图，
`graph.recipes/` 里每个配方文件触发一类失配（`ok` 没有、`extra` ①、`type` ②、`range` ③、`spec` / `nograph` ④），
`expected.json` 记着这张图的 specDigest 规范形与摘要，以及每个文件期望的失配条目（类别、参数、建议）。
编辑器的单测（`packages/editor/test/recipes.test.mjs`）逐条对着它断言，P4 的 CLI（Rust）用同一份；`pnpm check` 另外按
`schemaValid` 把每个配方文件对着 `recipe.schema.json` 过一遍（`nograph` 缺 `graph` 字段，按预期不通过，编辑器照样读）。
