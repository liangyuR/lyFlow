import argparse
import copy
import importlib.util
import json
import math
import shutil
import uuid
from pathlib import Path

from p4_contract import digest, execute, record, require


REPOSITORY = Path(__file__).resolve().parents[2]
CASES = {
    "p5-depth-inspection": {},
    "p5-depth-filters": {"mode": "fill", "kernelSize": 7},
    "p5-orthographic-roi": {},
    "p5-box-crop": {},
    "p5-pose-rigid": {"currentC": [.02, .12, .01]},
    "p5-pose-project-plane": {},
    "p5-circles-3d": {},
    "p5-mask-remove": {},
}
IDENTITIES = {"sourceCloudId", "parentCloudId", "resultCloudId", "sliceCloudId"}
BINDINGS = {
    "p5-depth-inspection": {
        "projectionQuality": {"sourceCloudId": "sourceCloud"},
        "projectionMapping": {"sourceCloudId": "projectedCloud", "parentCloudId": "sourceCloud"},
        "mapping": {"sourceCloudId": "cloud", "parentCloudId": "projectedCloud"},
        "fillQuality": {"sourceCloudId": "projectedCloud", "resultCloudId": "cloud"},
        "defects": {"sourceCloudId": "cloud"},
        "features": {"sourceCloudId": "cloud", "sliceCloudId": "sliceCloud"},
        "sliceGroups": {"sourceCloudId": "sliceCloud"},
    },
    "p5-depth-filters": {
        "projectionQuality": {"sourceCloudId": "sourceCloud"},
        "projectionMapping": {"sourceCloudId": "projectedCloud", "parentCloudId": "sourceCloud"},
        "mapping": {"sourceCloudId": "cloud", "parentCloudId": "projectedCloud"},
    },
    "p5-orthographic-roi": {
        "heightMapping": {"sourceCloudId": "heightCloud", "parentCloudId": "sourceCloud"},
        "heightQuality": {"sourceCloudId": "sourceCloud"},
        "mapping": {"sourceCloudId": "orthographicCloud", "parentCloudId": "sourceCloud"},
        "quality": {"sourceCloudId": "sourceCloud"},
        "roiMapping": {"sourceCloudId": "cloud", "parentCloudId": "orthographicCloud"},
        "roiQuality": {"sourceCloudId": "orthographicCloud"},
        "groups": {"sourceCloudId": "cloud"},
    },
    "p5-box-crop": {
        "sampleGroups": {"sourceCloudId": "sourceCloud"},
        "wireGroups": {"sourceCloudId": "wireframe"},
        "groups": {"sourceCloudId": "sourceCloud"},
        "support": {"sourceCloudId": "sourceCloud"},
        "cropQuality": {"sourceCloudId": "sourceCloud"},
    },
    "p5-mask-remove": {"quality": {"sourceCloudId": "sourceCloud"}},
}


def schemas(examples):
    path = REPOSITORY / "scripts/validate_schema.py"
    spec = importlib.util.spec_from_file_location("lyflow_p5_schema", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    schema = REPOSITORY / "schema/graph-doc.schema.json"
    validator = module.make_validator(json.loads(schema.read_text(encoding="utf-8")), str(schema))
    for name in CASES:
        validator.validate(json.loads((examples / f"{name}.lyflow.json").read_text(encoding="utf-8")))
    return validator


def observer_graph(doc, operators):
    result = copy.deepcopy(doc)
    nodes = {node["id"]: node for node in doc["nodes"]}
    roles = []
    for role, output in doc["outputs"].items():
        operator = operators[nodes[output["node"]]["op"]]
        ports = {port["name"]: port for port in operator["outputs"]}
        require(output["port"] in ports, f"Unregistered output port: {output}")
        if ports[output["port"]]["type"] != "PointCloud":
            continue
        observer = "__p5_observe_" + role
        frame = "workspace" if doc["id"] == "p5-orthographic-roi" else "world" if doc["id"] == "p5-box-crop" else "camera"
        result["nodes"].append({"id": observer, "op": "cloud.statistics", "params": {"frame": frame}})
        result["edges"].append({"id": observer, "from": output, "to": {"node": observer, "port": "cloud"}})
        result["outputs"][observer] = {"node": observer, "port": "statistics"}
        roles.append(role)
    return result, roles


def normalize(name, outputs, roles):
    values = copy.deepcopy(outputs)
    ids = {}
    for role in roles:
        stats = record(outputs, "__p5_observe_" + role, "cloud.Statistics.v1")
        require(outputs[role]["type"] == "PointCloud", f"{role}: expected native cloud")
        require(stats["count"] == outputs[role]["elementCount"], f"{role}: observer count mismatch")
        require(isinstance(stats["sourceCloudId"], str) and stats["sourceCloudId"].isdigit(), f"{role}: invalid cloud identity")
        ids[role] = stats["sourceCloudId"]
        if "sourceCloudId" in values[role]:
            require(values[role]["sourceCloudId"] == ids[role], f"{role}: native metadata identity mismatch")
            values[role]["sourceCloudId"] = "@cloud:" + role
    expected = copy.deepcopy(BINDINGS.get(name, {}))
    for role in roles:
        expected["__p5_observe_" + role] = {"sourceCloudId": role}
    for output, entry in outputs.items():
        if entry.get("value", {}).get("kind") != "Record":
            continue
        data = entry["value"]["data"]
        present = IDENTITIES.intersection(data) if isinstance(data, dict) else set()
        require(present == set(expected.get(output, {})), f"{name}/{output}: undeclared or missing identity binding {present}")
        for field in present:
            role = expected[output][field]
            require(data[field] == ids[role], f"{name}/{output}.{field}: identity bound to the wrong native cloud")
            values[output]["value"]["data"][field] = "@cloud:" + role
    return values


def cloud(outputs, name, bytes_per_point=31):
    value = outputs[name]
    require(value["type"] == "PointCloud", f"{name}: native PointCloud was replaced")
    require(value["byteSize"] == value["elementCount"] * bytes_per_point, f"{name}: optional channel payload sizes changed")
    return value["elementCount"]


def poses(outputs, name, dimension=3):
    data = record(outputs, name)
    require(outputs[name]["value"]["type"] in {f"vision.Pose{dimension}D.v1", f"vision.Poses{dimension}D.v1"}, f"{name}: wrong pose schema")
    items = data["items"] if "items" in data else [data]
    require(len({item["id"] for item in items}) == len(items), f"{name}: duplicate business ID")
    require(all(item["pickable"] is False for item in items), f"{name}: perception granted pickability")
    return items


def mask_binding(outputs, pose_name):
    items = poses(outputs, pose_name, 2)
    info = record(outputs, "maskInfo", "vision.Masks2D.v1")
    require(outputs["masks"]["value"]["shape"] == [len(items), info["height"], info["width"]], "Mask NHW metadata mismatch")
    require(len(info["items"]) == len(items), "Mask layer count differs from pose count")
    for index, (item, binding) in enumerate(zip(items, info["items"])):
        require(binding["id"] == item["id"] and binding["index"] == index and binding["hasMask"] is True, "Mask business ID/index binding changed")


def finite(values, label):
    require(all(isinstance(value, (int, float)) and math.isfinite(value) for value in values), f"{label}: expected finite numbers")


def check(name, outputs):
    require(not any(entry.get("missing", False) for entry in outputs.values()), f"{name}: missing graph output")
    if name in {"p5-depth-inspection", "p5-depth-filters"}:
        for role in ("sourceCloud", "projectedCloud", "cloud"):
            cloud(outputs, role)
    if name == "p5-depth-inspection":
        require(record(outputs, "fillQuality")["filledCount"] == outputs["filledIndices"]["elementCount"], "Hole fill count differs from its native indices")
        result = record(outputs, "defects", "inspection.Defects.v1")
        for item in result["items"]:
            finite((item["area"], item["volume"]), item["direction"])
        features = record(outputs, "features", "inspection.Profiles.v1")
        for role in ("attributes", "attributeValid"):
            require(outputs[role]["value"]["shape"] == [features["sliceCount"]], f"{role}: 逐截面输出结构不一致")
        require(outputs["profilePlot"]["type"] == "Image", "profilePlot: 应输出原生图像")
        cloud(outputs, "sliceCloud")
    elif name == "p5-depth-filters":
        record(outputs, "quality", "vision.DepthFilterQuality.v1")
    elif name == "p5-orthographic-roi":
        for role in ("sourceCloud", "heightCloud", "orthographicCloud", "cloud"):
            cloud(outputs, role)
        record(outputs, "quality")
        require(outputs["depth"]["value"]["depth"] == "f32", "Signed physical heights must remain float32")
        record(outputs, "groups", "cloud.Groups.v1")
        mask_binding(outputs, "instances")
    elif name == "p5-box-crop":
        total = cloud(outputs, "sourceCloud", 27)
        for role in ("wireframe", "cropped", "outside"):
            cloud(outputs, role, 27)
        count = outputs["cropped"]["elementCount"]
        require(count + outputs["outside"]["elementCount"] == total, "Crop partitions lost points")
        record(outputs, "sampleGroups")
    elif name == "p5-pose-rigid":
        record(outputs, "result", "geometry.RigidCheck3D.v2")
        record(outputs, "comparison")
        poses(outputs, "reference")
        poses(outputs, "current")
        m = outputs["transform"]["value"]["m"]
        require(len(m) == 16, "Registration transform is not a native 4x4 matrix")
        finite(m, "registration transform")
        require(record(outputs, "registration", "geometry.RigidCheck3D.v1")["transform"] == m, "Reported registration transform differs from the actual native output")
        for role in ("flipped", "rotated", "extended"):
            poses(outputs, role)
    elif name == "p5-pose-project-plane":
        mask_binding(outputs, "projected")
        for item in poses(outputs, "poses"):
            finite((*item["position"], *item["size"]), item["id"])
        require(record(outputs, "maskInfo")["frame"] == "image", "2D image frame was relabeled as a 3D frame")
    elif name == "p5-circles-3d":
        detected = record(outputs, "circles", "geometry.Circles3D.v1")["items"]
        for item in detected:
            finite((*item["center"], *item["normal"], item["radius"]), "circle")
        require([item["id"] for item in poses(outputs, "instances")] == [item["id"] for item in detected], "Circle/pose hypothesis IDs differ")
        mask_binding(outputs, "projected")
    elif name == "p5-mask-remove":
        total = cloud(outputs, "sourceCloud")
        quality = record(outputs, "quality", "cloud.MaskPrimitiveRemoval.v1")
        require(cloud(outputs, "cloud") == total - quality["removed"], "Mask removal lost or duplicated points")
        require(outputs["removedIndices"]["elementCount"] == quality["removed"] and outputs["unknownIndices"]["elementCount"] == quality["unknown"], "Mask removal native index counts disagree")
        mask_binding(outputs, "detections")


def main():
    parser = argparse.ArgumentParser(description="Verify all eight installed P5 examples and test override isolation on representative workflows")
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--examples", type=Path, default=REPOSITORY / "examples")
    parser.add_argument("--output-dir", type=Path, default=REPOSITORY / "build/p5-cli-contract")
    parser.add_argument("--schema-only", action="store_true")
    args = parser.parse_args()
    validator = schemas(args.examples)
    if args.schema_only:
        print(json.dumps({"mode": "schema-only", "schemas": len(CASES)}))
        return
    if args.cli is None or not args.cli.is_file():
        parser.error("--cli must point to an already built P5 CLI/runtime")
    truth = json.loads((args.examples / "p5-fixtures.json").read_text(encoding="utf-8"))
    for name, expected in truth["files"].items():
        path = args.examples / name
        require(path.stat().st_size == expected["bytes"] and digest(path) == expected["sha256"], f"Fixture size/SHA256 verification failed: {name}")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifests = execute(args.cli.resolve(), ["manifest"], output / "runtime-manifest")
    require(len(manifests) == 1 and "operators" in manifests[0], "CLI manifest is not the registered operator catalog")
    operators = {operator["id"]: operator for operator in manifests[0]["operators"]}
    saved = output / ("保存重载-" + uuid.uuid4().hex)
    saved.mkdir()
    for name in [*truth["files"], "p5-fixtures.json", *(f"{name}.lyflow.json" for name in CASES)]:
        shutil.copyfile(args.examples / name, saved / name)
    roles_by_case = {}
    for name in CASES:
        doc = json.loads((saved / f"{name}.lyflow.json").read_text(encoding="utf-8"))
        for node in doc["nodes"]:
            require(node["op"] in operators, f"P5 operator missing in actual CLI runtime: {node['op']}")
        observed, roles = observer_graph(doc, operators)
        validator.validate(observed)
        roles_by_case[name] = roles
        (saved / f"{name}-observed.lyflow.json").write_text(json.dumps(observed, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    original = {str(path.relative_to(saved)): digest(path) for path in saved.iterdir() if path.is_file()}
    report = {"mode": "formal-cli", "savedDirectory": str(saved), "cases": []}
    for name, override in CASES.items():
        defaults = None
        runs = 3 if override else 1
        for iteration in range(runs):
            options = []
            if iteration == 1:
                for key, value in override.items():
                    options.extend(("--param", key + "=" + json.dumps(value, ensure_ascii=False)))
            graph = saved / f"{name}-observed.lyflow.json"
            stem = output / f"{name}-{iteration}"
            execute(args.cli.resolve(), ["validate", str(graph), *options], stem.with_name(stem.name + "-validate"))
            lines = execute(args.cli.resolve(), ["run", str(graph), "--no-cache", "--parallel", "2", "--outputs", "--summary", *options], stem)
            require(len(lines) >= 2 and lines[-1].get("kind") == "run_summary", f"{name}: missing formal completion summary")
            require(lines[-1]["status"] == "ok" and not lines[-1]["contractViolations"], f"{name}: unhealthy formal run")
            values = lines[-2]
            normalized = normalize(name, values, roles_by_case[name])
            check(name, values)
            if iteration == 0:
                defaults = normalized
            elif iteration == 1:
                require(normalized != defaults, f"{name}: parameter override did not change the results")
            else:
                require(defaults == normalized, f"{name}: defaults were changed by the isolated override")
        report["cases"].append({"graph": name, "runs": runs, "override": override, "status": "passed"})
    require(original == {str(path.relative_to(saved)): digest(path) for path in saved.iterdir() if path.is_file()}, "Workflow execution changed saved source assets")
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    runs = sum(case["runs"] for case in report["cases"])
    print(json.dumps({"mode": "formal-cli", "schemas": len(CASES), "runs": runs, "status": "passed", "report": str(output / "report.json")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
