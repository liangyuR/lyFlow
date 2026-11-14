import argparse
import importlib.metadata
import json
import os
from pathlib import Path


def export(directory):
    if importlib.metadata.version("ultralytics") != "8.4.21":
        raise RuntimeError("P1 reproducible exports require ultralytics==8.4.21")
    if importlib.metadata.version("onnx") != "1.17.0":
        raise RuntimeError("P1 reproducible exports require onnx==1.17.0")
    torch_version = importlib.metadata.version("torch")
    if torch_version.split("+")[0] != "2.10.0":
        raise RuntimeError("P1 reproducible exports require torch==2.10.0")
    directory = directory.resolve()
    settings = directory / "ultralytics-settings"
    settings.mkdir(parents=True, exist_ok=True)
    os.environ["YOLO_CONFIG_DIR"] = str(settings)
    os.environ["YOLO_AUTOINSTALL"] = "false"
    from ultralytics import YOLO
    import onnx

    entries = []
    for name in ("yolov8n", "yolo11n", "yolo26n"):
        weights = directory / (name + ".pt")
        if not weights.is_file():
            raise FileNotFoundError(weights)
        model = YOLO(str(weights), task="detect")
        output = Path(model.export(format="onnx", imgsz=640, batch=1, dynamic=False,
                                  simplify=False, opset=17, device="cpu", half=False, nms=False))
        graph = onnx.load(str(output))
        onnx.checker.check_model(graph)
        if len(graph.graph.input) != 1 or len(graph.graph.output) != 1:
            raise ValueError("Detection export must have one input and one output")
        shape = lambda item: [dimension.dim_value for dimension in item.type.tensor_type.shape.dim]
        expected = [1, 300, 6] if name == "yolo26n" else [1, 84, 8400]
        if shape(graph.graph.input[0]) != [1, 3, 640, 640] or shape(graph.graph.output[0]) != expected:
            raise ValueError(f"Unexpected model signature for {name}")
        entries.append({"model": name, "file": output.name, "opset": 17,
                        "input": [1, 3, 640, 640], "output": expected,
                        "exporter": "ultralytics==8.4.21", "onnx": "1.17.0", "torch": torch_version})
    (directory / "yolo-export-provenance.json").write_text(json.dumps(entries, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets-dir", type=Path, default=Path(__file__).resolve().parents[1] / "build/p1-assets")
    export(parser.parse_args().assets_dir)
