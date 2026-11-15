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


if __name__ == '__main__':
    unittest.main(verbosity=2)
