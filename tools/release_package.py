"""Create an INTERNAL preparation ZIP from a hash-bound accepted VST3 bundle.

No upload, installation, public licensing grant, model weights or arbitrary extras.
Public release still requires the owner gates in docs/release/README.md.
"""
import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import re
from urllib.parse import quote
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUNDLE_FILES = (
    'Contents/Resources/moduleinfo.json',
    'Contents/x86_64-win/AI Rack Synth.vst3',
    'Contents/x86_64-win/AI Rack Synth Companion.exe',
)


VERSION_PATTERN = r'\d+\.\d+\.\d+-(?:prep|preview|alpha|beta)\.[A-Za-z0-9.]+'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def package(bundle, evidence, notices, version, output):
    if not re.fullmatch(VERSION_PATTERN, version):
        raise ValueError('Use a version such as 0.1.0-alpha.1; stable versions are not admitted.')
    if output.exists():
        raise FileExistsError('Output must be new; existing packages are never replaced.')
    if any(p.is_symlink() or p.is_junction() for p in (bundle, notices)):
        raise ValueError('Bundle and notice roots must not be links.')
    accepted = json.loads(evidence.read_text(encoding='utf-8-sig'))
    commit = accepted['source_commit']
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('The binary source commit must be recorded in full.')
    documentation_commit = accepted.get('documentation_commit', commit)
    if not re.fullmatch('[0-9a-f]{40}', documentation_commit):
        raise ValueError('The documentation commit must be recorded in full.')
    documentation_repository = accepted.get('documentation_repository', 'Planetary-Records/air')
    if not isinstance(documentation_repository, str) or not re.fullmatch(
            r'[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*', documentation_repository):
        raise ValueError('The documentation repository must be a GitHub owner/repository name.')
    expected = {row['path']: row for row in accepted['files']}
    actual = {p.relative_to(bundle).as_posix() for p in bundle.rglob('*') if p.is_file()}
    if actual != set(BUNDLE_FILES) or any(p.is_symlink() or p.is_junction() for p in bundle.rglob('*')):
        raise ValueError('Bundle must contain exactly the three admitted product files, without links.')
    payload = {}
    for name in BUNDLE_FILES:
        data = (bundle / name).read_bytes()
        if digest(data) != expected[name]['sha256'].lower() or len(data) != expected[name]['bytes']:
            raise ValueError('Accepted bundle hash/size mismatch: ' + name)
        payload['AI Rack Synth.vst3/' + name] = data
    notice_files = sorted(notices.rglob('*'))
    if not any(p.is_file() for p in notice_files):
        raise ValueError('Original third-party notices are required.')
    for p in notice_files:
        if p.is_symlink() or p.is_junction():
            raise ValueError('Notices must not contain links.')
        if p.is_file():
            if p.suffix.lower() not in ('.md', '.txt', '.json'):
                raise ValueError('Notices may only contain reviewed text or JSON files.')
            payload['licenses/' + p.relative_to(notices).as_posix()] = p.read_bytes()
    documents = dict((
        ('docs/user-guide/install.md', 'INSTALL.md'),
        ('docs/user-guide/getting-started.md', 'GETTING_STARTED.md'),
        ('docs/user-guide/checksums.md', 'CHECKSUMS.md'),
        ('docs/user-guide/faq.md', 'FAQ.md'),
        ('docs/user-guide/alpha.md', 'ALPHA.md'),
        ('docs/user-guide/ableton-live.md', 'ABLETON_LIVE.md'),
        ('docs/user-guide/reaper.md', 'REAPER.md'),
        ('docs/user-guide/fl-studio.md', 'FL_STUDIO.md'),
        ('docs/user-guide/testing.md', 'TESTING.md'),
        ('docs/user-guide/vst3.md', 'VST3.md'),
        ('docs/user-guide/tile-array.md', 'TILE_ARRAY.md'),
        ('docs/PRIVACY.md', 'PRIVACY.md'),
        ('docs/SUPPORT.md', 'SUPPORT.md'),
        ('THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md'),
        ('CHANGELOG.md', 'CHANGELOG.md'),
        ('LICENSE', 'LICENSE'),
        ('SECURITY.md', 'SECURITY.md'),
    ))
    for source, destination in documents.items():
        def link(match):
            target = match[1]
            if ':' in target or target.startswith('#'):
                return match[0]
            path, separator, anchor = target.partition('#')
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
            mapped = documents.get(resolved)
            if mapped is None:
                mapped = 'https://github.com/' + documentation_repository + '/blob/' + documentation_commit + '/' + quote(resolved)
            return '](' + mapped + (separator + anchor if separator else '') + ')'
        data = (ROOT / source).read_text(encoding='utf-8')
        payload[destination] = re.sub(r'\]\(([^)]+)\)', link, data).encode()
    # This manifest is factual provenance, not a public redistribution approval.
    payload['release.json'] = (json.dumps({
        'package_version': version, 'embedded_product_version': '0.1.0',
        'binary_source_commit': commit, 'public_release': False,
        'documentation_commit': documentation_commit,
        'documentation_repository': documentation_repository,
        'binary_display_name': accepted.get('binary_display_name', 'unrecorded; inspect module metadata'),
        'status': ('ALPHA' if '-alpha.' in version else 'PRIVATE BETA') + '; not a stable release; see release notes for checks and limitations',
        'accepted_manifest_sha256': digest(evidence.read_bytes()),
        'files': [{'path': name, 'sha256': digest(data), 'bytes': len(data)}
                  for name, data in sorted(payload.items())],
    }, indent=2) + '\n').encode()
    output.mkdir(parents=True)
    archive = output / f'air-{version}-Windows-x64.zip'
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(payload.items()):
            entry = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(entry, data)
    with zipfile.ZipFile(archive) as z:
        if z.testzip() is not None or any(z.read(name) != data for name, data in payload.items()):
            raise ValueError('Fresh archive verification failed.')
    (output / 'SHA256SUMS.txt').write_text(f'{digest(archive.read_bytes())}  {archive.name}\n', encoding='utf-8')
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'evidence', 'notices', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    try:
        print(package(args.bundle, args.evidence, args.notices, args.version, args.output))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'Package refused: {error}\n')


if __name__ == '__main__':
    main()
