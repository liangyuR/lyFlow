"""对着 JSON Schema 校验一个 JSON 文件。

    python validate_schema.py <data.json> <schema.json> [--each]

--each：数据文件是一个数组，逐个元素校验。执行事件的样例是一串事件，
schema 描述的却是**单条**事件，没有这个开关就得为样例再造一个包装 schema，
那份包装 schema 又不对应任何真实载荷 —— 契约里多一个不存在的东西，
比多一个命令行开关贵。
"""
import io
import json
import sys


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    each = "--each" in sys.argv[1:]
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    data_path, schema_path = args

    try:
        import jsonschema
    except ImportError:
        print("跳过 schema 校验：未安装 jsonschema (pip install jsonschema)", file=sys.stderr)
        return 0

    data = json.load(io.open(data_path, encoding="utf-8"))
    schema = json.load(io.open(schema_path, encoding="utf-8"))

    if each and not isinstance(data, list):
        print("--each 要求数据文件顶层是数组", file=sys.stderr)
        return 2
    items = data if each else [data]

    for i, item in enumerate(items):
        try:
            jsonschema.validate(item, schema)
        except jsonschema.ValidationError as e:
            parts = [str(p) for p in e.absolute_path]
            if each:
                parts.insert(0, f"[{i}]")
            loc = "/".join(parts) or "(root)"
            print(f"schema 校验失败 @ {loc}\n  {e.message}", file=sys.stderr)
            return 1

    if each:
        print(f"ok: {len(items)} 条符合 {schema_path}")
    elif isinstance(data, dict) and "operators" in data:
        ops = len(data.get("operators", []))
        types = len(data.get("types", []))
        print(f"ok: {ops} operator(s), {types} port type(s) 符合 {schema_path}")
    elif isinstance(data, dict) and "nodes" in data:
        print(f"ok: {len(data['nodes'])} node(s), {len(data.get('edges', []))} edge(s) "
              f"符合 {schema_path}")
    else:
        print(f"ok: {data_path} 符合 {schema_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
