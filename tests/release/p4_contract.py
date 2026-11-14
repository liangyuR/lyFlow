import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
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
    schema = REPOSITORY / "schema/graph-doc.schema.json"
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


def check(name, outputs, changed):
    require(not any(item.get("missing", False) for item in outputs.values()), f"{name}: missing graph output")
    if name.startswith("p4-annotations-"):
        items = instances(outputs)
        require(len(items) == (1 if changed else 3), "Annotation filter did not select the expected source instances")
        quality = record(outputs, "quality", "image.Annotations.v1")
        require(quality["retainedCount"] == len(items) and quality["sourceCount"] == 3, "Annotation source/filter count mismatch")
        masks(outputs)
        expected = ("annotation-2" if name.endswith("legacy") else "candidate") if changed else ("annotation-0" if name.endswith("legacy") else "plate-A")
        require(items[0]["id"] == expected, "Filtering changed original annotation identity")
        require(outputs["image"]["value"]["width"] == 64 and outputs["image"]["value"]["height"] == 48, "Wrong timestamp image")
    elif name.startswith("p4-segmentation"):
        masks(outputs)
        quality = record(outputs, "quality", "vision.SemanticSegmentation.v1")
        items = instances(outputs)
        if name.endswith("retained"):
            require(quality["retainedCount"] == (0 if changed else 2), "Per-ROI retained count mismatch")
            require(quality["roiCount"] == (3 if changed else 1), "Only missing-mask ROI must be inferred")
            require(len(instances(outputs, "annotationInstances")) == 3, "Source annotation instances changed")
            if not changed:
                require([item["id"] for item in items[:2]] == ["plate-A", "plate-B"], "Retained annotation IDs changed")
        else:
            require((not items) if changed else bool(items), "minArea override did not isolate semantic results")
            require(quality["roiCount"] == 1 and quality["retainedCount"] == 0, "Full image inference bookkeeping mismatch")
    elif name == "p4-ocr":
        text = record(outputs, "text", "std.Value.v1")["value"]
        expected = "12345\n12345\n2026" if changed else "LYFLOW12345\nINDUSTRIALOCR\n工业视觉12345\nHELLO2026"
        require(text.replace(" ", "") == expected, f"OCR truth mismatch: {text!r}")
        results = record(outputs, "results", "image.OCR.v1")
        regions = record(outputs, "regions", "image.TextRegions.v1")
        layout = record(outputs, "layout", "image.OCRCrops.v1")
        require(len(results["items"]) == 4 and results["items"][3]["orientation"] == 180, "OCR direction/order mismatch")
        require("工业视觉" in results["items"][2]["rawText"], "Unicode rawText lost during filtering")
        require(results["geometryHash"] == regions["geometryHash"] == layout["geometryHash"], "OCR geometry association changed")
        ids = [item["id"] for item in regions["items"]]
        require(ids == [item["id"] for item in results["items"]] == [item["id"] for item in instances(outputs)], "OCR region/pose/text IDs differ")
        require(outputs["quads"]["value"]["shape"] == [4, 4, 2], "OCR quads must remain a native Nx4x2 Tensor")
        if changed:
            require(results["items"][1]["text"] == "" and results["items"][1]["accepted"] is False, "Filtered empty line was accepted")
    elif name == "p4-barcodes":
        items = record(outputs, "detections", "image.Barcodes.v1")["items"]
        require(len(items) == (1 if changed else 2), "Barcode format selection count mismatch")
        require(sorted(item["text"] for item in items) == (["LyFlow-P4"] if changed else ["AAAA", "LyFlow-P4"]), "Barcode payload mismatch")
        require(all(item["valid"] for item in items), "Fixture barcode failed decoding")
        require([item["id"] for item in instances(outputs)] == [item["id"] for item in items], "Barcode quadrilateral/pose IDs differ")
    elif name == "p4-qr-refine":
        image = outputs["image"]["value"]
        size = 116 if changed else 232
        require(image["width"] == image["height"] == size, "QR module-size override mismatch")
        feedback = record(outputs, "feedback", "image.QrRefinement.v1")
        require(feedback["beforeDecoded"] is True and feedback["afterDecoded"] is True, "Clean QR must decode before and after reconstruction")
        require(feedback["improved"] is False, "Already decoded fixture must not claim improvement")
        for output in ("before", "after"):
            items = record(outputs, output, "image.Barcodes.v1")["items"]
            require(len(items) == 1 and items[0]["text"] == "LyFlow-P4", f"{output}: QR content changed")
        require(record(outputs, "after")["frame"] == "qr_refined", "Enhanced pixels must have their own frame")


def execute(cli, command, log):
    result = subprocess.run([str(cli), *command], capture_output=True, text=True, encoding="utf-8", timeout=240)
    log.with_suffix(".jsonl").write_text(result.stdout, encoding="utf-8")
    log.with_suffix(".stderr.log").write_text(result.stderr, encoding="utf-8")
    require(result.returncode == 0, f"CLI failed ({result.returncode}): {command}\n{result.stderr}\n{result.stdout[-6000:]}")
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def main():
    parser = argparse.ArgumentParser(description="Validate all P4 example schemas and saved CLI default/override/default contracts; no cases are skipped")
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
    manifest = json.loads((REPOSITORY / "packaging/p4-model-assets.json").read_text(encoding="utf-8"))
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
        for iteration, changed in enumerate((False, True, False)):
            options = []
            if changed:
                for key, value in overrides.items():
                    options.extend(("--param", key + "=" + json.dumps(value, ensure_ascii=False)))
            stem = output / f"{name}-{iteration}"
            execute(args.cli.resolve(), ["validate", str(graph), *options], stem.with_name(stem.name + "-validate"))
            lines = execute(args.cli.resolve(), ["run", str(graph), "--no-cache", "--parallel", "2", "--outputs", "--summary", *options], stem)
            require(len(lines) >= 2 and lines[-1].get("kind") == "run_summary", f"{name}: missing formal run summary")
            require(lines[-1]["status"] == "ok" and not lines[-1]["contractViolations"], f"{name}: unhealthy run summary")
            values = lines[-2]
            check(name, values, changed)
            if iteration == 0:
                defaults = values
            elif iteration == 2:
                require(values == defaults, f"{name}: default results changed after isolated override")
        report["cases"].append({"graph": name, "runs": 3, "override": overrides, "status": "passed"})
    require(original == {str(path.relative_to(saved)): digest(path) for path in saved.rglob("*") if path.is_file()}, "CLI changed saved source graphs/assets")
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"schemas": len(CASES), "runs": 3 * len(CASES), "status": "passed", "report": str(output / "report.json")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
