"""对着 JSON Schema 校验一个 JSON 文件。

    python validate_schema.py <data.json> <schema.json>

需要 jsonschema：pip install jsonschema
"""
import io
import json
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    data_path, schema_path = sys.argv[1], sys.argv[2]

    try:
        import jsonschema
    except ImportError:
        print("跳过 schema 校验：未安装 jsonschema (pip install jsonschema)", file=sys.stderr)
        return 0

    data = json.load(io.open(data_path, encoding="utf-8"))
    schema = json.load(io.open(schema_path, encoding="utf-8"))
    try:
        jsonschema.validate(data, schema)
    except jsonschema.ValidationError as e:
        loc = "/".join(str(p) for p in e.absolute_path) or "(root)"
        print(f"schema 校验失败 @ {loc}\n  {e.message}", file=sys.stderr)
        return 1

    ops = len(data.get("operators", []))
    types = len(data.get("types", []))
    print(f"ok: {ops} operator(s), {types} port type(s) 符合 {schema_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
