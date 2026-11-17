import copy
import importlib.util
import json
from pathlib import Path
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

    def problems(self, mutate=None, exported=None, subset=False, examples=None):
        data = copy.deepcopy(self.ledger)
        if mutate:
            mutate(data)
        return CHECK.validate(data, self.examples if examples is None else examples, exported, subset)

    def test_repository_ledger_matches_its_own_full_manifest(self):
        self.assertEqual(self.problems(exported=self.manifest), [])

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
        }
        for name, mutate in cases.items():
            with self.subTest(name):
                self.assertTrue(self.problems(mutate))

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


if __name__ == '__main__':
    unittest.main()
