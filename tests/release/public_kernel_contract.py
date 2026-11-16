import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN = re.compile(r'(?:lyflow_core|pcl_|opencv|onnxruntime|boost_|tbb|zxing|yaml)', re.I)


def run(arguments, cwd):
    result = subprocess.run(arguments, cwd=cwd, text=True, encoding='utf-8', errors='replace',
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    print(result.stdout, end='')
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {arguments}')
    return result.stdout


def imports(path):
    dumpbin = shutil.which('dumpbin')
    if not dumpbin:
        raise RuntimeError('Run from the MSVC developer environment: dumpbin is required')
    output = run([dumpbin, '/nologo', '/dependents', str(path)], ROOT)
    dependencies = re.findall(r'^\s+([\w.+-]+\.dll)\s*$', output, re.M | re.I)
    if not dependencies:
        raise RuntimeError(f'No PE import evidence for {path}')
    rejected = [name for name in dependencies if FORBIDDEN.search(name)]
    if rejected:
        raise RuntimeError(f'Unexpected heavy/runtime imports in {path}: {rejected}')
    return dependencies


def optional_backend(sdk, workspace, cmake):
    source = workspace / 'component-contract'
    source.mkdir()
    (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(lyflow_optional_backend LANGUAGES NONE)
if(REQUIRE_ROBUST)
  find_package(lyflow ${CONSUMER_VERSION} EXACT REQUIRED COMPONENTS geometry_kernel profile_robust_kernel)
else()
  find_package(lyflow ${CONSUMER_VERSION} EXACT REQUIRED COMPONENTS geometry_kernel OPTIONAL_COMPONENTS profile_robust_kernel)
  if(NOT lyflow_FOUND OR NOT TARGET lyflow::geometry_kernel OR
     lyflow_profile_robust_kernel_FOUND OR TARGET lyflow::profile_robust_kernel)
    message(FATAL_ERROR "Optional unavailable PCL backend must preserve the lightweight SDK")
  endif()
endif()
''', encoding='utf-8')
    version = json.loads((ROOT / 'package.json').read_text(encoding='utf-8'))['version']
    common = [cmake, '-S', str(source), '-G', 'Ninja', '-DCMAKE_TOOLCHAIN_FILE=',
              f'-Dlyflow_DIR={sdk}', f'-DCONSUMER_VERSION={version}',
              '-DCMAKE_DISABLE_FIND_PACKAGE_PCL=ON']
    run(common + ['-B', str(workspace / 'optional-backend')], workspace)
    if (sdk / 'lib/cmake/lyflow/lyflow-profile-robust-targets.cmake').is_file():
        result = subprocess.run(common + ['-B', str(workspace / 'required-backend'),
                                         '-DREQUIRE_ROBUST=ON'], cwd=workspace, text=True,
                                encoding='utf-8', errors='replace', stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        (workspace.parent / 'required-backend-rejected.log').write_text(result.stdout, encoding='utf-8')
        if result.returncode == 0 or 'CMAKE_DISABLE_FIND_PACKAGE_PCL' not in result.stdout:
            raise RuntimeError('A required unavailable robust backend must fail explicitly')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    arguments = parser.parse_args()
    sdk = arguments.sdk.resolve(strict=True)
    output = arguments.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = {}
    if arguments.runtime:
        evidence['runtimeImports'] = imports(arguments.runtime.resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix='public-kernels-', dir=output) as temporary:
        workspace = Path(temporary).resolve()
        if workspace.parent != output:
            raise RuntimeError('Unexpected temporary workspace location')
        isolated = workspace / 'sdk'
        isolated.mkdir()
        for name in ('include', 'lib', 'examples'):
            shutil.copytree(sdk / name, isolated / name)
        (isolated / 'bin').mkdir()
        for name in ('lyflow-config.cmake', 'lyflow-config-version.cmake'):
            shutil.copy2(sdk / name, isolated / name)
        if list(isolated.rglob('*.dll')):
            raise RuntimeError('Independent kernel SDK must contain no runtime DLLs')
        build = workspace / 'consumer'
        cmake = shutil.which('cmake')
        if not cmake:
            raise RuntimeError('cmake is required')
        optional_backend(isolated, workspace, cmake)
        run([cmake, '-S', str(isolated / 'examples/public-kernels'), '-B', str(build),
             '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TOOLCHAIN_FILE=',
             f'-Dlyflow_DIR={isolated}', '-DCMAKE_DISABLE_FIND_PACKAGE_PCL=ON',
             '-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON', '-DCMAKE_DISABLE_FIND_PACKAGE_Eigen3=ON',
             '-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON'], workspace)
        run([cmake, '--build', str(build), '--parallel', '2'], workspace)
        consumer = build / ('lyflow-public-kernels.exe' if os.name == 'nt' else 'lyflow-public-kernels')
        if os.name == 'nt':
            evidence['consumerImports'] = imports(consumer)
        run([str(consumer)], workspace)
        evidence.update(sdk=str(sdk), isolatedRuntimeDllCount=0, consumerPassed=True,
                        optionalBackendFallbackPassed=True)
    (output / 'evidence.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + '\n',
                                        encoding='utf-8')


if __name__ == '__main__':
    main()
