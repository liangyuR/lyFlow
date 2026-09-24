"""对着 JSON Schema 校验一个 JSON 文件。

    python validate_schema.py <data.json> <schema.json> [--each]

--each：数据文件是一个数组，逐个元素校验。执行事件的样例是一串事件，
schema 描述的却是**单条**事件，没有这个开关就得为样例再造一个包装 schema，
那份包装 schema 又不对应任何真实载荷 —— 契约里多一个不存在的东西，
比多一个命令行开关贵。
--expect-fail：反过来，数据**必须**不符合 schema（负例夹具用：一份谁都满足的约束同样是错的）。

schema 之间可以跨文件 $ref（图参数的规格复用 manifest 的 paramSpec，param-recipe P1.1）：
同目录下的 *.schema.json 按各自的 $id 登记进同一个 registry，引用在本地解析，不联网。
"""
import io
import json
import sys


def make_validator(schema, schema_path):
    """按 schema 自己的 $schema 选校验器，并把同目录的其它 schema 登记进 registry。"""
    import os

    import jsonschema
    from referencing import Registry, Resource

    registry = Registry()
    folder = os.path.dirname(os.path.abspath(schema_path))
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".schema.json"):
            continue
        other = json.load(io.open(os.path.join(folder, name), encoding="utf-8"))
        if "$id" in other:
            registry = registry.with_resource(other["$id"], Resource.from_contents(other))
    cls = jsonschema.validators.validator_for(schema)
    cls.check_schema(schema)
    return cls(schema, registry=registry)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    each = "--each" in sys.argv[1:]
    expect_fail = "--expect-fail" in sys.argv[1:]
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
    validator = make_validator(schema, schema_path)

    for i, item in enumerate(items):
        error = jsonschema.exceptions.best_match(validator.iter_errors(item))
        if expect_fail:
            if error is None:
                where = f" [{i}]" if each else ""
                print(f"负例居然通过了 {schema_path}: {data_path}{where}", file=sys.stderr)
                return 1
            continue
        if error is not None:
            parts = [str(p) for p in error.absolute_path]
            if each:
                parts.insert(0, f"[{i}]")
            loc = "/".join(parts) or "(root)"
            print(f"schema 校验失败 @ {loc}\n  {error.message}", file=sys.stderr)
            return 1

    if expect_fail:
        print(f"ok: {data_path} 按预期不符合 {schema_path}")
        return 0

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
