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
        data['sourceFingerprints']['packs/dts/ops/flush.cpp'] = '0' * 64
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
                REFRESH.refreshed(self.baseline, Path('not-an-evidence-file.json'), [], 'unit-test', '1.4.0')
        self.assertEqual(self.baseline, before)
        for field in ('operators', 'pythonExamples'):
            with self.subTest(field=field):
                damaged = copy.deepcopy(self.baseline)
                damaged[field].pop(0)
                with patch.object(REFRESH, 'load', side_effect=AssertionError('不应先读取manifest')):
                    with self.assertRaisesRegex(ValueError, '丢失原基线ID'):
                        REFRESH.refreshed(damaged, Path('not-an-evidence-file.json'), [], 'unit-test', '1.4.0')


if __name__ == '__main__':
    unittest.main(verbosity=2)
