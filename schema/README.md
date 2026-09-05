# Schema

三层之间的接口契约。改这里等于改跨语言 API，需要同步 C++ / Rust / 前端三侧。

| 文件 | 方向 | 说明 |
|---|---|---|
| [`operator-manifest.schema.json`](operator-manifest.schema.json) | C++ → 前端 | 算子描述全量包。启动时下发，热重载时重发。 |
| [`graph-doc.schema.json`](graph-doc.schema.json) | 前端 → C++ | 图文档。也是磁盘文件格式（`.lyflow.json`）。 |
| [`execution-event.schema.json`](execution-event.schema.json) | C++ → 前端 | 执行状态流。 |

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
