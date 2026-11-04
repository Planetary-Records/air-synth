import importlib.util
import pathlib
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[1] / 'tools/space_reverb_converter_cost_readback.py'
spec = importlib.util.spec_from_file_location('converter_cost', MODULE)
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


def fixture():
    lines = []
    cases = [(2,128,128,0),(2,1024,512,0),(2,128,17,0),(2,128,128,1),(1,64,64,0)]
    for order in range(2):
        for c, (factor, frames, packet, cold) in enumerate(cases):
            for backend in (0,1,3):
                lines.append(f'CONVERTER_COST_PAIR case={c} requested={backend} actual={2 if backend==3 else backend} order={order} factor={factor} physical={frames} packet={packet} cold={cold} slots=8 warm=64 measured=512 exact=1 finite=1 valid=1 capacity_acceptance=0')
                for n in range(512):
                    for position in range(2):
                        candidate = position ^ order
                        ns = 50 if candidate else 100
                        lines.append(f'CONVERTER_COST_SAMPLE case={c} requested={backend} order={order} parent={n} position={position} candidate={candidate} ns={ns}')
    lines.append('CONVERTER_COST_COMPLETE pairs=30 samples=30720 exact_and_contracts=1 caller=ordinary_unpaced workers=none capacity_acceptance=0')
    return lines


class CostReaderTests(unittest.TestCase):
    def test_complete_recomputes_every_pair(self):
        result = reader.parse(fixture())
        self.assertEqual(len(result),30)
        self.assertTrue(all(row['candidate_to_original_mean']==.5 and row['candidate_to_original_p99']==.5 for row in result))

    def test_missing_sample_rejects(self):
        lines=fixture();lines.pop(1)
        with self.assertRaises(AssertionError):reader.parse(lines)

    def test_duplicate_sample_rejects(self):
        lines=fixture();lines.append(lines[1])
        with self.assertRaises(AssertionError):reader.parse(lines)

    def test_wrong_order_rejects(self):
        lines=fixture();lines[1]=lines[1].replace('candidate=0','candidate=1')
        with self.assertRaises(AssertionError):reader.parse(lines)

    def test_wrong_actual_backend_rejects(self):
        lines=fixture();lines[0]=lines[0].replace('actual=0','actual=2')
        with self.assertRaises(AssertionError):reader.parse(lines)

    def test_nonpositive_duration_rejects(self):
        lines=fixture();lines[1]=lines[1].replace('ns=100','ns=0')
        with self.assertRaises(AssertionError):reader.parse(lines)

    def test_bad_exactness_rejects(self):
        lines=fixture();lines[0]=lines[0].replace('exact=1','exact=0')
        with self.assertRaises(AssertionError):reader.parse(lines)

    def test_no_capacity_waiver(self):
        lines=fixture();lines[-1]=lines[-1].replace('capacity_acceptance=0','capacity_acceptance=1')
        with self.assertRaises(AssertionError):reader.parse(lines)

    def test_truncated_footer_rejects(self):
        with self.assertRaises(AssertionError):reader.parse(fixture()[:-1])


if __name__=='__main__':unittest.main()
