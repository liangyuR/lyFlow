import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("matching_dependency_safety", ROOT / "scripts/fetch-matching-deps.py")
SAFETY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SAFETY)
STAMP = "prepared.json"
MAX_FILES = 20000
MAX_EXPANDED = 128 * 1024 * 1024
MAX_MEMBER = 16 * 1024 * 1024


def digest(path):
    value = hashlib.sha512()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def check_archive(path, asset):
    if SAFETY.link_like(path) or not path.is_file() or path.stat().st_size != asset["bytes"] or digest(path) != asset["sha512"]:
        raise ValueError(f"Archive size/SHA512 mismatch: {path}")


def read_manifest(path):
    asset = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(asset, dict):
        raise ValueError("Profile math dependency manifest must be an object")
    version = asset.get("version", "")
    if asset.get("schemaVersion") != 1 or asset.get("name") != "eigen3" or not isinstance(version, str) or not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("Unsupported profile math dependency manifest")
    pinned = json.loads((ROOT / "packaging/release.json").read_text(encoding="utf-8-sig"))["vcpkgVersions"]["eigen3"]
    if version != pinned or asset.get("url") != f"https://gitlab.com/libeigen/eigen/-/archive/{version}/eigen-{version}.tar.gz":
        raise ValueError("Eigen version/official source differs from the release lock")
    if asset.get("archive") != f"eigen-{version}.tar.gz" or not re.fullmatch(r"[0-9a-f]{128}", asset.get("sha512", "")):
        raise ValueError("Expected a pinned archive name and SHA512")
    if type(asset.get("bytes")) is not int or not 0 < asset["bytes"] <= 32 * 1024 * 1024:
        raise ValueError("Invalid archive byte budget")
    for key in ["keyHeaders", "licenseFiles"]:
        if not isinstance(asset.get(key), list) or not asset[key]:
            raise ValueError(f"Missing {key}")
        for name in asset[key]:
            if not isinstance(name, str):
                raise ValueError(f"Invalid {key} path")
            SAFETY.safe_parts(name)
    return asset


def safe_extract(archive, destination):
    with tarfile.open(archive, "r:gz") as source:
        checked, spellings, seen = [], {}, set()
        files, directories = set(), set()
        total, root = 0, None
        for entry in source:
            if len(checked) >= MAX_FILES:
                raise ValueError("Archive file count exceeds budget")
            parts = SAFETY.safe_parts(entry.name)
            root = root or parts[0]
            if root != parts[0]:
                raise ValueError("Archive must have one top-level source directory")
            if not (entry.isfile() or entry.isdir()) or entry.issparse():
                raise ValueError("Archive links/sparse/special files are forbidden")
            if entry.size < 0 or entry.size > MAX_MEMBER or (entry.isdir() and entry.size):
                raise ValueError("Archive member exceeds byte budget")
            total += entry.size
            if total > MAX_EXPANDED:
                raise ValueError("Expanded archive exceeds byte budget")
            for length in range(1, len(parts) + 1):
                spelling = "/".join(parts[:length])
                identity = spelling.casefold()
                if identity in spellings and spellings[identity] != spelling:
                    raise ValueError("Case-colliding archive path")
                spellings[identity] = spelling
            identity = "/".join(parts).casefold()
            if identity in seen or (len(parts) == 1 and not entry.isdir()):
                raise ValueError("Duplicate or invalid top-level archive entry")
            ancestors = {"/".join(parts[:length]).casefold() for length in range(1, len(parts))}
            if files.intersection(ancestors) or (entry.isfile() and identity in directories):
                raise ValueError("Archive file/directory paths conflict")
            directories.update(ancestors)
            (files if entry.isfile() else directories).add(identity)
            seen.add(identity)
            checked.append((entry, parts[1:]))
        if not checked:
            raise ValueError("Empty archive")
        destination.mkdir(parents=True, exist_ok=False)
        for entry, parts in checked:
            target = destination.joinpath(*parts)
            if entry.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(entry) as incoming, target.open("xb") as outgoing:
                    shutil.copyfileobj(incoming, outgoing, 1024 * 1024)


def acquire(asset, cache, archive_dir, offline):
    path = cache / asset["archive"]
    if path.exists() or path.is_symlink():
        check_archive(path, asset)
        return path
    if archive_dir is not None:
        candidate = archive_dir / asset["archive"]
        check_archive(candidate, asset)
        with candidate.open("rb") as incoming:
            return cache_archive(incoming, path, asset)
    if offline:
        raise FileNotFoundError(f"Offline archive not found: {path}")
    request = urllib.request.Request(asset["url"], headers={"User-Agent": "LyFlow-Profile-Math-Dependencies"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return cache_archive(response, path, asset)


def cache_archive(incoming, path, asset):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, suffix=".part", delete=False) as output:
            temporary = Path(output.name)
            count = 0
            while chunk := incoming.read(1024 * 1024):
                count += len(chunk)
                if count > asset["bytes"]:
                    raise ValueError("Archive download exceeds byte budget")
                output.write(chunk)
        check_archive(temporary, asset)
        if path.exists() or path.is_symlink():
            raise FileExistsError(f"Archive appeared concurrently: {path}")
        temporary.rename(path)
        temporary = None
        return path
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def check_source(source, asset):
    for name in [*asset["keyHeaders"], *asset["licenseFiles"], "cmake/Eigen3Config.cmake.in", "cmake/Eigen3ConfigVersion.cmake.in"]:
        if not (source / name).is_file() or SAFETY.link_like(source / name):
            raise ValueError(f"Required Eigen header/license/config is missing: {name}")
    text = (source / "Eigen/Version").read_text(encoding="utf-8")
    numbers = []
    for part in ["MAJOR", "MINOR", "PATCH"]:
        match = re.search(rf"^#define EIGEN_{part}_VERSION (\d+)$", text, re.MULTILINE)
        if not match:
            raise ValueError("Eigen source version header is invalid")
        numbers.append(match[1])
    if ".".join(numbers) != asset["version"]:
        raise ValueError("Eigen source version does not match the pinned archive")


def install_headers(source, install, work, asset, cmake):
    wrapper = work / "wrapper"
    wrapper.mkdir()
    notices = " ".join(f'"${{EIGEN_SOURCE}}/{name}"' for name in asset["licenseFiles"])
    (wrapper / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project(LyFlowProfileMathHeaders VERSION {asset["version"]} LANGUAGES NONE)
include(CMakePackageConfigHelpers)
add_library(Eigen INTERFACE)
target_include_directories(Eigen INTERFACE "$<INSTALL_INTERFACE:include/eigen3>")
install(TARGETS Eigen EXPORT Eigen3Targets)
install(EXPORT Eigen3Targets NAMESPACE Eigen3:: DESTINATION share/eigen3/cmake)
install(DIRECTORY "${{EIGEN_SOURCE}}/Eigen" DESTINATION include/eigen3)
install(DIRECTORY "${{EIGEN_SOURCE}}/unsupported/Eigen" DESTINATION include/eigen3/unsupported)
install(FILES {notices} DESTINATION share/eigen3)
configure_package_config_file("${{EIGEN_SOURCE}}/cmake/Eigen3Config.cmake.in"
  "${{CMAKE_CURRENT_BINARY_DIR}}/Eigen3Config.cmake" INSTALL_DESTINATION share/eigen3/cmake)
set(CVF_VERSION "${{PROJECT_VERSION}}")
configure_file("${{EIGEN_SOURCE}}/cmake/Eigen3ConfigVersion.cmake.in"
  "${{CMAKE_CURRENT_BINARY_DIR}}/Eigen3ConfigVersion.cmake" @ONLY)
install(FILES "${{CMAKE_CURRENT_BINARY_DIR}}/Eigen3Config.cmake"
  "${{CMAKE_CURRENT_BINARY_DIR}}/Eigen3ConfigVersion.cmake" DESTINATION share/eigen3/cmake)
''', encoding="utf-8")
    ninja = shutil.which("ninja")
    cmake_executable = Path(shutil.which(cmake) or cmake)
    if not ninja and len(cmake_executable.parents) >= 3:
        sibling = cmake_executable.parents[2] / "Ninja/ninja.exe"
        if sibling.is_file():
            ninja = str(sibling)
    generator = ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"] if ninja else []
    commands = [[cmake, "-S", str(wrapper), "-B", str(work / "cmake"), *generator, f"-DEIGEN_SOURCE={source.as_posix()}",
                 f"-DCMAKE_INSTALL_PREFIX={install.as_posix()}"], [cmake, "--install", str(work / "cmake")]]
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode:
            raise ValueError(f"Eigen header installation failed:\n{result.stdout}\n{result.stderr}")


def inventory(path):
    files = SAFETY.inventory(path)
    files.pop(STAMP, None)
    if (path / SAFETY.STAMP).is_file():
        files[SAFETY.STAMP] = SAFETY.sha256(path / SAFETY.STAMP)
    return files


def check_install(source, install, asset):
    for name in asset["keyHeaders"]:
        header = install / "include/eigen3" / name
        if not header.is_file() or SAFETY.link_like(header) or SAFETY.sha256(header) != SAFETY.sha256(source / name):
            raise ValueError(f"Installed Eigen header differs from source: {name}")
    for name in asset["licenseFiles"]:
        notice = install / "share/eigen3" / name
        if not notice.is_file() or SAFETY.link_like(notice) or SAFETY.sha256(notice) != SAFETY.sha256(source / name):
            raise ValueError(f"Installed Eigen license differs from source: {name}")
    for name in ["Eigen3Config.cmake", "Eigen3ConfigVersion.cmake", "Eigen3Targets.cmake"]:
        path = install / "share/eigen3/cmake" / name
        if not path.is_file() or SAFETY.link_like(path) or not path.stat().st_size:
            raise ValueError(f"Installed Eigen CMake configuration is missing: {name}")


def prepare(configuration, output, archive_dir=None, offline=False, verify_only=False, cmake="cmake"):
    asset = read_manifest(configuration)
    output = Path(os.path.abspath(output))
    for ancestor in [output, *output.parents]:
        if ancestor.exists() and SAFETY.link_like(ancestor):
            raise ValueError(f"Output must not traverse links/reparse points: {ancestor}")
    output.parent.mkdir(parents=True, exist_ok=True)
    cache = output.with_name(output.name + "-archives")
    if cache.is_symlink() or (cache.exists() and SAFETY.link_like(cache)):
        raise ValueError("Archive cache must not be a link/reparse point")
    cache.mkdir(exist_ok=True)
    signature = {"manifestSha256": SAFETY.sha256(configuration), "version": asset["version"], "archiveSha512": asset["sha512"]}
    if output.exists():
        marker = output / STAMP
        if not marker.is_file() or SAFETY.link_like(marker):
            raise ValueError("Unmanaged output; prepare into a clean output directory")
        existing = json.loads(marker.read_text(encoding="utf-8"))
        if existing.get("signature") != signature or existing.get("files") != inventory(output):
            raise ValueError("Prepared source/headers changed; use a clean output directory")
        check_archive(cache / asset["archive"], asset)
        check_source(output / "source", asset)
        check_install(output / "source", output / "install", asset)
    else:
        if verify_only:
            raise FileNotFoundError("Profile math dependencies are not prepared")
        archive = acquire(asset, cache, archive_dir, offline)
        with tempfile.TemporaryDirectory(prefix=output.name + "-prepare-", dir=output.parent) as temporary:
            work = Path(temporary)
            package = work / "package"
            package.mkdir()
            source = package / "source"
            safe_extract(archive, source)
            check_source(source, asset)
            install_headers(source, package / "install", work, asset, cmake)
            check_install(source, package / "install", asset)
            (package / STAMP).write_text(json.dumps({"signature": signature, "files": inventory(package)}, sort_keys=True, indent=2) + "\n", encoding="utf-8")
            if output.exists() or output.is_symlink():
                raise FileExistsError("Output appeared concurrently")
            package.rename(output)
    eigen_dir = output / "install/share/eigen3/cmake"
    print(json.dumps({"schemaVersion": 1, "version": asset["version"], "Eigen3_DIR": str(eigen_dir), "verified": True}))
    return eigen_dir


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=ROOT / "packaging/profile-math-deps.json")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--archive-dir", type=Path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    try:
        prepare(args.manifest, args.output_dir, args.archive_dir, args.offline, args.verify_only, args.cmake)
    except (OSError, ValueError, tarfile.TarError) as error:
        parser.exit(1, f"Profile math dependency preparation failed: {error}\n")


if __name__ == "__main__":
    main()
