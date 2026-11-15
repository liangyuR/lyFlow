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
        evidence.update(sdk=str(sdk), isolatedRuntimeDllCount=0, consumerPassed=True)
    (output / 'evidence.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + '\n',
                                        encoding='utf-8')


if __name__ == '__main__':
    main()
