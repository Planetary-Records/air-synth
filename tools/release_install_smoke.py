"""Exercise ONLY an explicitly compiled isolated installer; preserve the working VST3.

Run through RackSynthNativeRun --private-desktop. This is not a clean-VM/system-install test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import winreg


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def registry_state(hive, name, view):
    try:
        with winreg.OpenKey(hive, name, 0, winreg.KEY_READ | view) as key:
            values = []
            for index in range(winreg.QueryInfoKey(key)[1]):
                values.append(repr(winreg.EnumValue(key, index)))
            return hashlib.sha256('\n'.join(sorted(values)).encode()).hexdigest()
    except FileNotFoundError:
        return None


def smoke(output, baseline, pluginval=None):
    output = output.resolve()
    record = json.loads((output / 'installer.json').read_text(encoding='utf-8'))
    root = output / 'isolated-installation'
    if (record.get('isolated_test') is not True or record.get('compiled') is not True
            or record.get('public_release') is not False
            or Path(record['isolation_root']) != root or root.exists()
            or not record['installer_filename'].endswith('-Isolated-Test-Setup.exe')):
        raise ValueError('Only a fresh, compiled isolated-test output is admitted.')
    exe = output / 'compiled' / record['installer_filename']
    if digest(exe) != record['installer_sha256']:
        raise ValueError('Isolated executable checksum mismatch.')
    token = hashlib.sha256(str(root).casefold().encode()).hexdigest()[:16]
    if f'/DIsolatedTestRoot={root}' not in record['iscc_arguments'] or f'/DIsolatedTestId={token}' not in record['iscc_arguments']:
        raise ValueError('Isolated destination/identity receipt mismatch.')
    uninstall = r'Software\Microsoft\Windows\CurrentVersion\Uninstall'
    working_key = uninstall + r'\Planetary.air.Windows.VST3_is1'
    views = (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY)
    hives = (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE)
    working_registry = [registry_state(h, working_key, v) for h in hives for v in views]
    test_key = uninstall + r'\Planetary.air.Windows.VST3.IsolatedTest.' + token + '_is1'
    if any(registry_state(h, test_key, v) is not None for h in hives for v in views):
        raise ValueError('Existing test registration is preserved; select a fresh output.')
    original = json.loads(baseline.read_text(encoding='utf-8'))
    working_bundle = Path('C:/Program Files/Common Files/VST3/AI Rack Synth.vst3')

    def preserve():
        assert {p.relative_to(working_bundle).as_posix() for p in working_bundle.rglob('*') if p.is_file()} == {r['path'] for r in original['installed_bundle']}
        for row in original['installed_bundle']:
            if digest(working_bundle / row['path']) != row['sha256']:
                raise ValueError('Working VST3 changed; stop qualification.')
        assert [registry_state(h, working_key, v) for h in hives for v in views] == working_registry

    staged = output / 'payload'
    release = json.loads((staged / 'release.json').read_text(encoding='utf-8'))

    def installed_path(name):
        return (root / 'VST3' / name) if name.startswith('AI Rack Synth.vst3/') else (root / 'app' / name)

    def verify_payload():
        for row in release['files']:
            assert digest(installed_path(row['path'])) == row['sha256'], row['path']
        assert (root / 'app/release.json').read_bytes() == (staged / 'release.json').read_bytes()
        preserve()

    def execute(path, label):
        subprocess.run([str(path), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
                        f'/LOG={output / (label + ".log")}'], check=True, timeout=180)

    preserve()
    execute(exe, 'install')
    verify_payload()
    assert registry_state(winreg.HKEY_CURRENT_USER, test_key, winreg.KEY_WOW64_64KEY) is not None
    if pluginval is not None:
        with (output / 'installed-pluginval.log').open('w', encoding='utf-8') as log:
            subprocess.run([str(pluginval.resolve()), '--strictness-level', '5', '--random-seed', '20261003',
                            '--skip-gui-tests', '--validate', str(root / 'VST3/AI Rack Synth.vst3')],
                           check=True, timeout=180, stdout=log, stderr=subprocess.STDOUT)
        preserve()
    sentinels = (root / 'app/musician-preset.keep', root / 'user-data/project.keep')
    for p in sentinels:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b'isolated user file; preserve across reinstall/uninstall\n')
    execute(exe, 'reinstall')
    verify_payload()
    execute(root / 'app/unins000.exe', 'uninstall')
    for row in release['files']:
        assert not installed_path(row['path']).exists(), row['path']
    assert not (root / 'app/release.json').exists()
    assert all(p.read_bytes() == b'isolated user file; preserve across reinstall/uninstall\n' for p in sentinels)
    assert all(registry_state(h, test_key, v) is None for h in hives for v in views)
    preserve()
    result = {'install': 'PASS', 'same_version_reinstall': 'PASS', 'uninstall': 'PASS',
              'all_installed_payload_hashes': 'PASS', 'synthetic_user_files_preserved': True,
              'working_bundle_and_uninstall_registration_preserved': True,
              'isolated_installer_sha256': digest(exe), 'clean_vm_or_system_wide_install': 'not run'}
    result['installed_headless_pluginval'] = 'PASS' if pluginval is not None else 'not run'
    (output / 'installation-test.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installer-output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--pluginval', type=Path, help='Optionally validate the installed private bundle before uninstalling.')
    args = parser.parse_args()
    print(json.dumps(smoke(args.installer_output, args.baseline, args.pluginval)))
