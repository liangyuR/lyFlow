import argparse
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np


def qr_modules(text):
    params = cv2.QRCodeEncoder_Params()
    params.version = 1
    params.correction_level = cv2.QRCodeEncoder_CORRECT_LEVEL_L
    params.mode = cv2.QRCodeEncoder_MODE_BYTE
    encoded = cv2.QRCodeEncoder_create(params).encode(text)
    if encoded.shape != (25, 25):
        raise ValueError(f"Expected version-1 QR with two quiet modules: {encoded.shape}")
    return encoded[2:23, 2:23].copy()


def render_qr(modules, scale):
    return np.repeat(np.repeat(np.pad(modules, 4, constant_values=255), scale, axis=0), scale, axis=1)


def code39():
    widths = [255] * 20
    for index, symbol in enumerate((0x094, 0x109, 0x109, 0x109, 0x109, 0x094)):
        if index:
            widths.extend([255] * 2)
        for bit in range(8, -1, -1):
            widths.extend([0 if bit % 2 == 0 else 255] * (6 if symbol & (1 << bit) else 2))
    widths.extend([255] * 20)
    result = np.full((120, len(widths)), 255, dtype=np.uint8)
    result[10:110] = widths
    return result


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    modules = qr_modules("LyFlow-P4")
    qr = render_qr(modules, 8)
    mixed = np.full((300, 740), 255, dtype=np.uint8)
    small_qr = render_qr(modules, 6)
    bars = code39()
    mixed[35:35 + small_qr.shape[0], 25:25 + small_qr.shape[1]] = small_qr
    mixed[65:65 + bars.shape[0], 245:245 + bars.shape[1]] = bars
    for name, pixels in (("p4-qr-source.png", qr), ("p4-barcodes.png", mixed)):
        ok, encoded = cv2.imencode(".png", pixels)
        if not ok:
            raise ValueError(f"PNG encoding failed: {name}")
        (output / name).write_bytes(encoded.tobytes())
    truth = {
        "origin": "LyFlow generated barcode artwork; no external image or font is distributed",
        "generator": "scripts/generate-p4-barcode-fixtures.py",
        "qr": {"text": "LyFlow-P4", "format": "QRCode", "version": 1, "moduleCount": 21,
               "errorCorrection": "L", "mode": "byte", "quietZoneModules": 4,
               "sourceModulePixels": 8, "sourceSize": [232, 232],
               "modules": ["".join("1" if value == 0 else "0" for value in row) for row in modules]},
        "linear": {"text": "AAAA", "format": "Code39", "wideNarrowRatio": 3},
        "mixedSize": [740, 300],
        "files": {name: {"bytes": (output / name).stat().st_size,
                         "sha256": hashlib.sha256((output / name).read_bytes()).hexdigest()}
                  for name in ("p4-qr-source.png", "p4-barcodes.png")},
    }
    (output / "p4-barcode-truth.json").write_text(json.dumps(truth, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return truth


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Regenerate the owned P4 QR and Code39 fixtures with NumPy and OpenCV Python")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parents[1] / "examples")
    args = parser.parse_args()
    print(json.dumps(generate(args.output_dir), ensure_ascii=False, indent=2))
