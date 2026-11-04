"""The alpha label and runtime choices a musician depends on; no compiler or account needed."""
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CMAKE = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AlphaIdentityTests(unittest.TestCase):
    def test_release_label_is_an_alpha_of_the_numeric_product_version(self):
        label = re.search(r'set\(RACKSYNTH_RELEASE_VERSION "([^"]+)"\)', CMAKE)
        self.assertIsNotNone(label, 'CMake must name the release label')
        self.assertRegex(label[1], r'^\d+\.\d+\.\d+-alpha\.\d+$')
        numeric = re.search(r'project\(RackSynthFeasibility VERSION (\d+\.\d+\.\d+)', CMAKE)[1]
        self.assertTrue(label[1].startswith(numeric + '-'), 'the numeric host version must not move')
        self.assertIn('RACKSYNTH_RELEASE_VERSION="${RACKSYNTH_RELEASE_VERSION}"', CMAKE)
        self.assertRegex(CMAKE, r'target_compile_definitions\(RackSynthInstrument PUBLIC\s+RACKSYNTH_RELEASE_VERSION=')
        self.assertNotRegex(CMAKE, r'add_compile_definitions\([^)]*RACKSYNTH_RELEASE_VERSION')

    def test_shipping_builds_do_not_need_a_separate_cpp_runtime(self):
        setting = CMAKE.find('set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")')
        self.assertGreater(setting, 0)
        self.assertIn('if(NOT RACKSYNTH_BUILD_PROBES)\n    set(CMAKE_MSVC_RUNTIME_LIBRARY', CMAKE)
        self.assertLess(setting, CMAKE.find('FetchContent_MakeAvailable(JUCE)'),
                        'the runtime must be chosen before JUCE and the products are defined')

    def test_release_tools_admit_alpha_versions_only_as_prereleases(self):
        for name in ('release_package', 'release_installer'):
            pattern = re.compile(load(name).VERSION_PATTERN)
            with self.subTest(tool=name):
                for good in ('0.1.0-alpha.1', '0.1.0-alpha.20261101', '0.1.0-beta.1', '0.1.0-prep.20261028'):
                    self.assertRegex(good, pattern)
                for bad in ('0.1.0', '0.1.0-rc.1', '0.1.0-alpha', '0.1.0-alpha.', 'alpha.1', '0.1.0-ALPHA.1'):
                    self.assertNotRegex(bad, pattern)


if __name__ == '__main__':
    unittest.main()
