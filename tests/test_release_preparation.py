"""Portable package/installer fixture checks; no publication or installation."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest
import zipfile
ROOT = Path(__file__).resolve().parents[1]
class ReleasePackageTests(unittest.TestCase):
    def test_internal_package_is_bound_and_does_not_admit_extra_payload(self):
        spec = importlib.util.spec_from_file_location('release_package', ROOT / 'tools/release_package.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = base / 'AI Rack Synth.vst3'
            files = {}
            for name in module.BUNDLE_FILES:
                p = bundle / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'fixture payload')
                files[name] = p
            manifest = base / 'accepted.json'
            manifest.write_text(json.dumps({'source_commit': 'a' * 40, 'documentation_commit': 'b' * 40,
                'documentation_repository': 'Planetary-Records/air-public',
                'binary_display_name': 'AI Rack Synth', 'files': [
                {'path': name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
                for name, p in files.items()]}))
            notices = base / 'notices'
            notices.mkdir()
            (notices / 'original.txt').write_text('third-party fixture notice')
            output = base / 'package'
            result = module.package(bundle, manifest, notices, '0.1.0-prep.1', output)
            with zipfile.ZipFile(result) as z:
                self.assertIsNone(z.testzip())
                info = json.loads(z.read('release.json'))
                self.assertFalse(info['public_release'])
                self.assertEqual(info['binary_source_commit'], 'a' * 40)
                self.assertEqual(info['documentation_commit'], 'b' * 40)
                self.assertEqual(info.get('documentation_repository'), 'Planetary-Records/air-public')
                self.assertIn('https://github.com/Planetary-Records/air-public/blob/' + 'b' * 40 + '/CONTRIBUTING.md',
                              z.read('SUPPORT.md').decode())
                self.assertEqual(info['binary_display_name'], 'AI Rack Synth')
                self.assertIn('licenses/original.txt', z.namelist())
                self.assertIn('INSTALL.md', z.namelist())
                guides = ('CHECKSUMS.md', 'FAQ.md', 'ALPHA.md', 'ABLETON_LIVE.md',
                          'REAPER.md', 'FL_STUDIO.md', 'TESTING.md')
                for guide in guides:
                    self.assertIn(guide, z.namelist())
                for entry in ('GETTING_STARTED.md', 'INSTALL.md', 'VST3.md', 'FAQ.md', 'ALPHA.md'):
                    self.assertNotIn('/docs/user-guide/', z.read(entry).decode(),
                                     'first-session guides must be available offline')
                self.assertFalse(any('.log' in n or '.pdb' in n for n in z.namelist()))
                for name in z.namelist():
                    if name.endswith('.md'):
                        self.assertNotIn('/blob/' + 'a' * 40 + '/', z.read(name).decode())
                        for target in re.findall(r'\]\(([^)]+)\)', z.read(name).decode()):
                            if ':' not in target and not target.startswith('#'):
                                self.assertIn(target.split('#')[0], z.namelist())
            installer_spec = importlib.util.spec_from_file_location('release_installer', ROOT / 'tools/release_installer.py')
            installer = importlib.util.module_from_spec(installer_spec)
            installer_spec.loader.exec_module(installer)
            staged = installer.prepare(result, hashlib.sha256(result.read_bytes()).hexdigest(), base / 'installer')
            with zipfile.ZipFile(result) as z:
                for guide in guides:
                    self.assertEqual((staged / 'payload' / guide).read_bytes(), z.read(guide))
            self.assertIn(hashlib.sha256(result.read_bytes()).hexdigest(),
                          (output / 'SHA256SUMS.txt').read_text())
            repeated = module.package(bundle, manifest, notices, '0.1.0-prep.1', base / 'repeat')
            self.assertEqual(result.read_bytes(), repeated.read_bytes())
            accepted = json.loads(manifest.read_text())
            legacy = dict(accepted)
            del legacy['documentation_repository']
            manifest.write_text(json.dumps(legacy))
            legacy_result = module.package(bundle, manifest, notices, '0.1.0-prep.1', base / 'legacy')
            with zipfile.ZipFile(legacy_result) as z:
                self.assertEqual(json.loads(z.read('release.json'))['documentation_repository'], 'Planetary-Records/air')
            for index, bad_repository in enumerate(('https://evil.invalid/a/b', 'a/../b', 'a/b#c', 'a/.b', 'a/', 123)):
                manifest.write_text(json.dumps({**accepted, 'documentation_repository': bad_repository}))
                refused = base / ('bad-repository-' + str(index))
                with self.assertRaises(ValueError):
                    module.package(bundle, manifest, notices, '0.1.0-prep.1', refused)
                self.assertFalse(refused.exists())
            manifest.write_text(json.dumps(accepted))
            with self.assertRaises(FileExistsError):
                module.package(bundle, manifest, notices, '0.1.0-prep.1', output)
            files[module.BUNDLE_FILES[0]].write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                module.package(bundle, manifest, notices, '0.1.0-prep.2', base / 'bad')
            files[module.BUNDLE_FILES[0]].write_bytes(b'fixture payload')
            (bundle / 'private.log').write_text('must never ship')
            with self.assertRaises(ValueError):
                module.package(bundle, manifest, notices, '0.1.0-prep.2', base / 'extra')
            with self.assertRaises(ValueError):
                module.package(bundle, manifest, notices, '../bad', base / 'escape')

if __name__ == "__main__":
    unittest.main()
