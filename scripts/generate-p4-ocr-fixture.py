import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def generate(output, latin_font, chinese_font):
    latin = ImageFont.truetype(str(latin_font), 72)
    chinese = ImageFont.truetype(str(chinese_font), 64)
    image = Image.new("RGB", (1200, 800), "white")
    draw = ImageDraw.Draw(image)
    for text, y, font in [("LYFLOW 12345", 65, latin), ("INDUSTRIAL OCR", 235, latin), ("工业视觉 12345", 405, chinese)]:
        draw.text((90, y), text, font=font, fill="black")
    last = Image.new("RGB", (1000, 140), "white")
    ImageDraw.Draw(last).text((20, 30), "HELLO 2026", font=latin, fill="black")
    image.paste(last.rotate(180), (90, 600))
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output)
    metadata = {
        "origin": "LyFlow generated text fixture, not a photograph",
        "text": ["LYFLOW 12345", "INDUSTRIAL OCR", "工业视觉 12345", "HELLO 2026"],
        "lastLineRotationDegrees": 180,
        "fonts": [
            {"file": latin_font.name, "source": "Windows installed Arial Bold, raster output only, font not distributed"},
            {"file": chinese_font.name, "source": "Windows installed SimHei, raster output only, font not distributed"},
        ],
        "bytes": output.stat().st_size,
        "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
    }
    output.with_name(output.stem + "-truth.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate the P4 OCR raster fixture using locally licensed fonts and Pillow")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "examples/p4-ocr-scene.png")
    parser.add_argument("--latin-font", type=Path, default=Path("C:/Windows/Fonts/arialbd.ttf"))
    parser.add_argument("--chinese-font", type=Path, default=Path("C:/Windows/Fonts/simhei.ttf"))
    args = parser.parse_args()
    print(json.dumps(generate(args.output, args.latin_font, args.chinese_font), ensure_ascii=False, indent=2))
