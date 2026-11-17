import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('validate_schema', ROOT / 'scripts/validate_schema.py')
VALIDATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATE)


class LegacyContractUpgradeSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = ROOT / 'schema/graph-doc.schema.json'
        cls.schema = json.loads(path.read_text(encoding='utf-8'))
        cls.validator = VALIDATE.make_validator(cls.schema, path)

    def base(self):
        return dict(schemaVersion=1, id='legacy', nodes=[dict(id='n', op='t.src')], edges=[])

    def assert_valid(self, graph):
        errors = list(self.validator.iter_errors(graph))
        self.assertFalse(errors, '\n'.join(f'{list(e.absolute_path)}: {e.message}' for e in errors))

    def test_schema_is_valid_and_has_no_upgrade_definitions(self):
        type(self.validator).check_schema(self.schema)
        self.assertFalse({'contractUpgrade', 'contractUpgradeSource', 'gapUpgradeText', 'gapUpgradeProfile'}
                         & set(self.schema['$defs']))
        self.assertNotIn('contractUpgradeHistory', self.schema['properties']['meta']['properties'])

    def test_legacy_request_of_any_shape_still_validates_and_reaches_core(self):
        requests = [
            dict(schemaVersion=1, rules=[dict(kind='gapProfile.v1', unexpected=True)]),
            dict(schemaVersion=2, rules=[]),
            [],
            'legacy',
        ]
        for request in requests:
            with self.subTest(request=request):
                graph = self.base()
                graph['contractUpgrade'] = copy.deepcopy(request)
                self.assert_valid(graph)

    def test_history_is_plain_meta(self):
        graph = self.base()
        graph['meta'] = dict(contractUpgradeHistory=[dict(schemaVersion=1, future=True)], owner='keep')
        self.assert_valid(graph)
        graph['unknownTopLevel'] = True
        self.assertFalse(self.validator.is_valid(graph))


if __name__ == '__main__':
    unittest.main()
