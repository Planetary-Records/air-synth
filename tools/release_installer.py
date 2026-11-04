"""Prepare an INTERNAL air Windows installer from a verified ZIP; compile only with --iscc.

Does not build the instrument, install, download prerequisites, sign or publish.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUNDLE_FILES = (
    'AI Rack Synth.vst3/Contents/Resources/moduleinfo.json',
    'AI Rack Synth.vst3/Contents/x86_64-win/AI Rack Synth.vst3',
    'AI Rack Synth.vst3/Contents/x86_64-win/AI Rack Synth Companion.exe',
)
DOCUMENTS = ('INSTALL.md', 'GETTING_STARTED.md', 'CHECKSUMS.md', 'FAQ.md', 'ALPHA.md',
             'ABLETON_LIVE.md', 'REAPER.md', 'FL_STUDIO.md', 'TESTING.md', 'VST3.md', 'TILE_ARRAY.md', 'PRIVACY.md',
             'SUPPORT.md', 'THIRD_PARTY_NOTICES.md', 'CHANGELOG.md', 'LICENSE', 'SECURITY.md')
MAX_PAYLOAD_BYTES = 2 * 1024 ** 3


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_name(name):
    if not isinstance(name, str) or not name or any(c in name for c in '\\:<>"|?*\x00'):
        return False
    p = PurePosixPath(name)
    if p.is_absolute() or p.as_posix() != name:
        return False
    for part in p.parts:
        if part in ('.', '..') or part.endswith((' ', '.')) or any(ord(c) < 32 for c in part):
            return False
        stem = part.split('.')[0].rstrip(' .')
        if re.fullmatch(r'CON|CONIN\$|CONOUT\$|PRN|AUX|NUL|COM[0-9¹²³]+|LPT[0-9¹²³]+', stem, re.I):
            return False
    return True


VERSION_PATTERN = r'\d+\.\d+\.\d+-(?:prep|preview|alpha|beta)\.[A-Za-z0-9.]+'


def prepare(package, expected_sha256, output, isolated_test=False):
    if output.exists() or output.is_symlink():
        raise FileExistsError('Output must be new; existing files are preserved.')
    package_bytes = package.read_bytes()
    if not re.fullmatch(r'[0-9a-fA-F]{64}', expected_sha256) or digest(package_bytes) != expected_sha256.lower():
        raise ValueError('Preparation ZIP checksum mismatch.')
    if any(c in str(output.resolve()) for c in '"\r\n'):
        raise ValueError('Output path cannot contain installer macro delimiters.')
    with zipfile.ZipFile(io.BytesIO(package_bytes)) as z:
        entries = z.infolist()
        names = [entry.filename for entry in entries]
        if (not all(safe_name(n) for n in names) or len({n.casefold() for n in names}) != len(names)
                or sum(e.file_size for e in entries) > MAX_PAYLOAD_BYTES
                or any(e.is_dir() or stat.S_IFMT(e.external_attr >> 16) not in (0, stat.S_IFREG) for e in entries)):
            raise ValueError('Unsafe, duplicate or oversized ZIP entries.')
        if 'release.json' not in names:
            raise ValueError('A preparation package manifest is required.')
        record = json.loads(z.read('release.json'))
        version = record['package_version']
        embedded = record['embedded_product_version']
        if (record['public_release'] is not False
                or not re.fullmatch(VERSION_PATTERN, version)
                or not re.fullmatch(r'\d+\.\d+\.\d+', embedded)
                or not re.fullmatch(r'[0-9a-f]{40}', record['binary_source_commit'])):
            raise ValueError('Only an identified internal preview package is admitted.')
        rows = record['files']
        if len({r['path'].casefold() for r in rows}) != len(rows):
            raise ValueError('Duplicate manifest paths.')
        declared = {r['path']: r for r in rows}
        if set(names) != set(declared) | {'release.json'} or 'release.json' in declared:
            raise ValueError('ZIP members must match the complete manifest.')
        required = set(BUNDLE_FILES) | set(DOCUMENTS)
        if not required <= set(declared) or not any(n.startswith('licenses/') for n in declared):
            raise ValueError('Complete bundle, user documents and notices are required.')
        for name in declared:
            if name not in required and not (name.startswith('licenses/') and PurePosixPath(name).suffix.lower() in ('.txt', '.md', '.json')):
                raise ValueError('Unapproved installer payload: ' + name)
        payload = {}
        for name in names:
            data = z.read(name)
            if name != 'release.json':
                row = declared[name]
                if len(data) != row['bytes'] or digest(data) != row['sha256'].lower():
                    raise ValueError('Package member hash/size mismatch: ' + name)
            payload[name] = data
    # Validate everything before creating output; never extract ZIP paths blindly.
    output.mkdir(parents=True)
    stage = output / 'payload'
    for name, data in payload.items():
        target = stage.joinpath(*PurePosixPath(name).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    alpha = '-alpha.' in version
    (stage / 'INSTALLER_README.txt').write_text(
        ('air - ALPHA (early test version)\n\n'
         'This is unfinished software. It can misbehave or close your music program without warning.\n'
         'Save often and keep copies of your projects. Later alphas may not open sounds made with this one.\n\n'
         if alpha else 'air - private beta / preview\n\n') +
        'Close every DAW and the standalone instrument; save and back up projects first.\n'
        'Setup does not automatically close or restart applications.\n'
        'This replaces the complete AI Rack Synth.vst3 bundle in the shared VST3 folder.\n'
        f'Search for {record.get("binary_display_name", "air")} in the DAW; the legacy bundle filename preserves compatibility.\n' +
        ('No separate Microsoft Visual C++ runtime is needed.\n' if alpha else
         'A compatible Microsoft Visual C++ x64 runtime is required and is not bundled.\n') +
        'No model, account or online service is required for manual playback.\n'
        'Uninstall keeps presets, projects, account data and diagnostics.\n' +
        ('This installer is not signed, so Windows may show an unknown-publisher warning.\n' if alpha else
         'Private release; clean-machine installation and signing are not verified.\n'), encoding='utf-8')
    shutil.copyfile(ROOT / 'packaging/windows/air.iss', output / 'setup.iss')
    arguments = [f'/DStageDir={stage.resolve()}', f'/DPackageVersion={version}',
                 f'/DEmbeddedVersion={embedded}', f'/DOutputDir={(output / "compiled").resolve()}',
                 str((output / 'setup.iss').resolve())]
    isolation_root = (output / 'isolated-installation').resolve()
    if isolated_test:
        arguments[-1:-1] = [f'/DIsolatedTestRoot={isolation_root}',
                            '/DIsolatedTestId=' + digest(str(isolation_root).casefold().encode())[:16]]
        (stage / 'INSTALLER_README.txt').write_text(
            'ISOLATED INSTALLER TEST ONLY\nPrivate destination and separate test uninstall identity.\n'
            'Does not install into the shared VST3 folder. Not a distributable installer.\n', encoding='utf-8')
    suffix = 'Isolated-Test-Setup' if isolated_test else 'Setup'
    metadata = {'product': 'air', 'package_version': version, 'public_release': False,
                'compiled': False, 'input_package_sha256': expected_sha256.lower(),
                'binary_source_commit': record['binary_source_commit'],
                'isolated_test': isolated_test,
                'isolation_root': str(isolation_root) if isolated_test else None,
                'installer_filename': f'air-{version}-Windows-x64-{suffix}.exe',
                'iscc_arguments': arguments}
    (output / 'installer.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--iscc', type=Path, help='Explicitly compile using an already installed Inno Setup 6.3+ compiler.')
    parser.add_argument('--isolated-test', action='store_true', help='Compile a clearly marked test variant confined to a new output subfolder and separate per-user uninstall identity.')
    args = parser.parse_args()
    try:
        if args.iscc is not None and not args.iscc.is_file():
            raise ValueError('The explicitly selected ISCC compiler does not exist.')
        output = prepare(args.package, args.sha256, args.output, isolated_test=args.isolated_test)
        if args.iscc is not None:
            metadata = json.loads((output / 'installer.json').read_text(encoding='utf-8'))
            subprocess.run([str(args.iscc.resolve()), *metadata['iscc_arguments']], check=True)
            exe = output / 'compiled' / metadata['installer_filename']
            if not exe.is_file():
                raise ValueError('Compiler did not produce the expected installer.')
            (output / 'SHA256SUMS.txt').write_text(f'{digest(exe.read_bytes())}  {exe.name}\n', encoding='ascii')
            metadata['compiled'] = True
            metadata['compiler_sha256'] = digest(args.iscc.read_bytes())
            metadata['installer_sha256'] = digest(exe.read_bytes())
            metadata['installer_bytes'] = exe.stat().st_size
            (output / 'installer.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        print(output)
    except (ValueError, KeyError, TypeError, OSError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Installer refused: {error}\n')


if __name__ == '__main__':
    main()
