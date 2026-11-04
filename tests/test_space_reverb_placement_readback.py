import copy
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('placement', Path(__file__).parents[1] / 'tools/space_reverb_placement_readback.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def fixture():
    h = [0x3155504356525353, 1, 0, 44100, 64, 1, 0,
         6891, 13782, 1000000000, 100000000000, 1, 100000000000, 6891, 3, 100010000000, 0, 0, 15877400]
    records = []
    for n, duration in enumerate([200000, 900000, 500000]):
        release = h[10] + (n + 1) * h[9] * h[4] // h[3]
        records.append([release, release + 1000, release + 11000, release + 1000 + duration,
                        600000, 0, 0, 0, 8, 0, 0, 0, 0, 1])
    raw = struct.pack('<Q6I12Q', *h) + b''.join(struct.pack('<14Q', *r) for r in records)
    p = {'version': 1, 'diagnostic_only': True, 'topology_error': 0,
         'cpu_sets': [{'id': 256, 'group': 0, 'index': 0, 'core': 0, 'efficiency': 1, 'flags': 0},
                      {'id': 264, 'group': 0, 'index': 8, 'core': 8, 'efficiency': 0, 'flags': 0}],
         'warm_completed': 6891, 'measured': 3,
         'process_power_before': {'ok': 1, 'error': 0, 'control': 0, 'state': 0},
         'ideal_before': {'ok': 1, 'error': 0, 'group': 0, 'index': 0},
         'affinity_before': {'ok': 0, 'error': 5, 'group': 0, 'mask': 0},
         'placements': [[0, n, 0, 8, 0, 8] for n in range(6891)] +
                       [[1, 0, 0, 0, 0, 0], [1, 1, 0, 8, 0, 8], [1, 2, 0, 8, 0, 0]]}
    return raw, p


class PlacementTests(unittest.TestCase):
    def test_join_and_partition(self):
        raw, p = fixture()
        r = reader.analyze(raw, p)
        self.assertEqual(r['status'], 'DIAGNOSTIC_ONLY')
        self.assertIs(r['capacity_acceptance'], False)
        self.assertEqual(r['warm_callbacks'], 6891)
        self.assertEqual(r['measured_callbacks'], 3)
        self.assertEqual(r['endpoint_migrations'], 1)
        self.assertEqual(r['same_endpoint_efficiency']['1']['count'], 1)
        self.assertEqual(r['same_endpoint_efficiency']['1']['p99_seconds'], .0002)
        self.assertEqual(r['same_endpoint_efficiency']['0']['count'], 1)
        self.assertEqual(r['same_endpoint_efficiency']['0']['p99_seconds'], .0009)
        self.assertIn('cannot exclude', r['limitation'])

    def test_invalid_topology(self):
        raw, p = fixture()
        for change in ['duplicate', 'missing', 'error', 'id']:
            q = copy.deepcopy(p)
            if change == 'duplicate': q['cpu_sets'].append(q['cpu_sets'][0])
            if change == 'missing': q['cpu_sets'].pop()
            if change == 'error': q['topology_error'] = 5
            if change == 'id': q['cpu_sets'][1]['id'] = 256
            with self.subTest(change=change), self.assertRaises(ValueError): reader.analyze(raw, q)

    def test_reject_incomplete_or_bad_placement(self):
        raw, p = fixture()
        for change in ['missing', 'duplicate', 'ordinal', 'phase', 'count', 'flag', 'boolean']:
            q = copy.deepcopy(p)
            if change == 'missing': q['placements'].pop()
            if change == 'duplicate': q['placements'].append(q['placements'][-1])
            if change == 'ordinal': q['placements'][-1][1] = 0
            if change == 'phase': q['placements'][-1][0] = 0
            if change == 'count': q['measured'] = 4
            if change == 'flag': q['diagnostic_only'] = False
            if change == 'boolean': q['placements'][-1][2] = False
            with self.subTest(change=change), self.assertRaises(ValueError): reader.analyze(raw, q)

    def test_bad_raw(self):
        raw, p = fixture()
        for bad in [raw[:-1], raw + bytes(112), bytes(128) + raw[128:]]:
            with self.assertRaises(ValueError): reader.analyze(bad, p)
        b = bytearray(raw)
        struct.pack_into('<Q', b, 128 + 3 * 8, 0)
        with self.assertRaises(ValueError): reader.analyze(bytes(b), p)

    def test_partial_is_diagnostic(self):
        raw, p = fixture()
        r = reader.analyze(raw, p)
        self.assertFalse(r['complete_measurement'])
        self.assertFalse(r['capacity_acceptance'])
        self.assertEqual(sum(v['count'] for v in r['same_endpoint_efficiency'].values()), 2)

    def test_processor_and_api_observations(self):
        raw, p = fixture()
        r = reader.analyze(raw, p)
        self.assertEqual(r['same_endpoint_processor']['0:8']['p99_seconds'], .0009)
        self.assertEqual(r['measured_start_processor_counts'], {'0:0': 1, '0:8': 2})
        self.assertEqual(r['caller_observations']['affinity_before']['error'], 5)
        self.assertEqual(r['caller_observations']['process_power_before']['control'], 0)


if __name__ == '__main__': unittest.main()
