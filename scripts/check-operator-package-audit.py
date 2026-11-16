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
                    'core/src/profile*.cpp', 'core/src/profile*.h', 'core/src/graph_contract_upgrade.cpp',
                    'core/src/data.cpp', 'core/src/contract.cpp', 'core/src/python_runtime.cpp',
                    'core/src/runtime*.cpp', 'core/src/exec/disk_cache.cpp',
                    'core/src/exec/graph.cpp', 'core/src/status.cpp',
                    'core/include/lyflow/*.h', 'core/include/lyflow/*.hpp', 'core/CMakeLists.txt',
                    'core/cmake/*.cmake', 'core/cmake/*.cmake.in',
                    'kernels/include/**/*.h', 'kernels/src/*.cpp',
                    'packs/*/ops/*.cpp', 'packs/*/ops/*.h', 'packs/*/include/**/*.h',
                    'packs/std-pointcloud/algo/fit2d.cpp', 'packs/std-pointcloud/algo/fit2d.h',
                    'packs/*/include/**/*.hpp', 'packs/*/lyflow_op_pack.cmake',
                    'packs/*/lyflow_pack_dependencies.cmake',
                    'solutions/*/ops/*.cpp', 'solutions/*/ops/*.h', 'solutions/*/algo/*.cpp',
                    'solutions/*/algo/*.h', 'solutions/*/lyflow_op_pack.cmake',
                    'solutions/*/lyflow_pack_dependencies.cmake',
                    'examples/public_kernels.cpp', 'examples/public-kernels/CMakeLists.txt',
                    'examples/profile-robust/CMakeLists.txt', 'examples/profile-robust/main.cpp',
                    'python/lyflow_python/*.py',
                    'python/examples/**/*.py', 'packaging/release.json')


def source_hash(path):
    return hashlib.sha256(path.read_text(encoding='utf-8').encode('utf-8')).hexdigest()


def validate(data, manifests, generated_docs=None):
    problems = []
    generated_docs = generated_docs or {}

    def require(condition, message):
        if not condition:
            problems.append(message)

    require(data.get('schemaVersion') == 1 and data.get('status') in {'planned', 'in_progress', 'complete'}, '清单schema或规划状态不符')
    if data.get('status') != 'planned':
        baseline = data.get('baselineReference', {})
        require(bool(re.fullmatch(r'[0-9a-f]{40}', baseline.get('revision', ''))) and
                baseline.get('path') == 'docs/operator-package-audit.json', '缺少可追溯规划基线')
        require(baseline.get('nativeCount') == 251 and baseline.get('pythonExampleCount') == 7 and
                len(set(baseline.get('registeredNativeIds', []))) == 251 and
                len(set(baseline.get('pythonExampleIds', []))) == 7, '原251/7基线ID快照不完整')
        stages = data.get('stageLedger', [])
        require(bool(stages) and len({s.get('stage') for s in stages}) == len(stages), '缺少有效阶段ledger')
        for stage in stages:
            require(stage.get('status') in {'planned', 'pending_manifest', 'validated_manifest', 'complete'}, '阶段状态非法')
            if stage.get('status') in {'validated_manifest', 'complete'} and stage.get('evidenceKind') != 'planning_baseline':
                proof = stage.get('manifestEvidence', {})
                require(bool(re.fullmatch(r'[0-9a-f]{64}', proof.get('sha256', ''))) and
                        proof.get('operatorCount') == stage.get('expectedNativeCount') and
                        bool(stage.get('reviewedSourceChanges')), '阶段缺少真实manifest或源码复核证据')
            if stage.get('stage') == 'M1' and stage.get('status') == 'complete':
                require(stage.get('mergeEvidence', {}).get('pr') == 62 and
                        bool(re.fullmatch(r'[0-9a-f]{40}', stage.get('mergeEvidence', {}).get('revision', ''))),
                        'M1完成状态缺少PR62合并证据')
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
    retired = data.get('retiredOperators', [])
    retired_ids = [row.get('id') for row in retired]
    require(len(retired_ids) == len(set(retired_ids)) and not set(retired_ids) & set(registered),
            '退役ID重复或仍注册为活动计算')
    approved_retirements = {'dts.profile_clean': 'profile.clean', 'dts.split_faces': 'profile.split_faces'}
    for row in retired:
        require(row.get('retiredStage') == 'M3' and row.get('id') in approved_retirements and
                row.get('replacedBy') == [approved_retirements.get(row.get('id'))] and
                bool(re.fullmatch(r'[0-9a-f]{40}', row.get('sourceRevision', ''))) and
                bool(re.fullmatch(r'[0-9a-f]{64}', row.get('sourceFingerprint', ''))) and
                all(row.get(key) for key in ('source', 'currentVersion', 'currentPorts', 'currentContract', 'migration')),
                '退役记录缺少已审替代、原契约或Git来源')
        require(all(identifier in registered for identifier in row.get('replacedBy', [])), '退役替代算子未注册')
    if data.get('baselineReference'):
        baseline = data['baselineReference']
        require(set(baseline.get('registeredNativeIds', [])) <= {r['id'] for r in native} | set(retired_ids), '原生规划基线ID丢失')
        require(set(baseline.get('pythonExampleIds', [])) <= {r['id'] for r in examples}, 'Python规划基线ID丢失')
        for stage in data.get('stageLedger', []):
            if stage.get('status') in {'validated_manifest', 'complete'}:
                require(set(stage.get('introducedNativeIds', [])) <= {r['id'] for r in native} | set(retired_ids) and
                        set(stage.get('introducedPythonIds', [])) <= {r['id'] for r in examples}, '阶段已验证但新增ID缺失')
    require(dict(collections.Counter(r['currentPack'] for r in native)) == data.get('currentPackCounts'), '当前包统计不符')
    require(dict(collections.Counter(r['targetPack'] for r in native)) == data.get('targetPackCounts'), '目标包统计不符')
    orders = [r.get('migrationOrder') for r in rows]
    require(sorted(orders) == list(range(1, len(rows) + 1)), '迁移序号必须唯一连续')
    by_id = {r['id']: r for r in rows}
    m2 = next((s for s in data.get('stageLedger', []) if s.get('stage') == 'M2'), None)
    if m2 is not None:
        scoped = {r['id'] for r in native if r['wave'] == 'M2'}
        require(len(scoped) == 38 and set(m2.get('scopeNativeIds', [])) == scoped, 'M2必须完整登记原38项边界验收范围')
        require(m2.get('expectedNativeCount') == 256 and m2.get('expectedPythonExampleCount') == 8 and
                m2.get('introducedNativeIds') == [] and m2.get('introducedPythonIds') == [], 'M2不得隐式增加算子ID')
        for identifier in scoped:
            assessments = [a for a in by_id[identifier].get('stageAssessments', []) if a.get('stage') == 'M2']
            require(len(assessments) == 1 and assessments[0].get('status') in {'pending_manifest', 'manifest_verified'} and
                    bool(assessments[0].get('publicInterfaces')) and bool(assessments[0].get('dependencyBoundary')),
                    f'{identifier}: 缺少M2边界验收记录')
        if m2.get('status') in {'validated_manifest', 'complete'}:
            moves = m2.get('packageMoves', [])
            require(len(moves) == 3 and {m.get('id') for m in moves} ==
                    {'transform.make', 'register.constraints_3d', 'image.calibrated_distance'}, 'M2迁包范围必须精确为已审3项')
            for move in moves:
                row = by_id.get(move.get('id'), {})
                require(row.get('currentPack') == move.get('toPack') and any(
                    entry.get('stage') == 'M2' and entry.get('fromPack') == move.get('fromPack') and
                    entry.get('toPack') == move.get('toPack') and bool(entry.get('sourceRevision'))
                    for entry in row.get('packageHistory', [])), f"{move.get('id')}: M2迁包缺少原包/源码历史")
    m3 = next((stage for stage in data.get('stageLedger', []) if stage.get('stage') == 'M3'), None)
    if m3 and m3.get('status') in {'validated_manifest', 'complete'}:
        require(m3.get('expectedNativeCount') == 261 and len(m3.get('introducedNativeIds', [])) == 7 and
                set(m3.get('retiredNativeIds', [])) == set(approved_retirements) and
                set(retired_ids) == set(approved_retirements) and len(m3.get('scopeNativeIds', [])) == 11,
                'M3新增/退役/原11项范围登记不完整')
        require(set(m3.get('scopeNativeIds', [])) <= set(registered) | set(retired_ids), 'M3原始范围丢失')
    targets = set(data.get('targetPackages', []))
    m4a = next((stage for stage in data.get('stageLedger', []) if stage.get('stage') == 'M4a'), None)
    if m4a:
        require(m3 is not None and m3.get('status') == 'complete' and
                m3.get('mergeEvidence', {}).get('pr') == 64 and
                m3.get('mergeEvidence', {}).get('revision') == 'b22fde7fad01b7aea67886b6fe3345e92a5fe8bb',
                'M4a缺少已合并M3前置证据')
        require(m4a.get('expectedNativeCount') == 261 and m4a.get('expectedPythonExampleCount') == 8 and
                m4a.get('introducedNativeIds') == [] and m4a.get('introducedPythonIds') == [] and
                m4a.get('retiredNativeIds') == [],
                'M4a只能抽公共核，不得隐式新增/退役算子')
        if m4a.get('status') == 'complete':
            proof = m4a.get('mergeEvidence', {})
            require(proof.get('pr') == 65 and
                    proof.get('revision') == '02e15806136df452bd575ff0e20610db7636918c' and
                    proof.get('reviewedHead') == '2f2e99141b39ee94a105a01adf2dff40ce125337' and
                    proof.get('workflowRun') == 43824076118 and proof.get('jobCount') == 7 and
                    proof.get('conclusion') == 'success' and proof.get('codexReview') == 'completed_no_findings',
                    'M4a完成状态缺少PR65精确合并、CI或审查证据')
    m4b = next((stage for stage in data.get('stageLedger', []) if stage.get('stage') == 'M4b'), None)
    if m4b:
        additions = {'geometry.box_2d', 'profile.bind_geometry', 'profile.nearest_sample',
                     'profile.nearest_to_line', 'profile.filter_validity', 'profile.fit_line_robust',
                     'profile.fit_circle_robust', 'metrology.flush_point_line', 'metrology.circle_gap',
                     'metrology.point_offset'}
        deferred = {'gap.fit_line', 'gap.selected_point', 'gap.nearest_to_line',
                    'gap.drop_non_finite', 'gap.to_measurement_frame', 'gap.flush', 'gap.gap',
                    'gap.point_offset'}
        require(m4a is not None and m4a.get('status') == 'complete', 'M4b缺少已完成M4a前置证据')
        require(m4b.get('expectedNativeCount') == 271 and m4b.get('expectedPythonExampleCount') == 8 and
                m4b.get('targetProductVersion') == '1.4.4' and
                set(m4b.get('introducedNativeIds', [])) == additions and
                len(m4b.get('introducedNativeIds', [])) == len(additions) and
                m4b.get('introducedPythonIds') == [] and m4b.get('retiredNativeIds') == [],
                'M4b新增范围须精确为10项公共节点且不能提前退役旧ID')
        require(set(m4b.get('changedNativeIds', [])) ==
                {'gap.fit_line', 'gap.flush', 'gap.gap', 'gap.point_offset'} and
                len(m4b.get('changedNativeIds', [])) == 4,
                'M4b旧版本变更范围须精确为4项Gap适配节点')
        require(m4b.get('deferredRetirements', {}).get('stage') == 'M4c' and
                set(m4b.get('deferredRetirements', {}).get('nativeIds', [])) == deferred and
                deferred <= set(registered), 'M4b必须保留8项旧ID至M4c完成场景迁移')
        wave = next((stage for stage in data.get('stageLedger', []) if stage.get('stage') == 'M4'), {})
        require(wave.get('status') != 'complete', 'M4b不能代替整个M4场景波次验收')
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
            elif dependency in retired_ids:
                replacement = next(item['replacedBy'] for item in retired if item['id'] == dependency)
                require(all(by_id[item]['migrationOrder'] < row['migrationOrder'] for item in replacement),
                        f'{identifier}: 退役前置的替代算子顺序错误 {dependency}')
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
            if data.get('status') != 'planned' and isinstance(document, dict) and document.get('generatedBy'):
                require(document['generatedBy'] == data.get('evidence', {}).get('nativeManifestGeneratedBy'),
                        'manifest generatedBy与清单生成器证据不一致')
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
    details = set((ROOT / 'docs/operator-package-audit').glob('*.md'))
    details.update(ROOT / 'docs/operator-package-audit' / (row['currentPack'] + '.md') for row in rows)
    docs = [ROOT / 'docs/operator-package-boundaries.md', ROOT / 'docs/operator-package-migration-plan.md',
            ROOT / 'docs/operator-package-audit.md', *sorted(details)]
    for path in docs:
        available = path in generated_docs or path.is_file()
        require(available, f'缺少文档：{path.relative_to(ROOT)}')
        if not available:
            continue
        content = generated_docs[path] if path in generated_docs else path.read_text(encoding='utf-8')
        for link in re.findall(r'(?<!!)\[[^\]]*\]\(([^)]+)\)', content):
            link = link.strip('<>')
            if re.match(r'^[a-zA-Z][\w+.-]*:', link) or link.startswith('#'):
                continue
            destination = path.parent / link.split('#', 1)[0]
            require(destination in generated_docs or destination.exists(), f'{path.name}: 本地链接不存在 {link}')
    for row in rows:
        detail = ROOT / 'docs/operator-package-audit' / (row['currentPack'] + '.md')
        content = generated_docs.get(detail)
        if content is None and detail.is_file():
            content = detail.read_text(encoding='utf-8')
        require(content is not None and f" · {row['id']}\n" in content, f"{row['id']}: 缺少逐项文档")
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
