import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import stat
import tempfile
import urllib.parse
import urllib.request


REPOSITORY = Path(__file__).resolve().parents[1]
MAX_ASSET_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 512 * 1024 * 1024


def link_like(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return path.is_symlink() or bool(getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def checked_path(path):
    result = Path(os.path.abspath(path))
    for part in reversed((result, *result.parents)):
        if link_like(part):
            raise ValueError(f"Link/reparse paths are not allowed: {part}")
    return result


def filename(value):
    if (not isinstance(value, str) or not value or len(value) > 200
            or value in (".", "..") or value[-1] in " ."
            or any(ord(c) < 32 or c in '/\\:<>"|?*' for c in value)
            or PureWindowsPath(value).is_reserved()):
        raise ValueError("Asset name must be one portable plain filename")
    return value


def local_path(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("Repository asset must have a bounded relative POSIX path")
    parts = value.split("/")
    if any(not part or part in (".", "..") for part in parts) or PurePosixPath(value).is_absolute():
        raise ValueError("Repository asset must have a bounded relative POSIX path")
    for part in parts:
        filename(part)
    return Path(*parts)


def source_url(value):
    if not isinstance(value, str) or len(value) > 8192 or any(ord(c) < 33 for c in value):
        raise ValueError("Asset URL is invalid")
    parsed = urllib.parse.urlsplit(value)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username is not None or parsed.password is not None or parsed.fragment or parsed.port not in (None, 443):
        raise ValueError("Public asset source must be HTTPS without credentials")
    return value


class HttpsRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        source_url(new_url)
        return super().redirect_request(request, response, code, message, headers, new_url)


def digest(path, maximum=MAX_ASSET_BYTES):
    result = hashlib.sha256()
    count = 0
    with path.open("rb") as stream:
        while chunk := stream.read(65536):
            count += len(chunk)
            if count > maximum:
                raise ValueError("Asset changed beyond its fixed size during hashing")
            result.update(chunk)
    return result.hexdigest()


def matches(path, asset):
    path = checked_path(path)
    return path.is_file() and path.stat().st_size == asset["bytes"] and digest(path, asset["bytes"]) == asset["sha256"]


def manifest(path):
    path = checked_path(path)
    if not path.is_file() or path.stat().st_size > 256 * 1024:
        raise ValueError("Asset manifest must be a regular file of at most 256KiB")
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or type(document.get("schemaVersion")) is not int or document.get("schemaVersion") != 1 or not isinstance(document.get("assets"), list) or not 1 <= len(document["assets"]) <= 64:
        raise ValueError("Invalid P4 asset manifest")
    seen, total = set(), 0
    for asset in document["assets"]:
        if not isinstance(asset, dict):
            raise ValueError("Asset entry must be an object")
        name = filename(asset.get("name"))
        if name.casefold() in seen:
            raise ValueError("Duplicate asset filename")
        seen.add(name.casefold())
        size, sha = asset.get("bytes"), asset.get("sha256")
        if type(size) is not int or not 0 < size <= MAX_ASSET_BYTES:
            raise ValueError("Asset exceeds 64MiB budget")
        total += size
        if total > MAX_TOTAL_BYTES:
            raise ValueError("Asset set exceeds 512MiB budget")
        if not isinstance(sha, str) or len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
            raise ValueError("Asset SHA256 is invalid")
        if "reuseName" in asset:
            filename(asset["reuseName"])
        if "local" in asset:
            local_path(asset["local"])
            if "urls" in asset:
                raise ValueError("Local assets cannot also have download URLs")
        else:
            urls = asset.get("urls")
            if not isinstance(urls, list) or not 1 <= len(urls) <= 4:
                raise ValueError("Asset needs one to four fixed HTTPS sources")
            for url in urls:
                source_url(url)
    return document


def copy_bounded(original, destination, maximum):
    count = 0
    while chunk := original.read(65536):
        count += len(chunk)
        if count > maximum:
            raise ValueError("Asset exceeds fixed size")
        destination.write(chunk)


def fetch(configuration, output, asset_dirs=(), offline=False, verify_only=False):
    document = manifest(configuration)
    output = checked_path(output)
    if verify_only:
        if not output.is_dir():
            raise FileNotFoundError("Asset output directory is missing")
    else:
        output.mkdir(parents=True, exist_ok=True)
    output = checked_path(output)
    if not output.is_dir():
        raise ValueError("Asset output must be a directory")
    directories = [checked_path(directory) for directory in asset_dirs]
    opener = urllib.request.build_opener(HttpsRedirects())
    for asset in document["assets"]:
        target = checked_path(output / asset["name"])
        if target.exists():
            if not matches(target, asset):
                raise ValueError(f"Existing asset failed size/SHA256 validation: {target.name}")
            print(f"verified: {target.name}")
            continue
        if verify_only:
            raise FileNotFoundError(f"Asset missing: {target.name}")
        candidates = []
        for directory in directories:
            for name in dict.fromkeys((asset["name"], asset.get("reuseName", asset["name"]))):
                candidates.append(directory / name)
        if "local" in asset:
            candidates.insert(0, checked_path(REPOSITORY / local_path(asset["local"])))
        source = None
        for candidate in candidates:
            candidate = checked_path(candidate)
            if candidate.exists():
                if not matches(candidate, asset):
                    raise ValueError(f"Cached/repository asset failed size/SHA256 validation: {candidate.name}")
                if source is None:
                    source = candidate
        temporary = None
        try:
            if source is not None:
                with tempfile.NamedTemporaryFile(dir=output, prefix="lyflow-p4-", suffix=".part", delete=False) as stream:
                    temporary = Path(stream.name)
                    with source.open("rb") as original:
                        copy_bounded(original, stream, asset["bytes"])
            else:
                if offline or "local" in asset:
                    raise FileNotFoundError(f"Verified asset unavailable offline: {target.name}")
                failures = []
                for url in asset["urls"]:
                    try:
                        with tempfile.NamedTemporaryFile(dir=output, prefix="lyflow-p4-", suffix=".part", delete=False) as stream:
                            temporary = Path(stream.name)
                            request = urllib.request.Request(url, headers={"User-Agent": "LyFlow-P4"})
                            with opener.open(request, timeout=60) as response:
                                source_url(response.geturl())
                                copy_bounded(response, stream, asset["bytes"])
                        if not matches(temporary, asset):
                            raise ValueError("Downloaded asset failed size/SHA256 validation")
                        break
                    except (OSError, ValueError) as error:
                        failures.append(str(error))
                        if temporary is not None:
                            temporary.unlink(missing_ok=True)
                            temporary = None
                if temporary is None:
                    raise ValueError(f"All public sources failed for {target.name}: {'; '.join(failures)}")
            if not matches(temporary, asset):
                raise ValueError(f"Reused asset changed during copy: {target.name}")
            checked_path(target)
            os.link(temporary, target)
            print(f"prepared: {target.name}")
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Fetch fixed public P4 example models, dictionaries and attribution assets")
    parser.add_argument("--config", type=Path, default=REPOSITORY / "packaging/p4-model-assets.json")
    parser.add_argument("--output-dir", type=Path, default=REPOSITORY / "build/p4-assets")
    parser.add_argument("--asset-dir", type=Path, action="append", default=[])
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    fetch(args.config, args.output_dir, args.asset_dir, args.offline, args.verify_only)
