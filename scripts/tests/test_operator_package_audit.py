import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('check_audit', ROOT / 'scripts/check-operator-package-audit.py')
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class OperatorPackageLedger(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ledger = json.loads(CHECK.LEDGER.read_text(encoding='utf-8'))
        cls.examples = CHECK.python_example_ids()
        cls.manifest = {row['id']: row['currentPack'] for row in cls.ledger['operators']}

    def problems(self, mutate=None, exported=None, subset=False, examples=None, complete_packs=()):
        data = copy.deepcopy(self.ledger)
        if mutate:
            mutate(data)
        return CHECK.validate(data, self.examples if examples is None else examples, exported, subset,
                              complete_packs)

    def test_rejects_malformed_rows(self):
        cases = {
            'extra field': lambda d: d['operators'][0].update(notes='x'),
            'bad action': lambda d: d['operators'][0].update(action='rename'),
            'bad wave': lambda d: d['operators'][0].update(wave='M9'),
            'unknown target': lambda d: d['operators'][0].update(targetPack='nowhere'),
            'unsorted': lambda d: d['operators'].reverse(),
            'duplicate': lambda d: d['operators'].insert(1, copy.deepcopy(d['operators'][0])),
            'retired still active': lambda d: d['retiredOperators'].append({'id': d['operators'][0]['id'], 'replacedBy': [d['operators'][1]['id']]}),
            'unknown replacement': lambda d: d['retiredOperators'][0].update(replacedBy=['missing.op']),
            'wrong replacement field': lambda d: d['retiredOperators'][0].update(replacedBy=''),
        }
        for name, mutate in cases.items():
            with self.subTest(name):
                self.assertTrue(self.problems(mutate))
        self.assertEqual(self.problems(lambda d: d['retiredOperators'][0].update(replacedBy=[])), [])

    def test_python_examples_follow_node_declarations(self):
        self.assertTrue(self.problems(examples=self.examples + ['py.example.extra']))

    def test_manifest_full_and_subset(self):
        first = self.ledger['operators'][0]['id']
        unregistered = dict(self.manifest, **{'new.op': 'core'})
        moved = dict(self.manifest, **{first: 'elsewhere'})
        partial = {first: self.manifest[first]}
        self.assertTrue(self.problems(exported=unregistered))
        self.assertTrue(self.problems(exported=moved))
        self.assertTrue(self.problems(exported=partial))
        self.assertEqual(self.problems(exported=partial, subset=True), [])
        self.assertTrue(self.problems(exported=dict(partial, **{'new.op': 'core'}), subset=True))

        complete_pack = 'std-pointcloud'
        pointcloud = {identifier: pack for identifier, pack in self.manifest.items()
                      if pack == complete_pack}
        other_id, other_pack = next((identifier, pack) for identifier, pack in self.manifest.items()
                                    if pack != complete_pack)
        partial_without_pointcloud = {other_id: other_pack}
        partial_with_pointcloud = {**partial_without_pointcloud, **pointcloud}
        self.assertEqual(self.problems(exported=partial_with_pointcloud, subset=True,
                                       complete_packs=[complete_pack]), [])
        removed = next(iter(pointcloud))
        del partial_with_pointcloud[removed]
        self.assertIn(f'清单中登记但未注册：{removed}',
                      self.problems(exported=partial_with_pointcloud, subset=True,
                                    complete_packs=[complete_pack]))
        self.assertTrue(self.problems(exported=partial_without_pointcloud, subset=True,
                                      complete_packs=[complete_pack]))
        self.assertIn('清单中没有指定的完整包：missing-pack',
                      self.problems(exported=partial, subset=True, complete_packs=['missing-pack']))

    def test_conflicting_manifests_are_rejected(self):
        first = self.ledger['operators'][0]['id']
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for index, pack in enumerate((self.manifest[first], 'elsewhere')):
                path = Path(directory) / f'{index}.json'
                path.write_text(json.dumps([{'id': first, 'pack': pack}]), encoding='utf-8')
                paths.append(path)
            with self.assertRaises(ValueError):
                CHECK.manifest_operators(paths)


if __name__ == '__main__':
    unittest.main()
