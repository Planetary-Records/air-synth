"""Prepare exact historical source bytes for unchanged frozen generators."""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import tempfile

import saturation_numerical_stability as stability


def pinned_bytes(path, expected):
    raw = path.read_bytes()
    def identity(data):
        return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    if identity(raw) == expected:
        return raw
    if b'\r' in raw:
        raise ValueError('unknown historical source bytes: ' + str(path))
    restored = raw.replace(b'\n', b'\r\n')
    if identity(restored) != expected:
        raise ValueError('unknown historical source bytes: ' + str(path))
    return restored


def unredirected(path):
    for ancestor in (path, *path.parents):
        try:
            info = ancestor.lstat()
        except FileNotFoundError:
            continue
        stability.require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                          'redirected historical source path: ' + str(ancestor))


def materialize(source, output, *, cmake_output=False):
    source, output = Path(source), Path(output)
    stability.require(output.is_absolute(), 'absolute build output required')
    unredirected(source); unredirected(output)
    build = (stability.ROOT / 'build').resolve()
    stability.require(output.resolve().is_relative_to(build) and output.resolve() != build,
                      'historical source output must be under this worktree build')
    names = set(stability.NUMERICAL) | {'manifest.json'}
    stability.require(source.is_dir() and {p.name for p in source.iterdir()} == names,
                      'complete historical input inventory required')
    for name in names:
        path = source / name; unredirected(path)
        stability.require(path.is_file(), 'regular historical input required')
    manifest_raw = (source / 'manifest.json').read_bytes()
    manifest = json.loads(manifest_raw)
    stability.require(isinstance(manifest, dict) and isinstance(manifest.get('files'), dict)
                      and set(manifest['files']) == set(stability.NUMERICAL), 'historical manifest inventory')
    files = {name: pinned_bytes(source / name, manifest['files'][name]) for name in stability.NUMERICAL}
    files['manifest.json'] = manifest_raw
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.saturation-frozen-', dir=output.parent) as temporary:
        stage = Path(temporary) / 'complete'; stage.mkdir()
        for name, raw in files.items(): (stage / name).write_bytes(raw)
        # This independently binds the staged manifest and every byte to the original source record.
        stability.check_frozen(stage)
        if cmake_output and output.exists():
            unredirected(output)
            if output.is_dir() and not any(output.iterdir()):
                # CMake creates custom-output parents; rmdir refuses a newly appeared file.
                output.rmdir()
        if output.exists():
            stability.require(output.is_dir() and {p.name for p in output.iterdir()} == names,
                              'existing historical output inventory differs')
            for name, raw in files.items():
                path = output / name; unredirected(path)
                stability.require(path.is_file() and path.read_bytes() == raw, 'existing historical output differs: ' + name)
        else:
            stage.rename(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cmake-output', action='store_true')
    args = parser.parse_args()
    try:
        materialize(args.source, args.output, cmake_output=args.cmake_output)
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
