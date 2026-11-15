import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / 'docs/operator-package-audit.json'
FIELDS = ('id', 'currentPack', 'targetPack', 'action', 'wave', 'source', 'dependencies',
          'currentContract', 'targetContract', 'migrationPrerequisites', 'acceptance', 'notes')
CONTRACT_SOURCES = ('core/src/ops/*.cpp', 'core/src/registry.cpp', 'core/src/manifest.cpp',
                    'core/include/lyflow/*.h', 'core/include/lyflow/*.hpp', 'core/CMakeLists.txt',
                    'packs/*/ops/*.cpp', 'packs/*/lyflow_op_pack.cmake',
                    'python/examples/**/*.py', 'packaging/release.json')


def source_hash(path):
    return hashlib.sha256(path.read_text(encoding='utf-8').encode('utf-8')).hexdigest()


def validate(data, manifests):
    problems = []

    def require(condition, message):
        if not condition:
            problems.append(message)

    require(data.get('schemaVersion') == 1 and data.get('status') == 'planned', '清单schema或规划状态不符')
    native = data.get('operators', [])
    examples = data.get('pythonExamples', [])
    rows = native + examples
    identifiers = [r.get('id') for r in rows]
    require(len(identifiers) == len(set(identifiers)), '出现重复算子ID')
    require(len(native) == data.get('nativeCount'), '原生算子统计不符')
    registered = data.get('registeredNativeIds', [])
    require(bool(registered) and len(registered) == len(set(registered)), '缺少有效的独立注册ID快照')
    require({r['id'] for r in native} == set(registered), '原生清单与注册ID快照不一致')
    require(len(examples) == data.get('pythonExampleCount'), 'Python示例统计不符')
    require(dict(collections.Counter(r['currentPack'] for r in native)) == data.get('currentPackCounts'), '当前包统计不符')
    require(dict(collections.Counter(r['targetPack'] for r in native)) == data.get('targetPackCounts'), '目标包统计不符')
    orders = [r.get('migrationOrder') for r in rows]
    require(sorted(orders) == list(range(1, len(rows) + 1)), '迁移序号必须唯一连续')
    by_id = {r['id']: r for r in rows}
    targets = set(data.get('targetPackages', []))
    waves = set(data.get('migrationWaves', {}))
    for row in rows:
        identifier = row['id']
        for field in FIELDS:
            require(bool(row.get(field)), f'{identifier}: 缺少非空字段 {field}')
        require(row.get('action') in {'retain', 'move', 'split', 'solution'}, f'{identifier}: action非法')
        require(row.get('targetPack') in targets, f'{identifier}: 未定义目标包')
        require(row.get('wave') in waves, f'{identifier}: 未定义波次')
        require(isinstance(row.get('dependencies'), list), f'{identifier}: dependencies必须是列表')
        require(set(row.get('currentPorts', {})) == {'inputs', 'outputs'}, f'{identifier}: 缺端口快照')
        require(isinstance(row.get('currentParams'), list), f'{identifier}: 缺参数快照')
        for dependency in row.get('migrationPrerequisites', []):
            if dependency in by_id:
                prior = by_id[dependency]
                require(prior['migrationOrder'] < row['migrationOrder'], f'{identifier}: 算子前置顺序错误 {dependency}')
                require(prior['wave'] <= row['wave'], f'{identifier}: 前置来自更晚波次 {dependency}')
            elif re.fullmatch(r'M[0-7]', dependency):
                require(dependency <= row['wave'], f'{identifier}: 波次前置顺序错误 {dependency}')
        match = re.fullmatch(r'(.+):(\d+)', row.get('source', ''))
        require(match is not None, f'{identifier}: source必须为path:line')
        if match:
            path = ROOT / match[1]
            require(path.is_file() and path.resolve().is_relative_to(ROOT), f'{identifier}: 来源文件不存在/越界')
            if path.is_file():
                text = path.read_text(encoding='utf-8')
                line = int(match[2])
                require(1 <= line <= len(text.splitlines()), f'{identifier}: 来源行号越界')
                evidence = row.get('registrationEvidence', {})
                tokens = evidence.get('tokens', [])
                generated = evidence.get('kind') == 'generated' and bool(tokens) and all(token in text for token in tokens)
                require(identifier in text or generated, f'{identifier}: 来源中未找到ID或动态注册证据')
    for row in native:
        require(row.get('operatorKind') == 'native', f"{row['id']}: 原生分类错误")
    for row in examples:
        require(row.get('operatorKind') == 'python-example' and row['id'].startswith('py.'), f"{row['id']}: 示例分类错误")
    require([r['wave'] for r in sorted(native, key=lambda r: r['migrationOrder'])] == sorted(r['wave'] for r in native), '原生清单波次顺序不单调')
    current_sources = {p.relative_to(ROOT).as_posix() for pattern in CONTRACT_SOURCES
                       for p in ROOT.glob(pattern) if p.is_file()}
    registered_sources = set(data.get('contractSourcePaths', []))
    require(current_sources == registered_sources,
            '契约来源文件集合变化，需复核新增/删除文件：' + ', '.join(sorted(current_sources ^ registered_sources)))
    fingerprints = data.get('sourceFingerprints', {})
    for relative in sorted(registered_sources):
        path = ROOT / relative
        require(path.is_file(), f'指纹来源缺失：{relative}')
        if path.is_file():
            require(source_hash(path) == fingerprints.get(relative), f'契约源码已变化，需复核清单：{relative}')
    require(bool(data.get('sourceFingerprints')), '缺少源码指纹')
    assets = data.get('relatedAssets', {})
    require(len(assets.get('importers', [])) == 6, '本基线的6种导入器登记不符')
    require(len(assets.get('snippets', [])) == 7, '本基线的7个片段登记不符')
    require(len(assets.get('bundles', [])) == 6, '本基线的6种Bundle登记不符')
    if manifests:
        exported = {}
        for path in manifests:
            document = json.loads(path.read_text(encoding='utf-8-sig'))
            values = document if isinstance(document, list) else document['operators']
            for op in values:
                prior = exported.get(op['id'])
                require(prior is None or prior == op, f"多份manifest中描述冲突：{op['id']}")
                exported[op['id']] = op
        require(set(exported) == {r['id'] for r in native}, 'manifest原生ID全集与清单不一致')
        for row in native:
            op = exported.get(row['id'])
            if op is None:
                continue
            require(op.get('pack', 'core').split('@')[0] == row['currentPack'], f"{row['id']}: manifest包归属不符")
            require(op.get('version') == row.get('currentVersion'), f"{row['id']}: manifest版本不符")
            require({k: op.get(k, []) for k in ['inputs', 'outputs']} == row.get('currentPorts'), f"{row['id']}: 端口描述不符")
            require(op.get('params', []) == row.get('currentParams'), f"{row['id']}: 参数描述不符")
    docs = [ROOT / 'docs/operator-package-boundaries.md', ROOT / 'docs/operator-package-migration-plan.md',
            ROOT / 'docs/operator-package-audit.md', *sorted((ROOT / 'docs/operator-package-audit').glob('*.md'))]
    for path in docs:
        require(path.is_file(), f'缺少文档：{path.relative_to(ROOT)}')
        if not path.is_file():
            continue
        for link in re.findall(r'(?<!!)\[[^\]]*\]\(([^)]+)\)', path.read_text(encoding='utf-8')):
            link = link.strip('<>')
            if re.match(r'^[a-zA-Z][\w+.-]*:', link) or link.startswith('#'):
                continue
            destination = path.parent / link.split('#', 1)[0]
            require(destination.exists(), f'{path.name}: 本地链接不存在 {link}')
    for row in rows:
        detail = ROOT / 'docs/operator-package-audit' / (row['currentPack'] + '.md')
        require(detail.is_file() and f" · {row['id']}\n" in detail.read_text(encoding='utf-8'), f"{row['id']}: 缺少逐项文档")
    return problems


def main():
    parser = argparse.ArgumentParser(description='检查算子分包规划的静态完整性；不替代构建和算法验收。')
    parser.add_argument('--manifest', action='append', type=Path, default=[])
    args = parser.parse_args()
    data = json.loads(AUDIT.read_text(encoding='utf-8'))
    problems = validate(data, args.manifest)
    if problems:
        print('\n'.join(problems), file=sys.stderr)
        return 1
    print(f"OK: {data['nativeCount']} native operators, {data['pythonExampleCount']} Python examples, "
          f"{len(data['contractSourcePaths'])} contract sources; fields, order, sources and links verified")
    print(f"Historical source fingerprints retained for provenance: {len(data['sourceFingerprints'])}")
    print('Static audit only; no migrated runtime, algorithm, SDK or field acceptance is claimed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
