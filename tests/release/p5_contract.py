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
    "p5-depth-inspection": {"defectMode": "up"},
    "p5-depth-filters": {"mode": "fill", "kernelSize": 7},
    "p5-orthographic-roi": {"outputMode": "normal_angle"},
    "p5-box-crop": {"shape": "cylinder"},
    "p5-pose-rigid": {"currentC": [.02, .12, .01]},
    "p5-pose-project-plane": {"liftMode": "center"},
    "p5-circles-3d": {"imagePath": "p5-circle-tilted.png"},
    "p5-mask-remove": {"topExpand": 2},
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
        "removeGroups": {"sourceCloudId": "sourceCloud"},
        "cropQuality": {"sourceCloudId": "sourceCloud"},
        "quality": {"sourceCloudId": "sourceCloud"},
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


def cloud(outputs, name, count=None, bytes_per_point=31):
    value = outputs[name]
    require(value["type"] == "PointCloud", f"{name}: native PointCloud was replaced")
    if count is not None:
        require(value["elementCount"] == count, f"{name}: expected {count} points, got {value['elementCount']}")
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


def near(actual, expected, tolerance, label):
    require(math.isfinite(actual) and abs(actual - expected) <= tolerance, f"{label}: expected {expected}, got {actual}")


def check(name, outputs, changed, truth):
    require(not any(entry.get("missing", False) for entry in outputs.values()), f"{name}: missing graph output")
    if name in {"p5-depth-inspection", "p5-depth-filters"}:
        cloud(outputs, "sourceCloud", truth["scene"]["sourcePoints"])
        cloud(outputs, "projectedCloud", truth["scene"]["visiblePoints"])
        require(record(outputs, "projectionQuality")["visibleCount"] == 3063, "Z-buffer visibility changed")
        require(outputs["sourceImage"]["value"]["channels"] == 4, "RGBA input was discarded")
        require(outputs["depth"]["value"]["width"] == 64 and outputs["depth"]["value"]["height"] == 48, "Wrong organized extent")
    if name == "p5-depth-inspection":
        cloud(outputs, "cloud", 3072)
        require(record(outputs, "fillQuality")["filledCount"] == 9 and outputs["filledIndices"]["elementCount"] == 9, "Hole fill did not preserve observed versus synthetic support")
        result = record(outputs, "defects", "inspection.Defects.v1")
        require(result["areaModel"] == "observed_depth_orthogonal_footprint", "Defect footprint model changed")
        require(result["state"] == "defect" and result["good"] is False and result["valid"] is True, "Observed defects were classified as clear/unknown")
        require([item["direction"] for item in result["items"]] == (["up"] if changed else ["up", "down"]), "Signed defect selection changed")
        for item in result["items"]:
            direction = item["direction"]
            require(item["support"] == truth["scene"][direction + "Pixels"], "Wrong physical defect support")
            area = truth["scene"][direction + "AreaM2"]
            near(item["area"], area, 1e-9, direction + " area")
            near(item["volume"], truth["scene"][direction + "VolumeM3"], area * truth["scene"]["heightToleranceM"], direction + " volume")
        require(record(outputs, "features")["sliceCount"] == 6, "Wrong profile count")
        require(outputs["attributes"]["value"]["shape"] == [6], "Profile attributes lost their native batch")
        cloud(outputs, "sliceCloud")
        require(outputs["profiles"]["value"]["shape"][1] == 4 and outputs["supports"]["value"]["shape"][1] == 3, "Profile geometry/provenance tensors changed")
    elif name == "p5-depth-filters":
        quality = record(outputs, "quality", "vision.DepthFilterQuality.v1")
        require(quality["mode"] == ("fill" if changed else "mean"), "Filter mode override leaked")
        require(quality["filledPixels"] == (9 if changed else 0), "Invalid compensation count changed")
        require(quality["changedPixels"] == (9 if changed else 0), "Kernel-one no-op or fill changed observed samples")
        cloud(outputs, "cloud", 3072 if changed else 3063)
    elif name == "p5-orthographic-roi":
        for role in ("sourceCloud", "heightCloud", "orthographicCloud"):
            cloud(outputs, role, truth["orthographic"]["sourcePoints"])
        cloud(outputs, "cloud", 94)
        require(record(outputs, "quality")["outputMode"] == ("normal_angle" if changed else "height"), "Orthographic display mode override leaked")
        depth = outputs["depth"]["value"]
        require(depth["depth"] == "f32" and depth["min"][0] < 0 and depth["max"][0] > 0, "Signed physical heights were clamped")
        require(outputs["image"]["value"]["channels"] == (3 if changed else 1), "Physical height and RGB normal visualization were confused")
        group = record(outputs, "groups", "cloud.Groups.v1")
        require([(item["id"], item["count"]) for item in group["groups"]] == [("patch-up", 42), ("patch-down", 36), ("hole-region", 16)], "ROI masks were replaced by boxes or holes filled")
        mask_binding(outputs, "instances")
    elif name == "p5-box-crop":
        cloud(outputs, "sourceCloud", 1200, 27)
        for role in ("wireframe", "cropped", "outside", "cloud", "removed"):
            cloud(outputs, role, bytes_per_point=27)
        count = outputs["cropped"]["elementCount"]
        require((0 < count < 1200) if changed else count == 1200, "Finite cylinder/box membership did not change")
        require(count + outputs["outside"]["elementCount"] == 1200, "Crop partitions lost points")
        require(outputs["removed"]["elementCount"] == count and outputs["cloud"]["elementCount"] == 1200 - count, "Crop and compact removal disagree")
        require([item["id"] for item in record(outputs, "sampleGroups")["groups"]] == ["boxA", "boxB"], "Sampling changed box IDs")
    elif name == "p5-pose-rigid":
        result = record(outputs, "result", "geometry.RigidCheck3D.v1")
        require(result["accepted"] is (not changed) and result["observable"] is True, "Nonrigid observation was accepted or correct template mutated")
        require(record(outputs, "comparison")["matched"] is (not changed), "Registered pose comparison disagrees with rigidity")
        require([item["position"] for item in poses(outputs, "reference")] == [[0, 0, 0], [.1, 0, 0], [0, .1, 0]], "Observed C override also changed the reference template")
        require(poses(outputs, "current")[2]["position"] == ([.02, .12, .01] if changed else [0, .1, 0]), "Current C binding changed another pose")
        m = outputs["transform"]["value"]["m"]
        require(len(m) == 16 and all(math.isfinite(v) for v in m), "Registration transform is not native finite geometry")
        registration = record(outputs, "registration")
        require(registration["observable"] is True and registration["transformAvailable"] is True and registration["accepted"] is (not changed), "Registration availability was confused with rigid tolerance")
        require(registration["transform"] == m, "Reported registration transform differs from the actual native output")
        near(m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]), 1, 1e-5, "proper registration determinant")
        near(poses(outputs, "extended")[0]["position"][2], .2, 1e-12, "2D embedded offset")
    elif name == "p5-pose-project-plane":
        mask_binding(outputs, "projected")
        item = poses(outputs, "poses")[0]
        require(item["id"] == "plane-box" and item["frame"] == "camera", "Plane lift lost 3D identity/frame")
        near(item["position"][2], 1.01, 2e-7, "reference plane location")
        if changed:
            require(item["size"] == [.12, .08, .02], "Known center-mode dimensions changed")
        else:
            expected = sorted([14 * 1.01 / 120, 10 * 1.01 / 120])
            for actual, wanted in zip(sorted(item["size"][:2]), expected):
                near(actual, wanted, 2e-7, "observed pixel footprint dimension")
        require(record(outputs, "maskInfo")["frame"] == "image", "2D image frame was relabeled as a 3D frame")
    elif name == "p5-circles-3d":
        expected = truth["circles"]["tilted" if changed else "front"]
        detected = record(outputs, "circles", "geometry.Circles3D.v1")["items"]
        require(1 <= len(detected) <= 2, "Circle hypothesis count changed")
        found = False
        for item in detected:
            error = math.dist(item["center"], expected["centerM"])
            cosine = abs(sum(a * b for a, b in zip(item["normal"], expected["unorientedNormal"])))
            found |= error <= expected["centerToleranceM"] and cosine >= expected["normalCosineMinimum"]
            require(item["center"][2] > 0 and item["radius"] == expected["radiusM"], "Circle scale or positive depth is wrong")
        require(found, "No actual calibrated circle solution matches physical fixture truth")
        require([item["id"] for item in poses(outputs, "instances")] == [item["id"] for item in detected], "Circle/pose hypothesis IDs differ")
        mask_binding(outputs, "projected")
    elif name == "p5-mask-remove":
        cloud(outputs, "sourceCloud", 3066)
        removed = 104 if changed else 78
        cloud(outputs, "cloud", 3066 - removed)
        quality = record(outputs, "quality", "cloud.MaskPrimitiveRemoval.v1")
        require(quality["removed"] == removed and quality["unknown"] == 1 and quality["outOfBoundsKept"] == 1, "Mask/OOB/unknown decision changed")
        require(outputs["removedIndices"]["elementCount"] == removed and outputs["unknownIndices"]["elementCount"] == 1, "Mask removal native index counts disagree")
        mask_binding(outputs, "detections")


def main():
    parser = argparse.ArgumentParser(description="Verify eight P5 example schemas and formal saved CLI default/override/default contracts without skipping cases")
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
        for iteration, changed in enumerate((False, True, False)):
            options = []
            if changed:
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
            check(name, values, changed, truth)
            if iteration == 0:
                defaults = normalized
            elif iteration == 2:
                require(defaults == normalized, f"{name}: defaults were changed by the isolated override")
        report["cases"].append({"graph": name, "runs": 3, "override": override, "status": "passed"})
    require(original == {str(path.relative_to(saved)): digest(path) for path in saved.iterdir() if path.is_file()}, "Workflow execution changed saved source assets")
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"mode": "formal-cli", "schemas": len(CASES), "runs": 3 * len(CASES), "status": "passed", "report": str(output / "report.json")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
