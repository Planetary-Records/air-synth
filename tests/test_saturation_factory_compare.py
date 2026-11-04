"""Independent development verifier for the unlinked factory diagnostic packet.

Run through RackSynthNativeRun when --exporter is used (it launches native work).
No playback, reference plugin, DSP tuning or performance acceptance.
"""
import argparse
import array
import hashlib
import json
import math
import pathlib
import shutil
import struct
import subprocess
import sys

RATE, FRAMES, ACTIVE, FFT, GAP = 48000, 192000, 144000, 262144, 12000
TARGET = 10 ** (-24 / 20)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summary(values):
    assert len(values) > 0 and len(values) % 2 == 0
    assert all(math.isfinite(v) for v in values)
    rms = math.sqrt(math.fsum(v * v for v in values) / len(values))
    peak = max(map(abs, values))
    return dict(rms=rms, peak=peak, crest_db=20 * math.log10(peak / rms) if rms else 0,
                dc_left=math.fsum(values[::2]) / (len(values) // 2),
                dc_right=math.fsum(values[1::2]) / (len(values) // 2))


def close(actual, expected, name, tolerance=2e-12):
    assert math.isfinite(actual) and abs(actual - expected) <= tolerance * max(1, abs(expected)), (name, actual, expected)


def raw(path):
    a = array.array('d'); a.frombytes(path.read_bytes())
    assert sys.byteorder == 'little' and len(a) == FRAMES * 2
    return a


def wav(path, count):
    data = path.read_bytes()
    assert len(data) == 56 + count * 8
    assert data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt '
    assert struct.unpack_from('<I', data, 4)[0] == len(data) - 8
    assert struct.unpack_from('<IHHIIHH', data, 16) == (16, 3, 2, RATE, RATE * 8, 8, 32)
    assert data[36:40] == b'fact' and struct.unpack_from('<II', data, 40) == (4, count)
    assert data[48:52] == b'data' and struct.unpack_from('<I', data, 52)[0] == count * 8
    a = array.array('f'); a.frombytes(data[56:]); assert all(math.isfinite(v) and abs(v) < 1 for v in a)
    return a


def check_metrics(values, metrics):
    for key, value in summary(values).items():
        close(metrics[key], value, key)
    assert len(metrics['band_power']) == 4 and all(v >= 0 and math.isfinite(v) for v in metrics['band_power'])
    close(sum(metrics['band_power']), metrics['total_spectral_power'], 'band partition')
    close(metrics['total_spectral_power'], metrics['rms'] ** 2 * FRAMES / FFT, 'zero-padded Parseval', 1e-11)
    for window in metrics['windows']:
        begin, end = window['begin_frame'], window['end_frame']
        assert 0 <= begin < end <= FRAMES
        for key, value in summary(values[begin * 2:end * 2]).items():
            close(window[key], value, f"window {window['name']} {key}")
    assert {w['name'] for w in metrics['windows']} >= {'tail_first_20ms', 'tail_last_100ms', 'tail_full'}


def verify(root, manifest_path, expected_path):
    receipt = json.loads((root / 'comparisons.json').read_text())
    manifest = json.loads(manifest_path.read_text()); expected = json.loads(expected_path.read_text())
    assert receipt['input_manifest_sha256'] == sha(manifest_path)
    assert receipt['expected_settings_sha256'] == sha(expected_path)
    assert receipt['frames'] == FRAMES and receipt['fft_frames'] == FFT and receipt['sample_rate'] == RATE
    assert receipt['factor'] == 4 and receipt['selected_factor'] is None
    assert receipt['production'] == 'FORCED_BYPASS' and receipt['human_listening'] == 'NOT_RUN'
    assert len(receipt['factories']) == 2 and len(receipt['comparisons']) == 8
    frozen = {r['name']: r for r in manifest['inputs']}
    for factory in receipt['factories']:
        assert factory['slot_bypassed_normalized'] == 1
        assert len(factory['settings']) == 35
        fields = {r['key']: r for r in factory['settings']}
        assert set(fields) == set(expected['factories'][factory['name']])
        for key, row in fields.items():
            assert row['expected_physical'] == expected['factories'][factory['name']][key]
            assert row['actual_normalized'] == row['expected_normalized']
            assert math.isfinite(row['decoded_physical'])
    identities = []
    for record in receipt['comparisons']:
        assert record['partition_exact'] is True
        source = manifest_path.parent / frozen[record['passage']]['file']
        assert sha(source) == frozen[record['passage']]['sha256']
        copies = {}
        for kind in ['input', 'dry', 'wet']:
            signal = record[kind]; path = root / signal['raw_file']
            assert sha(path) == signal['raw_sha256']; identities.append(path)
            values = raw(path); check_metrics(values, signal['raw'])
            if kind == 'input':
                assert path.read_bytes() == source.read_bytes()
                assert max(map(abs, values)) == 1 / 16 and all(v == 0 for v in values[ACTIVE * 2:])
                continue
            gain = TARGET / summary(values)['rms']; close(signal['match_gain'], gain, 'stereo normalization gain')
            path = root / signal['matched_file']; assert sha(path) == signal['matched_sha256']; identities.append(path)
            values_float = wav(path, FRAMES); check_metrics(values_float, signal['matched'])
            assert abs(summary(values_float)['rms'] - TARGET) < 1e-9
            expected_float = array.array('f', (v * gain for v in values))
            assert values_float.tobytes() == expected_float.tobytes()
            for before, after in zip(signal['raw']['band_power'], signal['matched']['band_power']):
                close(after, before * gain * gain, 'raw vs matched spectral scale', 1e-9)
            copies[kind] = values_float
        ab_path = root / record['ab_file']; assert sha(ab_path) == record['ab_sha256']; identities.append(ab_path)
        ab = wav(ab_path, FRAMES * 2 + GAP)
        assert ab[:FRAMES * 2].tobytes() == copies['dry'].tobytes()
        assert all(v == 0 for v in ab[FRAMES * 2:(FRAMES + GAP) * 2])
        assert ab[(FRAMES + GAP) * 2:].tobytes() == copies['wet'].tobytes()
        for domain in ['raw', 'matched']:
            for i, ratio in enumerate(record[domain + '_band_power_wet_over_dry']):
                close(ratio, record['wet'][domain]['band_power'][i] / record['dry'][domain]['band_power'][i], 'wet/dry band power')
    assert len(identities) == 48 and len(set(identities)) == 48
    assert len(list(root.glob('*.f64'))) == 24 and len(list(root.glob('*.wav'))) == 24
    print('PASS independent packet: 70 factory fields, 24 raw / 16 matched / 8 A-B, hashes/extents/metrics/float gains/Parseval/partitions')
    return receipt


def run(args):
    root = args.output.resolve()
    if args.exporter and not args.verify_only:
        # A missing renderer must fail here before any output-dependent checks.
        result = subprocess.run([str(args.exporter.resolve()), '--render', str(args.inputs.resolve()), str(args.settings.resolve()), str(root)], check=False)
        assert result.returncode == 0, f'factory rendering must succeed, got exit {result.returncode}'
    receipt = verify(root, args.inputs.resolve(), args.settings.resolve())
    if args.exporter:
        before = {p.name: sha(p) for p in root.iterdir() if p.is_file()}
        result = subprocess.run([str(args.exporter.resolve()), '--render', str(args.inputs.resolve()), str(args.settings.resolve()), str(root)], check=False)
        assert result.returncode != 0 and before == {p.name: sha(p) for p in root.iterdir() if p.is_file()}
        print('PASS actual existing-output refusal, all original bytes unchanged')
        guards = root.with_name(root.name + '-rejections')
        guards.mkdir()
        manifest = json.loads(args.inputs.read_text())
        for row in manifest['inputs']:
            shutil.copyfile(args.inputs.parent / row['file'], guards / row['file'])
        cases = []
        bad_settings = json.loads(args.settings.read_text())
        bad_settings['factories']['Character Drive']['saturation.full.drive_db'] = 13
        cases.append(('settings', manifest, bad_settings, 'actual normalized factory differs'))
        bad_extent = dict(manifest, frames=FRAMES + 1)
        cases.append(('extent', bad_extent, json.loads(args.settings.read_text()), 'frozen input extent mismatch'))
        bad_hash = json.loads(args.inputs.read_text())
        bad_hash['inputs'][0]['sha256'] = '0' * 64
        cases.append(('hash', bad_hash, json.loads(args.settings.read_text()), 'frozen input identity mismatch'))
        for name, frozen_input, frozen_settings, message in cases:
            input_path, settings_path = guards / (name + '-inputs.json'), guards / (name + '-settings.json')
            input_path.write_text(json.dumps(frozen_input)); settings_path.write_text(json.dumps(frozen_settings))
            output = guards / (name + '-output')
            result = subprocess.run([str(args.exporter.resolve()), '--render', str(input_path), str(settings_path), str(output)], capture_output=True, text=True, check=False)
            assert result.returncode != 0 and message in result.stderr and not output.exists(), (name, result)
            print('PASS pre-render rejection:', name, result.stderr.strip())
    if args.receipt:
        args.receipt.write_text(json.dumps({'verified': True, 'comparisons_sha256': sha(root / 'comparisons.json'), 'ab_paths': [str(root / r['ab_file']) for r in receipt['comparisons']]}, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exporter', type=pathlib.Path)
    parser.add_argument('--verify-only', action='store_true', help='Check retained outputs; do not render again')
    parser.add_argument('--inputs', type=pathlib.Path, required=True)
    parser.add_argument('--settings', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--receipt', type=pathlib.Path)
    run(parser.parse_args())
