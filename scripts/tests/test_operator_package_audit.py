import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('refresh_audit', ROOT / 'scripts/refresh-operator-package-audit.py')
REFRESH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REFRESH)
CHECK = REFRESH.CHECKER


class M4cAuditContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.baseline = REFRESH.load(REFRESH.AUDIT)
        if any(stage['stage'] == 'M4c' for stage in cls.baseline['stageLedger']):
            cls.baseline['stageLedger'] = [stage for stage in cls.baseline['stageLedger'] if stage['stage'] != 'M4c']
            cls.baseline['operators'] = [row for row in cls.baseline['operators'] if row['id'] not in REFRESH.M4C_DEFINITIONS]
            for row in cls.baseline['operators']:
                original = next((item for item in row.get('contractHistory', []) if item.get('stage') == 'M4c'), None)
                if original:
                    row.update({key: copy.deepcopy(value) for key, value in original.items()
                                if key not in {'stage', 'sourceRevision'}})
                row['packageHistory'] = [item for item in row.get('packageHistory', []) if item.get('stage') != 'M4c']
                row['contractHistory'] = [item for item in row.get('contractHistory', []) if item.get('stage') != 'M4c']
            for row in cls.baseline['retiredOperators']:
                if row['retiredStage'] == 'M4c':
                    cls.baseline['operators'].append(copy.deepcopy(row))
                    source = row['source'].rsplit(':', 1)[0]
                    cls.baseline['sourceFingerprints'][source] = row['sourceFingerprint']
                    cls.baseline['contractSourcePaths'].append(source)
            cls.baseline['retiredOperators'] = [row for row in cls.baseline['retiredOperators'] if row['retiredStage'] != 'M4c']
            cls.baseline['relatedAssets'] = copy.deepcopy(cls.baseline['relatedAssetsBaseline'])
            cls.baseline.pop('relatedAssetsBaseline', None)
            for index, row in enumerate(sorted(cls.baseline['operators'] + cls.baseline['pythonExamples'],
                                              key=lambda item: (item['wave'], item['migrationOrder'])), 1):
                row['migrationOrder'] = index
        cls.baseline['sourceFingerprints']['core/src/registry.cpp'] = '0' * 64
        (ROOT / 'build').mkdir(exist_ok=True)
        cls.directory = tempfile.TemporaryDirectory(prefix='audit-unit-only-', dir=ROOT / 'build')
        cls.path = Path(cls.directory.name) / 'UNIT_TEST_ONLY-metadata.json'
        rows = {row['id']: dict(id=row['id'], version=row['currentVersion'],
                    pack=row['currentPackVersion'], label=row['label'], params=row['currentParams'],
                    **row['currentPorts']) for row in cls.baseline['operators']
                if row['id'] not in REFRESH.M4C_RETIREMENTS}
        for identifier, version in REFRESH.M4C_CHANGED_VERSIONS.items():
            rows[identifier]['version'] = version
            if identifier in REFRESH.M4C_SCENE:
                rows[identifier]['pack'] = 'solution-gap@0.1.0'
        for identifier, (pack, _, _, _) in REFRESH.M4C_DEFINITIONS.items():
            rows[identifier] = dict(id=identifier, version='1.0.0', pack=pack + '@0.0.0',
                                    label='UNIT TEST ONLY', params=[], inputs=[], outputs=[])
        assets = cls.baseline.get('relatedAssetsBaseline', cls.baseline['relatedAssets'])
        cls.fixture = dict(generatedBy='lyflow-core/1.4.5', operators=list(rows.values()))
        for key in ('snippets', 'importers'):
            cls.fixture[key] = copy.deepcopy(assets[key])
            for row in cls.fixture[key]:
                if row.get('pack', '').startswith('gap@'):
                    row['pack'] = 'solution-gap@0.1.0'
                for node in row.get('nodes', []):
                    node['op'] = REFRESH.M4C_RETIREMENTS.get(node['op'], [node['op']])[0]
                    node['opVersion'] = rows[node['op']]['version']
        cls.fixture['bundles'] = [row for row in assets['bundles'] if not row.get('pack', '').startswith('gap@')]
        cls.fixture['bundles'] += [dict(kind='gap.' + kind + '.v2', fields=[])
            for kind in ('ScanPair', 'RoiSet', 'LabelRows', 'LineFeature', 'ReferenceFeature',
                         'CirclePair', 'Location', 'TemplatePair', 'Alignment')]
        cls.path.write_text(json.dumps(cls.fixture), encoding='utf-8')
        cls.pending = REFRESH.pending_sources(cls.baseline)
        cls.reviewed = [item['path'] for item in cls.pending]
        cls.candidate = REFRESH.refreshed(cls.baseline, cls.path, cls.reviewed,
            'UNIT TEST ONLY: synthetic metadata; not a runtime manifest or delivery receipt', '1.4.5', 'M4c')
        cls.documents = {ROOT / 'docs/operator-package-audit' / f'{pack}.md': text
                         for pack, text in REFRESH.render_details(cls.candidate).items()}
        cls.documents[ROOT / 'docs/operator-package-audit.md'] = REFRESH.render_overview(cls.candidate)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_prepare_preserves_baseline_and_does_not_complete_wave(self):
        candidate = REFRESH.prepare(self.baseline, 'M4c')
        self.assertEqual(candidate['baselineReference'], self.baseline['baselineReference'])
        self.assertEqual(candidate['operators'], self.baseline['operators'])
        ledger = {stage['stage']: stage for stage in candidate['stageLedger']}
        self.assertEqual(ledger['M4b']['mergeEvidence'], CHECK.M4B_MERGE_PROOF)
        self.assertEqual(ledger['M4c']['status'], 'pending_manifest')
        self.assertNotEqual(ledger.get('M4', {}).get('status'), 'complete')
        bad = copy.deepcopy(self.baseline)
        next(stage for stage in bad['stageLedger'] if stage['stage'] == 'M4b')['manifestEvidence']['sha256'] = ''
        with self.assertRaisesRegex(ValueError, 'PR66'):
            REFRESH.prepare(bad, 'M4c')

    def test_complete_metadata_fixture_keeps_history_and_exact_boundaries(self):
        self.assertEqual(CHECK.validate(self.candidate, [self.path], self.documents), [])
        self.assertEqual(self.candidate['nativeCount'], 277)
        self.assertEqual(self.candidate['pythonExampleCount'], 8)
        self.assertEqual(self.candidate['stageLedger'][-1]['status'], 'validated_manifest')
        self.assertEqual(len(self.candidate['retiredOperators']), 10)
        active = set(self.candidate['registeredNativeIds'])
        self.assertFalse(active & set(REFRESH.M4C_RETIREMENTS))
        originals = {row['id']: row for row in self.baseline['operators']}
        for row in self.candidate['retiredOperators']:
            if row['retiredStage'] != 'M4c':
                continue
            self.assertEqual(row['source'], originals[row['id']]['source'])
            self.assertEqual(row['currentPorts'], originals[row['id']]['currentPorts'])
            self.assertEqual(row['replacedBy'], REFRESH.M4C_RETIREMENTS[row['id']])
            self.assertFalse((ROOT / row['source'].rsplit(':', 1)[0]).exists())
        self.assertEqual(self.candidate['relatedAssetsBaseline'], self.baseline['relatedAssets'])
        self.assertEqual(len(self.candidate['relatedAssets']['bundles']), 13)

    def test_scope_manifest_cannot_be_used_as_full_delivery_evidence(self):
        self.assert_rejected_fixture(dict(self.fixture, operators=self.fixture['operators'][:110]), '真实全包manifest')
        bad = copy.deepcopy(self.fixture)
        bad['operators'].append(bad['operators'][0])
        self.assert_rejected_fixture(bad, '真实全包manifest')
        bad['operators'][-1] = dict(bad['operators'][-1], id='gap.old_alias')
        self.assert_rejected_fixture(bad, '真实全包manifest')

    def assert_rejected_fixture(self, fixture, reason):
        path = Path(self.directory.name) / 'UNIT_TEST_ONLY-invalid.json'
        path.write_text(json.dumps(fixture), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, reason):
            REFRESH.refreshed(self.baseline, path, self.reviewed, 'test', '1.4.5', 'M4c')

    def test_exact_source_review_set_and_product_version_are_mandatory(self):
        self.assertTrue(self.reviewed)
        for paths in (self.reviewed[:-1], self.reviewed + ['not/a/contract.cpp']):
            with self.assertRaisesRegex(ValueError, '精确覆盖'):
                REFRESH.refreshed(self.baseline, self.path, paths, 'test', '1.4.5', 'M4c')
        with self.assertRaisesRegex(ValueError, '1.4.5'):
            REFRESH.refreshed(self.baseline, self.path, self.reviewed, 'test', '1.4.4', 'M4c')
        bad = copy.deepcopy(self.fixture)
        next(row for row in bad['operators'] if row['id'] == 'geometry.box_2d')['version'] = '9.0.0'
        self.assert_rejected_fixture(bad, '未声明')

    def test_recursive_contract_discovery_covers_scene_algorithms_and_snippets(self):
        paths = {p.relative_to(ROOT).as_posix() for pattern in CHECK.CONTRACT_SOURCES
                 for p in ROOT.glob(pattern) if p.is_file()}
        expected = {'core/src/graph_contract_upgrade_gap.h', 'core/src/graph_contract_upgrade_gap_rewrite.h',
            'solutions/gap/algo/gap_detection/GapUtils.cpp',
            'solutions/gap/algo/gap_core/MeasurementTypes.hpp',
            'solutions/gap/snippets/flush-line-end.lyflow-snippet.json',
            'packs/std-pointcloud/src/filters.cpp'}
        self.assertTrue(expected <= paths, expected - paths)
        self.assertFalse(set(self.baseline['contractSourcePaths']) - paths &
                         set(self.candidate['sourceFingerprints']))

    def test_checker_rejects_forged_retirement_assets_and_merge_receipt(self):
        candidate = copy.deepcopy(self.candidate)
        next(row for row in candidate['retiredOperators'] if row['id'] == 'gap.selected_point')['replacedBy'] = ['profile.nearest_sample']
        self.assertTrue(any('退役记录' in problem for problem in CHECK.validate(candidate, [], self.documents)))
        candidate = copy.deepcopy(self.candidate)
        candidate['relatedAssets']['snippets'][0]['nodes'][0]['opVersion'] = '0.0.0'
        self.assertTrue(any('snippets真实描述' in problem for problem in CHECK.validate(candidate, [self.path], self.documents)))
        candidate = copy.deepcopy(self.candidate)
        next(stage for stage in candidate['stageLedger'] if stage['stage'] == 'M4b')['mergeEvidence']['reviewedHead'] = '0' * 40
        self.assertTrue(any('PR66' in problem for problem in CHECK.validate(candidate, [], self.documents)))


if __name__ == '__main__':
    unittest.main()
