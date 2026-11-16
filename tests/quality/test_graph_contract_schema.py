import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'core/tests/fixtures/contract-upgrade'
SPEC = importlib.util.spec_from_file_location('validate_schema', ROOT / 'scripts/validate_schema.py')
VALIDATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATE)


class GapGraphContractSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = ROOT / 'schema/graph-doc.schema.json'
        cls.validator = VALIDATE.make_validator(json.loads(path.read_text(encoding='utf-8')), path)
        cls.graphs = json.loads((FIXTURES / 'gap-standard-v1.json').read_text(encoding='utf-8'))

    def graph(self, name='template'):
        graph = copy.deepcopy(self.graphs[name])
        fine = 'fine' in name
        declaration = dict(sourceId='file-channel-A', unit='m', fileFrame='file-frame',
                           sensorFrame='sensor-frame', fileHandedness='right')
        source = dict(node='n_load', op='gap.load_profile_pair', opVersion='1.0.0', port='primary') if fine else dict(
            node='n_scan', op='gap.read_scan', opVersion='1.1.0', port='scan')
        graph['contractUpgrade'] = dict(schemaVersion=1, rules=[dict(
            kind='gapProfile.v1', source=source,
            profiles=dict(primary=declaration, secondary=dict(declaration, sourceId='file-channel-B')),
            measurement=dict(frame='measurement-frame', plane='XY'), alreadyAligned=True,
            component=dict(style='fine' if fine else 'blocks', nodes=[node['id'] for node in graph['nodes']]),
            adapterIdPrefix='native_gap')])
        return graph

    def assert_valid(self, graph):
        errors = list(self.validator.iter_errors(graph))
        self.assertFalse(errors, '\n'.join(f'{list(e.absolute_path)}: {e.message}' for e in errors))

    def test_actual_core_block_and_fine_fixtures_accept_exact_requests_and_optional_templates(self):
        for name in self.graphs:
            with self.subTest(name=name):
                graph = self.graph(name)
                self.assert_valid(graph)
                graph['contractUpgrade']['rules'][0]['templates'] = dict(
                    unit='m', fileFrame='template-files', templateFrame='template-frame', handedness='reflected')
                self.assert_valid(graph)

    def test_shared_definition_scope_and_passive_history_roundtrip(self):
        graph = self.graph()
        request = copy.deepcopy(graph.pop('contractUpgrade'))
        rule = request['rules'][0]
        rule['scope'] = dict(subgraph='shared_gap')
        definition = {key: graph[key] for key in ['nodes', 'edges']}
        definition.update(inputs=[], outputs=[], params=[])
        scoped = dict(schemaVersion=1, id='scoped-gap', nodes=[], edges=[],
                      subgraphs=dict(shared_gap=definition), contractUpgrade=request)
        self.assert_valid(scoped)
        saved = json.loads(json.dumps(scoped))
        self.assertEqual(saved['contractUpgrade'], request)
        history = dict(schemaVersion=1, request=saved.pop('contractUpgrade'), appliedSources=[rule['source']])
        saved['meta'] = dict(contractUpgradeHistory=[history])
        self.assert_valid(saved)
        self.assertEqual(json.loads(json.dumps(saved))['meta']['contractUpgradeHistory'][0]['request'], request)

    def test_units_sources_required_fields_unknown_fields_and_budgets_are_strict(self):
        base = self.graph()
        changes = [
            (('profiles', 'primary', 'unit'), 'mm'),
            (('profiles', 'secondary', 'unit'), 'px'),
            (('profiles', 'primary', 'fileHandedness'), 'left'),
            (('profiles', 'primary', 'sensorFrame'), ''),
            (('profiles', 'primary', 'sourceId'), 'bad\0source'),
            (('measurement', 'plane'), 'XZ'),
            (('source', 'opVersion'), '1.0.0'),
            (('source', 'port'), 'primary'),
            (('source', 'op'), 'gap.fit_line'),
            (('source', 'node'), 'instance/n_scan'),
            (('alreadyAligned',), False),
            (('component', 'nodes'), []),
            (('component', 'nodes'), ['n_scan', 'n_scan']),
            (('component', 'nodes'), [f'n{i}' for i in range(4097)]),
            (('component', 'style'), 'automatic'),
            (('adapterIdPrefix',), 'has.dot'),
            (('adapterIdPrefix',), 'x' * 129),
            (('templates',), dict(unit='mm', fileFrame='file', templateFrame='template', handedness='right')),
        ]
        for path, value in changes:
            with self.subTest(path=path, value=str(value)[:80]):
                graph = copy.deepcopy(base)
                item = graph['contractUpgrade']['rules'][0]
                for key in path[:-1]:
                    item = item[key]
                item[path[-1]] = value
                self.assertFalse(self.validator.is_valid(graph))
        rule = base['contractUpgrade']['rules'][0]
        for path in [(), ('source',), ('profiles',), ('profiles', 'primary'), ('measurement',), ('component',)]:
            graph = copy.deepcopy(base)
            item = graph['contractUpgrade']['rules'][0]
            for key in path:
                item = item[key]
            item['unexpected'] = True
            self.assertFalse(self.validator.is_valid(graph), path)
        for key in rule:
            graph = copy.deepcopy(base)
            del graph['contractUpgrade']['rules'][0][key]
            self.assertFalse(self.validator.is_valid(graph), key)


if __name__ == '__main__':
    unittest.main()
