import argparse
import json
import os
from pathlib import Path
import tempfile
import urllib.request


def fetch(configuration, output):
    document = json.loads(configuration.read_text(encoding="utf-8"))
    output.mkdir(parents=True, exist_ok=True)
    for asset in document["assets"]:
        target = output / asset["name"]
        if Path(asset["name"]).name != asset["name"]:
            raise ValueError("Asset name must be a plain filename")
        if target.exists():
            if not target.is_file() or target.stat().st_size != asset["bytes"]:
                raise ValueError(f"Existing asset has an unexpected size: {target}")
            print(f"ready: {target.name}")
            continue
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=output, prefix="lyflow-model-", suffix=".part",
                                             delete=False) as stream:
                temporary = Path(stream.name)
                request = urllib.request.Request(asset["url"], headers={"User-Agent": "LyFlow-P1"})
                with urllib.request.urlopen(request, timeout=30) as response:
                    count = 0
                    while chunk := response.read(65536):
                        count += len(chunk)
                        if count > asset["bytes"]:
                            raise ValueError(f"Oversized asset: {asset['name']}")
                        stream.write(chunk)
                if count != asset["bytes"]:
                    raise ValueError(f"Truncated asset: {asset['name']}")
            if target.exists():
                raise FileExistsError(f"Asset appeared while downloading: {target}")
            os.replace(temporary, target)
            temporary = None
            print(f"downloaded: {target.name}")
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path,
                        default=Path(__file__).resolve().parents[1] / "packaging/p1-model-assets.json")
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build/p1-assets")
    arguments = parser.parse_args()
    fetch(arguments.config, arguments.output_dir)
