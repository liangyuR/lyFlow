import argparse
import importlib.util
import json
from pathlib import Path
import re
import tempfile


STAMP = ".lyflow-barcode-source.json"


def prepare(repository, output, archive_dir, offline, verify_only):
    specification = importlib.util.spec_from_file_location("lyflow_source_tools", repository / "scripts/fetch-matching-deps.py")
    tools = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(tools)
    configuration = repository / "packaging/barcode-deps.json"
    document = json.loads(configuration.read_text(encoding="utf-8-sig"))
    if document.get("schemaVersion") != 1 or not isinstance(document.get("patchRevision"), int) or len(document.get("assets", [])) != 1:
        raise ValueError("Unsupported barcode dependency manifest")
    asset = document["assets"][0]
    if asset.get("name") != "zxing" or asset.get("repository") != "zxing-cpp/zxing-cpp":
        raise ValueError("Expected the fixed ZXing-C++ source")
    if not re.fullmatch(r"[0-9a-f]{40}", asset["revision"]) or not re.fullmatch(r"[0-9a-f]{64}", asset["sha256"]):
        raise ValueError("Expected fixed source revision and archive SHA256")
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", asset["tag"]):
        raise ValueError("Expected a stable release tag")
    if len(tools.safe_parts(asset["archive"])) != 1 or not isinstance(asset["bytes"], int) or not 0 < asset["bytes"] <= 16 * 1024 * 1024:
        raise ValueError("Invalid archive filename or byte budget")
    expected_url = f"https://github.com/zxing-cpp/zxing-cpp/releases/download/{asset['tag']}/{asset['archive']}"
    if asset["url"] != expected_url:
        raise ValueError("Only the exact official release asset is supported")
    if len(tools.safe_parts(asset["patch"])) != 1:
        raise ValueError("Patch must be a plain filename")
    patch = repository / "packaging/patches" / asset["patch"]
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if tools.link_like(output):
        raise ValueError("Prepared output must not be a link")
    cache = output / "archives"
    cache.mkdir(exist_ok=True)
    if tools.link_like(cache):
        raise ValueError("Archive cache must not be a link")
    signature = {
        "manifestSha256": tools.sha256(configuration),
        "archiveSha256": asset["sha256"],
        "revision": asset["revision"],
        "patchRevision": document["patchRevision"],
        "patchSha256": tools.sha256(patch),
        "preparerSha256": tools.sha256(Path(__file__)),
        "sourceToolsSha256": tools.sha256(repository / "scripts/fetch-matching-deps.py"),
    }

    def inventory(source):
        files = tools.inventory(source)
        files.pop(STAMP, None)
        return files

    target = output / "zxing"
    marker = target / STAMP
    if target.exists():
        if tools.link_like(target) or not marker.is_file() or tools.link_like(marker):
            raise ValueError("Unmanaged barcode source directory")
        existing = json.loads(marker.read_text(encoding="utf-8"))
        if existing.get("signature") != signature or existing.get("files") != inventory(target):
            raise ValueError("Barcode source changed; prepare in a clean output directory")
    else:
        if verify_only:
            raise FileNotFoundError("ZXing-C++ source has not been prepared")
        archive = tools.acquire_archive(asset, cache, archive_dir, offline)
        with tempfile.TemporaryDirectory(prefix="prepare-", dir=output) as temporary:
            source = Path(temporary) / "source"
            tools.safe_extract(archive, source)
            tools.apply_patch(source, patch)
            for notice in asset["licenseFiles"]:
                tools.safe_parts(notice)
                if not (source / notice).is_file():
                    raise ValueError(f"Missing required source notice: {notice}")
            existing = {"signature": signature, "files": inventory(source)}
            (source / STAMP).write_text(json.dumps(existing, sort_keys=True, indent=2) + "\n", encoding="utf-8")
            if target.exists():
                raise FileExistsError("Barcode source appeared concurrently")
            source.rename(target)
    summary = {"schemaVersion": 1, "signature": signature, "inventorySha256": tools.sha256(marker)}
    prepared = output / "prepared.json"
    if verify_only:
        if not prepared.is_file() or tools.link_like(prepared) or json.loads(prepared.read_text(encoding="utf-8")) != summary:
            raise ValueError("Barcode dependency summary is stale")
    else:
        with tempfile.NamedTemporaryFile(mode="w", dir=output, prefix="prepared-", suffix=".json", encoding="utf-8", delete=False) as stream:
            temporary_summary = Path(stream.name)
            json.dump(summary, stream, sort_keys=True, indent=2)
            stream.write("\n")
        try:
            temporary_summary.replace(prepared)
        finally:
            temporary_summary.unlink(missing_ok=True)
    print(f"verified: ZXing-C++ {asset['tag']} {asset['revision']}")


def main():
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Prepare fixed ZXing-C++ release sources without network access during CMake.")
    parser.add_argument("--output-dir", type=Path, default=repository / "build/barcode-deps")
    parser.add_argument("--archive-dir", type=Path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    prepare(repository, args.output_dir, args.archive_dir.resolve() if args.archive_dir else None, args.offline, args.verify_only)


if __name__ == "__main__":
    main()
