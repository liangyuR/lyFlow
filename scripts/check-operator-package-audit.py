"""核对算子分包清单 docs/operator-package-audit.json。

不带参数：检查清单结构，并核对 Python 示例 ID 与 python/examples 中的 @node 声明一致。
--manifest：再核对 lyflow-dump-manifest 导出的原生算子与清单一致（ID 全集与当前包归属）。
--subset：manifest 只来自部分包组合时使用，只要求导出的每个算子都已登记且归属一致。
"""
import argparse
import ast
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEDGER = ROOT / 'docs/operator-package-audit.json'
FIELDS = ('id', 'currentPack', 'targetPack', 'action', 'wave')
ACTIONS = {'retain', 'move', 'split', 'solution'}
WAVE = re.compile(r'M[0-7]')


def python_example_ids(root=ROOT):
    ids = []
    for path in sorted((root / 'python/examples').rglob('*.py')):
        for item in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
            if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                for decorator in item.decorator_list:
                    if isinstance(decorator, ast.Call) and getattr(decorator.func, 'id', None) == 'node':
                        ids += [ast.literal_eval(k.value) for k in decorator.keywords if k.arg == 'id']
    return ids


def manifest_operators(paths):
    exported = {}
    for path in paths:
        document = json.loads(path.read_text(encoding='utf-8-sig'))
        for op in document if isinstance(document, list) else document['operators']:
            exported[op['id']] = op.get('pack', 'core').split('@')[0]
    return exported


def validate(data, example_ids, exported=None, subset=False):
    problems = []
    require = lambda ok, message: ok or problems.append(message)
    require(data.get('schemaVersion') == 2, 'schemaVersion 必须为 2')
    targets = set(data.get('targetPackages', []))
    native, examples = data.get('operators', []), data.get('pythonExamples', [])
    for row in native + examples:
        name = row.get('id', '<缺少id>')
        require(set(row) == set(FIELDS) and all(isinstance(row[k], str) and row[k] for k in FIELDS),
                f'{name}: 字段必须恰好为 {", ".join(FIELDS)} 且非空')
        require(row.get('targetPack') in targets, f'{name}: targetPack 不在 targetPackages 中')
        require(row.get('action') in ACTIONS, f'{name}: action 非法')
        require(WAVE.fullmatch(str(row.get('wave'))) is not None, f'{name}: wave 非法')
    for rows, label in ((native, 'operators'), (examples, 'pythonExamples')):
        ids = [row.get('id') for row in rows]
        require(ids == sorted(ids), f'{label} 须按 id 排序')
    ids = [row.get('id') for row in native + examples]
    require(len(ids) == len(set(ids)), '出现重复算子 ID')
    require(sorted(example_ids) == sorted(row.get('id') for row in examples),
            'pythonExamples 与 python/examples 中的 @node 声明不一致')
    retired = data.get('retiredOperators', [])
    retired_ids = {row.get('id') for row in retired}
    require(len(retired_ids) == len(retired) and not retired_ids & set(ids), '退役 ID 重复或仍在活动清单中')
    native_ids = {row['id'] for row in native if 'id' in row}
    for row in retired:
        require(set(row) == {'id', 'replacedBy'} and row.get('replacedBy') and set(row['replacedBy']) <= native_ids,
                f"{row.get('id')}: 退役记录须只含 id、replacedBy，且替代算子已登记")
    if exported is not None:
        packs = {row['id']: row['currentPack'] for row in native if 'id' in row and 'currentPack' in row}
        for identifier, pack in sorted(exported.items()):
            require(identifier in packs, f'{identifier}: 已注册但未登记')
            require(identifier not in packs or packs[identifier] == pack,
                    f'{identifier}: 当前包归属为 {pack}，清单写的是 {packs.get(identifier)}')
            require(identifier not in retired_ids, f'{identifier}: 已退役却仍在注册')
        if not subset:
            missing = sorted(set(packs) - set(exported))
            require(not missing, f'清单中登记但未注册：{", ".join(missing)}')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--manifest', action='append', type=Path, default=[])
    parser.add_argument('--subset', action='store_true')
    args = parser.parse_args()
    if args.subset and not args.manifest:
        parser.error('--subset 需要 --manifest')
    data = json.loads(LEDGER.read_text(encoding='utf-8'))
    exported = manifest_operators(args.manifest) if args.manifest else None
    problems = validate(data, python_example_ids(), exported, args.subset)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    scope = '（部分包组合）' if args.subset else ''
    checked = f'，manifest {len(exported)} 项{scope}一致' if exported is not None else ''
    print(f"OK: {len(data['operators'])} 原生算子，{len(data['pythonExamples'])} Python 示例{checked}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
