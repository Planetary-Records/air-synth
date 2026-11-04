"""The installer smoke driver must refuse a normal or altered setup before execution."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


@unittest.skipUnless(sys.platform == 'win32', 'Windows installer safety')
class InstallSmokeSafetyTests(unittest.TestCase):
    def test_normal_installer_is_never_executed(self):
        spec = importlib.util.spec_from_file_location('install_smoke', Path(__file__).resolve().parents[1] / 'tools/release_install_smoke.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(module.subprocess, 'run') as execute:
            output = Path(tmp)
            (output / 'installer.json').write_text(json.dumps({'isolated_test': False, 'compiled': True}))
            with self.assertRaises(ValueError):
                module.smoke(output, output / 'baseline.json')
            execute.assert_not_called()

    def test_changed_test_executable_is_never_executed(self):
        spec = importlib.util.spec_from_file_location('install_smoke', Path(__file__).resolve().parents[1] / 'tools/release_install_smoke.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(module.subprocess, 'run') as execute:
            output = Path(tmp)
            (output / 'compiled').mkdir()
            name = 'air-fixture-Isolated-Test-Setup.exe'
            (output / 'compiled' / name).write_bytes(b'changed fixture')
            record = {'isolated_test': True, 'compiled': True, 'public_release': False,
                      'isolation_root': str(output / 'isolated-installation'), 'installer_filename': name,
                      'installer_sha256': hashlib.sha256(b'accepted fixture').hexdigest()}
            (output / 'installer.json').write_text(json.dumps(record))
            with self.assertRaises(ValueError):
                module.smoke(output, output / 'baseline.json')
            execute.assert_not_called()
