import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_GRAPH = ROOT / 'core/cmake/module-graph.cmake'


def cmake_executable():
    selected = os.environ.get('LYFLOW_TEST_CMAKE') or shutil.which('cmake')
    if selected:
        return Path(selected)
    if os.name == 'nt':
        installed = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Microsoft Visual Studio'
        candidates = sorted(installed.glob('*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'))
        if candidates:
            return candidates[-1]
    raise FileNotFoundError('CMake is required; set LYFLOW_TEST_CMAKE to its executable path')


class ModuleGraphTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = cmake_executable()
        cls.scratch = (ROOT / 'build/module-graph-tests').resolve()
        if not cls.scratch.is_relative_to(ROOT.resolve()):
            raise ValueError('Test scratch directory must stay inside the worktree')
        cls.scratch.mkdir(parents=True, exist_ok=True)

    def check_cmake(self, body, error=None, configure=False):
        with tempfile.TemporaryDirectory(prefix='case-', dir=self.scratch) as temporary:
            directory = Path(temporary)
            self.assertTrue(directory.resolve().is_relative_to(self.scratch))
            content = ('cmake_minimum_required(VERSION 3.20)\n' +
                       ('project(module_graph LANGUAGES NONE)\n' if configure else '') +
                       f'include([==[{MODULE_GRAPH.as_posix()}]==])\n' + body)
            if configure:
                ninja = os.environ.get('LYFLOW_TEST_NINJA') or shutil.which('ninja')
                if not ninja and os.name == 'nt':
                    candidate = self.cmake.parents[2] / 'Ninja/ninja.exe'
                    if candidate.is_file():
                        ninja = str(candidate)
                if not ninja:
                    raise FileNotFoundError('Ninja is required for target-provider checks; set LYFLOW_TEST_NINJA')
                (directory / 'CMakeLists.txt').write_text(content, encoding='utf-8')
                command = [str(self.cmake), '-S', str(directory), '-B', str(directory / 'out'),
                           '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(ninja)]
            else:
                script = directory / 'case.cmake'
                script.write_text(content, encoding='utf-8')
                command = [str(self.cmake), '-P', str(script)]
            result = subprocess.run(command, cwd=directory, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=45)
            output = result.stdout + result.stderr
            if error is None:
                self.assertEqual(result.returncode, 0, output)
            else:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(error, output)

    def test_modules_are_dependency_ordered_deduplicated_and_resolved_again(self):
        self.check_cmake('''
lyflow_declare_module(NAME leaf TARGET leaf SETUP setup_leaf)
lyflow_declare_module(NAME middle TARGET middle SETUP setup_middle REQUIRES leaf)
lyflow_declare_module(NAME root TARGET root SETUP setup_root REQUIRES middle leaf)
lyflow_declare_module(NAME independent TARGET independent SETUP setup_independent)
lyflow_resolve_modules(ordered root independent root)
if(NOT ordered STREQUAL "leaf;middle;root;independent")
  message(FATAL_ERROR "Incorrect module ordering: ${ordered}")
endif()
lyflow_resolve_modules(again middle)
if(NOT again STREQUAL "leaf;middle")
  message(FATAL_ERROR "Module traversal retained previous results: ${again}")
endif()
''')

    def test_missing_modules_include_required_by_context(self):
        with self.subTest(kind='dependency'):
            self.check_cmake('''
lyflow_declare_module(NAME consumer TARGET consumer SETUP setup_consumer REQUIRES absent)
lyflow_resolve_modules(ordered consumer)
''', 'Missing lyFlow module: absent; required by consumer')
        with self.subTest(kind='selection'):
            self.check_cmake('lyflow_resolve_modules(ordered absent)\n', 'Missing lyFlow module: absent')

    def test_module_cycles_report_the_dependency_chain(self):
        self.check_cmake('''
lyflow_declare_module(NAME first TARGET first SETUP setup_first REQUIRES second)
lyflow_declare_module(NAME second TARGET second SETUP setup_second REQUIRES first)
lyflow_resolve_modules(ordered first)
''', 'lyFlow module dependency cycle: first;second;first')

    def test_module_declarations_cannot_silently_replace_an_existing_module(self):
        self.check_cmake('''
lyflow_declare_module(NAME repeated TARGET original SETUP setup_original)
lyflow_declare_module(NAME repeated TARGET replacement SETUP setup_replacement)
''', 'Duplicate lyFlow module: repeated')

    def test_target_setup_is_dependency_ordered_and_idempotent(self):
        self.check_cmake('''
function(setup_base)
  add_library(module_base INTERFACE)
  set_property(GLOBAL APPEND PROPERTY SETUP_TRACE base)
endfunction()
function(setup_consumer)
  if(NOT TARGET module_base)
    message(FATAL_ERROR "Consumer setup ran before its dependency")
  endif()
  add_library(module_consumer INTERFACE)
  set_property(GLOBAL APPEND PROPERTY SETUP_TRACE consumer)
endfunction()
lyflow_declare_module(NAME base TARGET module_base SETUP setup_base)
lyflow_declare_module(NAME consumer TARGET module_consumer SETUP setup_consumer REQUIRES base)
lyflow_configure_modules(consumer base consumer)
lyflow_configure_modules(consumer)
get_property(trace GLOBAL PROPERTY SETUP_TRACE)
get_property(enabled GLOBAL PROPERTY LYFLOW_ENABLED_MODULES)
get_target_property(owner module_consumer LYFLOW_MODULE_ID)
if(NOT trace STREQUAL "base;consumer" OR NOT enabled STREQUAL "base;consumer" OR NOT owner STREQUAL "consumer")
  message(FATAL_ERROR "Module setup was repeated or ownership changed: ${trace}/${enabled}/${owner}")
endif()
''', configure=True)

    def test_target_providers_cannot_claim_existing_or_another_module_target(self):
        with self.subTest(kind='undeclared-provider'):
            self.check_cmake('''
add_library(shared INTERFACE)
function(setup_claimant)
  message(FATAL_ERROR "Unexpected replacement setup")
endfunction()
lyflow_declare_module(NAME claimant TARGET shared SETUP setup_claimant)
lyflow_configure_modules(claimant)
''', 'Module target shared has an undeclared provider', configure=True)
        with self.subTest(kind='declared-provider-conflict'):
            self.check_cmake('''
function(setup_shared)
  add_library(shared INTERFACE)
endfunction()
lyflow_declare_module(NAME first TARGET shared SETUP setup_shared)
lyflow_declare_module(NAME second TARGET shared SETUP setup_shared)
lyflow_configure_modules(first second)
''', 'Module target shared has an undeclared provider', configure=True)

    def test_packs_are_ordered_and_missing_enabled_or_declared_packs_are_rejected(self):
        with self.subTest(kind='ordering'):
            self.check_cmake('''
lyflow_pack_dependencies(NAME base DEFAULT ON)
lyflow_pack_dependencies(NAME consumer DEFAULT ON PACKS base)
lyflow_resolve_packs(ordered consumer base consumer)
if(NOT ordered STREQUAL "base;consumer")
  message(FATAL_ERROR "Incorrect pack ordering: ${ordered}")
endif()
''')
        with self.subTest(kind='missing-enabled-dependency'):
            self.check_cmake('''
lyflow_pack_dependencies(NAME base DEFAULT OFF)
lyflow_pack_dependencies(NAME consumer DEFAULT ON PACKS base)
lyflow_resolve_packs(ordered consumer)
''', 'Missing enabled pack base; required by consumer')
        with self.subTest(kind='undeclared-selection'):
            self.check_cmake('lyflow_resolve_packs(ordered absent)\n', 'Missing lyFlow pack declaration: absent')
        declarations = '\n'.join(
            f'include("{(ROOT / "packs" / name / "lyflow_pack_dependencies.cmake").as_posix()}")'
            for name in ('glue', 'std-image'))
        with self.subTest(kind='glue-snippet-requires-image-registration'):
            self.check_cmake(declarations + '\nlyflow_resolve_packs(ordered glue)\n',
                             'Missing enabled pack std-image; required by glue')
        with self.subTest(kind='glue-image-registration-is-ordered'):
            self.check_cmake(declarations + '''
lyflow_resolve_packs(ordered glue std-image)
if(NOT ordered STREQUAL "std-image;glue")
  message(FATAL_ERROR "Snippet provider must be enabled before glue: ${ordered}")
endif()
''')

    def test_optional_backends_require_selection_and_are_shared_across_packs(self):
        declarations = '''
lyflow_declare_module(NAME portable TARGET portable SETUP setup_portable)
lyflow_declare_module(NAME backend TARGET backend SETUP setup_backend REQUIRES portable)
lyflow_declare_module(NAME dependent TARGET dependent SETUP setup_dependent REQUIRES backend)
lyflow_pack_dependencies(NAME public DEFAULT ON MODULES portable OPTIONAL_MODULES backend)
lyflow_pack_dependencies(NAME scene DEFAULT OFF MODULES backend)
'''
        self.check_cmake(declarations + '''
lyflow_resolve_pack_modules(ordered PACKS public)
get_property(active GLOBAL PROPERTY LYFLOW_PACK_public_ACTIVE_MODULES)
if(NOT ordered STREQUAL "portable" OR NOT active STREQUAL "portable")
  message(FATAL_ERROR "Unselected optional backend became required: ${ordered}/${active}")
endif()
lyflow_resolve_pack_modules(ordered PACKS public MODULES dependent)
get_property(active GLOBAL PROPERTY LYFLOW_PACK_public_ACTIVE_MODULES)
if(NOT ordered STREQUAL "portable;backend;dependent" OR NOT active STREQUAL "portable;backend")
  message(FATAL_ERROR "Transitive backend was not selected: ${ordered}/${active}")
endif()
lyflow_resolve_pack_modules(ordered PACKS public MODULES backend)
get_property(active GLOBAL PROPERTY LYFLOW_PACK_public_ACTIVE_MODULES)
if(NOT ordered STREQUAL "portable;backend" OR NOT active STREQUAL "portable;backend")
  message(FATAL_ERROR "Explicit public backend selection was lost: ${ordered}/${active}")
endif()
lyflow_resolve_pack_modules(ordered PACKS public scene)
get_property(active GLOBAL PROPERTY LYFLOW_PACK_public_ACTIVE_MODULES)
if(NOT ordered STREQUAL "portable;backend" OR NOT active STREQUAL "portable;backend")
  message(FATAL_ERROR "Scene-required backend was not shared: ${ordered}/${active}")
endif()
lyflow_resolve_pack_modules(ordered PACKS public)
get_property(active GLOBAL PROPERTY LYFLOW_PACK_public_ACTIVE_MODULES)
if(NOT ordered STREQUAL "portable" OR NOT active STREQUAL "portable")
  message(FATAL_ERROR "Backend selection leaked between resolutions: ${ordered}/${active}")
endif()
''')
        self.check_cmake(declarations +
                         'lyflow_resolve_pack_modules(ordered PACKS public MODULES absent)\n',
                         'Missing lyFlow module: absent')
        self.check_cmake(declarations +
                         'lyflow_resolve_pack_modules(ordered PACKS absent)\n',
                         'Missing lyFlow pack declaration: absent')
        self.check_cmake(declarations + '''
lyflow_pack_dependencies(NAME broken DEFAULT ON OPTIONAL_MODULES absent)
lyflow_resolve_pack_modules(ordered PACKS broken)
''', 'Missing lyFlow module: absent')
        self.check_cmake('''
function(setup_portable)
  add_library(portable INTERFACE)
endfunction()
function(setup_heavy)
  message(FATAL_ERROR "Unselected backend setup was invoked")
endfunction()
lyflow_declare_module(NAME portable TARGET portable SETUP setup_portable)
lyflow_declare_module(NAME heavy TARGET heavy SETUP setup_heavy REQUIRES portable)
lyflow_pack_dependencies(NAME public DEFAULT ON MODULES portable OPTIONAL_MODULES heavy)
lyflow_resolve_pack_modules(ordered PACKS public)
lyflow_configure_modules(${ordered})
if(NOT TARGET portable OR TARGET heavy)
  message(FATAL_ERROR "Optional backend changed the portable target closure")
endif()
''', configure=True)

    def test_pack_cycles_are_rejected_before_configuration(self):
        self.check_cmake('''
lyflow_pack_dependencies(NAME first DEFAULT ON PACKS second)
lyflow_pack_dependencies(NAME second DEFAULT ON PACKS first)
lyflow_resolve_packs(ordered first second)
''', 'lyFlow pack dependency cycle: first;second;first')

    def test_pack_dependency_declarations_cannot_be_overwritten(self):
        self.check_cmake('''
lyflow_pack_dependencies(NAME repeated DEFAULT ON MODULES first)
lyflow_pack_dependencies(NAME repeated DEFAULT OFF MODULES second)
''', 'Duplicate lyFlow pack dependency declaration: repeated')

    def test_solution_identity_is_explicit_and_retired_names_are_not_aliases(self):
        declaration = (ROOT / 'solutions/dts/lyflow_pack_dependencies.cmake').as_posix()
        self.check_cmake(f'include("{declaration}")\n' + """
lyflow_pack_dependencies(NAME std-profile DEFAULT ON)
lyflow_pack_dependencies(NAME std-metrology DEFAULT ON)
lyflow_pack_dependencies(NAME std-geometry DEFAULT ON)
lyflow_resolve_packs(ordered solution-dts std-profile std-metrology std-geometry)
if(NOT ordered STREQUAL "std-profile;std-metrology;std-geometry;solution-dts")
  message(FATAL_ERROR "Scene dependency ordering is incorrect: ${ordered}")
endif()
get_property(replacement GLOBAL PROPERTY LYFLOW_REPLACED_PACK_dts)
if(NOT replacement STREQUAL "solution-dts")
  message(FATAL_ERROR "Retired identity has no actionable replacement")
endif()
""")
        with self.subTest(kind='retired-name-does-not-select-solution'):
            self.check_cmake(f'include("{declaration}")\nlyflow_resolve_packs(ordered dts)\n',
                             'Missing lyFlow pack declaration: dts')
        with self.subTest(kind='missing-explicit-public-pack'):
            self.check_cmake(f'include("{declaration}")\n' +
                             'lyflow_pack_dependencies(NAME std-profile DEFAULT ON)\n' +
                             'lyflow_resolve_packs(ordered solution-dts)\n',
                             'Missing enabled pack std-profile; required by solution-dts')
        for declarations in (
            'lyflow_pack_dependencies(NAME dts DEFAULT OFF)\nlyflow_pack_dependencies(NAME solution-dts DEFAULT OFF REPLACES dts)',
            'lyflow_pack_dependencies(NAME solution-dts DEFAULT OFF REPLACES dts)\nlyflow_pack_dependencies(NAME dts DEFAULT OFF)',
            'lyflow_pack_dependencies(NAME first DEFAULT OFF REPLACES dts)\nlyflow_pack_dependencies(NAME second DEFAULT OFF REPLACES dts)',
            'lyflow_pack_dependencies(NAME solution-dts DEFAULT OFF REPLACES solution-dts)',
        ):
            with self.subTest(declarations=declarations):
                self.check_cmake(declarations, 'retired')


if __name__ == '__main__':
    unittest.main(verbosity=2)
