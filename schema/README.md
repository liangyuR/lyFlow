# Schema

三层之间的接口契约。改这里等于改跨语言 API，需要同步 C++ / Rust / 前端三侧。

| 文件 | 方向 | 说明 |
|---|---|---|
| [`operator-manifest.schema.json`](operator-manifest.schema.json) | C++ → 前端 | 算子描述全量包。启动时下发，热重载时重发。 |
| [`graph-doc.schema.json`](graph-doc.schema.json) | 前端 → C++ | 图文档。也是磁盘文件格式（`.lyflow.json`）。 |
| [`execution-event.schema.json`](execution-event.schema.json) | C++ → 前端 | 执行状态流。 |

示例见 [`examples/`](examples/)，两个示例都已通过对应 schema 校验。

本地校验：

```bash
python -m pip install jsonschema
python - <<'PY'
import io, json, jsonschema
for s, d in [('schema/operator-manifest.schema.json', 'schema/examples/manifest.example.json'),
             ('schema/graph-doc.schema.json', 'schema/examples/graph.example.lyflow.json')]:
    jsonschema.validate(json.load(io.open(d, encoding='utf-8')),
                        json.load(io.open(s, encoding='utf-8')))
    print('ok:', d)
PY
```
