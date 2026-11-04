"""CPU preparation checks. No invocation of the CPU measurement mode is permitted here."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import struct
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
ARGS = None


def recover_default(source):
    """Select the original branches of only the new private CPU conditionals."""
    result, stack = [], []
    for line in source.splitlines(keepends=True):
        if re.match(r'#if defined\(RACKSYNTH_SATURATION_CPU_(ONLY|POLICY)\)', line):
            stack.append([False, True])
        elif line.startswith('#if'):
            stack.append([True, False])
            if all(s[0] for s in stack):
                result.append(line)
        elif line.startswith('#else') and stack:
            if stack[-1][1]:
                stack[-1][0] = not stack[-1][0]
            elif all(s[0] for s in stack):
                result.append(line)
        elif line.startswith('#endif') and stack:
            include = all(s[0] for s in stack)
            _, cpu = stack.pop()
            if not cpu and include:
                result.append(line)
        elif all(s[0] for s in stack):
            result.append(line)
    if stack:
        raise ValueError('unclosed conditionals')
    return ''.join(result)


class SourcePolicy(unittest.TestCase):
    def test_default_source_exact_recovery(self):
        source = (ROOT / 'probes/saturation/SaturationFeasibility.cpp').read_text()
        recovered = recover_default(source)
        # Accepted quality-cohort evidence extended the default source; CPU work stays pinned below.
        inventory_entry = '"contracts/SaturationOptimizationWitness.h",'
        self.assertEqual(recovered.count(inventory_entry), 1)
        self.assertEqual(hashlib.sha256(recovered.replace(inventory_entry, '', 1).encode()).hexdigest(),
                         '50761a4e3029070e86a2eaed7c286355218d85bb2edef451a50315c9d648c5d6')
        cpu_work = 'SaturationSettings cpuSettings(' + recovered.split(
            'SaturationSettings cpuSettings(', 1)[1].split('void finish(Evidence& evidence)', 1)[0]
        self.assertEqual(hashlib.sha256(cpu_work.encode()).hexdigest(),
                         'ac3fc90e6ed760d2ec83d11c098483acc1fbdd3b13695cd829f16cad5eadf10b')
        self.assertEqual(source.count('const auto before=Clock::now();'), 1)

    def test_private_target_separation_and_no_measurement_ctest(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        for name in ('RackSynthSaturationTransitionCpu', 'RackSynthSaturationCpuPolicyTests'):
            self.assertIn(f'target_compile_definitions({name} PRIVATE RACKSYNTH_SATURATION_CPU_ONLY=1', cmake)
            self.assertIn(f'target_link_libraries({name} PRIVATE RackSynthSaturationTransitionCandidate RackSynthSaturationAnalysis)', cmake)
        self.assertIn('probes/saturation/SaturationFeasibility.cpp probes/saturation/SaturationCpuPolicyTests.cpp', cmake)
        self.assertNotIn('#include "SaturationFeasibility.cpp"', (ROOT / 'probes/saturation/SaturationCpuPolicyTests.cpp').read_text())
        for command in re.findall(r'racksynth_add_test\(NAME RackSynthSaturationCpu.*?\)', cmake, re.S):
            self.assertNotIn('--measure', command)
        source = (ROOT / 'probes/saturation/SaturationFeasibility.cpp').read_text()
        describe = source.split('// CPU description begins\n', 1)[1].split('// CPU description ends\n', 1)[0]
        for forbidden in ('Candidate ', 'Candidate(', '.process(', 'timings(', 'Clock::now'):
            self.assertNotIn(forbidden, describe)


class NativePreparation(unittest.TestCase):
    def run_saved(self, image, arguments, label):
        result = subprocess.run([str(image), *arguments], capture_output=True)
        (ARGS.output / (label + '.stdout')).write_bytes(result.stdout)
        (ARGS.output / (label + '.stderr')).write_bytes(result.stderr)
        (ARGS.output / (label + '.exit')).write_text(str(result.returncode) + '\n')
        return result

    def setUp(self):
        if ARGS is None:
            self.skipTest('native checks require explicit images')

    def test_policy_synthetic_and_parser_helpers(self):
        if ARGS.check not in ('all', 'policy'):
            self.skipTest('other bounded cohort')
        result = self.run_saved(ARGS.policy, [str(ARGS.output / 'synthetic')], 'policy')
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        summary = json.loads((ARGS.output / 'synthetic/self-check.json').read_text())
        self.assertEqual(summary['actual_measurement'], 'NOT_RUN')
        self.assertGreaterEqual(summary['synthetic_checks'], 20)
        for name, code in (('pass', 0), ('reject', 1), ('invalid', 2), ('missing', 2),
                           ('duplicate', 2), ('unknown', 2), ('nonfinite', 2)):
            receipt = json.loads((ARGS.output / f'synthetic/{name}.json').read_text())
            self.assertEqual(receipt['exit_code'], code)
            self.assertTrue(receipt['synthetic'])
            self.assertEqual(receipt['actual_measurement'], 'NOT_RUN')
            self.assertFalse(receipt['adoptable'])
            self.assertIsNone(receipt['selected_factor'])

    def test_describe_complete_deterministic(self):
        if ARGS.check not in ('all', 'description'):
            self.skipTest('other bounded cohort')
        directories = [ARGS.output / f'description-{i}' for i in range(2)]
        for i, directory in enumerate(directories):
            result = self.run_saved(ARGS.cpu, ['--describe', '--output', str(directory)], f'describe-{i}')
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        self.assertEqual((directories[0] / 'workload.json').read_bytes(), (directories[1] / 'workload.json').read_bytes())
        manifest = json.loads((directories[0] / 'workload.json').read_text())
        self.assertEqual(len(manifest['cases']), 24)
        self.assertEqual({(x['suite'], x['rate'], x['block'], x['moving']) for x in manifest['cases']},
                         {(suite, rate, block, moving) for suite in ('base', 'structural_overlap')
                          for rate in (44100, 48000, 96000) for block in (64, 512) for moving in (False, True)})
        self.assertEqual(len(manifest['settings']), 24)
        self.assertEqual({(x['unit'], x['endpoint']) for x in manifest['settings']},
                         {(unit, endpoint) for unit in range(8) for endpoint in ('fixed', 'A', 'B')})
        for setting in manifest['settings']:
            self.assertEqual(len(setting['lanes']), 4)
            self.assertEqual([x['active'] for x in setting['lanes']], [False, True, True, True])
        self.assertEqual(manifest['schedule_self_check']['slots'], 8)
        self.assertTrue(manifest['schedule_self_check']['pass'])
        self.assertEqual(manifest['schedule_self_check']['active_lanes_per_slot'], 3)
        self.assertEqual(manifest['schedule_self_check']['cases'], 12)
        self.assertEqual(manifest['schedule_self_check']['observed_boundaries_per_case'], 600)
        self.assertEqual(manifest['schedule_self_check']['callback_observations_per_boundary'], 2)
        self.assertEqual(manifest['schedule_self_check']['focused_regression_callbacks'], [27776, 27840, 27904])
        self.assertTrue(manifest['coefficients'])
        self.assertTrue(manifest['sources'])
        self.assertEqual(manifest['image']['sha256'], hashlib.sha256(ARGS.cpu.read_bytes()).hexdigest())
        for source in manifest['sources']:
            path = ROOT / ('CMakeLists.txt' if source['file'] == 'CMakeLists.txt' else 'probes/saturation/' + source['file'])
            self.assertEqual(source['sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
        for row in manifest['coefficients'] + manifest['compensation_tables']:
            self.assertTrue(row['values'])
            self.assertEqual(row['sha256'], hashlib.sha256(struct.pack('<' + 'd' * len(row['values']), *row['values'])).hexdigest())
        for directory in directories:
            receipt = json.loads((directory / 'decision.json').read_text())
            self.assertEqual(receipt['cpu_validation'], 'DEFERRED')
            self.assertEqual(receipt['workload_sha256'], hashlib.sha256((directory / 'workload.json').read_bytes()).hexdigest())
            self.assertFalse(receipt['adoptable'])
            self.assertIsNone(receipt['selected_factor'])
            self.assertEqual(set(receipt['not_run'].values()), {'NOT_RUN'})
            self.assertEqual({p.name for p in directory.iterdir()}, {'workload.json', 'decision.json'})

    def test_safe_cli_refusals_before_mutation(self):
        if ARGS.check not in ('all', 'refusals'):
            self.skipTest('other bounded cohort')
        existing = ARGS.output / 'existing'
        existing.mkdir()
        marker = existing / 'sentinel'
        marker.write_bytes(b'preserved')
        fresh = ARGS.output / 'must-not-exist'
        cases = [[], ['--output', str(fresh)], ['--factor', '4', '--output', str(fresh)],
                 ['--describe'], ['--describe', '--output', 'relative'],
                 ['--describe', '--output', str(existing)],
                 ['--describe', '--describe', '--output', str(fresh)],
                 ['--describe', '--factor', '4', '--output', str(fresh)],
                 ['--describe', '--output', str(fresh), '--output', str(fresh)],
                 ['--describe', '--output', str(fresh), '--unknown'],
                 ['--describe', '--output', str(fresh), '--factor'],
                 ['--describe', '--output', '']]
        for index, arguments in enumerate(cases):
            with self.subTest(arguments=arguments):
                result = self.run_saved(ARGS.cpu, arguments, f'refusal-{index}')
                self.assertEqual(result.returncode, 2)
                self.assertFalse(fresh.exists())
                self.assertEqual(marker.read_bytes(), b'preserved')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--cpu', type=Path)
    parser.add_argument('--policy', type=Path)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--check', choices=('all', 'policy', 'description', 'refusals'), default='all')
    options = parser.parse_args()
    if not options.self_test:
        if not options.cpu or not options.policy or not options.output_root:
            parser.error('native checks require --cpu, --policy, --output-root')
        ARGS = options
        ARGS.output = ARGS.output_root.resolve() / ('cpu-preparation-' + uuid.uuid4().hex)
        ARGS.output.mkdir(parents=True, exist_ok=False)
        print('EVIDENCE_DIRECTORY=' + str(ARGS.output), flush=True)
    unittest.main(argv=[__file__], verbosity=2)
