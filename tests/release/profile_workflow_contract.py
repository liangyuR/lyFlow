import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

from public_robust_kernel_contract import isolated_workspace


ROOT = Path(__file__).resolve().parents[2]
PUBLIC_PACKS = {"std-profile", "std-geometry", "std-metrology"}
FORBIDDEN_PUBLIC = re.compile(r"opencv|onnxruntime|kiss.?matcher|small.?gicp|yaml|zxing", re.I)
DLL_IMPORT = re.compile(r"^\s+([\w.+-]+\.dll)\s*$", re.M | re.I)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def execute(arguments, directory, output, label, environment):
    result = subprocess.run(arguments, cwd=directory, env=environment, text=True,
                            encoding="utf-8", errors="replace", capture_output=True,
                            timeout=300, check=False)
    (output / f"{label}.stdout.log").write_text(result.stdout, encoding="utf-8")
    (output / f"{label}.stderr.log").write_text(result.stderr, encoding="utf-8")
    require(result.returncode == 0,
            f"{label} failed ({result.returncode}): {result.stderr[-4000:]}")
    return result.stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def record(outputs, name, expected):
    value = outputs[name]["value"]
    require(value["kind"] == "Record" and value["type"] == expected,
            f"Unexpected record contract for {name}")
    return value["data"]


def normalize(value):
    if isinstance(value, dict):
        return {key: ("@source-profile" if key in {"sourceCloudId", "cloudId"} else normalize(item))
                for key, item in value.items() if key != "byteSize"}
    if isinstance(value, list):
        return [normalize(item) for item in value]
    return value


def identical(a, b, path="outputs"):
    if isinstance(a, dict) and isinstance(b, dict):
        require(a.keys() == b.keys(), f"Host output keys differ at {path}")
        for key in a:
            identical(a[key], b[key], f"{path}.{key}")
    elif isinstance(a, list) and isinstance(b, list):
        require(len(a) == len(b), f"Host output lengths differ at {path}")
        for index, (left, right) in enumerate(zip(a, b)):
            identical(left, right, f"{path}[{index}]")
    elif isinstance(a, float) and isinstance(b, float):
        require(struct.pack(">d", a) == struct.pack(">d", b),
                f"Host floating-point bits differ at {path}: {a} / {b}")
    else:
        require(type(a) is type(b) and a == b, f"Host output differs at {path}: {a} / {b}")


def runtime_imports(runtime, dumpbin, workspace, output, environment, public):
    files = sorted(runtime.rglob("*.dll"))
    require(files, "Selected runtime contains no DLLs")
    providers = {path.name.casefold(): path for path in files}
    require(len(providers) == len(files), "Runtime contains ambiguous duplicate DLL names")
    if public:
        require(not any(FORBIDDEN_PUBLIC.search(path.name) for path in files),
                "Public runtime directory contains a forbidden vision/inference/matching dependency")
    system = Path(os.environ["SystemRoot"]) / "System32"
    evidence = []
    for binary in files if public else [runtime / "lyflow_core.dll"]:
        text = execute([dumpbin, "/nologo", "/dependents", str(binary)], workspace,
                       output, f"runtime-imports-{binary.name}", environment)
        names = DLL_IMPORT.findall(text)
        require(names, f"No PE import evidence for runtime binary {binary.name}")
        if public:
            require(not any(FORBIDDEN_PUBLIC.search(name) for name in names),
                    f"Public runtime imports a forbidden dependency: {binary.name}")
            for name in names:
                require(name.casefold() in providers or (system / name).is_file() or
                        name.casefold().startswith(("api-ms-", "ext-ms-")),
                        f"Public runtime dependency is absent from its directory/System32: {name}")
        evidence.append({"binary": binary.name, "imports": names})
    return evidence


def main():
    parser = argparse.ArgumentParser(description="Run the installed public Profile workflow through release CLI and C ABI")
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--graph", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--public-runtime", action="store_true",
                        help="Require exactly the public packs and reject heavy backend DLL/import closure")
    args = parser.parse_args()
    require(os.name == "nt", "This installed SDK/client acceptance gate requires Windows and MSVC")
    sdk = args.sdk.resolve(strict=True)
    cli = args.cli.resolve(strict=True)
    runtime_source = (args.runtime_dir or sdk / "bin").resolve(strict=True)
    graph = (args.graph or sdk / "examples/m4-native-profile.lyflow.json").resolve(strict=True)
    consumer_source = sdk / "examples/embed_profile_workflow.cpp"
    require(consumer_source.is_file(), "Installed SDK is missing the public workflow consumer")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / "evidence.json"
    receipt.unlink(missing_ok=True)
    document = json.loads(graph.read_text(encoding="utf-8-sig"))
    csv = graph.parent / "m4-native-profile.csv"
    require(csv.is_file(), "Public workflow CSV fixture is absent")
    version = json.loads((ROOT / "package.json").read_text(encoding="utf-8"))["version"]
    cmake, dumpbin = shutil.which("cmake"), shutil.which("dumpbin")
    require(cmake and dumpbin, "Use the MSVC developer environment with CMake/Ninja and dumpbin")
    environment = os.environ.copy()
    environment.pop("LYFLOW_CACHE_DIR", None)
    with isolated_workspace(output) as workspace:
        runtime = workspace / "runtime"
        shutil.copytree(runtime_source, runtime)
        executable = runtime / "lyflow.exe"
        shutil.copy2(cli, executable)
        dll = runtime / "lyflow_core.dll"
        require(dll.is_file(), "Selected runtime has no native core")
        info = json.loads(execute([str(executable), "info"], workspace, output, "cli-info", environment))
        require(Path(info["corePath"]).resolve(strict=True) == dll.resolve(strict=True),
                "CLI loaded a different core; use a release CLI rather than a development-path debug build")
        require(info["version"] == version, "Selected runtime product version differs from the checkout")
        manifest = json.loads(execute([str(executable), "manifest"], workspace, output,
                                      "cli-manifest", environment))
        operators = {item["id"]: item for item in manifest["operators"]}
        pack_set = {item.get("pack", "core").split("@", 1)[0] for item in manifest["operators"]}
        if args.public_runtime:
            require(pack_set == PUBLIC_PACKS | {"core"},
                    f"Public runtime has an unexpected registered pack set: {sorted(pack_set)}")
        runtime_closure = runtime_imports(runtime, dumpbin, workspace, output, environment,
                                          args.public_runtime)
        required = {}
        for node in document["nodes"]:
            descriptor = operators.get(node["op"])
            require(descriptor is not None, f"Missing public workflow operator: {node['op']}")
            pack = descriptor["pack"].split("@", 1)[0]
            require(pack in PUBLIC_PACKS, f"Workflow unexpectedly depends on a scene/backend operator: {node['op']}")
            required[node["op"]] = pack
        source = workspace / "consumer-source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.20)
project(lyflow_public_profile_workflow LANGUAGES CXX)
find_package(lyflow ${CONSUMER_VERSION} EXACT REQUIRED COMPONENTS client)
add_executable(profile-workflow-consumer "${SDK_EXAMPLES}/embed_profile_workflow.cpp")
target_compile_features(profile-workflow-consumer PRIVATE cxx_std_17)
target_link_libraries(profile-workflow-consumer PRIVATE lyflow::client)
if(MSVC)
  target_compile_options(profile-workflow-consumer PRIVATE /EHsc /utf-8)
endif()
""", encoding="utf-8")
        build = workspace / "consumer"
        execute([cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=",
                 f"-Dlyflow_DIR:PATH={sdk.as_posix()}",
                 f"-DSDK_EXAMPLES:PATH={(sdk / 'examples').as_posix()}",
                 f"-DCONSUMER_VERSION={version}", "-DCMAKE_DISABLE_FIND_PACKAGE_PCL=ON",
                 "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_Eigen3=ON"],
                workspace, output, "consumer-configure", environment)
        execute([cmake, "--build", str(build), "--parallel", "2"], workspace, output,
                "consumer-build", environment)
        consumer = build / "profile-workflow-consumer.exe"
        imports = execute([dumpbin, "/nologo", "/dependents", str(consumer)], workspace,
                          output, "consumer-imports", environment)
        imported = re.findall(r"^\s+([\w.+-]+\.dll)\s*$", imports, re.M | re.I)
        require(imported and not any(re.search(r"lyflow_core|pcl_|opencv|onnxruntime", name, re.I)
                                     for name in imported),
                "C ABI workflow consumer must link only the header client, without backend/engine imports")
        text = execute([str(executable), "run", str(graph), "--outputs", "--summary", "--no-cache"],
                       workspace, output, "cli-workflow", environment)
        lines = [json.loads(line) for line in text.splitlines() if line.strip()]
        summaries = [value for value in lines if value.get("kind") == "run_summary"]
        require(len(summaries) == 1 and summaries[0]["status"] == "ok", "CLI workflow did not complete successfully")
        candidates = [value for value in lines if "flush" in value and "indexMap" in value]
        require(len(candidates) == 1, "CLI did not return the named public workflow outputs")
        values = candidates[0]
        for name in ("flush", "dy", "distance"):
            value = values[name]["value"]
            require(value["ok"] is True and value["unit"] == "m" and
                    math.isclose(value["value"], 0.002, rel_tol=0, abs_tol=1e-9),
                    f"{name} must be a valid 0.002 m measurement")
        profile = values["profile"]["value"]
        require(profile["kind"] == "Profile" and profile["count"] == 16 and
                profile["validCount"] == 14 and profile["descriptor"]["unit"] == "mm",
                "CLI source Profile slots or raw unit changed")
        mapping = record(values, "indexMap", "profile.Selection.v2")
        selection = record(values, "selection", "profile.SampleSelection.v2")
        quality = record(values, "fitQuality", "profile.LineFitRobust.v2")
        require(mapping["sourceCloudId"] == profile["cloudId"] == selection["sourceCloudId"],
                "CLI native source identities are not bound to the same original Profile")
        require(mapping["indexCount"] == 13 and mapping["weightedInlierCount"] == 13 and len(mapping) == 6,
                "Unique/weighted inlier mapping changed or contains large index arrays")
        require(selection["slotIndex"] == 15 and selection["sourceIndex"] == 15,
                "Nearest sample lost the original physical index")
        require(quality["uniqueInlierCount"] == 13 and quality["weightedInlierCount"] == 13,
                "Fitter quality conflates observations and weights")
        embedded = json.loads(execute([str(consumer), str(dll), str(graph)], workspace, output,
                                      "embedded-workflow", environment))
        require(embedded["type"] == "lyflow.PublicProfileWorkflowAcceptance.v1" and
                embedded["version"] == info["version"], "Embedded host loaded a different native product")
        require(embedded["inputProfileShared"] is True and embedded["indexMapSmall"] is True,
                "Embedded host did not validate Profile ownership/index delivery")
        identical(normalize(values), normalize(embedded["namedOutputs"]))
        evidence = {"type": "lyflow.PublicProfileWorkflowDelivery.v1", "version": info["version"],
                    "sdk": str(sdk), "graphSha256": digest(graph), "csvSha256": digest(csv),
                    "runtimeSha256": digest(dll), "cliCorePathVerified": True,
                    "publicRuntime": args.public_runtime, "registeredPacks": sorted(pack_set),
                    "runtimeImports": runtime_closure,
                    "requiredOperatorPacks": required, "clientImports": imported,
                    "slots": 16, "validSlots": 14, "uniqueInliers": 13,
                    "weightedInliers": 13, "selectedSourceIndex": 15,
                    "flushMeters": values["flush"]["value"]["value"],
                    "normalizedNamedOutputsIdentical": True,
                    "floatingBitsIdentical": True, "cliPassed": True, "embeddedPassed": True}
    require(not workspace.exists(), "Temporary workflow runtime was not removed")
    evidence["temporaryWorkspaceRemoved"] = True
    receipt.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Installed public Profile workflow passed through release CLI and independent C ABI host")


if __name__ == "__main__":
    main()
