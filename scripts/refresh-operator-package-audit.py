import argparse
import ast
import collections
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / 'docs/operator-package-audit.json'
SPEC = importlib.util.spec_from_file_location('audit_checker', ROOT / 'scripts/check-operator-package-audit.py')
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)
BASELINE_REVISION = 'a2f6883'
ADDITIONS = {
    'profile.from_cloud': dict(currentPack='std-profile', targetPack='std-profile', source='packs/std-profile/ops/register.cpp',
        currentContract='显式声明frame、m/mm、plane、handedness、sampleOrder与sourceId；共享只读PointCloud，保留全部槽与可选通道。有限XYZ标validity=1，NaN标0，来源索引0..N-1；不排序、不换轴、不压缩。',
        targetContract='原生Profile公共适配，8字段描述与validity/sourceIndices独立数组；调用方确认acquisition，不从点序猜测采集语义。',
        acceptance='1280槽/NaN/可选通道保留；缺单位/帧拒绝，Inf及非法通道拒绝；共享输入不变；轻量无PCL/CV/ORT。'),
    'profile.remap_axes': dict(currentPack='std-profile', targetPack='std-profile', source='packs/std-profile/ops/register.cpp',
        currentContract='按明确轴排列及+1/-1符号映射XYZ和法线，更新目标frame、plane与handedness；单位、有效性、槽序、sourceId和sourceIndices不变。',
        targetContract='反射与轴置换通过显式Profile适配，不伪装右手刚体Transform；不排序、不丢NaN或有限无效槽。',
        acceptance='6种轴排列及符号奇偶、反射往返、法线和源索引一致；非法符号或目标帧失败不发布输出；输入不变。'),
    'profile.to_cloud': dict(currentPack='std-profile', targetPack='std-profile', source='packs/std-profile/ops/register.cpp',
        currentContract='显式m/mm换算XYZ，法线不缩放且轴不旋转；裸PointCloud无效槽置NaN，同时输出完整Profile与仅含小元数据的profile.Descriptor.v1 Record。',
        targetContract='标准裸云消费者使用明确米制；完整Profile维持原始无效载荷、validity和sourceIndices，禁止宣称裸云+小Record可无损往返。',
        acceptance='mm↔m往返、有限无效槽仅裸云置NaN、通道/索引保留；溢出与取消不发布部分结果，上游缓冲不变。'),
    'image.upgrade_plane_calib': dict(currentPack='std-image', targetPack='std-image', source='packs/std-image/ops/calib_upgrade.cpp',
        currentContract='image.PlaneCalib旧记录显式升级schemaVersion=1与imageFrame/planeFrame；缺unit必须选择m/mm；H齐次归一化保持映射；已有v1记录拒绝重标帧。',
        targetContract='共享core平面标定解析用于跨包消费；旧非版本化分支保持兼容，统一的版本化分支不猜单位或帧。',
        acceptance='缺单位/错误单位/版本/奇异H拒绝；m/mm映射一致、齐次尺度不敏感、幂等升级、帧冲突失败且输入输出原子。'),
    'dts.geometry_to_metric': dict(currentPack='dts', targetPack='solution-dts', source='packs/dts/ops/geometry_adapter.cpp',
        currentContract='调用方显式声明dts.geometry.v1及旧生产者；旧DTS点/线位置和端点毫米数值乘0.001，方向不缩放，保留原XZ解释；不处理Measurement。',
        targetContract='M1迁移桥接保留在场景适配层，M3按来源/旧版本插入或移除，不能将旧Meter标签视为真实米制或重复缩放。',
        acceptance='旧毫米+Meter夹具转米、方向/垂足/端点一致；Pixel/类型不匹配/非有限输入拒绝且无部分输出；旧业务符号保持。'),
}
CHANGED = ('pose.to_world_plane', 'image.calibrated_distance')
EXAMPLE_ID = 'py.profile_copy.offset_z'
M1_MERGED_REVISION = 'f0c043c6341755e8f254ac499a1593df087b6d94'
M2_MOVES = {
    'transform.make': dict(fromPack='std-pointcloud', toPack='std-geometry', source='packs/std-geometry/ops/transform_make.cpp'),
    'register.constraints_3d': dict(fromPack='std-pointcloud', toPack='std-geometry', source='packs/std-geometry/ops/constraints_3d.cpp'),
    'image.calibrated_distance': dict(fromPack='std-image', toPack='std-metrology', source='packs/std-metrology/ops/register.cpp'),
}


def assess_m2(row, status):
    if row['id'] == 'image.calibrated_distance':
        interfaces = ['kernels/include/lyflow/kernel/metrology.h', 'core/include/lyflow/calibration.h']
        boundary = 'std-metrology -> metrology-kernel -> data-contract；C++17/std及公开core校验，无OpenCV/PCL/ORT。'
    else:
        interfaces = ['kernels/include/lyflow/kernel/geometry.h', 'kernels/include/lyflow/kernel/rigid.h', 'core/include/lyflow/pose.h']
        if row['id'] == 'register.constraints_3d':
            interfaces = ['kernels/include/lyflow/kernel/constraints.h']
        boundary = 'std-geometry -> geometry-kernel -> data-contract；公开几何/刚体/Record契约，无PCL/OpenCV/ORT及跨包私有ops头。'
    records = row.setdefault('stageAssessments', [])
    record = next((entry for entry in records if entry.get('stage') == 'M2'), None)
    if record is None:
        record = dict(stage='M2')
        records.append(record)
    record.update(status=status, publicInterfaces=interfaces, dependencyBoundary=boundary,
        scope='M2公共核、轻量包归属与声明式依赖边界；原ID/版本/数据数值与错误语义保持。',
        acceptance=('待真实全包manifest核验；' if status == 'pending_manifest' else '当前描述已与真实全包manifest核验；') +
        '独立静态核、无重后端、既有算子与安装SDK数值回归由阶段验证证据记录，不代表M3场景算法迁移。')


def relocate_operator(row, op, stage):
    actual_pack = op.get('pack', 'core').split('@')[0]
    move = M2_MOVES.get(row['id']) if stage == 'M2' else None
    if move:
        if actual_pack != move['toPack'] or row['targetPack'] != move['toPack']:
            raise ValueError(f"{row['id']}: M2真实manifest必须包含已审迁包结果 {move['toPack']}")
        if op.get('version') != row['currentVersion'] or op.get('params', []) != row['currentParams'] or {
            key: op.get(key, []) for key in ('inputs', 'outputs')
        } != row['currentPorts']:
            raise ValueError(f"{row['id']}: M2迁包必须保持原版本、端口和参数，不得隐式改契约")
    if actual_pack != row['currentPack']:
        if not move or row['currentPack'] != move['fromPack']:
            raise ValueError(f"{row['id']}: 包变更必须先人工审计目标/依赖/契约")
        row.setdefault('packageHistory', []).append(dict(stage='M2', fromPack=row['currentPack'],
            fromPackVersion=row.get('currentPackVersion'), source=row['source'], currentVersion=row['currentVersion'], sourceRevision=M1_MERGED_REVISION,
            toPack=actual_pack))
        row['currentPack'] = actual_pack
        row['source'] = move['source']


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def artifact_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest_generator(manifest):
    generator = manifest.get('generatedBy') if isinstance(manifest, dict) else None
    if not isinstance(generator, str) or not generator.strip():
        raise ValueError('真实全包manifest必须提供非空generatedBy，不得使用产品版本代替')
    return generator


def prepare(data, stage='M2'):
    result = copy.deepcopy(data)
    if 'baselineReference' not in result:
        revision = subprocess.check_output(['git', 'rev-parse', BASELINE_REVISION], cwd=ROOT, text=True).strip()
        original = json.loads(subprocess.check_output(['git', 'show', f'{revision}:docs/operator-package-audit.json'], cwd=ROOT, text=True, encoding='utf-8'))
        result['baselineReference'] = dict(revision=revision, path='docs/operator-package-audit.json',
            sourceRevision=original['sourceRevision'], productVersion=original['productVersion'],
            nativeCount=original['nativeCount'], pythonExampleCount=original['pythonExampleCount'],
            registeredNativeIds=original['registeredNativeIds'], pythonExampleIds=sorted(r['id'] for r in original['pythonExamples']),
            evidence=original['evidence'])
    result['status'] = 'in_progress'
    stages = result.setdefault('stageLedger', [])
    if not any(s['stage'] == 'M0' for s in stages):
        stages.insert(0, dict(stage='M0', status='complete', expectedNativeCount=251,
            expectedPythonExampleCount=7, evidenceKind='planning_baseline',
            note='原逐项清单与分包规划已交付；不代表任何算法迁移已完成。'))
    if not any(s['stage'] == 'M1' for s in stages):
        stages.append(dict(stage='M1', status='pending_manifest', expectedNativeCount=256,
            expectedPythonExampleCount=8, introducedNativeIds=list(ADDITIONS), introducedPythonIds=[EXAMPLE_ID],
            changedNativeIds=list(CHANGED), note='仅公共契约/显式适配；M3场景算法迁移尚未实施；阶段完成需另行运行与交付验收。'))
    if stage == 'M2':
        m1 = next(s for s in stages if s['stage'] == 'M1')
        if not m1.get('manifestEvidence'):
            raise ValueError('M2需要先完成M1真实manifest基线，不能跳过公共契约验收')
        m1['status'] = 'complete'
        m1.setdefault('mergeEvidence', {}).update(pr=62, url='https://github.com/liangyuR/lyFlow/pull/62', revision=M1_MERGED_REVISION)
        scoped = sorted(row['id'] for row in result['operators'] if row['wave'] == 'M2')
        if not any(s['stage'] == 'M2' for s in stages):
            stages.append(dict(stage='M2', status='pending_manifest', expectedNativeCount=256,
                expectedPythonExampleCount=8, targetProductVersion='1.4.1', introducedNativeIds=[], introducedPythonIds=[],
                changedNativeIds=scoped, scopeNativeIds=scoped, relatedNativeIds=['register.constrained_fit'],
                packageMoves=[dict(id=identifier, **move) for identifier, move in M2_MOVES.items()],
                note='38项轻量几何/测量边界；3项迁包保持原ID/版本；M3/DTS场景算法未在本阶段迁移。'))
        for row in result['operators']:
            if row['wave'] == 'M2' and not any(entry.get('stage') == 'M2' for entry in row.get('stageAssessments', [])):
                assess_m2(row, 'pending_manifest')
    return result


def example_declarations():
    rows = {}
    for path in sorted((ROOT / 'python/examples').rglob('*.py')):
        for item in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
            if not isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            for decorator in item.decorator_list:
                if not isinstance(decorator, ast.Call) or not isinstance(decorator.func, ast.Name) or decorator.func.id != 'node':
                    continue
                declaration = {key.arg: ast.literal_eval(key.value) for key in decorator.keywords}
                identifier = declaration['id']
                if identifier in rows:
                    raise ValueError(f'重复Python示例ID: {identifier}')
                rows[identifier] = (path.relative_to(ROOT).as_posix(), decorator.lineno, declaration)
    return rows


def pending_sources(data):
    paths = {p.relative_to(ROOT).as_posix() for pattern in CHECKER.CONTRACT_SOURCES for p in ROOT.glob(pattern) if p.is_file()}
    prior = set(data['contractSourcePaths'])
    fingerprints = data['sourceFingerprints']
    return [dict(path=p, previousSha256=fingerprints.get(p),
                 sha256=CHECKER.source_hash(ROOT / p) if p in paths else None,
                 kind='removed' if p not in paths else 'tracking_added' if p not in prior else 'modified')
            for p in sorted(paths | prior) if p not in paths or p not in prior or
            CHECKER.source_hash(ROOT / p) != fingerprints.get(p)]


def refreshed(data, manifest_path, reviewed_paths, review_note, product_version, stage='M2'):
    result = prepare(data, stage)
    for field, baseline_ids, added in (
        ('operators', set(result['baselineReference']['registeredNativeIds']) | (set(ADDITIONS) if stage == 'M2' else set()), set(ADDITIONS)),
        ('pythonExamples', set(result['baselineReference']['pythonExampleIds']) | ({EXAMPLE_ID} if stage == 'M2' else set()), {EXAMPLE_ID}),
    ):
        identifiers = [row['id'] for row in result[field]]
        if len(identifiers) != len(set(identifiers)) or not set(baseline_ids) <= set(identifiers) or not set(identifiers) <= set(baseline_ids) | added:
            raise ValueError(f'{field}: 现有清单丢失原基线ID、重复或包含未经定义的ID，拒绝刷新')
    manifest = load(manifest_path)
    operators = manifest if isinstance(manifest, list) else manifest['operators']
    exported = {op['id']: op for op in operators}
    expected = set(result['baselineReference']['registeredNativeIds']) | set(ADDITIONS)
    if len(exported) != len(operators) or set(exported) != expected:
        raise ValueError('需要真实全包manifest且ID全集等于原251项+已审5项；不得用轻量构建或占位描述刷新')
    generator = manifest_generator(manifest)
    if generator != 'lyflow-core/' + product_version:
        raise ValueError('请求的产品版本与真实manifest.generatedBy不一致')
    paths = {p.relative_to(ROOT).as_posix() for pattern in CHECKER.CONTRACT_SOURCES for p in ROOT.glob(pattern) if p.is_file()}
    fingerprints = result['sourceFingerprints']
    source_changes = pending_sources(result)
    changed = {item['path'] for item in source_changes}
    reviewed = {Path(p).as_posix() for p in reviewed_paths}
    if not review_note.strip() or changed != reviewed:
        raise ValueError('源码复核路径必须精确覆盖变化集合；未复核=' + ','.join(sorted(changed - reviewed)) +
                         '；多余=' + ','.join(sorted(reviewed - changed)))
    existing = {row['id']: row for row in result['operators']}
    for identifier, definition in ADDITIONS.items():
        if identifier not in existing:
            existing[identifier] = dict(id=identifier, action='solution' if definition['currentPack'] == 'dts' else 'retain', wave='M1', operatorKind='native',
                prerequisites=['M0'], migrationPrerequisites=['M0'], relatedOperators=[],
                dependencies=['core公开Profile/几何/Record契约与C++标准库；无PCL/OpenCV/ONNX Runtime直接依赖'],
                notes='M1新增显式契约适配；不替代M3轮廓与DTS场景算法迁移。', **definition)
    ordered = sorted(result['operators'], key=lambda r: r['migrationOrder'])
    additions = [existing[key] for key in ADDITIONS if key not in {r['id'] for r in ordered}]
    split = next((i for i, row in enumerate(ordered) if row['wave'] > 'M1'), len(ordered))
    ordered[split:split] = additions
    for row in ordered:
        op = exported[row['id']]
        relocate_operator(row, op, stage)
        row.update(currentVersion=op['version'], currentPackVersion=op.get('pack', 'core'),
                   label=op.get('label', row.get('label')), currentPorts={k: op.get(k, []) for k in ('inputs', 'outputs')},
                   currentParams=op.get('params', []), descriptionEvidence=f'{stage}真实全包manifest核对；目标/依赖/约束保留原规划，阶段验收另行登记。')
        source = row['source'].rsplit(':', 1)[0] if re.search(r':\d+$', row['source']) else row['source']
        lines = (ROOT / source).read_text(encoding='utf-8').splitlines()
        line = next((i for i, text in enumerate(lines, 1) if '"' + row['id'] + '"' in text), None)
        if line is None:
            line = int(row['source'].rsplit(':', 1)[1])
        row['source'] = f'{source}:{line}'
        if row['id'] in CHANGED:
            update = dict(stage='M1', version=op['version'],
                contract='新增显式frame参数，版本化PlaneCalib使用共享schemaVersion=1/frame/unit解析；旧非版本化分支保持兼容。',
                acceptance='v1帧缺失/不匹配拒绝；米/毫米一致；旧参数与旧标定路径不漂移。')
            updates = row.setdefault('contractUpdates', [])
            previous = next((item for item in updates if item.get('stage') == 'M1'), None)
            if previous is None:
                updates.append(update)
            else:
                previous.update(update)
    examples = result['pythonExamples']
    declarations = example_declarations()
    if set(declarations) != set(result['baselineReference']['pythonExampleIds']) | {EXAMPLE_ID}:
        raise ValueError('Python示例全集与基线7项+Profile示例不一致')
    if not any(row['id'] == EXAMPLE_ID for row in examples):
        examples.append(dict(id=EXAMPLE_ID, currentPack='python-examples', targetPack='python-sdk-examples',
            action='retain', wave='M7', operatorKind='python-example',
            dependencies=['Python>=3.11、NumPy、lyflow_python；用户解释器环境，无原生重后端'],
            currentContract='输入Profile.copy后仅平移validity=1槽Z；offset_m按米声明并对mm载荷显式乘1000；保留NaN/有限无效槽、索引、描述及可选通道。',
            targetContract='展示公共Profile契约与只读复制，不实现M3场景检测算法；不把大数组塞Record。',
            prerequisites=['M1', 'M6'], migrationPrerequisites=['M1', 'M6'], relatedOperators=[],
            acceptance='1280槽且2个无效槽保留、原始输入不变；真实worker往返及单位换算、readonly、取消回归。',
            notes='导入后才注册，不计入固定原生全集；独立main自检不是现场算法验收。'))
    for row in examples:
        path, line, declaration = declarations[row['id']]
        row.update(source=f'{path}:{line}', exampleDeclaration=declaration, label=declaration['label'],
                   currentVersion=declaration['version'], currentPackVersion='python-examples',
                   currentPorts={key: [dict(name=name, type=kind) for name, kind in declaration.get(key, {}).items()] for key in ('inputs', 'outputs')},
                   currentParams=[dict(name=name, **spec) for name, spec in declaration.get('params', {}).items()],
                   descriptionEvidence='当前Python装饰器AST与函数实现复核；仅用户导入后注册。')
    for index, row in enumerate(ordered + sorted(examples, key=lambda r: r.get('migrationOrder', 999999)), 1):
        row['migrationOrder'] = index
    result.update(operators=ordered, nativeCount=len(ordered), pythonExampleCount=len(examples),
                  registeredNativeIds=sorted(exported), productVersion=product_version,
                  sourceRevision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  currentPackCounts=dict(collections.Counter(r['currentPack'] for r in ordered)),
                  targetPackCounts=dict(collections.Counter(r['targetPack'] for r in ordered)), contractSourcePaths=sorted(paths))
    fingerprints.update({p: CHECKER.source_hash(ROOT / p) for p in paths})
    for path in set(result['contractSourcePaths']) - paths:
        fingerprints.pop(path, None)
    stage_entry = next(s for s in result['stageLedger'] if s['stage'] == stage)
    stage_entry.update(status='validated_manifest', reviewedChange=review_note,
                 reviewedSourceChanges=stage_entry.get('reviewedSourceChanges', []) + source_changes,
                 manifestEvidence=dict(sha256=artifact_hash(manifest_path), operatorCount=len(exported),
                                       path=manifest_path.resolve().relative_to(ROOT).as_posix()))
    if stage == 'M2':
        for row in ordered:
            if row['wave'] == 'M2':
                assess_m2(row, 'manifest_verified')
    result['evidence'] = dict(nativeManifestGeneratedBy=generator,
        nativeManifestOperatorCount=len(exported), manifestSha256=artifact_hash(manifest_path),
        sourceState='reviewed-working-tree', note='当前全包manifest与逐项元数据一致；构建/算法/现场验收按阶段ledger另行记录。')
    return result


def render_details(data):
    documents = {}
    actions = {'retain': '保留并验边界', 'move': '迁移归属', 'split': '拆公共能力/适配', 'solution': '保留场景职责'}
    rows = sorted(data['operators'] + data['pythonExamples'], key=lambda r: r['migrationOrder'])
    for pack in dict.fromkeys(row['currentPack'] for row in rows):
        entries = [row for row in rows if row['currentPack'] == pack]
        lines = [f'# {pack} 逐算子迁移清单', '',
            f"状态：实施中；当前 {len(entries)} 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。", '',
            '下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。', '']
        for row in entries:
            path, line = row['source'].rsplit(':', 1)
            def ports(kind):
                return ', '.join(f"`{p['name']}:{p['type']}{'?' if p.get('required') is False else ''}`" for p in row['currentPorts'][kind]) or '无'
            units = ', '.join(f"{p['name']}={p['unit']}" for p in row['currentParams'] if p.get('unit')) or '参数未统一声明单位；以逐项契约及源码为准'
            lines += [f"## {row['migrationOrder']:03} · {row['id']}", '',
                f"- 当前：`{pack}` / `{row['currentVersion']}` / {row['label']}。目标：`{row['targetPack']}`；{actions[row['action']]}；**{row['wave']}**。",
                f'- 源码：[{path}:{line}](../../{path}#L{line})。', f"- 端口：{ports('inputs')} → {ports('outputs')}。",
                f'- 参数单位标注：{units}。', '- 依赖：' + '；'.join(row['dependencies']),
                '- 当前契约：' + row['currentContract'], '- 目标契约：' + row['targetContract'],
                '- 迁移前置：' + '；'.join(row['migrationPrerequisites']),
                '- 关联算子：' + ('；'.join(row.get('relatedOperators', [])) or '无额外逐ID前置；公共协议见前置项'),
                '- 验收：' + row['acceptance'], '- 说明：' + row['notes']]
            for update in row.get('contractUpdates', []):
                lines += [f"- {update['stage']}契约更新（{update['version']}）：{update['contract']}；验收：{update['acceptance']}"]
            for assessment in row.get('stageAssessments', []):
                lines += [f"- {assessment['stage']}边界验收（{assessment['status']}）：{assessment['scope']}",
                    '- 阶段依赖边界：' + assessment['dependencyBoundary'],
                    '- 公开接口：' + '；'.join(assessment['publicInterfaces']),
                    '- 阶段验收边界：' + assessment['acceptance']]
            for move in row.get('packageHistory', []):
                lines += [f"- {move['stage']}迁包来源：`{move['fromPack']}` / `{move['source']}` @ `{move['sourceRevision']}`；原目标包和依赖规划保留。"]
            lines += ['']
        documents['python-examples' if pack == 'python-examples' else pack] = '\n'.join(lines)
    return documents


def render_overview(data):
    baseline = data['baselineReference']
    current_stage = next(s for s in reversed(data['stageLedger']) if s['stage'] != 'M0')
    manifest_example = f"build/{current_stage['stage'].lower()}-full/manifest.json"
    version_example = current_stage.get('targetProductVersion', data['productVersion'])
    lines = ['# 现有算子分包审计', '',
        f"状态：实施中。当前产品 {data['productVersion']}，{data['nativeCount']} 个原生算子及 {data['pythonExampleCount']} 个随附Python示例。", '',
        f"原规划基线仍为 {baseline['nativeCount']} 原生 / {baseline['pythonExampleCount']} 示例，源码基线 `{baseline['sourceRevision']}`；"
        f"完整原文件可从 Git `{baseline['revision']}:docs/operator-package-audit.json` 追溯，当前机器清单的baselineReference保留原ID全集与证据摘要。", '',
        '参见[包边界](operator-package-boundaries.md)、[迁移计划](operator-package-migration-plan.md)、[完整JSON](operator-package-audit.json)及[Profile契约](profile-data-contract.md)。', '',
        '## 当前包逐项入口', '', '| 当前归属 | 数量 | 逐项清单 |', '| --- | ---: | --- |']
    for pack, count in data['currentPackCounts'].items():
        lines.append(f'| {pack} | {count} | [查看](operator-package-audit/{pack}.md) |')
    lines += [f"| 原生合计 | {data['nativeCount']} | 每个注册ID恰好一次 |",
        f"| python-examples | {data['pythonExampleCount']} | [查看](operator-package-audit/python-examples.md) |", '',
        '## 目标主归属', '', '| 目标包 | 当前原生算子主归属数量 |', '| --- | ---: |']
    lines += [f'| {pack} | {count} |' for pack, count in data['targetPackCounts'].items()]
    lines += ['', '每项仍只有一个主责任归属。split可能抽出公共内核并保留场景适配，统计不预测未来注册数量，也不是数据流拓扑。', '',
        '## 阶段证据', '', '| 阶段 | 状态 | 原生 / 示例 | 证据边界 |', '| --- | --- | --- | --- |']
    for stage in data['stageLedger']:
        proof = stage.get('manifestEvidence', {})
        evidence = ('全包manifest SHA256 `' + proof['sha256'] + '`；描述匹配，不代表阶段全部验收已完成') if proof else stage['note']
        if stage.get('mergeEvidence'):
            evidence += f"；[PR{stage['mergeEvidence']['pr']}]({stage['mergeEvidence']['url']})已合并 `{stage['mergeEvidence']['revision']}`"
        lines.append(f"| {stage['stage']} | {stage['status']} | {stage['expectedNativeCount']} / {stage['expectedPythonExampleCount']} | {evidence} |")
    waves = collections.Counter(row['wave'] for row in data['operators'])
    lines += ['', '## 波次和顺序', '', '| 波次 | 内容 | 原生项数 |', '| --- | --- | ---: |']
    lines += [f'| {wave} | {label} | {waves[wave]} |' for wave, label in data['migrationWaves'].items()]
    lines += ['', '原项目标包、依赖、迁移前置、数据约束与验收要求保留，M2当前依赖与验收边界另列；M1/M2不表示M3场景算法已迁移。序号随新增项重排，只提供满足已列前置的一种实施顺序，不改变惰性调度或业务定义。', '',
        '## 静态检查与复核刷新', '', '```powershell', 'python scripts/check-operator-package-audit.py',
        f'python scripts/check-operator-package-audit.py --manifest {manifest_example}', '```', '',
        '静态检查比较契约来源集合/指纹、ID、版本、包归属、端口、参数及本地文档链接；不能替代运行、SDK、算法或现场验收。全包manifest必须来自真实启用全部包的构建，轻量构建不构成全集证据。', '',
        '审查实际源码差异后执行刷新，逐个传入变化路径：', '', '```powershell',
        f'python scripts/refresh-operator-package-audit.py --stage {current_stage["stage"]} --manifest {manifest_example} --product-version {version_example} --reviewed-change "当前阶段公共契约、包归属及依赖边界逐项复核" --reviewed-path <变化路径1> --reviewed-path <变化路径2>',
        '```', '',
        '工具要求reviewed-path精确覆盖所有变化契约文件；拒绝漏审、虚增ID或缺少全包描述。M2仅允许明确定义的3项迁包，保持版本/端口/参数，并保留原包和源码历史。只刷新当前manifest快照、源码锚点、统计及逐项Markdown，不覆盖原有目标/依赖/约束。源码复核前后SHA256与真实manifest SHA256写入阶段ledger。重复运行且源码不变时可不传reviewed-path，保留先前复核证据；工具只将当前阶段设为validated_manifest，完成需另行验收后登记，先前阶段证据保持。', '',
        'Python示例由源码装饰器AST发现，只有用户导入后注册；用户lib.*/py.*、外部包和测试节点不属于固定原生全集。原6导入器、7片段、6Bundle等附属资产仍按机器清单登记。', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description='复核后刷新当前manifest元数据；保留逐项迁移语义和原251/7 Git基线。')
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--list-pending-sources', action='store_true')
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--reviewed-path', action='append', default=[])
    parser.add_argument('--reviewed-change', default='')
    parser.add_argument('--product-version')
    parser.add_argument('--stage', choices=['M1', 'M2'], default='M2')
    args = parser.parse_args()
    if args.list_pending_sources:
        print(json.dumps(pending_sources(load(AUDIT)), ensure_ascii=False, indent=2))
        return
    if args.prepare:
        if args.manifest or args.reviewed_path or args.reviewed_change:
            parser.error('--prepare不刷新manifest或指纹')
        data = prepare(load(AUDIT), args.stage)
        documents = {}
    else:
        if not args.manifest or not args.product_version:
            parser.error('刷新需要--manifest和--product-version')
        data = refreshed(load(AUDIT), args.manifest, args.reviewed_path, args.reviewed_change, args.product_version, args.stage)
        documents = {ROOT / 'docs/operator-package-audit' / f'{pack}.md': content for pack, content in render_details(data).items()}
        documents[ROOT / 'docs/operator-package-audit.md'] = render_overview(data)
        problems = CHECKER.validate(data, [args.manifest], documents)
        if problems:
            raise ValueError('候选清单校验失败，未写回：\n' + '\n'.join(problems))
    AUDIT.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    for path, content in documents.items():
        path.write_text(content, encoding='utf-8')
    print(f"Audit {'prepared; manifest pending' if args.prepare else 'refreshed from real manifest'}: {data['nativeCount']} native, {data['pythonExampleCount']} Python")


if __name__ == '__main__':
    main()
