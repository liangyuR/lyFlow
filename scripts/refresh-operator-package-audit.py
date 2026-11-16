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

M2_MERGED_REVISION = '36c969149e119614b7d221c8006d1a311d6d2995'
M3_MERGED_REVISION = 'b22fde7fad01b7aea67886b6fe3345e92a5fe8bb'
M4A_MERGED_REVISION = '02e15806136df452bd575ff0e20610db7636918c'
M4B_DEFINITIONS = {
    'geometry.box_2d': ('std-geometry', 'packs/std-geometry/ops/geometry.cpp',
        '显式构造米制或像素Box2D；两轴边界须有限、在float32范围且min<=max，不自动交换边界。',
        'm/px、相等边界、逆序/非有限/溢出边界拒绝；作为外部物理ROI时须另外显式绑定Profile坐标上下文。'),
    'profile.bind_geometry': ('std-profile', 'packs/std-profile/ops/selection.cpp',
        '调用方显式确认frame与plane，为米制点/线/圆/框绑定完整Profile来源和几何内容hash，profile.Geometry.v2关系为declared；不缩放或换轴。',
        '缺帧/错平面/像素/非法几何拒绝；来源不冒充拟合结果、只读输入和取消原子性。'),
    'profile.nearest_sample': ('std-profile', 'packs/std-profile/ops/selection.cpp',
        '按明确分析轴和m/mm换算米，扫描有效槽到锚点的欧氏距离；等距取原槽较前者，输出真实米制样本、原slot/sourceIndex及完整来源绑定。',
        'NaN/finite-invalid跳过、非单调原序、YZ/XY/XZ、单位与错帧/手性/hash；保留轮廓和参考几何来源、无大数组Record、取消不发布。'),
    'profile.nearest_to_line': ('std-profile', 'packs/std-profile/ops/selection.cpp',
        '按到同物理空间无限直线的垂距选择有效原槽，不限幅到显示线段；米制输出保留原轮廓与参考几何来源、原slot和物理源索引。',
        '线段外最近点、等距原序、非单位方向、掩码/NaN、上下文错配、来源预算与取消原子性。'),
    'profile.filter_validity': ('std-profile', 'packs/std-profile/ops/selection.cpp',
        '按既有有效掩码和XYZ有限性显式保槽或压缩；通道、原sourceCount/sourceIndices不变，valid/invalid Indices绑定输入cloud.id，保槽removedCount=0。',
        '有限无效与NaN、空/全无效、所有可选通道、原槽Indices与物理索引映射、保槽/压缩计数、只读和取消原子性。'),
    'profile.fit_line_robust': ('std-profile', 'packs/std-profile/ops/robust_features.cpp',
        '原生Profile明确米制分析平面，复用公共双迹线RANSAC、近端选段重拟合、free/band/fixed方向与中位回退；保留最终多重权重与原槽索引，几何上下文汇集全部来源。',
        '不以TLS替换RANSAC；42个已有定义直线夹具逐位一致，固定方向重复权重、唯一观察内点、NaN掩码、同帧ROI/参考、单位与取消/失败原子性。'),
    'profile.fit_circle_robust': ('std-profile', 'packs/std-profile/ops/robust_features.cpp',
        '原生Profile有效样本映射米制分析平面；固定种子Circle2D RANSAC/LM，自由、固定半径、软先验三模式及越界夹限明确分开，原槽及来源不排序不丢失。',
        '短弧/半径上下界/固定/先验/奇异圆心、m/mm和各平面、NaN/finite-invalid、原槽映射与输入只读、取消和失败原子性。'),
    'metrology.flush_point_line': ('std-metrology', 'packs/std-metrology/ops/profile_dimensions.cpp',
        '同空间米制点线及v1/v2内容绑定上下文；显式正法向和canonical_axis口径，保持既有float方向及毫米补偿数值路径；输出m或mm测量、垂足与显示段。',
        '正负/水平竖直/归一化容差/零长度显示段、补偿与输出单位；像素/错帧/非有限/溢出拒绝，不把无效测量转业务合格。'),
    'metrology.circle_gap': ('std-metrology', 'packs/std-metrology/ops/profile_dimensions.cpp',
        '同空间米制两圆及来源上下文；圆心连线边界距离和基准切线距离是两个显式定义，保持Gap毫米数值/偏置/符号顺序，输出m或mm及来源绑定显示段。',
        '两定义/左右交换/间隙符号/重合圆心拒绝、基准线单位与帧/手性/hash校验；合法定义量测与旧库逐位对照。'),
    'metrology.point_offset': ('std-metrology', 'packs/std-metrology/ops/profile_dimensions.cpp',
        '同空间米制点按b-a分解两轴位移，增益后绝对值再加毫米偏置；欧氏距离独立于补偿，保留全部来源并支持零位移显示段。',
        '正负/交换/增益abs偏置顺序/m和mm输出、零位移；像素/错帧/非法增益/非有限/溢出拒绝，失败及取消不发布。'),
}
M4B_CHANGED_VERSIONS = {
    'gap.fit_line': '1.1.1', 'gap.flush': '1.1.1', 'gap.gap': '1.0.1', 'gap.point_offset': '1.0.1',
}
M4B_DEFERRED_RETIREMENTS = ('gap.fit_line', 'gap.selected_point', 'gap.nearest_to_line',
    'gap.drop_non_finite', 'gap.to_measurement_frame', 'gap.flush', 'gap.gap', 'gap.point_offset')
M3_RETIREMENTS = {
    'dts.profile_clean': 'profile.clean',
    'dts.split_faces': 'profile.split_faces',
}
M3_DEFINITIONS = {
    'profile.input': ('std-profile', 'packs/std-profile/ops/input.cpp',
        '显式描述的原生Profile输入；CSV v1严格解析指定平面两轴与可选强度，NaN保槽，Inf/非法行拒绝；宿主可注入完整LYPR。',
        'CSV各平面/m和mm/NaN/通道/预算/取消/中文路径；完整LYPR跨CLI与独立宿主一致，坏输入不运行不落部分输出。'),
    'profile.clean': ('std-profile', 'packs/std-profile/ops/processing.cpp',
        '原始槽邻域中值后按强度与有限性降有效掩码；默认preserve_slots，显式compact保留原sourceCount/sourceIndices和可选通道，不排序。',
        '无效中心不补值；缺样/有限无效/端点/even窗口/附加通道/输入只读/取消；旧DTS压缩有效数值等价。'),
    'profile.split_faces': ('std-profile', 'packs/std-profile/ops/processing.cpp',
        'acquisition轮廓转mm工作视图，压缩有效样本后按相邻采样间断分片，片内稳定x排序；profile.Faces.v1绑定完整源契约/hash及槽/源索引。',
        'mm/m及各分析平面、缺槽/重复x/重叠面/RMS与角度严格边界；不修改源数组，faces与另一轮廓混配拒绝。'),
    'profile.fit_line': ('std-profile', 'packs/std-profile/ops/native_analysis.cpp',
        '按原生Profile声明平面和m/mm显式换算SI，TLS拟合；默认拒绝无效槽，valid_only显式过滤；输出米制Line2D及源绑定统计/几何上下文。',
        'TLS浮点收窄后RMS、退化/范围/残差/NaN策略、通道有限性；统计与线hash对应、输入只读。'),
    'profile.transform_pair': ('std-profile', 'packs/std-profile/ops/native_analysis.cpp',
        '显式共同帧、输出帧和平面及三段刚体变换；输出SI Profile并保槽/掩码/通道/源索引，valid_only无效槽仅换单位而不冒充观测。',
        '变换组合顺序、proper rigid、法线旋转、源身份/手性、无效槽策略及失败原子性。'),
    'metrology.profile_gap_flush': ('std-metrology', 'packs/std-metrology/ops/profile_metrology.cpp',
        '消费米制左右拟合线与各自profile.LineFit.v1，验证线hash/帧/平面/手性；间隙取实采样边缘差，面差为referenceX处右减左，不当作法向距离。',
        '左右/符号/采样范围/RMS/外推/竖直退化及上下文混配；Measurement单位m，质量不替生产判据。'),
    'metrology.signed_point_line': ('std-metrology', 'packs/std-metrology/ops/profile_metrology.cpp',
        '米制点线及各自profile.Geometry.v1，验证帧/平面/手性/实际几何hash；指定正法向的有符号距离与垂足，公共核支持显式方向规范。',
        '符号/反向/垂直/非法方向/不同单位帧与非有限拒绝；DTS复用DeclaredUnit保持旧float方向数值路径。'),
}
M3_MOVES = {
    'profile.analyze': dict(fromPack='std-pointcloud', toPack='std-profile', source='packs/std-profile/ops/analysis.cpp'),
    'profile.project_pair': dict(fromPack='std-pointcloud', toPack='std-profile', source='packs/std-profile/ops/analysis.cpp'),
    'profile.gap_flush': dict(fromPack='std-pointcloud', toPack='std-metrology', source='packs/std-metrology/ops/profile_gap_flush.cpp'),
}
for _id, _file in {
    'dts.profile_in': 'profile_in', 'dts.seal_dome': 'seal_dome', 'dts.pick_metal': 'pick_metal',
    'dts.seal_root': 'seal_root', 'dts.flush': 'flush', 'dts.profile_bundle': 'bundle',
    'dts.geometry_to_metric': 'geometry_adapter',
}.items():
    M3_MOVES[_id] = dict(fromPack='dts', toPack='solution-dts', source=f'solutions/dts/ops/{_file}.cpp')


def prepare_m3(data):
    result = prepare(data, 'M2')
    m2 = next(item for item in result['stageLedger'] if item['stage'] == 'M2')
    if not m2.get('manifestEvidence'):
        raise ValueError('M3需要先验证M2全包manifest')
    m2.update(status='complete', mergeEvidence=dict(pr=63,
        url='https://github.com/liangyuR/lyFlow/pull/63', revision=M2_MERGED_REVISION))
    if not any(item['stage'] == 'M3' for item in result['stageLedger']):
        result['stageLedger'].append(dict(stage='M3', status='pending_manifest',
            expectedNativeCount=261, expectedPythonExampleCount=8, targetProductVersion='1.4.2',
            introducedNativeIds=sorted(M3_DEFINITIONS), introducedPythonIds=[],
            retiredNativeIds=sorted(M3_RETIREMENTS),
            scopeNativeIds=sorted(row['id'] for row in result['operators'] if row['wave'] == 'M3'),
            note='公共轮廓/度量、DTS场景v2及显式图迁移；待整体验证/SDK交付/CI及审查，不以manifest代替运行验收。'))
    return result


def new_m3_rows():
    return [dict(id=identifier, currentPack=pack, targetPack=pack, source=source,
        action='retain', wave='M3', operatorKind='native',
        dependencies=['公开Profile/几何/Record契约与C++17；分析内核仅编译期Eigen5.0.1，无PCL/OpenCV/ORT'],
        currentContract=contract, targetContract=contract, acceptance=acceptance,
        prerequisites=['M1', 'M2'], migrationPrerequisites=['M1', 'M2'], relatedOperators=[],
        notes='M3明确新增公共能力；保留既有数值方法与源契约，非旧ID别名或第二份场景算法。')
        for identifier, (pack, source, contract, acceptance) in M3_DEFINITIONS.items()]


def retire_m3_rows(result):
    retired = result.setdefault('retiredOperators', [])
    for row in list(result['operators']):
        replacement = M3_RETIREMENTS.get(row['id'])
        if not replacement:
            continue
        if any(item['id'] == row['id'] for item in retired):
            raise ValueError('退役ID重复登记')
        retired.append(dict(**copy.deepcopy(row), retiredStage='M3', replacedBy=[replacement],
            sourceFingerprint=result['sourceFingerprints'][row['source'].rsplit(':', 1)[0]],
            sourceRevision=M2_MERGED_REVISION,
            migration='显式dtsProfile.v1图升级插入Profile适配并保留节点ID/参数/UI；不注册旧计算别名。'))
        result['operators'].remove(row)
    existing = {row['id'] for row in result['operators']}
    additions = [row for row in new_m3_rows() if row['id'] not in existing]
    ordered = sorted(result['operators'], key=lambda row: row['migrationOrder'])
    split = next((i for i, row in enumerate(ordered) if row['wave'] >= 'M3'), len(ordered))
    ordered[split:split] = additions
    for index, row in enumerate(ordered + result['pythonExamples'], 1):
        row['migrationOrder'] = index
    result['operators'] = ordered


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
    move = M2_MOVES.get(row['id']) if stage == 'M2' else M3_MOVES.get(row['id']) if stage == 'M3' else None
    if move:
        if actual_pack != move['toPack'] or (stage == 'M2' and row['targetPack'] != move['toPack']):
            raise ValueError(f"{row['id']}: M2真实manifest必须包含已审迁包结果 {move['toPack']}")
        if (stage == 'M2' or row['id'].startswith('profile.')) and (op.get('version') != row['currentVersion'] or op.get('params', []) != row['currentParams'] or {
            key: op.get(key, []) for key in ('inputs', 'outputs')
        } != row['currentPorts']):
            raise ValueError(f"{row['id']}: M2迁包必须保持原版本、端口和参数，不得隐式改契约")
    if actual_pack != row['currentPack']:
        if not move or row['currentPack'] != move['fromPack']:
            raise ValueError(f"{row['id']}: 包变更必须先人工审计目标/依赖/契约")
        row.setdefault('packageHistory', []).append(dict(stage=stage, fromPack=row['currentPack'],
            fromPackVersion=row.get('currentPackVersion'), source=row['source'], currentVersion=row['currentVersion'], sourceRevision=M1_MERGED_REVISION if stage == 'M2' else M2_MERGED_REVISION,
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
    if stage == 'M4b':
        result = copy.deepcopy(data)
        m4a = next((entry for entry in result.get('stageLedger', []) if entry['stage'] == 'M4a'), {})
        if (m4a.get('status') not in {'validated_manifest', 'complete'} or
                m4a.get('manifestEvidence', {}).get('operatorCount') != 261 or
                not re.fullmatch(r'[0-9a-f]{64}', m4a.get('manifestEvidence', {}).get('sha256', '')) or
                m4a.get('expectedNativeCount') != 261 or
                m4a.get('targetProductVersion') != '1.4.3'):
            raise ValueError('M4b需要M4a真实全包manifest与PR65前置验收')
        m4a.update(status='complete', mergeEvidence=dict(pr=65,
            url='https://github.com/liangyuR/lyFlow/pull/65', revision=M4A_MERGED_REVISION,
            reviewedHead='2f2e99141b39ee94a105a01adf2dff40ce125337', workflowRun=43824076118,
            jobCount=7, conclusion='success', codexReview='completed_no_findings',
            reviewReceipt='https://github.com/liangyuR/lyFlow/pull/65#issuecomment-6910298242'),
            note='公共稳健拟合核、可选模块与安装SDK已验收；PR65七项CI通过、同head Codex无问题并合并。整个M4及场景导入迁移不计作完成。')
        if not any(entry['stage'] == 'M4b' for entry in result['stageLedger']):
            result['stageLedger'].append(dict(stage='M4b', status='pending_manifest',
                expectedNativeCount=271, expectedPythonExampleCount=8, targetProductVersion='1.4.4',
                introducedNativeIds=sorted(M4B_DEFINITIONS), introducedPythonIds=[], retiredNativeIds=[],
                changedNativeIds=sorted(M4B_CHANGED_VERSIONS),
                deferredRetirements=dict(stage='M4c', nativeIds=sorted(M4B_DEFERRED_RETIREMENTS)),
                note='新增公共Profile、度量与Box2D构造；旧Gap适配复用同一公共核并patch隔离缓存，非法输入明确拒绝。旧8ID及场景/Bundle/六导入器留M4c，整个M4未完成。'))
        return result
    if stage == 'M4a':
        result = copy.deepcopy(data)
        m3 = next((entry for entry in result.get('stageLedger', []) if entry['stage'] == 'M3'), {})
        proof = m3.get('mergeEvidence', {})
        if m3.get('status') != 'complete' or proof.get('pr') != 64 or proof.get('revision') != M3_MERGED_REVISION:
            raise ValueError('M4a需要M3已完成且PR64合并证据准确，不能以源码存在代替前置验收')
        if not any(entry['stage'] == 'M4a' for entry in result['stageLedger']):
            result['stageLedger'].append(dict(stage='M4a', status='pending_manifest',
                expectedNativeCount=261, expectedPythonExampleCount=8,
                targetProductVersion='1.4.3',
                introducedNativeIds=[], introducedPythonIds=[], retiredNativeIds=[],
                note='M4首个切片：单一RANSAC/LM实现的公共静态核与可选后端声明；注册ID/版本/端口/参数保持，整个M4的38项迁移尚未完成。'))
        return result
    if stage == 'M3':
        return prepare_m3(data)
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


def introduce_m4b_rows(result):
    existing = {row['id'] for row in result['operators']}
    additions = []
    for identifier, (pack, source, contract, acceptance) in M4B_DEFINITIONS.items():
        if identifier in existing:
            continue
        backend = identifier in {'profile.fit_line_robust', 'profile.fit_circle_robust'}
        additions.append(dict(id=identifier, currentPack=pack, targetPack=pack, source=source,
            action='retain', wave='M4', operatorKind='native',
            dependencies=[('公开profile_robust_kernel/profile_features，显式可选PCL后端；不依赖OpenCV/ORT/匹配后端' if backend else
                           '公开Profile/几何/度量契约与C++17；无PCL/OpenCV/ORT运行依赖')],
            currentContract=contract, targetContract=contract, acceptance=acceptance,
            prerequisites=['M1', 'M2', 'M3'], migrationPrerequisites=['M1', 'M2', 'M3'],
            relatedOperators=[], notes='M4b新增公共能力；不注册旧ID别名，不声称M4c场景/Bundle/导入图迁移已经完成。'))
    ordered = sorted(result['operators'], key=lambda row: row['migrationOrder'])
    split = next((index for index, row in enumerate(ordered) if row['wave'] > 'M4'), len(ordered))
    ordered[split:split] = additions
    for index, row in enumerate(ordered + result['pythonExamples'], 1):
        row['migrationOrder'] = index
    result['operators'] = ordered


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
    if stage == 'M4a' and product_version != '1.4.3':
        raise ValueError('M4a公共SDK使用产品版本1.4.3，不能复用旧版本声明')
    if stage == 'M4b' and product_version != '1.4.4':
        raise ValueError('M4b公共节点使用产品版本1.4.4，不能复用旧缓存版本声明')
    if stage == 'M3':
        retire_m3_rows(result)
    if stage == 'M4b':
        introduce_m4b_rows(result)
    native_expected = set(result['baselineReference']['registeredNativeIds']) | set(ADDITIONS)
    if stage in {'M3', 'M4a', 'M4b'}:
        native_expected = (native_expected - set(M3_RETIREMENTS)) | set(M3_DEFINITIONS)
    if stage == 'M4b':
        native_expected |= set(M4B_DEFINITIONS)
    for field, required in [('operators', native_expected),
                            ('pythonExamples', set(result['baselineReference']['pythonExampleIds']) | {EXAMPLE_ID})]:
        identifiers = [row['id'] for row in result[field]]
        allowed = required if stage in {'M2', 'M3', 'M4a', 'M4b'} else required | set(ADDITIONS)
        if len(identifiers) != len(set(identifiers)) or not set(identifiers) <= allowed:
            raise ValueError(f'{field}: 重复或未经定义的ID，拒绝刷新')
        original = set(result['baselineReference']['registeredNativeIds'] if field == 'operators' else
                       result['baselineReference']['pythonExampleIds'])
        if stage == 'M1' and not original <= set(identifiers):
            raise ValueError(f'{field}: 原基线ID丢失，拒绝刷新')
        if stage in {'M2', 'M3', 'M4a', 'M4b'} and set(identifiers) != required:
            raise ValueError(f'{field}: 现有清单丢失已审ID，拒绝刷新')
    manifest = load(manifest_path)
    operators = manifest if isinstance(manifest, list) else manifest['operators']
    exported = {op['id']: op for op in operators}
    expected = native_expected
    if len(exported) != len(operators) or set(exported) != expected:
        raise ValueError('需要真实全包manifest且ID全集精确匹配已审新增/迁移/退役清单；不得用轻量构建或占位描述刷新')
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
        if stage == 'M4b' and row['id'] not in M4B_DEFINITIONS:
            if (op.get('pack', 'core').split('@')[0] != row['currentPack'] or
                    op.get('params', []) != row['currentParams'] or
                    {key: op.get(key, []) for key in ('inputs', 'outputs')} != row['currentPorts'] or
                    op.get('version') != M4B_CHANGED_VERSIONS.get(row['id'], row['currentVersion'])):
                raise ValueError(f"{row['id']}: M4b仅允许已审旧Gap节点patch，不能隐式改包、端口或参数")
            if row['id'] in M4B_CHANGED_VERSIONS and row['currentVersion'] != op['version']:
                row.setdefault('contractHistory', []).append(dict(stage='M4b',
                    version=row['currentVersion'], pack=row['currentPackVersion'],
                    ports=copy.deepcopy(row['currentPorts']), params=copy.deepcopy(row['currentParams']),
                    sourceRevision=M4A_MERGED_REVISION))
        if stage == 'M4a' and (op.get('pack', 'core').split('@')[0] != row['currentPack'] or
                op.get('version') != row['currentVersion'] or op.get('params', []) != row['currentParams'] or
                {key: op.get(key, []) for key in ('inputs', 'outputs')} != row['currentPorts']):
            raise ValueError(f"{row['id']}: M4a公共核提取不得改变注册包、版本、端口或参数；节点迁移应在后续切片显式声明")
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
    if stage == 'M3':
        for row in ordered:
            if row['id'] in M3_MOVES or row['id'] in M3_DEFINITIONS:
                assessment = dict(stage='M3', status='manifest_verified',
                    scope='原生Profile公共链、DTS场景v2与显式旧图迁移；旧源输入壳保留至M7以支持既有宿主注入。',
                    publicInterfaces=['kernels/include/lyflow/kernel/profile.h',
                                      'kernels/include/lyflow/kernel/profile_analysis.h',
                                      'kernels/include/lyflow/kernel/metrology.h',
                                      'packs/std-profile/include/lyflow_profile/context.h'],
                    dependencyBoundary='仅标准库和编译期Eigen；无PCL/OpenCV/ORT；场景DTS依赖公开轮廓/度量核及原生Profile契约。',
                    acceptance='真实全包manifest描述核验；完整数值、SDK、CLI与图升级回归另由运行证据证明。')
                rows = row.setdefault('stageAssessments', [])
                rows[:] = [item for item in rows if item.get('stage') != 'M3'] + [assessment]
                if row['id'].startswith('dts.') and row['id'] != 'dts.profile_in':
                    updates = row.setdefault('contractUpdates', [])
                    updates[:] = [item for item in updates if item.get('stage') != 'M3'] + [dict(
                        stage='M3', version=row['currentVersion'],
                        contract='场景v2使用显式Profile的XZ/acquisition/frame/hand/source上下文；工作数值mm但裸几何真实m；Record与实际几何hash绑定。旧v1不自动猜单位，显式dtsProfile.v1整图升级保留资产。',
                        acceptance='既有默认/相邻基准和失败行为不变；签名、源索引、单位及桥v1/v2避免重复缩放；独立宿主数值一致。')]
    if stage == 'M4b':
        for row in ordered:
            if row['id'] in M4B_DEFINITIONS or row['id'] in M4B_CHANGED_VERSIONS:
                assessments = row.setdefault('stageAssessments', [])
                assessments[:] = [entry for entry in assessments if entry.get('stage') != 'M4b'] + [dict(
                    stage='M4b', status='manifest_verified',
                    scope='公共Profile/几何/度量与旧Gap薄适配；场景/Bundle和导入图迁移仍在M4c。',
                    publicInterfaces=['kernels/include/lyflow/kernel/profile_features.h',
                                      'kernels/include/lyflow/kernel/profile_dimensions.h',
                                      'packs/std-profile/include/lyflow_profile/context.h'],
                    dependencyBoundary='稳健拟合显式选择PCL；选择/几何/度量保持无PCL/CV/ORT；不存在执行另一包节点伪装C++依赖。',
                    acceptance='当前全包manifest精确核验；42个已有定义直线、281个已有定义量测逐位对照和12非法输入行为变化另由实际运行记录证明，不宣称全部输入兼容。')]
            if row['id'] in M4B_CHANGED_VERSIONS:
                updates = row.setdefault('contractUpdates', [])
                updates[:] = [entry for entry in updates if entry.get('stage') != 'M4b'] + [dict(
                    stage='M4b', version=row['currentVersion'],
                    contract='旧端口/参数保留并复用公共核；NaN无效样本不参与直线选择，像素/非有限/重合圆心/溢出等非法量测明确拒绝；patch版本隔离旧缓存，不以包版本代替算子版本。',
                    acceptance='42个已有定义直线与281个已有定义量测定义逐位一致；12非法输入预期变化单独登记，仍需完整CI/SDK与场景后续验收。')]
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
    historical_packs = {item['currentPack'] for item in data.get('retiredOperators', [])}
    historical_packs.update(item['fromPack'] for row in rows for item in row.get('packageHistory', []))
    for pack in historical_packs - set(documents):
        documents[pack] = ('# ' + pack + ' 历史归属\n\n本目录的活动计算已按显式迁移分配到新包。'
            '参见[当前清单](../operator-package-audit.md)中的逐项入口与退役记录；'
            '原契约、来源修订与迁移去向保留在[机器清单](../operator-package-audit.json)。\n')
    return documents


def render_overview(data):
    baseline = data['baselineReference']
    current_stage = next(s for s in reversed(data['stageLedger']) if s['stage'] in {'M1', 'M2', 'M3', 'M4a', 'M4b'})
    manifest_example = current_stage.get('manifestEvidence', {}).get(
        'path', f"build/{current_stage['stage'].lower()}-full/manifest.json")
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
        evidence = ('全包manifest SHA256 `' + proof['sha256'] + '`；' +
                    ('阶段完成另由合并、CI及运行记录证明' if stage['status'] == 'complete' else
                     '描述匹配，不代表阶段全部验收已完成')) if proof else stage['note']
        if stage.get('mergeEvidence'):
            evidence += f"；[PR{stage['mergeEvidence']['pr']}]({stage['mergeEvidence']['url']})已合并 `{stage['mergeEvidence']['revision']}`"
        lines.append(f"| {stage['stage']} | {stage['status']} | {stage.get('expectedNativeCount', '待确定')} / {stage.get('expectedPythonExampleCount', '待确定')} | {evidence} |")
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
    if data.get('retiredOperators'):
        lines += ['', '## 已退役计算ID', '', '| 原ID | 显式替代 | 历史源码 |', '| --- | --- | --- |']
        for row in data['retiredOperators']:
            original = row['source'].rsplit(':', 1)[0]
            url = f"https://github.com/liangyuR/lyFlow/blob/{row['sourceRevision']}/{original}"
            replacements = ', '.join('`' + value + '`' for value in row['replacedBy'])
            lines.append(f"| `{row['id']}` | {replacements}；通过显式图升级 | [原契约及实现]({url}) |")
        lines += ['', '完整原行、端口/参数/约束及源码修订保留在JSON的retiredOperators；退役ID不计入当前注册全集，也不保留算法别名。', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description='复核后刷新当前manifest元数据；保留逐项迁移语义和原251/7 Git基线。')
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--list-pending-sources', action='store_true')
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--reviewed-path', action='append', default=[])
    parser.add_argument('--reviewed-change', default='')
    parser.add_argument('--product-version')
    parser.add_argument('--stage', choices=['M1', 'M2', 'M3', 'M4a', 'M4b'], default='M2')
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
    AUDIT.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    for path, content in documents.items():
        path.write_text(content, encoding='utf-8', newline='\n')
    print(f"Audit {'prepared; manifest pending' if args.prepare else 'refreshed from real manifest'}: {data['nativeCount']} native, {data['pythonExampleCount']} Python")


if __name__ == '__main__':
    main()
