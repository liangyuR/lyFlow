import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_module_graph import cmake_executable


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'core/cmake/release-identity.cmake'
CONTRACT = ROOT / 'packaging/release.json'


class ReleaseIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = cmake_executable()
        cls.configuration = json.loads(CONTRACT.read_text(encoding='utf-8-sig'))
        cls.scratch = ROOT / 'build/release-identity-tests'
        cls.scratch.mkdir(parents=True, exist_ok=True)

    def check(self, packs=None, modules=None, expected=False, configuration=None, error=None):
        packs = self.configuration['packs'] if packs is None else packs
        modules = self.configuration['modules'] if modules is None else modules
        with tempfile.TemporaryDirectory(dir=self.scratch) as temporary:
            directory = Path(temporary)
            contract = CONTRACT
            if configuration is not None:
                contract = directory / 'release.json'
                contract.write_text(json.dumps(configuration), encoding='utf-8')
            script = directory / 'check.cmake'
            script.write_text(
                'cmake_minimum_required(VERSION 3.20)\n' +
                f'include([==[{HELPER.as_posix()}]==])\n' +
                f'lyflow_release_combination_matches(matched [==[{contract.as_posix()}]==] '
                f'"{";".join(packs)}" "{";".join(modules)}")\n' +
                f'if(NOT matched STREQUAL "{"TRUE" if expected else "FALSE"}")\n'
                '  message(FATAL_ERROR "Unexpected release identity eligibility: ${matched}")\n'
                'endif()\n', encoding='utf-8')
            result = subprocess.run([str(self.cmake), '-P', str(script)], capture_output=True,
                                    text=True, encoding='utf-8', errors='replace', timeout=30)
            output = result.stdout + result.stderr
            if error:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(error, output)
            else:
                self.assertEqual(result.returncode, 0, output)

    def test_exact_release_combination_accepts_order_and_duplicate_normalization(self):
        self.check(expected=True)
        self.check(packs=list(reversed(self.configuration['packs'])) + [self.configuration['packs'][0]],
                   modules=list(reversed(self.configuration['modules'])) + [self.configuration['modules'][0]],
                   expected=True)

    def test_extra_missing_and_retired_pack_identities_cannot_claim_official_revision(self):
        packs = self.configuration['packs']
        self.check(packs=packs + ['solution-dts'])
        self.check(packs=packs[:-1])
        self.check(packs=['gap' if name == 'solution-gap' else name for name in packs])

    def test_empty_partial_and_additional_explicit_modules_are_not_release_combination(self):
        modules = self.configuration['modules']
        self.check(modules=[])
        self.check(modules=modules[:-1])
        self.check(modules=modules + ['profile-robust-kernel'])

    def test_release_contract_is_the_only_combination_source(self):
        alternate = dict(self.configuration, packs=['custom-scene'], modules=['custom-module'])
        self.check(packs=['custom-scene'], modules=['custom-module'], expected=True, configuration=alternate)
        self.check(configuration=alternate)

    def test_malformed_or_duplicate_release_contract_fails_instead_of_granting_identity(self):
        for field in ['packs', 'modules']:
            with self.subTest(field=field):
                self.check(configuration=dict(self.configuration, **{field: 'not-an-array'}),
                           error=f'Release {field} must be an array')
                duplicate = self.configuration[field] + [self.configuration[field][0]]
                self.check(configuration=dict(self.configuration, **{field: duplicate}),
                           error='invalid or duplicate identity')

    def test_product_cohort_versions_and_retired_release_identities_are_synchronized(self):
        root_version = json.loads((ROOT / 'package.json').read_text(encoding='utf-8'))['version']
        self.assertEqual(root_version, '1.4.5')
        for relative in ['app/package.json', 'packages/editor/package.json', 'packages/mcp/package.json',
                         'packages/visualization/package.json', 'examples/host-react/package.json', 'bridge/tauri.conf.json']:
            self.assertEqual(json.loads((ROOT / relative).read_text(encoding='utf-8'))['version'], root_version, relative)
        for relative in ['bridge/Cargo.toml', 'crates/lyflow-client/Cargo.toml', 'python/pyproject.toml']:
            self.assertIn(f'version = "{root_version}"', (ROOT / relative).read_text(encoding='utf-8'), relative)
        for directory in ['examples', 'tests/release']:
            for cmake in (ROOT / directory).rglob('CMakeLists.txt'):
                pins = re.findall(r'find_package\(\s*lyflow\s+(\d+\.\d+\.\d+)\s+EXACT\b',
                                  cmake.read_text(encoding='utf-8'))
                for version in pins:
                    self.assertEqual(version, root_version, str(cmake.relative_to(ROOT)))
        self.assertNotIn('gap', self.configuration['packs'])
        self.assertIn('solution-gap', self.configuration['packs'])
        self.assertNotIn('gap.fit_line', self.configuration['requiredOperators'])
        self.assertNotIn('gap.flush', self.configuration['requiredOperators'])


if __name__ == '__main__':
    unittest.main()
