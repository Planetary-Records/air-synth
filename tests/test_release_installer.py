"""Installer staging safety using dummy bytes only; never compile or install."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_installer', ROOT / 'tools/release_installer.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class ReleaseInstallerTests(unittest.TestCase):
    def test_isolated_test_has_private_destination_and_distinct_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            package, checksum = self.fixture(base)
            result = installer.prepare(package, checksum, base / 'isolated', isolated_test=True)
            record = json.loads((result / 'installer.json').read_text())
            self.assertTrue(record['isolated_test'])
            self.assertEqual(Path(record['isolation_root']), (result / 'isolated-installation').resolve())
            self.assertIn('Isolated-Test-Setup.exe', record['installer_filename'])
            self.assertTrue(any(a.startswith('/DIsolatedTestRoot=') for a in record['iscc_arguments']))
            self.assertTrue(any(a.startswith('/DIsolatedTestId=') for a in record['iscc_arguments']))
            self.assertFalse((result / 'isolated-installation').exists())

    def fixture(self, base, edit=None, extra=None):
        payload = {name: b'fixture bytes' for name in installer.BUNDLE_FILES}
        payload.update({name: b'fixture document' for name in installer.DOCUMENTS})
        payload['licenses/fixture.txt'] = b'original third-party notice'
        record = {'public_release': False, 'package_version': '0.1.0-prep.1',
                  'embedded_product_version': '0.1.0', 'binary_source_commit': 'a' * 40,
                  'files': [{'path': n, 'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
                            for n, b in payload.items()]}
        if edit:
            edit(payload, record)
        payload['release.json'] = (json.dumps(record) + '\n').encode()
        if extra:
            payload.update(extra)
        package = base / 'fixture.zip'
        with zipfile.ZipFile(package, 'w') as z:
            for name, data in payload.items():
                z.writestr(name, data)
        return package, hashlib.sha256(package.read_bytes()).hexdigest()

    def test_verified_package_stages_complete_bundle_and_notices(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            package, digest = self.fixture(base)
            result = installer.prepare(package, digest, base / 'prepared')
            record = json.loads((result / 'installer.json').read_text(encoding='utf-8'))
            self.assertFalse(record['public_release'])
            self.assertFalse(record['compiled'])
            self.assertFalse(record['isolated_test'])
            self.assertIsNone(record['isolation_root'])
            self.assertFalse(any(a.startswith('/DIsolatedTest') for a in record['iscc_arguments']))
            self.assertEqual(record['input_package_sha256'], digest)
            self.assertEqual((result / 'payload/licenses/fixture.txt').read_bytes(), b'original third-party notice')
            for name in installer.BUNDLE_FILES:
                self.assertEqual((result / 'payload' / name).read_bytes(), b'fixture bytes')
            script = (result / 'setup.iss').read_text(encoding='utf-8')
            self.assertIn('AppName=air', script)
            self.assertIn(r'{commoncf64}\VST3\AI Rack Synth.vst3', script)
            self.assertIn('CloseApplications=no', script)
            self.assertIn('RestartApplications=no', script)
            self.assertNotIn('[UninstallDelete]', script)
            self.assertNotIn('[Run]', script)
            self.assertFalse((result / 'compiled').exists())

    def test_wrong_zip_hash_refused_before_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            package, _ = self.fixture(base)
            with self.assertRaises(ValueError):
                installer.prepare(package, '0' * 64, base / 'output')
            self.assertFalse((base / 'output').exists())

    def test_payload_tampering_and_undeclared_files_refused(self):
        for mutation in ('tamper', 'extra'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                edit = (lambda p, r: p.update({'INSTALL.md': b'tampered'})) if mutation == 'tamper' else None
                package, digest = self.fixture(base, edit=edit, extra={'private.log': b'private'} if mutation == 'extra' else None)
                with self.assertRaises(ValueError):
                    installer.prepare(package, digest, base / 'output')
                self.assertFalse((base / 'output').exists())

    def test_traversal_windows_aliases_and_case_collisions_refused(self):
        for name in ('../escape.txt', 'C:/escape.txt', 'licenses/../escape.txt',
                     'licenses/file.txt:stream', 'licenses/CON.txt', 'licenses/notice.txt.',
                     'licenses/bad*.txt', 'licenses/CONOUT$.txt', 'licenses/CON .txt',
                     'licenses/COM0.log', 'install.md'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                def edit(payload, record):
                    payload[name] = b'fixture'
                    record['files'].append({'path': name, 'bytes': 7,
                                            'sha256': hashlib.sha256(b'fixture').hexdigest()})
                package, digest = self.fixture(base, edit=edit)
                with self.assertRaises(ValueError):
                    installer.prepare(package, digest, base / 'output')
                self.assertFalse((base / 'output').exists())

    def test_stable_versions_and_public_claims_refused(self):
        for key, value in (('package_version', '0.1.0'), ('package_version', '../bad'),
                           ('public_release', True), ('embedded_product_version', '0.1.bad')):
            with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                package, digest = self.fixture(base, edit=lambda p, r: r.update({key: value}))
                with self.assertRaises(ValueError):
                    installer.prepare(package, digest, base / 'output')

    def test_existing_output_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            package, digest = self.fixture(base)
            output = base / 'output'
            output.mkdir()
            (output / 'keep.txt').write_text('preserve')
            with self.assertRaises(FileExistsError):
                installer.prepare(package, digest, output)
            self.assertEqual((output / 'keep.txt').read_text(), 'preserve')

    def test_links_and_incomplete_bundle_refused(self):
        for mutation in ('link', 'missing-companion'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                if mutation == 'missing-companion':
                    def edit(payload, record):
                        name = installer.BUNDLE_FILES[-1]
                        del payload[name]
                        record['files'] = [r for r in record['files'] if r['path'] != name]
                    package, digest = self.fixture(base, edit=edit)
                else:
                    package, _ = self.fixture(base)
                    with zipfile.ZipFile(package) as z:
                        payload = {n: z.read(n) for n in z.namelist()}
                    with zipfile.ZipFile(package, 'w') as z:
                        for name, data in payload.items():
                            info = zipfile.ZipInfo(name)
                            if name == 'licenses/fixture.txt':
                                info.create_system = 3
                                info.external_attr = 0o120777 << 16
                            z.writestr(info, data)
                    digest = hashlib.sha256(package.read_bytes()).hexdigest()
                with self.assertRaises(ValueError):
                    installer.prepare(package, digest, base / 'output')
                self.assertFalse((base / 'output').exists())


if __name__ == '__main__':
    unittest.main()
