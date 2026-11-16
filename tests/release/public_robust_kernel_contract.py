import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN = re.compile(r'(?:lyflow_core|opencv|onnxruntime|kiss.?matcher|small.?gicp|yaml)', re.I)
IMPORT = re.compile(r'^\s+([\w.+-]+\.dll)\s*$', re.M | re.I)


@contextmanager
def isolated_workspace(output):
    temporary = tempfile.TemporaryDirectory(prefix='profile-robust-sdk-', dir=output)
    workspace = Path(temporary.name).resolve()
    try:
        if workspace.parent != output:
            raise RuntimeError('Unexpected temporary workspace location')
        yield workspace
    finally:
        for attempt in range(8):
            try:
                temporary.cleanup()
                break
            except OSError as error:
                if getattr(error, 'winerror', None) not in {5, 32, 145} or attempt == 7:
                    raise
                time.sleep(min(0.25 * 2 ** attempt, 2))


def run(arguments, directory, log, *, environment=None, timeout=300):
    result = subprocess.run(arguments, cwd=directory, env=environment, text=True, encoding='utf-8',
                            errors='replace', stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout, check=False)
    log.write_text(result.stdout, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}); see {log}\n{result.stdout[-6000:]}')
    return result.stdout


def dependencies(binary, dumpbin, log):
    output = run([dumpbin, '/nologo', '/dependents', str(binary)], binary.parent, log)
    result = IMPORT.findall(output)
    if not result:
        raise RuntimeError(f'No PE import evidence for {binary}')
    rejected = [name for name in result if FORBIDDEN.search(name)]
    if rejected:
        raise RuntimeError(f'Forbidden robust-kernel imports in {binary}: {rejected}')
    return result


def isolate_runtime(consumer, destination, provider_dirs, logs):
    dumpbin = shutil.which('dumpbin')
    if not dumpbin:
        raise RuntimeError('MSVC developer environment with dumpbin is required')
    system = Path(os.environ['SystemRoot']) / 'System32'
    destination.mkdir()
    executable = destination / consumer.name
    shutil.copy2(consumer, executable)
    providers = []
    for directory in provider_dirs:
        if directory.is_dir():
            providers.append({path.name.casefold(): path for path in directory.glob('*.dll')})
    queue = [executable]
    visited = set()
    closure = []
    while queue:
        binary = queue.pop()
        identity = binary.name.casefold()
        if identity in visited:
            continue
        visited.add(identity)
        imported = dependencies(binary, dumpbin, logs / f'imports-{binary.name}.log')
        closure.append({'binary': binary.name, 'imports': imported})
        for name in imported:
            if name.casefold().startswith(('api-ms-', 'ext-ms-')) or (system / name).is_file():
                continue
            target = destination / name
            if target.is_file():
                continue
            source = next((provider[name.casefold()] for provider in providers
                           if name.casefold() in provider), None)
            if source is None:
                raise RuntimeError(f'Missing installed/provider dependency {name} imported by {binary.name}')
            shutil.copy2(source, target)
            closure.append({'copied': name, 'source': str(source),
                            'sha256': hashlib.sha256(source.read_bytes()).hexdigest()})
            queue.append(target)
    if not any('pcl_' in name.casefold() for item in closure for name in item.get('imports', [])):
        raise RuntimeError('This Windows shared-PCL gate did not observe the PCL backend imports')
    environment = os.environ.copy()
    environment['PATH'] = str(system)
    for name in ('LYFLOW_CORE_DLL', 'LYFLOW_CORE_DIR', 'LYFLOW_CORE_BIN', 'PCL_ROOT'):
        environment.pop(name, None)
    return executable, environment, closure


def main():
    parser = argparse.ArgumentParser(description='Build and run an isolated installed robust profile SDK consumer')
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path)
    parser.add_argument('--installed-dir', type=Path)
    arguments = parser.parse_args()
    if os.name != 'nt':
        raise RuntimeError('This PE-import and runtime isolation acceptance gate requires Windows')
    sdk = arguments.sdk.resolve(strict=True)
    output = arguments.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / 'evidence.json'
    receipt.unlink(missing_ok=True)
    cmake = shutil.which('cmake')
    if not cmake:
        raise RuntimeError('cmake and Ninja are required')
    configured_toolchain = arguments.toolchain or os.environ.get('CMAKE_TOOLCHAIN_FILE')
    if not configured_toolchain:
        default = Path('C:/vcpkg/scripts/buildsystems/vcpkg.cmake')
        configured_toolchain = default if default.is_file() else None
    toolchain = Path(configured_toolchain).resolve(strict=True) if configured_toolchain else None
    configured_install = arguments.installed_dir or os.environ.get('VCPKG_INSTALLED_DIR')
    installed = Path(configured_install).resolve(strict=True) if configured_install else None
    with isolated_workspace(output) as workspace:
        isolated = workspace / 'sdk'
        isolated.mkdir()
        for name in ('include', 'lib', 'examples'):
            shutil.copytree(sdk / name, isolated / name)
        (isolated / 'bin').mkdir()
        for name in ('lyflow-config.cmake', 'lyflow-config-version.cmake'):
            shutil.copy2(sdk / name, isolated / name)
        if list(isolated.rglob('*.dll')):
            raise RuntimeError('Isolated static SDK must not contain LyFlow or provider DLLs')
        build = workspace / 'consumer'
        configure = [cmake, '-S', str(isolated / 'examples/profile-robust'), '-B', str(build),
                     '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', f'-Dlyflow_DIR={isolated}',
                     '-DVCPKG_MANIFEST_MODE=OFF', '-DVCPKG_MANIFEST_INSTALL=OFF',
                     '-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON',
                     '-DCMAKE_DISABLE_FIND_PACKAGE_onnxruntime=ON',
                     f'-DCMAKE_TOOLCHAIN_FILE={toolchain or ""}']
        if installed:
            configure.append(f'-DVCPKG_INSTALLED_DIR={installed.as_posix()}')
        run(configure, workspace, output / 'configure.log')
        cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
        provider = re.search(r'^PCL_DIR:[^=]+=(.+)$', cache, re.M)
        if not provider:
            raise RuntimeError('Configured consumer did not record its PCL provider')
        pcl_directory = Path(provider.group(1).strip()).resolve(strict=True)
        if installed and not pcl_directory.is_relative_to(installed):
            raise RuntimeError('PCL was resolved outside the selected verified installation')
        run([cmake, '--build', str(build), '--parallel', '2'], workspace, output / 'build.log')
        binary_dirs = [sdk / 'bin']
        binary_dirs.extend(Path(line) for line in
                           (build / 'provider-binary-dirs.txt').read_text(encoding='utf-8').splitlines() if line)
        executable, environment, imports = isolate_runtime(build / 'lyflow-profile-robust.exe',
                                                            workspace / 'runtime', binary_dirs, output)
        result = subprocess.run([str(executable)], cwd=workspace, env=environment, text=True,
                                encoding='utf-8', capture_output=True, timeout=60, check=False)
        (output / 'consumer.stdout.log').write_text(result.stdout, encoding='utf-8')
        (output / 'consumer.stderr.log').write_text(result.stderr, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(f'Robust consumer failed ({result.returncode}): {result.stderr}')
        report = json.loads(result.stdout)
        if report.get('type') != 'lyflow.ProfileRobustSdkAcceptance.v1' or report.get('fitMethods') != 4:
            raise RuntimeError('Unexpected robust kernel acceptance report')
        for name in ('radiusPrior', 'inputSlotsPreserved', 'arraysReadonly',
                     'invalidOutputAtomic', 'cancelOutputAtomic', 'profileFeatures',
                     'weightedInliersPreserved', 'featureOutputAtomic'):
            if report.get(name) is not True:
                raise RuntimeError(f'Robust kernel acceptance failed: {name}')
        evidence = {'sdk': str(sdk), 'toolchain': str(toolchain) if toolchain else None,
                    'installedDir': str(installed) if installed else None,
                    'pclConfigDirectory': str(pcl_directory),
                    'isolatedSdkRuntimeDllCount': 0, 'runtimePath': 'System32 only',
                    'runtimeImports': imports, 'consumer': report, 'consumerPassed': True}
    if workspace.exists():
        raise RuntimeError('Temporary SDK workspace was not removed')
    evidence['temporaryWorkspaceRemoved'] = True
    receipt.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('Installed robust profile SDK consumer passed with an isolated PCL dependency closure')


if __name__ == '__main__':
    main()
