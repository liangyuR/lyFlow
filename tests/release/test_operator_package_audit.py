import collections
import copy
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('package_audit', ROOT / 'scripts/check-operator-package-audit.py')
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)
REFRESH_SPEC = importlib.util.spec_from_file_location('audit_refresh', ROOT / 'scripts/refresh-operator-package-audit.py')
REFRESH = importlib.util.module_from_spec(REFRESH_SPEC)
REFRESH_SPEC.loader.exec_module(REFRESH)


class PackageAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.baseline = json.loads((ROOT / 'docs/operator-package-audit.json').read_text(encoding='utf-8'))

    def test_baseline(self):
        self.assertEqual(CHECKER.validate(self.baseline, []), [])

    def test_missing_operator_with_synchronized_counts_is_rejected(self):
        data = copy.deepcopy(self.baseline)
        data['operators'] = [r for r in data['operators'] if r['id'] != 'gap.measure_reference']
        data['nativeCount'] = len(data['operators'])
        data['currentPackCounts'] = dict(collections.Counter(r['currentPack'] for r in data['operators']))
        data['targetPackCounts'] = dict(collections.Counter(r['targetPack'] for r in data['operators']))
        for index, row in enumerate(sorted(data['operators'] + data['pythonExamples'], key=lambda r: r['migrationOrder']), 1):
            row['migrationOrder'] = index
        self.assertTrue(any('注册ID快照' in p for p in CHECKER.validate(data, [])))

    def test_unrelated_test_change_is_allowed(self):
        data = copy.deepcopy(self.baseline)
        data['sourceFingerprints']['core/tests/test_cache.cpp'] = '0' * 64
        self.assertEqual(CHECKER.validate(data, []), [])

    def test_contract_change_requires_review(self):
        data = copy.deepcopy(self.baseline)
        data['sourceFingerprints']['solutions/dts/ops/flush.cpp'] = '0' * 64
        self.assertTrue(any('契约源码已变化' in p for p in CHECKER.validate(data, [])))
        documents = {ROOT / 'docs/operator-package-audit' / f'{pack}.md': content
                     for pack, content in REFRESH.render_details(data).items()}
        self.assertTrue(any('契约源码已变化' in p for p in CHECKER.validate(data, [], documents)))

    def test_new_package_without_existing_file_edits_is_rejected(self):
        new_file = ROOT / 'packs/new-pack/lyflow_op_pack.cmake'
        original_glob = Path.glob
        original_is_file = Path.is_file

        def glob(path, pattern):
            values = list(original_glob(path, pattern))
            if path == ROOT and pattern == 'packs/*/lyflow_op_pack.cmake':
                values.append(new_file)
            return iter(values)

        def is_file(path):
            return path == new_file or original_is_file(path)

        with patch.object(Path, 'glob', glob), patch.object(Path, 'is_file', is_file):
            problems = CHECKER.validate(self.baseline, [])
        self.assertTrue(any('契约来源文件集合变化' in p for p in problems))

    def test_refresh_keeps_original_semantic_constraints_and_renders_every_row(self):
        before = copy.deepcopy(self.baseline)
        rendered = REFRESH.render_details(self.baseline)
        self.assertEqual(self.baseline, before)
        for row in before['operators'] + before['pythonExamples']:
            content = rendered[row['currentPack']]
            self.assertIn(' · ' + row['id'] + '\n', content)
            for key in ('currentContract', 'targetContract', 'acceptance', 'notes'):
                self.assertIn(row[key], content)
            for dependency in row['dependencies']:
                self.assertIn(dependency, content)
        self.assertEqual(rendered, REFRESH.render_details(self.baseline))
        prepared = REFRESH.prepare(self.baseline, 'M2')
        original = {row['id']: row for row in before['operators'] + before['pythonExamples']}
        for row in prepared['operators'] + prepared['pythonExamples']:
            for key in ('targetPack', 'action', 'wave', 'dependencies', 'currentContract', 'targetContract',
                        'migrationPrerequisites', 'acceptance', 'notes'):
                self.assertEqual(row.get(key), original[row['id']].get(key), (row['id'], key))
        m1 = next(stage for stage in prepared['stageLedger'] if stage['stage'] == 'M1')
        self.assertEqual(m1['status'], 'complete')
        self.assertEqual(m1['mergeEvidence']['pr'], 62)
        self.assertEqual(m1['mergeEvidence']['revision'], REFRESH.M1_MERGED_REVISION)
        self.assertEqual(m1['manifestEvidence'], next(stage for stage in before['stageLedger'] if stage['stage'] == 'M1')['manifestEvidence'])
        scoped = [row for row in prepared['operators'] if row['wave'] == 'M2']
        self.assertEqual(len(scoped), 38)
        self.assertTrue(all(any(item['stage'] == 'M2' for item in row['stageAssessments']) for row in scoped))
        self.assertFalse(any(any(item['stage'] == 'M2' for item in row.get('stageAssessments', []))
                             for row in prepared['operators'] if row['wave'] == 'M3'))

    def test_m1_additions_are_explicit_semantic_definitions_not_manifest_placeholders(self):
        self.assertEqual(set(REFRESH.ADDITIONS), {'profile.from_cloud', 'profile.remap_axes', 'profile.to_cloud',
            'image.upgrade_plane_calib', 'dts.geometry_to_metric'})
        for row in REFRESH.ADDITIONS.values():
            self.assertTrue(all(row[key] for key in ('currentContract', 'targetContract', 'acceptance')))
            self.assertNotIn('currentPorts', row)
            self.assertNotIn('currentParams', row)
        declaration = REFRESH.example_declarations()[REFRESH.EXAMPLE_ID][2]
        self.assertEqual(declaration['inputs'], {'profile': 'Profile'})
        self.assertEqual(declaration['outputs'], {'profile': 'Profile'})
        self.assertEqual(REFRESH.manifest_generator({'generatedBy': 'lyflow-core/1.4.0'}), 'lyflow-core/1.4.0')
        with self.assertRaisesRegex(ValueError, 'generatedBy'):
            REFRESH.manifest_generator({'coreVersion': '1.4.0'})
        self.assertEqual(set(REFRESH.M2_MOVES), {'transform.make', 'register.constraints_3d', 'image.calibrated_distance'})
        by_id = {row['id']: row for row in self.baseline['operators']}
        for identifier, move in REFRESH.M2_MOVES.items():
            with self.subTest(move=identifier):
                row = copy.deepcopy(by_id[identifier])
                row['currentPack'] = move['fromPack']
                row.pop('packageHistory', None)
                original = copy.deepcopy(row)
                op = dict(id=identifier, pack=move['toPack'] + '@0.1.0', version=row['currentVersion'],
                          inputs=row['currentPorts']['inputs'], outputs=row['currentPorts']['outputs'], params=row['currentParams'])
                REFRESH.relocate_operator(row, op, 'M2')
                self.assertEqual(row['currentPack'], move['toPack'])
                self.assertEqual(row['packageHistory'][0]['fromPack'], move['fromPack'])
                for key in ('targetPack', 'dependencies', 'currentContract', 'targetContract', 'acceptance', 'notes'):
                    self.assertEqual(row[key], original[key])
                moved = copy.deepcopy(row)
                REFRESH.relocate_operator(row, op, 'M2')
                self.assertEqual(row, moved)
                with self.assertRaisesRegex(ValueError, '保持原版本'):
                    REFRESH.relocate_operator(row, {**op, 'version': '99.0.0'}, 'M2')
                self.assertEqual(row, moved)
        row = copy.deepcopy(by_id['flow.select'])
        original = copy.deepcopy(row)
        with self.assertRaisesRegex(ValueError, '先人工审计'):
            REFRESH.relocate_operator(row, {'pack': 'std-metrology@0.1.0'}, 'M2')
        self.assertEqual(row, original)

    def test_baseline_reference_and_stage_proof_cannot_be_silently_dropped(self):
        data = copy.deepcopy(self.baseline)
        data['baselineReference']['registeredNativeIds'].pop()
        self.assertTrue(any('251/7基线' in p for p in CHECKER.validate(data, [])))
        data = copy.deepcopy(self.baseline)
        stage = next(s for s in data['stageLedger'] if s['stage'] == 'M1')
        stage['status'] = 'validated_manifest'
        stage.pop('manifestEvidence', None)
        self.assertTrue(any('真实manifest' in p for p in CHECKER.validate(data, [])))

    def test_refresh_rejects_partial_manifest_before_touching_audit(self):
        before = copy.deepcopy(self.baseline)
        with patch.object(REFRESH, 'load', return_value={'operators': [{'id': 'flow.select'}]}):
            with self.assertRaisesRegex(ValueError, '真实全包manifest'):
                REFRESH.refreshed(self.baseline, Path('not-an-evidence-file.json'), [], 'unit-test', '1.4.2', 'M3')
        self.assertEqual(self.baseline, before)
        for field in ('operators', 'pythonExamples'):
            with self.subTest(field=field):
                damaged = copy.deepcopy(self.baseline)
                damaged[field].pop(0)
                with patch.object(REFRESH, 'load', side_effect=AssertionError('不应先读取manifest')):
                    with self.assertRaisesRegex(ValueError, '丢失已审ID'):
                        REFRESH.refreshed(damaged, Path('not-an-evidence-file.json'), [], 'unit-test', '1.4.2', 'M3')
        for identifier in REFRESH.ADDITIONS:
            damaged = copy.deepcopy(self.baseline)
            damaged['operators'] = [row for row in damaged['operators'] if row['id'] != identifier]
            with patch.object(REFRESH, 'load', side_effect=AssertionError('不应先读取manifest')):
                with self.assertRaisesRegex(ValueError, '丢失已审ID'):
                    REFRESH.refreshed(damaged, Path('not-an-evidence-file.json'), [], 'unit-test', '1.4.2', 'M3')

    def test_m3_retirement_is_explicit_reversible_to_history_and_not_an_alias(self):
        before = copy.deepcopy(self.baseline)
        prepared = REFRESH.prepare(before, 'M3')
        self.assertEqual(before, self.baseline)
        ledger = next(item for item in prepared['stageLedger'] if item['stage'] == 'M3')
        self.assertEqual(ledger['expectedNativeCount'], 261)
        self.assertEqual(len(ledger['scopeNativeIds']), 11)
        self.assertEqual(set(ledger['retiredNativeIds']), set(REFRESH.M3_RETIREMENTS))
        REFRESH.retire_m3_rows(prepared)
        active = {row['id']: row for row in prepared['operators']}
        retired = {row['id']: row for row in prepared['retiredOperators']}
        self.assertFalse(set(REFRESH.M3_RETIREMENTS) & set(active))
        self.assertEqual(len(active), 261)
        self.assertEqual(set(REFRESH.M3_DEFINITIONS), set(ledger['introducedNativeIds']))
        for identifier, replacement in REFRESH.M3_RETIREMENTS.items():
            original = next((row for row in before['operators'] if row['id'] == identifier), None)
            self.assertEqual(retired[identifier]['replacedBy'], [replacement])
            self.assertIn(replacement, active)
            if original is not None:
                for key, value in original.items():
                    self.assertEqual(retired[identifier][key], value, (identifier, key))
        snapshot = copy.deepcopy(prepared)
        REFRESH.retire_m3_rows(prepared)
        self.assertEqual(prepared, snapshot)
        self.assertEqual(next(item for item in prepared['stageLedger'] if item['stage'] == 'M2')['mergeEvidence']['pr'], 63)

    def test_retirement_cannot_hide_a_deleted_baseline_operator(self):
        prepared = REFRESH.prepare(self.baseline, 'M3')
        REFRESH.retire_m3_rows(prepared)
        prepared['retiredOperators'][0]['replacedBy'] = ['missing.unreviewed']
        self.assertTrue(any('退役' in problem for problem in CHECKER.validate(prepared, [])))
        prepared['retiredOperators'].append(copy.deepcopy(prepared['retiredOperators'][0]))
        self.assertTrue(any('退役ID重复' in problem for problem in CHECKER.validate(prepared, [])))

    def test_scene_contract_sources_are_tracked_without_heavy_backends(self):
        patterns = CHECKER.CONTRACT_SOURCES
        self.assertIn('solutions/*/ops/*.cpp', patterns)
        self.assertIn('solutions/*/algo/*.cpp', patterns)
        self.assertIn('kernels/include/**/*.h', patterns)
        self.assertIn('packs/*/include/**/*.h', patterns)

    def test_m4a_requires_merged_m3_and_does_not_complete_the_whole_wave(self):
        before = copy.deepcopy(self.baseline)
        before['stageLedger'] = [entry for entry in before['stageLedger'] if entry['stage'] != 'M4a']
        prepared = REFRESH.prepare(before, 'M4a')
        m4a = next(entry for entry in prepared['stageLedger'] if entry['stage'] == 'M4a')
        self.assertEqual(m4a['expectedNativeCount'], 261)
        self.assertEqual(m4a['introducedNativeIds'], [])
        self.assertEqual(m4a['introducedPythonIds'], [])
        self.assertEqual(m4a['retiredNativeIds'], [])
        self.assertEqual(m4a['status'], 'pending_manifest')
        self.assertEqual(next(entry for entry in prepared['stageLedger'] if entry['stage'] == 'M4')['status'], 'planned')
        self.assertIn('| M4 | planned | 待确定 / 待确定 |', REFRESH.render_overview(prepared))
        for mutation in ('status', 'merge'):
            damaged = copy.deepcopy(self.baseline)
            m3 = next(entry for entry in damaged['stageLedger'] if entry['stage'] == 'M3')
            if mutation == 'status':
                m3['status'] = 'validated_manifest'
            else:
                m3['mergeEvidence']['revision'] = '0' * 40
            with self.assertRaisesRegex(ValueError, 'PR64合并证据'):
                REFRESH.prepare(damaged, 'M4a')
            self.assertTrue(any('M4a缺少已合并M3前置证据' in problem for problem in CHECKER.validate(damaged, [])))
        m4a['introducedPythonIds'] = ['py.unreviewed.node']
        self.assertTrue(any('M4a只能抽公共核' in problem for problem in CHECKER.validate(prepared, [])))

    def test_m4a_refuses_hidden_operator_contract_changes(self):
        first = min(self.baseline['operators'], key=lambda row: row['migrationOrder'])['id']
        operators = [dict(id=row['id'], version=row['currentVersion'], pack=row['currentPackVersion'],
                          params=row['currentParams'], **row['currentPorts'])
                     for row in self.baseline['operators']]
        for field in ('version', 'pack', 'params', 'inputs', 'outputs'):
            with self.subTest(field=field):
                manifest = dict(generatedBy='lyflow-core/1.4.3', operators=copy.deepcopy(operators))
                op = next(row for row in manifest['operators'] if row['id'] == first)
                if field in ('version', 'pack'):
                    op[field] = '999.0.0' if field == 'version' else 'unexpected-package@1.0.0'
                else:
                    op[field] = op[field] + [dict(name='unexpected', type='PointCloud')]
                with patch.object(REFRESH, 'load', return_value=manifest), patch.object(REFRESH, 'pending_sources', return_value=[]):
                    with self.assertRaisesRegex(ValueError, '不得改变注册包、版本、端口或参数'):
                        REFRESH.refreshed(self.baseline, Path('unit-fixture.json'), [], 'contract guard', '1.4.3', 'M4a')


if __name__ == '__main__':
    unittest.main(verbosity=2)
