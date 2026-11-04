import importlib.util,pathlib,unittest
spec=importlib.util.spec_from_file_location('stage',pathlib.Path(__file__).resolve().parents[1]/'tools/space_reverb_modal_stage_readback.py')
reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)

def fixture():
    lines=[]
    for c in range(4):
        lines.append(f'MODAL_STAGE_CASE case={c} factor={c//2+1} host={512 if c%2 else 64} rate=96000 modes=128 slots=8 warm=64 measured=512 valid=1 finite=1 live_adoptions=0 capacity_acceptance=0')
        for n in range(512):
            for stage in reader.STAGES:lines.append(f'MODAL_STAGE_SAMPLE case={c} parent={n} stage={stage} ns={0 if c<2 and stage in ("down","up") else 100}')
    return lines+['MODAL_STAGE_COMPLETE cases=4 samples=20480 valid=1 caller=ordinary_unpaced live_adoptions=0 capacity_acceptance=0']

class StageReaderTests(unittest.TestCase):
    def test_complete_recomputes(self):
        rows=reader.parse(fixture());self.assertEqual(len(rows),4);self.assertEqual(rows[2]['stage_costs'][4]['mean_ns'],100)
    def test_incomplete_duplicate_invalid_reject(self):
        for kind in ('missing','duplicate','wrong_case','bad_finite','waiver'):
            lines=fixture()
            if kind=='missing':lines.pop(1)
            elif kind=='duplicate':lines.append(lines[1])
            elif kind=='wrong_case':lines[1]=lines[1].replace('case=0','case=4')
            elif kind=='bad_finite':lines[0]=lines[0].replace('finite=1','finite=0')
            else:lines[-1]=lines[-1].replace('capacity_acceptance=0','capacity_acceptance=1')
            with self.subTest(kind=kind),self.assertRaises(AssertionError):reader.parse(lines)

if __name__=='__main__':unittest.main()
