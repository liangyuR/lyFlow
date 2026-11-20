import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import uuid
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
CASES = {
    "p4-annotations-native": {"pickableOnly": True, "includeOccluded": False},
    "p4-annotations-legacy": {"pickableOnly": True, "includeOccluded": False},
    "p4-segmentation": {"minArea": 1048576},
    "p4-segmentation-retained": {"replaceExistingMask": True},
    "p4-ocr": {"characterSet": "digits"},
    "p4-barcodes": {"formats": "QRCodeModel2"},
    "p4-qr-refine": {"modulePixels": 4},
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def schemas(examples):
    script = REPOSITORY / "scripts/validate_schema.py"
    spec = importlib.util.spec_from_file_location("lyflow_schema", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    schema = examples.parent / "schema/graph-doc.schema.json"
    validator = module.make_validator(json.loads(schema.read_text(encoding="utf-8")), str(schema))
    for name in CASES:
        path = examples / f"{name}.lyflow.json"
        validator.validate(json.loads(path.read_text(encoding="utf-8")))


def record(outputs, name, type=None):
    value = outputs[name]["value"]
    require(value["kind"] == "Record", f"{name}: expected native Record metadata")
    if type is not None:
        require(value["type"] == type, f"{name}: unexpected Record type {value['type']}")
    return value["data"]


def instances(outputs, name="instances"):
    data = record(outputs, name, "vision.Poses2D.v1")
    require(data["unit"] == "px", f"{name}: expected original image pixels")
    items = data["items"]
    require(len({item["id"] for item in items}) == len(items), f"{name}: duplicate instance IDs")
    require(all(item["pickable"] is False for item in items), f"{name}: perception cannot grant pickability")
    return items


def masks(outputs):
    items = instances(outputs)
    info = record(outputs, "maskInfo", "vision.Masks2D.v1")
    value = outputs["masks"]["value"]
    require(value["kind"] == "Tensor", "Masks must remain a native Tensor")
    require(value["shape"] == [len(items), info["height"], info["width"]], "Mask NHW/source size mismatch")
    require(len(info["items"]) == len(items), "Mask bindings do not match instances")
    for index, (pose, mask) in enumerate(zip(items, info["items"])):
        require(mask["id"] == pose["id"] and mask["index"] == index, "Mask index/ID binding mismatch")
        if "maskIndex" in pose["attributes"]:
            require(pose["attributes"]["maskIndex"] == index, "Pose maskIndex mismatch")
        require(pose["attributes"]["hasMask"] == mask["hasMask"], "Pose/mask presence mismatch")


def check(name, outputs):
    require(not any(item.get("missing", False) for item in outputs.values()), f"{name}: missing graph output")
    if name.startswith("p4-annotations-"):
        masks(outputs)
        quality = record(outputs, "quality", "image.Annotations.v1")
        require(quality["retainedCount"] == len(instances(outputs)), "Annotation retained count differs from published instances")
    elif name.startswith("p4-segmentation"):
        masks(outputs)
        record(outputs, "quality", "vision.SemanticSegmentation.v1")
        if name.endswith("retained"):
            instances(outputs, "annotationInstances")
    elif name == "p4-ocr":
        record(outputs, "text", "std.Value.v1")
        results = record(outputs, "results", "image.OCR.v1")
        regions = record(outputs, "regions", "image.TextRegions.v1")
        layout = record(outputs, "layout", "image.OCRCrops.v1")
        require(results["geometryHash"] == regions["geometryHash"] == layout["geometryHash"], "OCR geometry association changed")
        ids = [item["id"] for item in regions["items"]]
        require(ids == [item["id"] for item in results["items"]] == [item["id"] for item in instances(outputs)], "OCR region/pose/text IDs differ")
        require(outputs["quads"]["value"]["shape"] == [len(ids), 4, 2], "OCR quads must remain a native Nx4x2 Tensor")
    elif name == "p4-barcodes":
        items = record(outputs, "detections", "image.Barcodes.v1")["items"]
        require([item["id"] for item in instances(outputs)] == [item["id"] for item in items], "Barcode quadrilateral/pose IDs differ")
    elif name == "p4-qr-refine":
        record(outputs, "feedback", "image.QrRefinement.v1")
        record(outputs, "before", "image.Barcodes.v1")
        require(record(outputs, "after", "image.Barcodes.v1")["frame"] == "qr_refined", "Enhanced pixels must have their own frame")


def check_override(name, defaults, outputs):
    if name.startswith("p4-annotations-"):
        original = instances(defaults)
        expected = [item["id"] for item in original
                    if item["attributes"]["annotationPickable"] and not item["attributes"]["occluded"]]
        require(0 < len(expected) < len(original) and [item["id"] for item in instances(outputs)] == expected,
                f"{name}: annotation filters must keep only pickable, unobscured source IDs")
    elif name == "p4-segmentation":
        require(instances(defaults) and not instances(outputs), "Minimum area failed to filter segments")
    elif name == "p4-segmentation-retained":
        require(record(defaults, "quality")["retainedCount"] > 0 and record(outputs, "quality")["retainedCount"] == 0,
                "Mask replacement still retained annotation masks")
    elif name == "p4-ocr":
        original = record(defaults, "text")["value"]
        digits = ["".join(c for c in line if c in "0123456789") for line in original.splitlines()]
        expected = "\n".join(line for line in digits if line)
        require(expected and expected != original and record(outputs, "text")["value"] == expected,
                "Digit filter failed to retain only the recognized digits")
    elif name == "p4-barcodes":
        original = record(defaults, "detections")["items"]
        expected = [(item["format"], item["text"]) for item in original if item["format"] in {"QRCode", "QRCodeModel2"}]
        actual = [(item["format"], item["text"]) for item in record(outputs, "detections")["items"]]
        require(0 < len(expected) < len(original) and actual == expected, "Format filter failed to retain only QR codes")
    elif name == "p4-qr-refine":
        original = record(defaults, "feedback")["modulePixels"]
        pixels = CASES[name]["modulePixels"]
        require(original != pixels and record(outputs, "feedback")["modulePixels"] == pixels and
                all(outputs["image"]["value"][axis] * original == defaults["image"]["value"][axis] * pixels
                    for axis in ("width", "height")), "Module size override failed to resize the refined image")
    else:
        raise AssertionError(f"{name}: missing parameter override check")


def execute(cli, command, log):
    result = subprocess.run([str(cli), *command], capture_output=True, text=True, encoding="utf-8", timeout=240)
    log.with_suffix(".jsonl").write_text(result.stdout, encoding="utf-8")
    log.with_suffix(".stderr.log").write_text(result.stderr, encoding="utf-8")
    require(result.returncode == 0, f"CLI failed ({result.returncode}): {command}\n{result.stderr}\n{result.stdout[-6000:]}")
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def main():
    parser = argparse.ArgumentParser(description="Verify all installed P4 examples and parameter overrides, including default restoration on the barcode workflow")
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--examples", type=Path, default=REPOSITORY / "examples")
    parser.add_argument("--assets", type=Path, default=REPOSITORY / "examples/p4-assets")
    parser.add_argument("--output-dir", type=Path, default=REPOSITORY / "build/p4-cli-contract")
    parser.add_argument("--schema-only", action="store_true")
    args = parser.parse_args()
    schemas(args.examples)
    if args.schema_only:
        print(json.dumps({"schemas": len(CASES), "mode": "schema-only"}))
        return
    if args.cli is None or not args.cli.is_file():
        parser.error("--cli must point to a built CLI loading the P4 runtime")
    manifest = json.loads((args.examples.resolve().parent / "packaging/p4-model-assets.json").read_text(encoding="utf-8"))
    for asset in manifest["assets"]:
        source = args.assets / asset["name"]
        require(source.stat().st_size == asset["bytes"] and digest(source) == asset["sha256"], f"Asset verification failed: {source}")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    saved = output / ("保存重载-" + uuid.uuid4().hex)
    saved.mkdir()
    for source in args.examples.iterdir():
        if source.is_file() and (source.name.startswith("p4-") or source.name == "1700000000000-工件,样例.png"):
            shutil.copyfile(source, saved / source.name)
    (saved / "p4-assets").mkdir()
    for asset in manifest["assets"]:
        shutil.copyfile(args.assets / asset["name"], saved / "p4-assets" / asset["name"])
    original = {str(path.relative_to(saved)): digest(path) for path in saved.rglob("*") if path.is_file()}
    report = {"mode": "formal-cli", "savedDirectory": str(saved), "cases": []}
    for name, overrides in CASES.items():
        graph = saved / f"{name}.lyflow.json"
        defaults = None
        runs = 3 if name == "p4-barcodes" else 2
        for iteration in range(runs):
            options = []
            if iteration == 1:
                for key, value in overrides.items():
                    options.extend(("--param", key + "=" + json.dumps(value, ensure_ascii=False)))
            stem = output / f"{name}-{iteration}"
            execute(args.cli.resolve(), ["validate", str(graph), *options], stem.with_name(stem.name + "-validate"))
            lines = execute(args.cli.resolve(), ["run", str(graph), "--no-cache", "--parallel", "2", "--outputs", "--summary", *options], stem)
            require(len(lines) >= 2 and lines[-1].get("kind") == "run_summary", f"{name}: missing formal run summary")
            require(lines[-1]["status"] == "ok" and not lines[-1]["contractViolations"], f"{name}: unhealthy run summary")
            values = lines[-2]
            check(name, values)
            if iteration == 0:
                defaults = values
            elif iteration == 1:
                check_override(name, defaults, values)
            else:
                require(values == defaults, f"{name}: default results changed after isolated override")
        report["cases"].append({"graph": name, "runs": runs, "override": overrides, "status": "passed"})
    require(original == {str(path.relative_to(saved)): digest(path) for path in saved.rglob("*") if path.is_file()}, "CLI changed saved source graphs/assets")
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"schemas": len(CASES), "runs": sum(case["runs"] for case in report["cases"]), "status": "passed", "report": str(output / "report.json")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
