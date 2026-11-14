import argparse
import hashlib
import json
import ntpath
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shutil
import stat
import subprocess
import tempfile
import urllib.request
import zipfile


NAMES = {"kiss-matcher", "robin", "small-gicp", "xenium", "tbb"}
STAMP = ".lyflow-matching-source.json"
MAX_FILES = 20000
MAX_EXPANDED = 256 * 1024 * 1024


def link_like(path):
    return path.is_symlink() or bool(getattr(path.lstat(), "st_file_attributes", 0) & 1024)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def safe_parts(name):
    path = PurePosixPath(name)
    if not name or "\\" in name or path.is_absolute() or not path.parts:
        raise ValueError(f"Unsafe archive/patch path: {name!r}")
    for part in path.parts:
        reserved = ntpath.isreserved(part) if hasattr(ntpath, "isreserved") else PureWindowsPath(part).is_reserved()
        if part in {".", ".."} or any(c in ':<>"|?*' for c in part) or part.rstrip(" .") != part or reserved or any(ord(c) < 32 for c in part):
            raise ValueError(f"Unsafe archive/patch path: {name!r}")
    return path.parts


def check_archive(path, asset):
    if not path.is_file() or link_like(path) or path.stat().st_size != asset["bytes"] or sha256(path) != asset["sha256"]:
        raise ValueError(f"Archive size/SHA256 mismatch: {path}")


def read_manifest(configuration):
    document = json.loads(configuration.read_text(encoding="utf-8-sig"))
    if document.get("schemaVersion") != 1 or not isinstance(document.get("patchRevision"), int):
        raise ValueError("Unsupported matching dependency manifest")
    assets = document.get("assets", [])
    if len(assets) != 5 or {asset.get("name") for asset in assets} != NAMES:
        raise ValueError("Matching manifest must contain exactly the five fixed CPU dependencies")
    for asset in assets:
        for key in ["name", "archive"]:
            if len(safe_parts(asset[key])) != 1:
                raise ValueError(f"Expected a plain {key}")
        if not re.fullmatch(r"[0-9a-f]{40}", asset["revision"]) or not re.fullmatch(r"[0-9a-f]{64}", asset["sha256"]):
            raise ValueError("Expected fixed revision and SHA256")
        if not isinstance(asset["bytes"], int) or not 0 < asset["bytes"] <= 64 * 1024 * 1024:
            raise ValueError("Archive byte budget is invalid")
        if not asset["url"].startswith("https://codeload.github.com/"):
            raise ValueError("Only fixed GitHub codeload archives are supported")
        if "patch" in asset and len(safe_parts(asset["patch"])) != 1:
            raise ValueError("Patch name must be a plain filename")
    return document


def acquire_archive(asset, cache, archive_dir, offline):
    source = archive_dir / asset["archive"] if archive_dir else None
    if source is not None and source.exists():
        check_archive(source, asset)
        return source
    target = cache / asset["archive"]
    if target.exists():
        check_archive(target, asset)
        return target
    if offline:
        raise FileNotFoundError(f"Offline archive not found: {asset['archive']}")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=cache, prefix="download-", suffix=".part", delete=False) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(asset["url"], headers={"User-Agent": "LyFlow-Matching-Dependencies"})
            with urllib.request.urlopen(request, timeout=60) as response:
                count = 0
                while chunk := response.read(1024 * 1024):
                    count += len(chunk)
                    if count > asset["bytes"]:
                        raise ValueError(f"Oversized archive: {asset['name']}")
                    output.write(chunk)
        check_archive(temporary, asset)
        if target.exists():
            raise FileExistsError(f"Archive appeared concurrently: {target}")
        temporary.rename(target)
        temporary = None
        return target
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def safe_extract(archive, destination):
    with zipfile.ZipFile(archive) as source:
        entries = source.infolist()
        if not entries or len(entries) > MAX_FILES:
            raise ValueError("Archive file count exceeds budget")
        total = 0
        root = None
        seen = set()
        spellings = {}
        checked = []
        for entry in entries:
            parts = safe_parts(entry.orig_filename)
            if root is None:
                root = parts[0]
            if parts[0] != root:
                raise ValueError("Archive must have one top-level source directory")
            for count in range(1, len(parts) + 1):
                spelling = "/".join(parts[:count])
                identity = spelling.casefold()
                if identity in spellings and spellings[identity] != spelling:
                    raise ValueError(f"Case-colliding archive directory: {entry.orig_filename}")
                spellings[identity] = spelling
            identity = "/".join(parts).casefold()
            if identity in seen:
                raise ValueError(f"Duplicate/case-colliding archive entry: {entry.filename}")
            seen.add(identity)
            mode = (entry.external_attr >> 16) & 0xFFFF
            kind = stat.S_IFMT(mode)
            if kind not in {0, stat.S_IFREG, stat.S_IFDIR}:
                raise ValueError(f"Archive links/special files are forbidden: {entry.filename}")
            if entry.flag_bits & 1:
                raise ValueError("Encrypted archives are unsupported")
            if entry.file_size > 32 * 1024 * 1024:
                raise ValueError("Archive member exceeds byte budget")
            total += entry.file_size
            if total > MAX_EXPANDED:
                raise ValueError("Expanded archive exceeds byte budget")
            checked.append((entry, parts[1:]))
        destination.mkdir(parents=True, exist_ok=False)
        for entry, parts in checked:
            if not parts:
                if not entry.is_dir():
                    raise ValueError("Top-level archive entry must be a directory")
                continue
            target = destination.joinpath(*parts)
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(entry) as input_stream, target.open("xb") as output_stream:
                    shutil.copyfileobj(input_stream, output_stream, 1024 * 1024)


def apply_patch(source, patch):
    text = patch.read_text(encoding="utf-8-sig")
    headers = 0
    for line in text.splitlines():
        if line.startswith(("--- ", "+++ ")):
            name = line[4:].split("\t", 1)[0]
            if name == "/dev/null":
                continue
            if not name.startswith(("a/", "b/")):
                raise ValueError("Patch paths must use a/ or b/ prefixes")
            safe_parts(name[2:])
            headers += 1
    if not headers:
        raise ValueError("Empty matching dependency patch")
    environment = os.environ.copy()
    for name in ["GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES"]:
        environment.pop(name, None)
    environment["GIT_CEILING_DIRECTORIES"] = str(source.parent.resolve())
    for extra in [["--check"], []]:
        result = subprocess.run(["git", "-C", str(source), "apply", "--whitespace=nowarn", *extra, str(patch.resolve())],
                                env=environment, capture_output=True, text=True, check=False)
        if result.returncode:
            raise ValueError(f"Pinned source patch failed ({patch.name}): {result.stderr.strip()}")


def inventory(source):
    result = {}

    def failed(error):
        raise error

    for directory, folders, files in os.walk(source, topdown=True, followlinks=False, onerror=failed):
        root = Path(directory)
        for name in sorted(folders + files):
            path = root / name
            if link_like(path):
                raise ValueError(f"Source link/reparse point is forbidden: {path}")
            if path.is_file() and path != source / STAMP:
                result[path.relative_to(source).as_posix()] = sha256(path)
    return result


def prepare(configuration, output, archive_dir=None, offline=False, verify_only=False):
    document = read_manifest(configuration)
    repository = Path(__file__).resolve().parents[1]
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cache = output / "archives"
    cache.mkdir(exist_ok=True)
    if link_like(cache):
        raise ValueError("Archive cache must not be a link/reparse point")
    source_hashes = {}
    for asset in document["assets"]:
        patch = repository / "packaging/patches" / asset["patch"] if "patch" in asset else None
        signature = {"manifestSha256": sha256(configuration), "patchRevision": document["patchRevision"],
                     "archiveSha256": asset["sha256"], "revision": asset["revision"],
                     "patchSha256": sha256(patch) if patch else None}
        target = output / asset["name"]
        marker = target / STAMP
        if target.exists():
            if link_like(target) or not marker.is_file() or link_like(marker):
                raise ValueError(f"Unmanaged matching source directory: {target}")
            existing = json.loads(marker.read_text(encoding="utf-8"))
            if existing.get("signature") != signature or existing.get("files") != inventory(target):
                raise ValueError(f"Matching source changed; prepare into a clean output directory: {target}")
            print(f"verified: {asset['name']}")
        else:
            if verify_only:
                raise FileNotFoundError(f"Matching dependency source is not prepared: {target}")
            archive = acquire_archive(asset, cache, archive_dir, offline)
            with tempfile.TemporaryDirectory(prefix="prepare-", dir=output) as temporary:
                source = Path(temporary) / "source"
                safe_extract(archive, source)
                if patch:
                    apply_patch(source, patch)
                for license_file in asset["licenseFiles"]:
                    safe_parts(license_file)
                    if not (source / license_file).is_file():
                        raise ValueError(f"Required source notice is missing: {license_file}")
                existing = {"signature": signature, "files": inventory(source)}
                marker_text = json.dumps(existing, sort_keys=True, indent=2) + "\n"
                (source / STAMP).write_text(marker_text, encoding="utf-8")
                if target.exists():
                    raise FileExistsError(f"Source appeared concurrently: {target}")
                source.rename(target)
            print(f"prepared: {asset['name']}")
        source_hashes[asset["name"]] = {"signature": signature, "inventorySha256": sha256(target / STAMP)}
    summary = {"schemaVersion": 1, "manifestSha256": sha256(configuration),
               "patchRevision": document["patchRevision"], "sources": source_hashes}
    if verify_only:
        prepared = output / "prepared.json"
        if not prepared.is_file() or link_like(prepared) or json.loads(prepared.read_text(encoding="utf-8")) != summary:
            raise ValueError("Matching dependency preparation summary is stale")
        return
    with tempfile.NamedTemporaryFile(mode="w", dir=output, prefix="prepared-", suffix=".json", encoding="utf-8", delete=False) as stream:
        temporary_summary = Path(stream.name)
        json.dump(summary, stream, sort_keys=True, indent=2)
        stream.write("\n")
    try:
        temporary_summary.replace(output / "prepared.json")
    finally:
        temporary_summary.unlink(missing_ok=True)


def main():
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Prepare the five pinned LyFlow CPU matching dependencies; PMC is excluded.")
    parser.add_argument("--output-dir", type=Path, default=repository / "build/matching-deps")
    parser.add_argument("--archive-dir", type=Path, help="Reuse previously verified zip archives before downloading")
    parser.add_argument("--offline", action="store_true", help="Forbid network access and require local archives")
    parser.add_argument("--verify-only", action="store_true", help="Verify prepared source inventories without downloading or extracting")
    arguments = parser.parse_args()
    prepare(repository / "packaging/matching-deps.json", arguments.output_dir,
            arguments.archive_dir.resolve() if arguments.archive_dir else None, arguments.offline, arguments.verify_only)


if __name__ == "__main__":
    main()
