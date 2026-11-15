import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path

from p5_contract import CASES, check, digest, require


def run_consumer(consumer, runtime, graph, output, environment, label, timeout):
    result = subprocess.run(
        [str(consumer), str(runtime), str(graph)],
        cwd=output,
        env=environment,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
        check=False,
    )
    (output / f"{label}.stdout.log").write_text(result.stdout, encoding="utf-8")
    (output / f"{label}.stderr.log").write_text(result.stderr, encoding="utf-8")
    return result


def profile_contract(consumer, runtime, output, environment):
    require(consumer.is_file(), "Installed Profile SDK consumer must be compiled")
    result = subprocess.run([str(consumer), str(runtime)], cwd=output, env=environment,
                            capture_output=True, text=True, encoding="utf-8", timeout=60, check=False)
    (output / "profile-sdk.stdout.log").write_text(result.stdout, encoding="utf-8")
    (output / "profile-sdk.stderr.log").write_text(result.stderr, encoding="utf-8")
    require(result.returncode == 0, f"Installed Profile SDK consumer failed: {result.stderr}")
    report = json.loads(result.stdout)
    require(report.get("type") == "lyflow.ProfileSdkAcceptance.v1" and report.get("slots") == 8
            and report.get("validSlots") == 6 and report.get("unit") == "m", "Profile SDK slot/unit contract changed")
    for key in ("optionalChannelsPreserved", "sourceMappingPreserved", "badInputRejected"):
        require(report.get(key) is True, f"Profile SDK contract failed: {key}")
    for key in ("gap", "flush"):
        value = report.get(key, {})
        require(value.get("ok") is True and value.get("unit") == "m"
                and abs(value.get("value", 0) - 0.002) < 1e-7, f"Typed {key} must be 0.002 m")
    return report


def missing_dependency(sdk, consumer, output, environment, bait_directory):
    binary = (sdk / "bin").resolve(strict=True)
    require(binary.parent == sdk, "SDK bin must belong to the selected SDK")
    dependency = binary / "opencv_imgproc510.dll"
    require(dependency.is_file() and dependency.resolve().parent == binary, "Expected an SDK-owned OpenCV DLL")
    disabled = dependency.with_suffix(".dll.disabled")
    require(not disabled.exists(), "Refusing to overwrite an existing disabled OpenCV DLL")
    require(disabled.resolve().parent == binary, "Disabled dependency must remain inside SDK bin")
    bait = output / dependency.name
    require(output != binary and bait.resolve().parent == output, "Bait directory must be separate from SDK bin")
    require(not bait.exists(), "Refusing to overwrite an existing CWD bait file")
    expected = digest(dependency)
    path_bait = bait_directory if bait_directory is not None else output
    require(path_bait != binary, "PATH bait must provide the dependency outside SDK bin")
    if bait_directory is not None:
        supplied = path_bait / dependency.name
        require(supplied.is_file() and digest(supplied) == expected, "PATH bait must contain the same valid OpenCV DLL")
    renamed = False
    created = False
    try:
        with bait.open("xb") as target:
            created = True
            with dependency.open("rb") as source:
                shutil.copyfileobj(source, target)
        require(digest(bait) == expected, "CWD bait copy is not the valid installed dependency")
        negative_environment = environment.copy()
        negative_environment["PATH"] += os.pathsep + str(path_bait)
        dependency.rename(disabled)
        renamed = True
        result = run_consumer(
            consumer, binary / "lyflow_core.dll", sdk / "examples/p5-depth-inspection.lyflow.json",
            output, negative_environment, "missing-opencv-cwd-path-bait", 30,
        )
        require(result.returncode == 1, f"Missing OpenCV did not reject the client load: {result.returncode}")
        require(not result.stdout.strip() and "GetLastError=126" in result.stderr, "Negative case did not fail at Windows dependency loading")
    finally:
        try:
            if renamed:
                require(not dependency.exists(), "Cannot restore OpenCV over an unexpected replacement")
                disabled.rename(dependency)
        finally:
            if created:
                bait.unlink()
    require(dependency.is_file() and digest(dependency) == expected and not disabled.exists(), "OpenCV dependency was not restored exactly")
    return {"name": "missing-opencv-cwd-path-bait", "status": "rejected"}


def main():
    parser = argparse.ArgumentParser(description="Verify eight installed P5 C++ SDK workflows and reject Windows CWD/PATH dependency bait")
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--consumer", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--bait-dir", type=Path)
    parser.add_argument("--profile-consumer", type=Path)
    parser.add_argument("--profile-only", action="store_true")
    args = parser.parse_args()
    require(os.name == "nt", "This isolated SDK contract requires Windows; it cannot be skipped")
    sdk = args.sdk.resolve(strict=True)
    consumer = args.consumer.resolve(strict=True)
    require(sdk.is_dir() and consumer.is_file(), "SDK and compiled consumer must already exist")
    runtime = (sdk / "bin/lyflow_core.dll").resolve(strict=True)
    require(runtime.parent.parent == sdk, "Runtime DLL must belong to the selected installed SDK")
    output = args.output_dir.resolve()
    require(output != (sdk / "bin").resolve(), "Output directory must differ from SDK bin")
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    profile_consumer = (args.profile_consumer or consumer.with_name("profile_sdk_consumer.exe")).resolve(strict=True)
    profile = profile_contract(profile_consumer, runtime, output, environment)
    if args.profile_only:
        report = {"mode": "installed-cpp-profile-sdk", "sdk": str(sdk), "profile": profile, "noSkipped": True}
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False))
        return
    examples = sdk / "examples"
    truth_path = examples / "p5-fixtures.json"
    truth = json.loads(truth_path.read_text(encoding="utf-8"))
    for name, expected in truth["files"].items():
        path = examples / name
        require(path.resolve(strict=True).parent == examples.resolve(), f"Fixture must remain inside installed examples: {name}")
        require(path.stat().st_size == expected["bytes"] and digest(path) == expected["sha256"], f"Installed fixture size/SHA256 mismatch: {name}")
    sources = [truth_path, *(examples / name for name in truth["files"]), *(examples / f"{name}.lyflow.json" for name in CASES)]
    original = {str(path): digest(path) for path in sources}
    report = {"mode": "installed-cpp-sdk", "sdk": str(sdk), "consumer": str(consumer), "profile": profile, "cases": []}
    for name in CASES:
        result = run_consumer(consumer, runtime, examples / f"{name}.lyflow.json", output, environment, name, 240)
        require(result.returncode == 0, f"{name}: independent SDK consumer failed ({result.returncode}): {result.stderr}")
        check(name, json.loads(result.stdout), False, truth)
        report["cases"].append({"name": name, "status": "passed"})
        print(f"Installed C++ SDK: {name} passed")
    bait_directory = args.bait_dir.resolve(strict=True) if args.bait_dir is not None else None
    report["cases"].append(missing_dependency(sdk, consumer, output, environment, bait_directory))
    require(original == {str(path): digest(path) for path in sources}, "SDK workflows changed installed source assets")
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "passed", "workflows": len(CASES), "dependencyNegative": "rejected", "report": str(output / "report.json")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
