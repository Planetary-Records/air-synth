"""Repeatable private VST3 alpha smoke; short diagnostics never qualify the hour soak.

Run directly: this script acquires a separate NativeRun lease for each phase.
It does not install software, open an editor, sign in or play through speakers.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def render_scope(report, seconds):
    expected = {(rate, block) for rate in (44100, 48000, 96000)
                for block in (64, 128, 256, 512, 1024)}
    rows = report.get('rate_buffer_rows', [])
    if (report.get('success') is not True or report.get('simultaneous_instances') != 10
            or report.get('requested_soak_seconds') != seconds
            or report.get('soak_sample_rate') != 48000
            or report.get('rendered_frames_per_instance', 0) < seconds * 48000
            or len(rows) != len(expected)
            or {(r.get('sample_rate'), r.get('buffer')) for r in rows} != expected
            or not report.get('soak_elapsed_seconds', 0) >= seconds):
        raise ValueError('Incomplete or mismatched native smoke receipt.')
    for row in rows:
        for name in ('partition_difference', 'offline_realtime_difference'):
            if not 0 <= row.get(name, float('nan')) <= 1e-6:
                raise ValueError('Native smoke audio mismatch or invalid measurement.')
    full = seconds == 3600 and report['soak_elapsed_seconds'] >= 3600
    if report.get('full_hour_soak') is not full:
        raise ValueError('Incorrect full-hour qualification flag.')
    return full


def fresh_output(path):
    resolved = path.resolve()
    if not resolved.is_relative_to(ROOT / 'build') or resolved == ROOT / 'build' or resolved.exists():
        raise ValueError('Output must be a fresh directory inside this checkout/build.')
    return resolved


def smoke(bundle, validator, renderer, native, output, seconds=3600):
    if not 1 <= seconds <= 3600:
        raise ValueError('Soak seconds must be1..3600; only3600 qualifies.')
    output = fresh_output(output)
    bundle, validator, renderer, native = (p.resolve() for p in (bundle, validator, renderer, native))
    watched = [bundle / 'Contents/Resources/moduleinfo.json',
               bundle / 'Contents/x86_64-win/AI Rack Synth.vst3',
               bundle / 'Contents/x86_64-win/AI Rack Synth Companion.exe', validator, renderer, native]
    before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in watched}
    output.mkdir(parents=True)
    receipt = {'complete': False, 'success': False, 'full_smoke_complete': False,
               'input_sha256': before, 'requested_soak_seconds': seconds}
    def run(label, executable, args, limit):
        with (output / (label + '.log')).open('w', encoding='utf-8') as log:
            subprocess.run([str(native), '--private-desktop', '--wait-seconds', '300',
                            '--run-seconds', str(limit), '--', str(executable), *args],
                           stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=limit + 360)
    try:
        run('validator', validator, ['--strictness-level', '5', '--random-seed', '20261003',
                                    '--skip-gui-tests', '--validate', str(bundle)], 180)
        receipt['validator'] = 'PASS'
        run('renderer', renderer, ['--alpha-smoke', '--soak-seconds', str(seconds),
                                  '--plugin', str(bundle), '--output', str(output / 'render')], seconds + 300)
        report = json.loads((output / 'render/alpha-smoke.json').read_text(encoding='utf-8'))
        matrix = json.loads((output / 'render/report.json').read_text(encoding='utf-8'))
        if matrix.get('success') is not True or len(matrix.get('fixtures', [])) != 30:
            raise ValueError('Existing30-fixture state/audio matrix is incomplete.')
        receipt['full_smoke_complete'] = render_scope(report, seconds)
        if before != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in watched}:
            raise ValueError('A tested input changed during the smoke.')
        receipt.update(success=True, native=report, matrix='30fixtures/90freshinstances PASS')
    except Exception as error:
        receipt['error'] = str(error)
        raise
    finally:
        receipt['complete'] = True
        (output / 'smoke-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--validator', type=Path, required=True)
    parser.add_argument('--renderer', type=Path, default=ROOT / 'build/windows-release/RackSynthRender_artefacts/Release/RackSynthRender.exe')
    parser.add_argument('--native-run', type=Path, default=ROOT / 'build/native-run/Debug/RackSynthNativeRun.exe')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--soak-seconds', type=int, default=3600, help='1..3600; shorter runs are diagnostic only')
    args = parser.parse_args()
    result = smoke(args.plugin, args.validator, args.renderer, args.native_run, args.output, args.soak_seconds)
    print('PASS: ' + ('full hour smoke complete' if result['full_smoke_complete'] else 'short diagnostic; full hour qualification still required'))
