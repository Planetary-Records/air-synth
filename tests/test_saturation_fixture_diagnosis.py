"""Fixed-purpose checks of the real diagnostic and unchanged fixture consumer."""
import argparse
import csv
import datetime
import hashlib
import json
import math
import pathlib
import re
import struct
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CORPUS = ROOT / 'probes/saturation/contracts/fixtures/candidate-20261021'
PARTITIONS = (1, 17, 64, 127, 512)
ARGS = None


def identity(path):
    data = path.read_bytes()
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def corpus_identity():
    return {p.name: identity(p) for p in sorted(CORPUS.iterdir())}


class FixtureDiagnosisTests(unittest.TestCase):
    def setUp(self):
        if ARGS is None:
            self.skipTest('requires explicit --native, --acceptance and fresh --output')

    def run_native(self, label, image, args):
        argv = [str(image), *map(str, args)]
        start = datetime.datetime.now(datetime.timezone.utc).isoformat()
        result = subprocess.run(argv, cwd=ROOT, capture_output=True, timeout=120)
        log = ARGS.output / (label + '.log')
        log.write_bytes(result.stdout + result.stderr)
        receipt = dict(argv=argv, cwd=str(ROOT), start_utc=start,
                       end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                       exit=result.returncode, image=identity(image), log=identity(log))
        (ARGS.output / (label + '.command.json')).write_text(json.dumps(receipt, indent=2) + '\n')
        return result

    def test_same_acceptance_kernel_and_compile_settings(self):
        selected = []
        compile_options = []
        for image in (ARGS.native, ARGS.acceptance):
            build = image.parents[2]
            logs = build / (image.stem + '.dir') / 'Release'
            reads = list(logs.rglob('link.read.1.tlog'))
            commands = list(logs.rglob('CL.command.1.tlog'))
            self.assertEqual(len(reads), 1)
            self.assertEqual(len(commands), 1)
            libraries = [line.strip() for line in reads[0].read_text(encoding='utf-16').splitlines()
                         if re.search(r'\\RACKSYNTHSATURATION(?:TRANSITION)?CANDIDATE\.LIB$', line.upper())]
            self.assertEqual(len(set(libraries)), 1)
            selected.append(libraries[0].upper())
            command = commands[0].read_text(encoding='utf-16')
            compile_options.append(re.findall(r'/(?:O\S*|fp:\S+|arch:\S+|favor:\S+|std:\S+|M[DT]d?)(?=\s)', command))
        self.assertEqual(selected[0], selected[1], 'diagnosis must inventory the acceptance processor')
        self.assertEqual(compile_options[0], compile_options[1])
        self.assertIn('/fp:precise', compile_options[0])
        (ARGS.output / 'same-kernel-proof.json').write_text(json.dumps(dict(
            library=selected[0], identity=identity(pathlib.Path(selected[0])),
            compile_options=compile_options[0]), indent=2) + '\n')

    def test_complete_actual_comparisons(self):
        before = corpus_identity()
        self.assertEqual(len(before), 27)
        output = ARGS.output / 'diagnostic'
        result = self.run_native('diagnostic', ARGS.native, ['--output', output])
        self.assertEqual(result.returncode, 1, result.stderr.decode(errors='replace'))
        self.assertTrue((output / 'diagnosis.json').is_file(), 'missing complete diagnostic: pre-change consumer fails fast')
        report = json.loads((output / 'diagnosis.json').read_text())
        self.assertEqual(report['status'], 'fixture_mismatch')
        for field, value in dict(scope='diagnostic_only', selected_factor=None, adoptable=False,
                                 production='FORCED_BYPASS', cpu_validation='DEFERRED',
                                 comparisons=100, frames=2048, channels=2).items():
            self.assertEqual(report[field], value, field)
        expected_files = {p.name: p.name for p in CORPUS.glob('settled-*.f64')}
        expected_files.update({'event-characterization': 'event-expected.f64',
                               'serial-characterization': 'serial-expected.f64'})
        with (output / 'comparisons.csv').open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        keys = [(row['case'], int(row['partition'])) for row in rows]
        self.assertEqual(len(keys), 100)
        self.assertEqual(len(set(keys)), 100)
        self.assertEqual(set(keys), {(case, partition) for case in expected_files for partition in PARTITIONS})
        passed = 0
        for row in rows:
            expected = (CORPUS / expected_files[row['case']]).read_bytes()
            self.assertEqual(row['expected_file'], expected_files[row['case']])
            name = f"{row['case']}.partition-{row['partition']}.actual.f64"
            self.assertEqual(row['actual_file'], name)
            actual = (output / name).read_bytes()
            self.assertEqual(len(actual), 2048 * 2 * 8)
            a, b = struct.unpack('<4096d', actual), struct.unpack('<4096d', expected)
            self.assertTrue(all(map(math.isfinite, a)))
            maximum = max(abs(x - y) for x, y in zip(a, b))
            bits_a, bits_b = struct.unpack('<4096Q', actual), struct.unpack('<4096Q', expected)
            different = [i for i, (x, y) in enumerate(zip(bits_a, bits_b)) if x != y]
            self.assertEqual(float(row['max_absolute']), maximum)
            self.assertEqual(int(row['bit_differences']), len(different))
            self.assertEqual(row['pass'], str(maximum <= 1e-12 and not different).lower())
            passed += row['pass'] == 'true'
            first = different[0] if different else None
            self.assertEqual(row['first_mismatch_frame'], str(first // 2) if first is not None else '')
            self.assertEqual(row['first_mismatch_channel'], str(first % 2) if first is not None else '')
            self.assertEqual(row['actual_bits'], f'{bits_a[first]:016x}' if first is not None else '')
            self.assertEqual(row['expected_bits'], f'{bits_b[first]:016x}' if first is not None else '')
        self.assertEqual(report['passed'], passed)
        self.assertEqual(report['failed'], 100 - passed)
        self.assertGreater(report['failed'], 0)
        self.assertEqual(corpus_identity(), before)
        (ARGS.output / 'independent-metrics.json').write_text(json.dumps(dict(
            comparisons=100, passed=passed, failed=100-passed, corpus=before), indent=2) + '\n')

    def test_refusals_before_mutation(self):
        existing = ARGS.output / 'existing'
        existing.mkdir()
        (existing / 'sentinel').write_bytes(b'unchanged')
        blocked = ARGS.output / 'not-a-directory'
        blocked.write_bytes(b'unchanged')
        cases = [([], None), (['--output'], None),
                 (['--output', 'relative-fixture-diagnosis'], ROOT / 'relative-fixture-diagnosis'),
                 (['--unknown', ARGS.output / 'unknown'], ARGS.output / 'unknown'),
                 (['--output', ARGS.output / 'duplicate', '--output', ARGS.output / 'duplicate'], ARGS.output / 'duplicate'),
                 (['--output', ARGS.output / 'extra', '--unknown'], ARGS.output / 'extra'),
                 (['--output', existing], None), (['--output', blocked / 'unwritable'], None)]
        for n, (args, untouched) in enumerate(cases):
            with self.subTest(args=args):
                if untouched is not None:
                    self.assertFalse(untouched.exists())
                result = self.run_native(f'refusal-{n}', ARGS.native, args)
                self.assertEqual(result.returncode, 2, 'must explicitly refuse; a native crash is not a pass')
                self.assertIn(b'REFUSED ', result.stderr)
                if untouched is not None:
                    self.assertFalse(untouched.exists(), 'invalid CLI mutated the output')
        self.assertEqual(list(existing.iterdir()), [existing / 'sentinel'])
        self.assertEqual((existing / 'sentinel').read_bytes(), b'unchanged')
        self.assertEqual(blocked.read_bytes(), b'unchanged')

    def test_acceptance_fail_fast_and_original_semantics(self):
        result = self.run_native('acceptance', ARGS.acceptance, [])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout.count(b'COMPARE case='), 1)
        self.assertIn(b'comparison: saved expected output mismatch', result.stderr)
        result = self.run_native('acceptance-semantics', ARGS.acceptance,
                                 ['--semantic-tests', ARGS.output / 'semantic-tests'])
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        self.assertIn(b'SEMANTIC parser_tests=19 comparator_tests=1', result.stdout)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=pathlib.Path, required=True)
    parser.add_argument('--acceptance', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    ARGS = parser.parse_args()
    for name in ('native', 'acceptance'):
        if not getattr(ARGS, name).is_absolute() or not getattr(ARGS, name).is_file():
            parser.error(f'--{name} requires an absolute executable')
    if not ARGS.output.is_absolute() or ARGS.output.exists():
        parser.error('--output requires a fresh absolute directory')
    ARGS.output.mkdir(parents=True)
    unittest.main(argv=[__file__], verbosity=2)
